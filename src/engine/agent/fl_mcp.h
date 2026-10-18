#pragma once
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Model Context Protocol core: JSON-RPC 2.0 dispatch, tool registry and the hand-off of tool calls to the
 * owner thread. Transport-neutral; fl_mcp_http.h binds it to HTTP.
 *
 * Threading: fl_mcp_handle may be called from any number of transport threads. Tool bodies always run on the
 * owner thread through the host's post function, so they may touch the project and UI freely. Tools are
 * registered before the transport starts and never removed.
 */
typedef struct Fl_Mcp Fl_Mcp;
typedef struct Fl_McpCall Fl_McpCall;

/* Tool body, run on the owner thread. It must call exactly one fl_mcp_return_* function, now or later
   (for example once a frame has been captured); arguments stay valid until then. */
typedef void (*Fl_McpToolFn)(Fl_McpCall *call, const cJSON *arguments, void *user_data);

typedef struct Fl_McpTool {
    const char *name;           /* [a-z0-9_]+ */
    const char *description;
    const char *input_schema;   /* JSON Schema object as JSON text */
    bool read_only;             /* never changes the document */
    Fl_McpToolFn run;
    void *user_data;
} Fl_McpTool;

/* Schedule task(data) on the owner thread. Callable from any thread; false when the owner cannot accept it. */
typedef bool (*Fl_McpPostFn)(void (*task)(void *data), void *data, void *user_data);

typedef struct Fl_McpDesc {
    const char *server_name, *server_version;
    const char *instructions;   /* usage guidance returned to the client at initialize */
    Fl_McpPostFn post;
    void *post_data;
    uint32_t call_timeout_ms;   /* zero selects 30 seconds */
} Fl_McpDesc;

/* Create a server core; NULL on invalid description or allocation failure. */
Fl_Mcp *fl_mcp_create(const Fl_McpDesc *desc);
/* Release the registry. Stop the transport first. NULL is safe. */
void fl_mcp_destroy(Fl_Mcp *mcp);
/* Copy a tool into the registry; false for invalid names/schemas or duplicates. */
bool fl_mcp_register(Fl_Mcp *mcp, const Fl_McpTool *tool);
/* Make every in-flight and future tool call fail immediately with "shutting down". Thread-safe. */
void fl_mcp_shutdown(Fl_Mcp *mcp);

/*
 * Handle one JSON-RPC message.
 *
 * body, length  Request payload (UTF-8 JSON).
 * Returns       Malloc'd JSON response, or NULL when the message was a notification (HTTP 202 Accepted).
 */
char *fl_mcp_handle(Fl_Mcp *mcp, const char *body, size_t length);

/* Complete a call with a JSON value rendered as text content. Consumes value. */
void fl_mcp_return_json(Fl_McpCall *call, cJSON *value);
/* Complete a call with a structured error {error:{code,message}}; isError is set. */
void fl_mcp_return_error(Fl_McpCall *call, const char *code, const char *format, ...);
/* Complete a call with a pre-built error object {code,message,...}. Consumes error. */
void fl_mcp_return_error_object(Fl_McpCall *call, cJSON *error);
/* Complete a call with a JSON value and an image. Consumes value (may be NULL). */
void fl_mcp_return_image(Fl_McpCall *call, cJSON *value, const unsigned char *bytes, size_t size, const char *mime_type);
