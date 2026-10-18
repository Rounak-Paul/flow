#include "fl_mcp_http.h"
#include "core/fl_log.h"
#include <civetweb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "mcp"
#define MAX_BODY (8u * 1024u * 1024u)

struct Fl_McpHttp {
    struct mg_context *context;
    Fl_Mcp *mcp;
    char *token;
    uint16_t port;
};

/* Send a short plain response. */
static int reply(struct mg_connection *c, int status, const char *reason, const char *extra_header)
{
    mg_printf(c, "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nCache-Control: no-store\r\n%s\r\n", status, reason, extra_header ? extra_header : "");
    return status;
}

/* True when value is a loopback host name, optionally with a port, as in "localhost:8080" or "[::1]". */
static bool loopback_host(const char *host)
{
    static const char *names[] = {"127.0.0.1", "localhost", "[::1]"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
        size_t n = strlen(names[i]);
        if (!strncmp(host, names[i], n) && (!host[n] || (host[n] == ':' && host[n + 1]))) return true;
    }
    return false;
}

/* True when an Origin header names a loopback page. */
static bool loopback_origin(const char *origin)
{
    if (!strncmp(origin, "http://", 7)) return loopback_host(origin + 7);
    if (!strncmp(origin, "https://", 8)) return loopback_host(origin + 8);
    return false;
}

/* Compare secrets without an early exit on the first differing byte. */
static bool secret_equal(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    unsigned char diff = (unsigned char)(na != nb);
    for (size_t i = 0; i < nb; ++i) diff |= (unsigned char)(a[i < na ? i : 0] ^ b[i]);
    return diff == 0;
}

/* Read the request body into a NUL-terminated buffer; NULL on oversize, short read or allocation failure. */
static char *read_body(struct mg_connection *c, const struct mg_request_info *info, size_t *length)
{
    long long declared = info->content_length;
    if (declared < 0 || declared > (long long)MAX_BODY) return NULL;
    char *body = malloc((size_t)declared + 1);
    if (!body) return NULL;
    size_t have = 0;
    while (have < (size_t)declared) {
        int n = mg_read(c, body + have, (size_t)declared - have);
        if (n <= 0) {free(body); return NULL;}
        have += (size_t)n;
    }
    body[have] = 0; *length = have;
    return body;
}

static int handle_mcp(struct mg_connection *c, void *data)
{
    Fl_McpHttp *http = data;
    const struct mg_request_info *info = mg_get_request_info(c);
    if (strcmp(info->request_method, "POST")) return reply(c, 405, "Method Not Allowed", "Allow: POST\r\n");
    const char *host = mg_get_header(c, "Host"), *origin = mg_get_header(c, "Origin"), *auth = mg_get_header(c, "Authorization");
    if ((host && !loopback_host(host)) || (origin && !loopback_origin(origin))) return reply(c, 403, "Forbidden", NULL);
    if (!auth || strncmp(auth, "Bearer ", 7) || !secret_equal(auth + 7, http->token))
        return reply(c, 401, "Unauthorized", "WWW-Authenticate: Bearer\r\n");
    size_t length = 0;
    char *body = read_body(c, info, &length);
    if (!body) return reply(c, 413, "Payload Too Large", NULL);
    char *response = fl_mcp_handle(http->mcp, body, length);
    free(body);
    if (!response) return reply(c, 202, "Accepted", NULL);
    size_t size = strlen(response);
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n\r\n", size);
    mg_write(c, response, size);
    free(response);
    return 200;
}

Fl_McpHttp *fl_mcp_http_start(const Fl_McpHttpDesc *desc)
{
    if (!desc || !desc->mcp || !desc->token || !desc->token[0]) return NULL;
    Fl_McpHttp *http = calloc(1, sizeof(*http));
    if (!http) return NULL;
    http->mcp = desc->mcp; http->token = malloc(strlen(desc->token) + 1);
    if (!http->token) {free(http); return NULL;}
    strcpy(http->token, desc->token);
    mg_init_library(0);
    char ports[32];
    snprintf(ports, sizeof(ports), "127.0.0.1:%u", (unsigned)desc->port);
    const char *options[] = {"listening_ports", ports, "num_threads", "4", "enable_keep_alive", "yes",
                             "request_timeout_ms", "120000", "enable_directory_listing", "no", NULL};
    http->context = mg_start(NULL, NULL, options);
    struct mg_server_port bound[1] = {0};
    if (!http->context || mg_get_server_ports(http->context, 1, bound) < 1) {
        if (http->context) mg_stop(http->context);
        free(http->token); free(http); return NULL;
    }
    http->port = (uint16_t)bound[0].port;
    mg_set_request_handler(http->context, "/mcp", handle_mcp, http);
    FL_LOG_INFO(LOG_TAG, "MCP endpoint listening on http://127.0.0.1:%u/mcp", (unsigned)http->port);
    return http;
}

uint16_t fl_mcp_http_port(const Fl_McpHttp *http) { return http ? http->port : 0; }

void fl_mcp_http_stop(Fl_McpHttp *http)
{
    if (!http) return;
    mg_stop(http->context);
    free(http->token); free(http);
}
