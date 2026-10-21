#pragma once
#include "core/fl_event.h"
#include "fl_workspace.h"

/*
 * Editor-side MCP service: owns the loopback HTTP endpoint, the document tools and the editor-only tools
 * (get_editor_state, set_editor_state, render_frame through the GPU scene renderer). Requests are served on network threads and executed on the
 * UI thread through the application bus, so tools see exactly what the user sees and every edit goes through the
 * same project operations as the GUI. The connection details (port, bearer token) persist in the user's config
 * directory as mcp.json.
 */
typedef struct Fl_McpService Fl_McpService;
typedef struct Fl_Exporter Fl_Exporter;

/* Start the endpoint; fonts and jobs (borrowed, outliving the service) serve the offscreen scene renderer behind render_frame. Returns NULL when the server cannot start; the editor then runs without MCP. */
Fl_McpService *fl_mcp_service_create(Fl_EventBus *bus, Ca_Instance *instance, Fl_Fonts *fonts, Fl_JobSystem *jobs);
/* Stop serving, fail in-flight calls and free. Safe with the project still attached. NULL is safe. */
void fl_mcp_service_destroy(Fl_McpService *service);
/* Expose an open project and its workspace to agents. */
void fl_mcp_service_attach(Fl_McpService *service, Fl_Project *project, Ca_Window *window, Fl_Workspace *workspace);
/* Withdraw the project before it or its window is destroyed; pending renders fail. */
void fl_mcp_service_detach(Fl_McpService *service);
/* The bound loopback port, or zero. */
unsigned fl_mcp_service_port(const Fl_McpService *service);
/* Borrow the video exporter shared with export_video. NULL when the service is absent. */
Fl_Exporter *fl_mcp_service_exporter(Fl_McpService *service);
