#include "flow_test.h"
#include "scripting/fl_lua_value.h"
#include <lauxlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Capture one returned expression, leaving the caller-owned partial byte stream releasable on every failure. */
static bool capture(lua_State *lua, Fl_Binary *bytes, const char *source, char error[256])
{
    lua_settop(lua,0);
    CHECK(luaL_loadstring(lua,source)==LUA_OK && lua_pcall(lua,0,1,0)==LUA_OK);
    int top=lua_gettop(lua); bool ok=fl_lua_value_capture(lua,-1,bytes,error);
    CHECK(lua_gettop(lua)==top); return ok;
}

/* Checkpoint values preserve numeric types and binary strings; malformed or unbounded values never publish. */
int main(void)
{
    lua_State *lua=luaL_newstate(); CHECK(lua); if (!lua) return 1;
    Fl_Binary bytes={0}; char error[256];
    CHECK(capture(lua,&bytes,"return {integer=9223372036854775807, float=1.0, zero=-0.0, text='a\\0b', array={true,false,3}, empty={}}",error));
    lua_settop(lua,0); CHECK(fl_lua_value_push(lua,bytes.data,bytes.size,error));
    lua_getfield(lua,-1,"integer"); CHECK(lua_isinteger(lua,-1) && lua_tointeger(lua,-1)==INT64_MAX); lua_pop(lua,1);
    lua_getfield(lua,-1,"float"); CHECK(!lua_isinteger(lua,-1) && lua_tonumber(lua,-1)==1); lua_pop(lua,1);
    lua_getfield(lua,-1,"zero"); CHECK(signbit(lua_tonumber(lua,-1))); lua_pop(lua,1);
    lua_getfield(lua,-1,"text"); size_t size; const char *text=lua_tolstring(lua,-1,&size);
    CHECK(size==3 && !memcmp(text,"a\0b",3)); lua_pop(lua,1);
    lua_getfield(lua,-1,"array"); CHECK(lua_rawlen(lua,-1)==3); lua_pop(lua,1);
    lua_settop(lua,0);
    for (size_t i=0;i<bytes.size;++i) {CHECK(!fl_lua_value_push(lua,bytes.data,i,error)); CHECK(lua_gettop(lua)==0);}
    CHECK(!capture(lua,&bytes,"return {callback=function() end}",error) && strstr(error,"functions"));
    CHECK(!capture(lua,&bytes,"return {[1]=1,[3]=3}",error) && strstr(error,"holes"));
    CHECK(!capture(lua,&bytes,"return {[1]=1,name='mixed'}",error) && strstr(error,"mix"));
    CHECK(!capture(lua,&bytes,"local value={}; value.loop=value; return value",error) && strstr(error,"nesting"));
    CHECK(!capture(lua,&bytes,"return 0/0",error) && strstr(error,"finite"));
    lua_settop(lua,0); lua_pushlstring(lua,(const char *)bytes.data,0);
    char *large=malloc(FL_LUA_VALUE_LIMIT+1); CHECK(large);
    if (large) {
        memset(large,'a',FL_LUA_VALUE_LIMIT+1); lua_pushlstring(lua,large,FL_LUA_VALUE_LIMIT+1);
        CHECK(!fl_lua_value_capture(lua,-1,&bytes,error) && strstr(error,"byte limit")); free(large);
    }
    free(bytes.data); lua_close(lua);
    return FLOW_TEST_RESULT("fl_lua_value_tests");
}
