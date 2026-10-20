#pragma once
#include "doc/fl_binary.h"
#include <lua.h>

#define FL_LUA_VALUE_LIMIT (64u * 1024u)
#define FL_LUA_VALUE_NODES 4096u
#define FL_LUA_VALUE_DEPTH 16u

/* Capture bounded explicit state, preserving integer/float identity and binary strings. Tables must be dense arrays
   or string-keyed objects. out owns all partial storage even if Lua raises an allocation error inside the caller's
   protected invocation; clear it at that invocation's boundary. */
bool fl_lua_value_capture(lua_State *lua, int index, Fl_Binary *out, char error[256]);
/* Push one captured value under the caller's Lua protection. False restores the Lua stack and records a diagnostic. */
bool fl_lua_value_push(lua_State *lua, const unsigned char *bytes, size_t size, char error[256]);
