#pragma once
#include "fl_mcp.h"
#include "project/fl_project.h"

/*
 * Agent authoring surface: the document tools exposed over MCP. Everything an agent can do is a typed
 * operation applied through fl_project_apply, grouped into atomic, undoable changesets. Times are seconds,
 * rotations degrees, colours "#RRGGBB[AA]". Owner thread only.
 */
typedef struct Fl_Agent Fl_Agent;

typedef struct Fl_AgentHost {
    Fl_Project *(*project)(void *user_data);    /* currently open project, or NULL */
    void *user_data;
} Fl_AgentHost;

/* Create the authoring service; NULL on allocation failure. */
Fl_Agent *fl_agent_create(const Fl_AgentHost *host);
/* Release history and state. NULL is safe. */
void fl_agent_destroy(Fl_Agent *agent);
/* Forget undo history; call when the open project changes. */
void fl_agent_reset(Fl_Agent *agent);
/* Register every document tool with the MCP core. */
bool fl_agent_register(Fl_Agent *agent, Fl_Mcp *mcp);
/* Run a document tool synchronously. Returns the result object, or NULL with *error = {code, message, ...}.
   Both are owned by the caller. */
cJSON *fl_agent_call(Fl_Agent *agent, const char *tool, const cJSON *arguments, cJSON **error);
/* Server instructions describing the workflow, for the MCP initialize response. */
const char *fl_agent_instructions(void);

/* Failure description shared by argument parsing and operations. */
typedef struct Fl_Fail { char code[32]; char message[600]; } Fl_Fail;
/* Record a failure and return false so callers can `return fl_fail(...)`. */
bool fl_fail(Fl_Fail *fail, const char *code, const char *format, ...);
/* Convert a failure to its JSON error object. */
cJSON *fl_fail_json(const Fl_Fail *fail);
