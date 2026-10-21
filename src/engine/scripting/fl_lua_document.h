#pragma once
#include "doc/fl_json.h"
#include <lua.h>

/* Copy a Lua document table into bounded owned JSON. Dense numeric tables become arrays; empty native
   collection fields become arrays. Rejects cycles, mixed keys, nonfinite values and unsupported Lua types. */
cJSON *fl_lua_document_capture(lua_State *lua, int index, Fl_Json *context);
