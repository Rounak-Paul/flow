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
#define SCRIPT_STEP (FL_TICKS_PER_SECOND / 60)
#define SCRIPT_HOOK_INTERVAL 1000

/* Cached program, private behavior state and player for one entity/document revision. */
typedef struct Fl_ScriptProgram {
    int ref, environment, self;
    Fl_Tick simulated;
    Fl_AnimationPlayer *player;
    bool compile_failed, behavior, execution_failed;
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

/* Resolve the active entity definition without retaining document pointers between calls. */
static const Fl_Animation *active_animation(lua_State *L,Fl_ScriptRuntime **out)
{
    Fl_ScriptRuntime *r=*(Fl_ScriptRuntime **)lua_getextraspace(L);*out=r;
    if (!r->scene) {luaL_error(L,"animation is unavailable during teardown");return NULL;}
    const Fl_Animation *a=fl_entity_animation(&r->scene->entities[r->entity]);
    if (!a) {luaL_error(L,"entity has no Animation component");return NULL;}
    return a;
}

/* Create an independent player at the immutable graph's entry state. */
static void require_player(lua_State *L,Fl_ScriptRuntime *r)
{
    const Fl_Animation *a=fl_entity_animation(&r->scene->entities[r->entity]);
    Fl_ScriptProgram *program=&r->programs[r->entity];
    if (!a||program->player) return;
    program->player=malloc(sizeof(*program->player));
    if (!program->player) {luaL_error(L,"cannot allocate animation player");return;}
    fl_animation_player_reset(program->player,a);
}

/* Play a named state, optionally crossfading for seconds. */
static int animation_play(lua_State *L)
{
    Fl_ScriptRuntime *r;const Fl_Animation *a=active_animation(L,&r);
    const char *name=luaL_checkstring(L,2);double seconds=luaL_optnumber(L,3,0);
    const Fl_AnimationState *state=fl_animation_state_named(a,name);
    if (!state||!isfinite(seconds)||seconds<0||seconds>(double)(UINT64_C(1)<<62)/FL_TICKS_PER_SECOND) return luaL_error(L,"invalid animation state or blend duration");
    require_player(L,r);
    if (!fl_animation_player_play(r->programs[r->entity].player,a,state->id,(uint64_t)llround(seconds*FL_TICKS_PER_SECOND))) return luaL_error(L,"invalid animation blend duration");
    return 0;
}

/* Read or set a typed graph parameter; trigger parameters use trigger(). */
static int animation_parameter(lua_State *L)
{
    Fl_ScriptRuntime *r;const Fl_Animation *a=active_animation(L,&r);
    const char *name=luaL_checkstring(L,2);size_t index=0;
    while (index<a->parameter_count&&strcmp(a->parameters[index].name,name)) ++index;
    if (index==a->parameter_count) return luaL_error(L,"unknown animation parameter");
    require_player(L,r);Fl_AnimationPlayer *player=r->programs[r->entity].player;
    bool write=lua_gettop(L)>=3;
    Fl_AnimationParameterKind kind=a->parameters[index].kind;
    if (!write) {if (kind==FL_ANIM_NUMBER) lua_pushnumber(L,player->parameters[index]);else lua_pushboolean(L,player->parameters[index]!=0);return 1;}
    if (kind==FL_ANIM_TRIGGER) return luaL_error(L,"use animation:trigger for trigger parameters");
    if (kind==FL_ANIM_BOOL) {luaL_checktype(L,3,LUA_TBOOLEAN);player->parameters[index]=lua_toboolean(L,3);}
    else {double value=luaL_checknumber(L,3);if (!isfinite(value)) return luaL_error(L,"parameter must be finite");player->parameters[index]=value;}
    return 0;
}

/* Fire a trigger retained until a matching transition consumes it. */
static int animation_trigger(lua_State *L)
{
    Fl_ScriptRuntime *r;const Fl_Animation *a=active_animation(L,&r);const char *name=luaL_checkstring(L,2);
    for (size_t i=0;i<a->parameter_count;++i) if (!strcmp(a->parameters[i].name,name)&&a->parameters[i].kind==FL_ANIM_TRIGGER) {
        require_player(L,r);r->programs[r->entity].player->parameters[i]=1;return 0;
    }
    return luaL_error(L,"unknown animation trigger");
}

/* Report the active state name for behavior decisions. */
static int animation_state_name(lua_State *L)
{
    Fl_ScriptRuntime *r;const Fl_Animation *a=active_animation(L,&r);require_player(L,r);
    const Fl_AnimationState *state=fl_animation_state(a,r->programs[r->entity].player->state);
    lua_pushstring(L,state&&state->id ? state->name : "Timeline");return 1;
}

/* Pause the animation player without pausing entity behavior. */
static int animation_pause(lua_State *L)
{
    Fl_ScriptRuntime *r;active_animation(L,&r);require_player(L,r);
    r->programs[r->entity].player->playing=false;return 0;
}

/* Resume animation playback from its retained state-local time. */
static int animation_resume(lua_State *L)
{
    Fl_ScriptRuntime *r;active_animation(L,&r);require_player(L,r);
    r->programs[r->entity].player->playing=true;return 0;
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
    lua_createtable(L,0,6);
    static const luaL_Reg methods[]={{"play",animation_play},{"parameter",animation_parameter},{"trigger",animation_trigger},{"state",animation_state_name},{"pause",animation_pause},{"resume",animation_resume},{NULL,NULL}};
    luaL_setfuncs(L,methods,0);lua_setfield(L,self,"animation");
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

/* Evaluate base/keyframe channels at a simulation step without recursively executing scripts. */
static Fl_EntityState sample_base(Fl_ScriptRuntime *r)
{
    const Fl_Entity *e = &r->scene->entities[r->entity];
    Fl_EntityState value = {.transform = {.scale_x = 1, .scale_y = 1}, .appearance = fl_widget_appearance(e), .opacity = fl_widget_opacity(e)};
    fl_transform_decode(fl_entity_component(e, FL_COMPONENT_TRANSFORM), &value.transform);
    return value;
}

/* Refresh the simulation clock while retaining procedural values and custom self state. */
static void set_self_tick(lua_State *L, int self, Fl_Tick tick)
{
    lua_pushinteger(L, tick); lua_setfield(L, self, "tick");
}

/* Release the old state when replaying from the beginning or recovering an initialization failure. */
static void reset_behavior(lua_State *L, Fl_ScriptProgram *program)
{
    luaL_unref(L, LUA_REGISTRYINDEX, program->environment); program->environment = LUA_NOREF;
    luaL_unref(L, LUA_REGISTRYINDEX, program->self); program->self = LUA_NOREF;
    program->simulated = 0;free(program->player);program->player=NULL;
}

/* Instantiate one isolated script, with behavior state retained only for on_update scripts. */
static void instantiate(lua_State *L, Fl_ScriptRuntime *r, Fl_ScriptProgram *program, int *env, int *self)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, program->ref); int chunk = lua_gettop(L);
    environment(L, r->scene->entities[r->entity].id); *env = lua_gettop(L);
    lua_pushvalue(L, *env); lua_setupvalue(L, chunk, 1);
    lua_pushvalue(L, chunk); lua_call(L, 0, 0);
    lua_getfield(L, *env, "on_update"); program->behavior = lua_isfunction(L, -1); lua_pop(L, 1);
    lua_getfield(L, *env, "on_evaluate"); bool formula = lua_isfunction(L, -1); lua_pop(L, 1);
    if (formula == program->behavior) luaL_error(L, "define either on_update(self, dt, time) or on_evaluate(self, time)");
    if (!program->behavior) {free(program->player);program->player=NULL;}
    require_player(L,r);
    Fl_EntityState base = program->behavior ? sample_base(r) : *r->state;
    double before[FL_PROP_COUNT][4] = {{0}}; state_values(&base, before);
    push_self(L, r, before); *self = lua_gettop(L);
    if (program->behavior) {
        lua_pushinteger(L, 0); lua_setfield(L, *self, "tick");
        lua_pushvalue(L, *env); program->environment = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, *self); program->self = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_getfield(L, *env, "on_init");
    if (lua_isfunction(L, -1)) {lua_pushvalue(L, *self); lua_call(L, 1, 0);}
    else lua_pop(L, 1);
    read_self(L, *self, &base);
}

/* Run bounded fixed steps; backward seeks recreate and replay the entity's behavior. */
static void update_behavior(lua_State *L, Fl_ScriptRuntime *r, Fl_ScriptProgram *program, int env, int self)
{
    Fl_Tick target = r->tick < 0 ? 0 : r->tick;
    while (target - program->simulated >= SCRIPT_STEP) {
        Fl_Tick next = program->simulated + SCRIPT_STEP;
        Fl_EntityState base = sample_base(r);
        set_self_tick(L,self,next);
        lua_getfield(L, env, "on_update"); lua_pushvalue(L, self);
        lua_pushnumber(L, (double)SCRIPT_STEP / FL_TICKS_PER_SECOND);
        lua_pushnumber(L, (double)next / FL_TICKS_PER_SECOND); lua_call(L, 3, 0);
        read_self(L, self, &base);
        const Fl_Animation *a=fl_entity_animation(&r->scene->entities[r->entity]);
        if (a) fl_animation_player_step(program->player,a,r->scene,SCRIPT_STEP);
        program->simulated = next;
    }
    set_self_tick(L,self,r->tick);
    Fl_EntityState result = *r->state;
    read_self(L, self, &result);
    *r->state = result;
}

/* Compile and execute Lua inside a protected call, committing outputs only after successful completion. */
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
    if (r->tick < program->simulated) reset_behavior(L, program);
    int env, self;
    if (program->environment == LUA_NOREF) instantiate(L, r, program, &env, &self);
    else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, program->environment); env = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, program->self); self = lua_gettop(L);
    }
    if (program->behavior) update_behavior(L, r, program, env, self);
    else {
        lua_getfield(L, env, "on_evaluate"); lua_pushvalue(L, self);
        lua_pushnumber(L, (double)r->tick / FL_TICKS_PER_SECOND); lua_call(L, 2, 0);
        Fl_EntityState result = *r->state;
        read_self(L, self, &result); *r->state = result;
        const Fl_Animation *a=fl_entity_animation(&r->scene->entities[r->entity]);
        if (a) fl_animation_player_step(program->player,a,r->scene,r->tick<0 ? 0 : r->tick);
    }
    return 0;
}

/* Invoke optional behavior teardown under the same execution and memory limits. */
static int destroy_behavior(lua_State *L)
{
    Fl_ScriptRuntime *r = *(Fl_ScriptRuntime **)lua_getextraspace(L);
    Fl_ScriptProgram *program = &r->programs[r->entity];
    lua_rawgeti(L, LUA_REGISTRYINDEX, program->environment);
    lua_getfield(L, -1, "on_destroy");
    if (lua_isfunction(L, -1)) {lua_rawgeti(L, LUA_REGISTRYINDEX, program->self); lua_call(L, 1, 0);}
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
    for (size_t i = 0; i < r->count; ++i)
        r->programs[i].ref = r->programs[i].environment = r->programs[i].self = LUA_NOREF;
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
    if (r->lua) {
        for (size_t i = 0; i < r->count; ++i) if (r->programs[i].environment != LUA_NOREF && r->programs[i].self != LUA_NOREF) {
            r->entity = i; r->instructions = SCRIPT_INSTRUCTIONS;
            lua_sethook(r->lua, instruction_limit, LUA_MASKCOUNT, SCRIPT_HOOK_INTERVAL);
            lua_pushcfunction(r->lua, destroy_behavior);
            if (lua_pcall(r->lua, 0, 0, 0) != LUA_OK) {
                const char *error = lua_tostring(r->lua, -1);
                FL_LOG_ERROR("script", "on_destroy: %s", error ? error : "Lua teardown failed");
            }
            lua_sethook(r->lua, NULL, 0, 0); lua_settop(r->lua, 0);
        }
        lua_close(r->lua);
    }
    for (size_t i=0;i<r->count;++i) free(r->programs[i].player);
    free(r->programs); free(r);
}

bool fl_script_runtime_evaluate(Fl_ScriptRuntime *r, const Fl_Scene *s, size_t entity, Fl_Tick tick, Fl_EntityState *state)
{
    if (!r || !s || entity >= r->count || entity >= s->entity_count || !state) return false;
    Fl_Script script;
    bool scripted=fl_script_decode(fl_entity_component(&s->entities[entity],FL_COMPONENT_SCRIPT),&script)&&script.enabled;
    if (!scripted) {
        const Fl_Animation *a=fl_entity_animation(&s->entities[entity]);
        if (!a) return true;
        Fl_ScriptProgram *program=&r->programs[entity];
        if (program->execution_failed) return false;
        if (!program->player) {program->player=malloc(sizeof(*program->player));if (!program->player) return false;fl_animation_player_reset(program->player,a);}
        if (tick<program->simulated) {fl_animation_player_reset(program->player,a);program->simulated=0;}
        Fl_Tick target=tick<0 ? 0 : tick;
        if (!a->transition_count) {fl_animation_player_step(program->player,a,s,target-program->simulated);program->simulated=target;}
        else {
            unsigned steps=0;
            while (target-program->simulated>=SCRIPT_STEP&&steps++<1000000) {fl_animation_player_step(program->player,a,s,SCRIPT_STEP);program->simulated+=SCRIPT_STEP;}
            if (target-program->simulated>=SCRIPT_STEP) {
                snprintf(program->error,sizeof(program->error),"animation replay step limit exceeded");
                program->execution_failed=true;free(program->player);program->player=NULL;
                FL_LOG_ERROR("animation","Entity %016llx: %s",(unsigned long long)s->entities[entity].id,program->error);return false;
            }
        }
        return true;
    }
    Fl_ScriptProgram *program = &r->programs[entity];
    if (program->compile_failed || program->execution_failed) return false;
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
        if (program->behavior) {program->execution_failed = true; reset_behavior(L, program);}
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

const Fl_AnimationPlayer *fl_script_runtime_player(const Fl_ScriptRuntime *r,size_t entity)
{
    return r&&entity<r->count ? r->programs[entity].player : NULL;
}
