#pragma once
#include "core/fl_event.h"
#include "fl_workspace.h"

/*
 * Editor-side MCP service: owns the loopback HTTP endpoint, the document tools and the editor-only tools
 * (render_frame, get_editor_state, set_editor_state). Requests are served on network threads and executed on the
 * UI thread through the application bus, so tools see exactly what the user sees and every edit goes through the
 * same project operations as the GUI. The connection details (port, bearer token) persist in the user's config
 * directory as mcp.json.
 */
typedef struct Fl_McpService Fl_McpService;

/* Start the endpoint. Returns NULL when the server cannot start; the editor then runs without MCP. */
Fl_McpService *fl_mcp_service_create(Fl_EventBus *bus, Ca_Instance *instance);
/* Stop serving, fail in-flight calls and free. Safe with the project still attached. NULL is safe. */
void fl_mcp_service_destroy(Fl_McpService *service);
/* Expose an open project and its workspace to agents. */
void fl_mcp_service_attach(Fl_McpService *service, Fl_Project *project, Ca_Window *window, Fl_Workspace *workspace);
/* Withdraw the project before it or its window is destroyed; pending renders fail. */
void fl_mcp_service_detach(Fl_McpService *service);
/* The bound loopback port, or zero. */
unsigned fl_mcp_service_port(const Fl_McpService *service);
