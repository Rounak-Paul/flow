#include "fl_script_runtime.h"
#include "time/fl_scene_state.h"
#include "core/fl_log.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_MEMORY_LIMIT (16u * 1024u * 1024u)
#define SCRIPT_INSTRUCTIONS 1000000
#define SCRIPT_HOOK_INTERVAL 1000

/* Compiled chunk and latest diagnostic for one entity and document revision. */
typedef struct Fl_ScriptProgram {
    int ref;
    bool compile_failed;
    char error[256];
} Fl_ScriptProgram;
struct Fl_ScriptRuntime {
    lua_State *lua;
    size_t memory, count;
    Fl_ScriptProgram *programs;
    int instructions;
    const Fl_Scene *scene;
    size_t entity;
    Fl_Tick tick;
    Fl_EntityState *state;
    Fl_Script script;
};

typedef union Fl_LuaAllocation { max_align_t alignment; size_t size; } Fl_LuaAllocation;

/* Account for actual allocation sizes; shrinking never fails or undercounts retained memory. */
static void *script_allocate(void *data, void *pointer, size_t old_size, size_t size)
{
    (void)old_size;
    Fl_ScriptRuntime *r = data;
    Fl_LuaAllocation *allocation = pointer ? (Fl_LuaAllocation *)pointer - 1 : NULL;
    size_t old = allocation ? allocation->size : 0;
    if (!size) {free(allocation); r->memory -= old; return NULL;}
    if (size > SCRIPT_MEMORY_LIMIT - sizeof(*allocation)) return NULL;
    size_t total = sizeof(*allocation) + size;
    if (total <= old) return pointer;
    if (r->memory - old > SCRIPT_MEMORY_LIMIT - total) return NULL;
    Fl_LuaAllocation *next = realloc(allocation, total);
    if (!next) return NULL;
    next->size = total; r->memory = r->memory - old + total;
    return next + 1;
}

/* Interrupt unbounded code; protected calls and coroutines are not exposed to scripts. */
static void instruction_limit(lua_State *L, lua_Debug *debug)
{
    (void)debug;
    Fl_ScriptRuntime *r = *(Fl_ScriptRuntime **)lua_getextraspace(L);
    r->instructions -= SCRIPT_HOOK_INTERVAL;
    if (r->instructions <= 0) luaL_error(L, "script instruction limit exceeded");
}

/* Install a fresh library table, avoiding cross-frame mutable library state. */
static void library(lua_State *L, int environment, const char *name, lua_CFunction open)
{
    open(L); lua_setfield(L, environment, name);
}

/* Sort supported table keys independently of Lua's randomized hash traversal. */
static int compare_keys(lua_State *L)
{
    int a = lua_type(L, 1), b = lua_type(L, 2);
    bool less = a != b ? a < b : a == LUA_TBOOLEAN ? lua_toboolean(L, 1) < lua_toboolean(L, 2) : lua_compare(L, 1, 2, LUA_OPLT);
    lua_pushboolean(L, less); return 1;
}

/* Advance over the captured sorted keys of a deterministic pairs traversal. */
static int next_pair(lua_State *L)
{
    lua_Integer index = lua_tointeger(L, lua_upvalueindex(3)) + 1;
    lua_pushinteger(L, index); lua_replace(L, lua_upvalueindex(3));
    if (lua_rawgeti(L, lua_upvalueindex(2), index) == LUA_TNIL) return 0;
    lua_pushvalue(L, -1); lua_rawget(L, lua_upvalueindex(1));
    return 2;
}

/* Snapshot numeric/string/boolean keys in stable order for generic Lua loops. */
static int ordered_pairs(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L); int keys = lua_gettop(L); lua_Integer count = 0;
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        int type = lua_type(L, -2);
        if (type != LUA_TNUMBER && type != LUA_TSTRING && type != LUA_TBOOLEAN) return luaL_error(L, "pairs requires numeric, string or boolean keys");
        lua_pop(L, 1); lua_pushvalue(L, -1); lua_rawseti(L, keys, ++count);
    }
    luaopen_table(L); lua_getfield(L, -1, "sort"); lua_pushvalue(L, keys); lua_pushcfunction(L, compare_keys);
    lua_call(L, 2, 0); lua_pop(L, 1);
    lua_pushvalue(L, 1); lua_pushvalue(L, keys); lua_pushinteger(L, 0); lua_pushcclosure(L, next_pair, 3);
    lua_pushnil(L); lua_pushnil(L); return 3;
}

/* Exclude pointer-derived strings from deterministic scripts. */
static int scalar_string(lua_State *L)
{
    int type = lua_type(L, 1);
    if (type != LUA_TNIL && type != LUA_TBOOLEAN && type != LUA_TNUMBER && type != LUA_TSTRING)
        return luaL_error(L, "tostring requires a scalar value");
    luaL_tolstring(L, 1, NULL); return 1;
}

/* Require an explicit seed rather than Lua's ambient-time seed fallback. */
static int explicit_seed(lua_State *L)
{
    int count = lua_gettop(L);
    luaL_checkinteger(L, 1);
    if (count > 1) luaL_checkinteger(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1)); lua_insert(L, 1); lua_call(L, count, LUA_MULTRET);
    return lua_gettop(L);
}

/* Build an isolated environment with deterministic standard-library access. */
static void environment(lua_State *L, Fl_Id entity)
{
    lua_newtable(L); int env = lua_gettop(L);
    static const char *base[] = {"assert", "error", "ipairs", "select", "tonumber", "type", "rawequal", "rawget", "rawset", "rawlen"};
    for (size_t i = 0; i < sizeof(base) / sizeof(*base); ++i) {lua_getglobal(L, base[i]); lua_setfield(L, env, base[i]);}
    lua_pushvalue(L, env); lua_setfield(L, env, "_G");
    lua_pushcfunction(L, ordered_pairs); lua_setfield(L, env, "pairs");
    lua_pushcfunction(L, scalar_string); lua_setfield(L, env, "tostring");
    library(L, env, "math", luaopen_math);
    library(L, env, "string", luaopen_string);
    library(L, env, "table", luaopen_table);
    library(L, env, "utf8", luaopen_utf8);
    lua_getfield(L, env, "math"); lua_getfield(L, -1, "randomseed");
    lua_pushcclosure(L, explicit_seed, 1); lua_setfield(L, -2, "randomseed");
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, (lua_Integer)(entity & INT64_MAX)); lua_pushinteger(L, 0);
    lua_call(L, 2, 0); lua_pop(L, 1);
}

/* Read an entity's numeric channels without exposing native pointers to Lua. */
static void state_values(const Fl_EntityState *s, double values[FL_PROP_COUNT][4])
{
    const double channels[] = {s->transform.x, s->transform.y, s->transform.width, s->transform.height,
        s->transform.rotation, s->transform.scale_x, s->transform.scale_y, s->opacity, 0, 0,
        s->appearance.stroke_width, s->appearance.font_size};
    for (int p = 0; p < FL_PROP_COUNT; ++p) values[p][0] = channels[p];
    for (int c = 0; c < 4; ++c) {values[FL_PROP_FILL][c] = s->appearance.fill[c]; values[FL_PROP_STROKE][c] = s->appearance.stroke[c];}
}

/* Add a scalar or RGBA array to a Lua table. */
static void push_value(lua_State *L, int property, const double value[4])
{
    if (property != FL_PROP_FILL && property != FL_PROP_STROKE) lua_pushnumber(L, value[0]);
    else {
        lua_createtable(L, 4, 0);
        for (int c = 0; c < 4; ++c) {lua_pushnumber(L, value[c]); lua_rawseti(L, -2, c + 1);}
    }
}

/* Construct the per-evaluation self table from the current document state. */
static void push_self(lua_State *L, Fl_ScriptRuntime *r, const double values[FL_PROP_COUNT][4])
{
    lua_createtable(L, 0, 6); int self = lua_gettop(L);
    lua_createtable(L, 0, 7);
    for (int p = FL_PROP_X; p <= FL_PROP_SCALE_Y; ++p) {push_value(L, p, values[p]); lua_setfield(L, -2, fl_property_name((Fl_AnimProperty)p));}
    lua_setfield(L, self, "transform");
    lua_createtable(L, 0, 4);
    for (int p = FL_PROP_FILL; p < FL_PROP_COUNT; ++p) {push_value(L, p, values[p]); lua_setfield(L, -2, fl_property_name((Fl_AnimProperty)p));}
    lua_setfield(L, self, "appearance");
    lua_pushnumber(L, values[FL_PROP_OPACITY][0]); lua_setfield(L, self, "opacity");
    lua_pushinteger(L, r->tick); lua_setfield(L, self, "tick");
    lua_pushnumber(L, (double)fl_scene_length(r->scene) / FL_TICKS_PER_SECOND); lua_setfield(L, self, "duration");
    char id[24]; snprintf(id, sizeof(id), "%016llx", (unsigned long long)r->scene->entities[r->entity].id);
    lua_pushstring(L, id); lua_setfield(L, self, "entity");
}

/* Read all outputs first so invalid/undeclared writes cannot partially affect the frame. */
static void read_self(lua_State *L, int self, Fl_EntityState *output)
{
    double after[FL_PROP_COUNT][4] = {{0}};
    for (int p = 0; p < FL_PROP_COUNT; ++p) {
        bool nested = p != FL_PROP_OPACITY;
        if (nested) {
            lua_getfield(L, self, p <= FL_PROP_SCALE_Y ? "transform" : "appearance");
            luaL_checktype(L, -1, LUA_TTABLE);
        }
        lua_getfield(L, nested ? -1 : self, fl_property_name((Fl_AnimProperty)p));
        bool colour = p == FL_PROP_FILL || p == FL_PROP_STROKE;
        if (colour) luaL_checktype(L, -1, LUA_TTABLE);
        for (int c = 0; c < (colour ? 4 : 1); ++c) {
            if (colour) lua_rawgeti(L, -1, c + 1);
            if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "%s must contain numbers", fl_property_name((Fl_AnimProperty)p));
            after[p][c] = lua_tonumber(L, -1);
            if (colour) lua_pop(L, 1);
        }
        lua_pop(L, nested ? 2 : 1);
        if (!fl_property_value_valid((Fl_AnimProperty)p, after[p])) luaL_error(L, "invalid %s output", fl_property_name((Fl_AnimProperty)p));
    }
    for (int p=0;p<FL_PROP_COUNT;++p) fl_entity_state_apply(output,(Fl_AnimProperty)p,after[p]);
}

/* Compile once per revision, then run on_evaluate in a fresh environment; outputs commit only on success. */
static int evaluate(lua_State *L)
{
    Fl_ScriptRuntime *r = *(Fl_ScriptRuntime **)lua_getextraspace(L);
    Fl_ScriptProgram *program = &r->programs[r->entity];
    if (program->ref == LUA_NOREF) {
        char name[48]; snprintf(name, sizeof(name), "entity:%016llx", (unsigned long long)r->scene->entities[r->entity].id);
        if (luaL_loadbufferx(L, r->script.source, r->script.source_size, name, "t") != LUA_OK) {
            program->compile_failed = true;
            return lua_error(L);
        }
        program->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, program->ref); int chunk = lua_gettop(L);
    environment(L, r->scene->entities[r->entity].id); int env = lua_gettop(L);
    lua_pushvalue(L, env); lua_setupvalue(L, chunk, 1);
    lua_pushvalue(L, chunk); lua_call(L, 0, 0);
    lua_getfield(L, env, "on_evaluate");
    if (!lua_isfunction(L, -1)) return luaL_error(L, "define on_evaluate(self, time)");
    lua_pop(L, 1);
    double before[FL_PROP_COUNT][4] = {{0}}; state_values(r->state, before);
    push_self(L, r, before); int self = lua_gettop(L);
    lua_getfield(L, env, "on_evaluate"); lua_pushvalue(L, self);
    lua_pushnumber(L, (double)r->tick / FL_TICKS_PER_SECOND); lua_call(L, 2, 0);
    Fl_EntityState result = *r->state;
    read_self(L, self, &result); *r->state = result;
    return 0;
}

/* Initialize the base function template under allocation protection. */
static int initialize(lua_State *L) {luaopen_base(L); return 0;}

Fl_ScriptRuntime *fl_script_runtime_create(const Fl_Scene *s)
{
    if (!s || s->entity_count > SIZE_MAX / sizeof(Fl_ScriptProgram)) return NULL;
    Fl_ScriptRuntime *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->count = s->entity_count;
    r->programs = r->count ? calloc(r->count, sizeof(*r->programs)) : NULL;
    if (r->count && !r->programs) {free(r); return NULL;}
    for (size_t i = 0; i < r->count; ++i) r->programs[i].ref = LUA_NOREF;
    r->lua = lua_newstate(script_allocate, r);
    if (!r->lua) {fl_script_runtime_destroy(r); return NULL;}
    *(Fl_ScriptRuntime **)lua_getextraspace(r->lua) = r;
    lua_pushcfunction(r->lua, initialize);
    if (lua_pcall(r->lua, 0, 0, 0) != LUA_OK) {fl_script_runtime_destroy(r); return NULL;}
    return r;
}

void fl_script_runtime_destroy(Fl_ScriptRuntime *r)
{
    if (!r) return;
    if (r->lua) lua_close(r->lua);
    free(r->programs); free(r);
}

bool fl_script_runtime_evaluate(Fl_ScriptRuntime *r, const Fl_Scene *s, size_t entity, Fl_Tick tick, Fl_EntityState *state)
{
    if (!r || !s || entity >= r->count || entity >= s->entity_count || !state) return false;
    Fl_Script script;
    if (!fl_script_decode(fl_entity_component(&s->entities[entity], FL_COMPONENT_SCRIPT), &script) || !script.enabled) return true;
    Fl_ScriptProgram *program = &r->programs[entity];
    if (program->compile_failed) return false;
    r->scene = s; r->entity = entity; r->tick = tick; r->state = state; r->script = script;
    r->instructions = SCRIPT_INSTRUCTIONS;
    lua_State *L = r->lua;
    lua_sethook(L, instruction_limit, LUA_MASKCOUNT, SCRIPT_HOOK_INTERVAL);
    lua_pushcfunction(L, evaluate);
    bool ok = lua_pcall(L, 0, 0, 0) == LUA_OK;
    lua_sethook(L, NULL, 0, 0);
    if (!ok) {
        const char *error = lua_tostring(L, -1);
        if (!error) error = "Lua evaluation failed";
        if (strcmp(program->error, error)) FL_LOG_ERROR("script", "Entity %016llx: %s", (unsigned long long)s->entities[entity].id, error);
        snprintf(program->error, sizeof(program->error), "%s", error);
    } else program->error[0] = 0;
    lua_settop(L, 0);
    if (program->ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, program->ref); lua_pushnil(L); lua_setupvalue(L, -2, 1); lua_pop(L, 1);
    }
    lua_settop(L, 0);
    lua_gc(L, LUA_GCCOLLECT);
    r->scene = NULL; r->state = NULL; r->script = (Fl_Script){0};
    return ok;
}

const char *fl_script_runtime_error(const Fl_ScriptRuntime *r, size_t entity)
{
    return r && entity < r->count ? r->programs[entity].error : "";
}
