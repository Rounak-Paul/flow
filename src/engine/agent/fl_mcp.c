#include "fl_mcp.h"
#include "core/fl_thread.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMEOUT_MS 30000u
#define PROTOCOL_LATEST "2025-06-18"

typedef struct Fl_McpEntry {
    char *name, *description;
    cJSON *schema;
    bool read_only;
    Fl_McpToolFn run;
    void *user_data;
} Fl_McpEntry;

struct Fl_Mcp {
    char *server_name, *server_version, *instructions;
    Fl_McpPostFn post;
    void *post_data;
    uint32_t timeout_ms;
    Fl_McpEntry *tools;
    size_t tool_count;
    Fl_Mutex mutex;
    Fl_CondVar changed;
    bool closing;
};

/* One tool invocation shared between the transport thread (waiter) and the owner thread (runner). */
struct Fl_McpCall {
    Fl_Mcp *mcp;
    const Fl_McpEntry *entry;
    cJSON *arguments, *result;
    unsigned refs;
    bool done;
};

static char *copy(const char *text)
{
    size_t n = text ? strlen(text) + 1 : 0;
    char *out = n ? malloc(n) : NULL;
    if (out) memcpy(out, text, n);
    return out;
}

Fl_Mcp *fl_mcp_create(const Fl_McpDesc *d)
{
    if (!d || !d->server_name || !d->server_version || !d->post) return NULL;
    Fl_Mcp *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->server_name = copy(d->server_name); m->server_version = copy(d->server_version);
    m->instructions = d->instructions ? copy(d->instructions) : NULL;
    m->post = d->post; m->post_data = d->post_data;
    m->timeout_ms = d->call_timeout_ms ? d->call_timeout_ms : DEFAULT_TIMEOUT_MS;
    if (!m->server_name || !m->server_version || (d->instructions && !m->instructions) ||
        !fl_mutex_init(&m->mutex)) {fl_mcp_destroy(m); return NULL;}
    if (!fl_cond_init(&m->changed)) {fl_mutex_destroy(&m->mutex); free(m->server_name); free(m->server_version); free(m->instructions); free(m); return NULL;}
    return m;
}

void fl_mcp_destroy(Fl_Mcp *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->tool_count; ++i) {
        free(m->tools[i].name); free(m->tools[i].description); cJSON_Delete(m->tools[i].schema);
    }
    free(m->tools); free(m->server_name); free(m->server_version); free(m->instructions);
    fl_cond_destroy(&m->changed); fl_mutex_destroy(&m->mutex);
    free(m);
}

bool fl_mcp_register(Fl_Mcp *m, const Fl_McpTool *t)
{
    if (!m || !t || !t->name || !t->name[0] || !t->description || !t->input_schema || !t->run) return false;
    for (const char *c = t->name; *c; ++c) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
    for (size_t i = 0; i < m->tool_count; ++i) if (!strcmp(m->tools[i].name, t->name)) return false;
    cJSON *schema = cJSON_Parse(t->input_schema);
    if (!schema || !cJSON_IsObject(schema)) {cJSON_Delete(schema); return false;}
    Fl_McpEntry *grown = realloc(m->tools, (m->tool_count + 1) * sizeof(*grown));
    if (!grown) {cJSON_Delete(schema); return false;}
    m->tools = grown;
    Fl_McpEntry *e = &m->tools[m->tool_count];
    *e = (Fl_McpEntry){copy(t->name), copy(t->description), schema, t->read_only, t->run, t->user_data};
    if (!e->name || !e->description) {free(e->name); free(e->description); cJSON_Delete(schema); return false;}
    ++m->tool_count;
    return true;
}

void fl_mcp_shutdown(Fl_Mcp *m)
{
    if (!m) return;
    fl_mutex_lock(&m->mutex); m->closing = true; fl_cond_broadcast(&m->changed); fl_mutex_unlock(&m->mutex);
}

/* Drop one reference; the last one frees the call. The mcp mutex guards the count. */
static void release(Fl_McpCall *call)
{
    Fl_Mcp *m = call->mcp;
    fl_mutex_lock(&m->mutex);
    bool last = --call->refs == 0;
    fl_mutex_unlock(&m->mutex);
    if (!last) return;
    cJSON_Delete(call->arguments); cJSON_Delete(call->result); free(call);
}

/* Publish the result, wake the waiter and drop the runner's reference. */
static void finish(Fl_McpCall *call, cJSON *result)
{
    Fl_Mcp *m = call->mcp;
    fl_mutex_lock(&m->mutex);
    if (call->done) {fl_mutex_unlock(&m->mutex); cJSON_Delete(result); return;}
    call->result = result; call->done = true;
    fl_cond_broadcast(&m->changed);
    fl_mutex_unlock(&m->mutex);
    release(call);
}

/* Build the MCP tool-result envelope around content blocks. */
static cJSON *envelope(cJSON *content, bool is_error)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {cJSON_Delete(content); return NULL;}
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", is_error);
    return result;
}

/* A single text block holding compact JSON. */
static cJSON *text_block(const cJSON *value)
{
    char *text = cJSON_PrintUnformatted(value);
    cJSON *block = cJSON_CreateObject();
    if (!text || !block) {free(text); cJSON_Delete(block); return NULL;}
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", text);
    free(text);
    return block;
}

void fl_mcp_return_json(Fl_McpCall *call, cJSON *value)
{
    cJSON *content = cJSON_CreateArray(), *block = value ? text_block(value) : NULL;
    cJSON_Delete(value);
    if (content && block) cJSON_AddItemToArray(content, block); else cJSON_Delete(block);
    finish(call, envelope(content, false));
}

void fl_mcp_return_error_object(Fl_McpCall *call, cJSON *error)
{
    cJSON *wrapper = cJSON_CreateObject(), *content = cJSON_CreateArray();
    if (wrapper && error) cJSON_AddItemToObject(wrapper, "error", error); else cJSON_Delete(error);
    cJSON *block = wrapper ? text_block(wrapper) : NULL;
    cJSON_Delete(wrapper);
    if (content && block) cJSON_AddItemToArray(content, block); else cJSON_Delete(block);
    finish(call, envelope(content, true));
}

void fl_mcp_return_error(Fl_McpCall *call, const char *code, const char *format, ...)
{
    char message[1024];
    va_list args; va_start(args, format); vsnprintf(message, sizeof(message), format, args); va_end(args);
    cJSON *error = cJSON_CreateObject();
    if (error) {cJSON_AddStringToObject(error, "code", code); cJSON_AddStringToObject(error, "message", message);}
    fl_mcp_return_error_object(call, error);
}

/* Standard base64 for image content. */
static char *base64(const unsigned char *bytes, size_t size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (size > (SIZE_MAX - 4) / 4 * 3) return NULL;
    char *out = malloc((size + 2) / 3 * 4 + 1), *w = out;
    if (!out) return NULL;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = (uint32_t)bytes[i] << 16 | (i + 1 < size ? (uint32_t)bytes[i + 1] << 8 : 0) | (i + 2 < size ? bytes[i + 2] : 0);
        *w++ = alphabet[v >> 18 & 63]; *w++ = alphabet[v >> 12 & 63];
        *w++ = i + 1 < size ? alphabet[v >> 6 & 63] : '='; *w++ = i + 2 < size ? alphabet[v & 63] : '=';
    }
    *w = 0; return out;
}

void fl_mcp_return_image(Fl_McpCall *call, cJSON *value, const unsigned char *bytes, size_t size, const char *mime_type)
{
    cJSON *content = cJSON_CreateArray(), *image = cJSON_CreateObject(), *meta = value ? text_block(value) : NULL;
    char *encoded = base64(bytes, size);
    cJSON_Delete(value);
    if (!content || !image || !encoded) {
        free(encoded); cJSON_Delete(content); cJSON_Delete(image); cJSON_Delete(meta);
        fl_mcp_return_error(call, "internal", "out of memory encoding image"); return;
    }
    cJSON_AddStringToObject(image, "type", "image");
    cJSON_AddStringToObject(image, "data", encoded); free(encoded);
    cJSON_AddStringToObject(image, "mimeType", mime_type);
    if (meta) cJSON_AddItemToArray(content, meta);
    cJSON_AddItemToArray(content, image);
    finish(call, envelope(content, false));
}

/* Owner-thread task: run the tool with the call's arguments. */
static void run_task(void *data)
{
    Fl_McpCall *call = data;
    fl_mutex_lock(&call->mcp->mutex);
    bool skip = call->mcp->closing || call->done;
    fl_mutex_unlock(&call->mcp->mutex);
    if (skip) {fl_mcp_return_error(call, "shutting_down", "Flow is closing the project"); return;}
    call->entry->run(call, call->arguments, call->entry->user_data);
}

/* Run a tool on the owner thread and wait for its result. Returns the envelope, owned by the caller. */
static cJSON *call_tool(Fl_Mcp *m, const Fl_McpEntry *entry, cJSON *arguments)
{
    Fl_McpCall *call = calloc(1, sizeof(*call));
    if (!call) {cJSON_Delete(arguments); return NULL;}
    call->mcp = m; call->entry = entry; call->arguments = arguments; call->refs = 2;
    if (!m->post(run_task, call, m->post_data)) {
        release(call); release(call); return NULL;
    }
    fl_mutex_lock(&m->mutex);
    bool timed_out = false;
    while (!call->done && !m->closing && !timed_out) timed_out = !fl_cond_wait_ms(&m->changed, &m->mutex, m->timeout_ms);
    cJSON *result = call->done ? call->result : NULL;
    if (call->done) call->result = NULL;
    fl_mutex_unlock(&m->mutex);
    if (!result) {
        cJSON *content = cJSON_CreateArray(), *error = cJSON_CreateObject(), *wrapper = cJSON_CreateObject();
        if (error) {
            cJSON_AddStringToObject(error, "code", timed_out ? "timeout" : "shutting_down");
            cJSON_AddStringToObject(error, "message", timed_out ? "The editor did not finish the call in time; it may still complete" : "Flow is closing the project");
        }
        if (wrapper && error) cJSON_AddItemToObject(wrapper, "error", error); else cJSON_Delete(error);
        cJSON *block = wrapper ? text_block(wrapper) : NULL;
        cJSON_Delete(wrapper);
        if (content && block) cJSON_AddItemToArray(content, block); else cJSON_Delete(block);
        result = envelope(content, true);
    }
    release(call);
    return result;
}

/* JSON-RPC response with result or error, rendered to text. Consumes result. */
static char *respond(const cJSON *id, cJSON *result, int code, const char *message)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {cJSON_Delete(result); return NULL;}
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    if (message) {
        cJSON *error = cJSON_AddObjectToObject(response, "error");
        if (error) {cJSON_AddNumberToObject(error, "code", code); cJSON_AddStringToObject(error, "message", message);}
        cJSON_Delete(result);
    } else cJSON_AddItemToObject(response, "result", result ? result : cJSON_CreateObject());
    char *text = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return text;
}

static cJSON *initialize_result(const Fl_Mcp *m, const cJSON *params)
{
    static const char *supported[] = {PROTOCOL_LATEST, "2025-03-26", "2024-11-05"};
    const cJSON *asked = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
    const char *version = supported[0];
    for (size_t i = 0; cJSON_IsString(asked) && i < sizeof(supported) / sizeof(*supported); ++i)
        if (!strcmp(asked->valuestring, supported[i])) version = supported[i];
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *capabilities = cJSON_AddObjectToObject(result, "capabilities");
    cJSON *tools = capabilities ? cJSON_AddObjectToObject(capabilities, "tools") : NULL;
    if (tools) cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    if (info) {cJSON_AddStringToObject(info, "name", m->server_name); cJSON_AddStringToObject(info, "version", m->server_version);}
    if (m->instructions) cJSON_AddStringToObject(result, "instructions", m->instructions);
    return result;
}

static cJSON *tools_list_result(const Fl_Mcp *m)
{
    cJSON *result = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(result, "tools");
    for (size_t i = 0; list && i < m->tool_count; ++i) {
        const Fl_McpEntry *e = &m->tools[i];
        cJSON *tool = cJSON_CreateObject();
        if (!tool) break;
        cJSON_AddStringToObject(tool, "name", e->name);
        cJSON_AddStringToObject(tool, "description", e->description);
        cJSON_AddItemToObject(tool, "inputSchema", cJSON_Duplicate(e->schema, true));
        cJSON *hints = cJSON_AddObjectToObject(tool, "annotations");
        if (hints) {cJSON_AddBoolToObject(hints, "readOnlyHint", e->read_only); cJSON_AddBoolToObject(hints, "openWorldHint", false);}
        cJSON_AddItemToArray(list, tool);
    }
    return result;
}

char *fl_mcp_handle(Fl_Mcp *m, const char *body, size_t length)
{
    cJSON *request = m && body ? cJSON_ParseWithLength(body, length) : NULL;
    if (!request) return respond(NULL, NULL, -32700, "Parse error");
    if (!cJSON_IsObject(request)) {cJSON_Delete(request); return respond(NULL, NULL, -32600, "Batches and non-object messages are not supported");}
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (!cJSON_IsString(method) || (id && !cJSON_IsString(id) && !cJSON_IsNumber(id))) {
        cJSON_Delete(request); return respond(NULL, NULL, -32600, "Invalid request");
    }
    if (!id) {cJSON_Delete(request); return NULL;}
    char *response = NULL;
    if (!strcmp(method->valuestring, "initialize")) response = respond(id, initialize_result(m, params), 0, NULL);
    else if (!strcmp(method->valuestring, "ping")) response = respond(id, NULL, 0, NULL);
    else if (!strcmp(method->valuestring, "tools/list")) response = respond(id, tools_list_result(m), 0, NULL);
    else if (!strcmp(method->valuestring, "tools/call")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
        const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        const Fl_McpEntry *entry = NULL;
        for (size_t i = 0; cJSON_IsString(name) && i < m->tool_count; ++i) if (!strcmp(m->tools[i].name, name->valuestring)) entry = &m->tools[i];
        if (!entry) response = respond(id, NULL, -32602, "Unknown tool");
        else if (arguments && !cJSON_IsObject(arguments)) response = respond(id, NULL, -32602, "arguments must be an object");
        else {
            cJSON *owned = arguments ? cJSON_Duplicate(arguments, true) : cJSON_CreateObject();
            cJSON *result = owned ? call_tool(m, entry, owned) : NULL;
            response = result ? respond(id, result, 0, NULL) : respond(id, NULL, -32603, "Flow could not run the tool");
        }
    } else response = respond(id, NULL, -32601, "Method not found");
    cJSON_Delete(request);
    return response;
}
