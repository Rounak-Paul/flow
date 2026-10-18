#pragma once
#include "fl_mcp.h"

/*
 * Streamable-HTTP transport for the MCP core: a loopback-only server answering POST /mcp.
 * Requests need "Authorization: Bearer <token>"; Origin and Host headers must name the loopback interface, which
 * blocks browser pages and DNS-rebinding attacks from reaching the editor.
 */
typedef struct Fl_McpHttp Fl_McpHttp;

typedef struct Fl_McpHttpDesc {
    Fl_Mcp *mcp;
    uint16_t port;              /* zero picks a free port */
    const char *token;          /* required shared secret */
} Fl_McpHttpDesc;

/* Start serving on 127.0.0.1; NULL when the port is taken or the server cannot start. */
Fl_McpHttp *fl_mcp_http_start(const Fl_McpHttpDesc *desc);
/* The port actually bound. */
uint16_t fl_mcp_http_port(const Fl_McpHttp *http);
/* Stop accepting requests and wait for handlers to return. Call fl_mcp_shutdown first so waiting calls end. NULL is safe. */
void fl_mcp_http_stop(Fl_McpHttp *http);
