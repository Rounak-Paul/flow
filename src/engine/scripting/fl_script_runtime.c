#include "project/fl_source_asset.h"
#include "fl_script_runtime.h"
#include "fl_lua_value.h"
#include "fl_lua_document.h"
#include "time/fl_scene_state.h"
#include "core/fl_log.h"
#include "doc/fl_json.h"
#include "doc/fl_document.h"
#include "schema/fl_parameters.h"
#include "schema/fl_properties.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_TEXT_LIMIT (16u * 1024u * 1024u)
#define SCRIPT_CHECKPOINTS 9u
#define SCRIPT_STATE_LIMIT (16u * 1024u * 1024u)
#define SCRIPT_REPLAY_LIMIT INT64_C(1000000)
#define SCRIPT_MEMORY_LIMIT (16u * 1024u * 1024u)
#define SCRIPT_INSTRUCTIONS 1000000
#define SCRIPT_HOOK_INTERVAL 1000
#define SCRIPT_IMPORT_LIMIT 128u
#define SCRIPT_IMPORT_DEPTH 32u

typedef struct Fl_ImportChunk {Fl_Id asset; int ref;} Fl_ImportChunk;

typedef struct Fl_ScriptSnapshot {unsigned char *bytes; size_t size; int64_t step;} Fl_ScriptSnapshot;

/* Compiled chunk and latest diagnostic for one entity and document revision. */
typedef struct Fl_ScriptProgram {
    int ref;
    Fl_Id component, asset;
    cJSON *parameters, *parameter_schema, *effective_parameters, *bindings;
    bool schema_ready, compile_failed;
    bool lifecycle_ready, has_initialize, has_step, has_sample, has_build, state_initialized;
    Fl_Tick step_ticks;
    int64_t simulation_cycle;
    Fl_ScriptSnapshot latest, checkpoints[SCRIPT_CHECKPOINTS];
    size_t checkpoint_cursor;
    Fl_Binary scratch;
    char error[256];
} Fl_ScriptProgram;
struct Fl_ScriptRuntime {
    lua_State *lua;
    size_t memory, count, entity_count, program_index;
    size_t *starts;
    Fl_ScriptProgram *programs;
    char **texts;
    size_t text_count, text_capacity, text_bytes;
    int instructions;
    const atomic_bool *cancel;
    const Fl_Scene *scene;
    size_t entity;
    Fl_Tick tick;
    Fl_TimeSample time;
    Fl_TimeMapping mapping;
    size_t simulation_bytes;
    Fl_EntityState *state;
    const Fl_Asset *script;
    cJSON *build_output, *binding_output;
    Fl_ImportChunk imports[SCRIPT_IMPORT_LIMIT];
    size_t import_count;
};

typedef union Fl_LuaAllocation { max_align_t alignment; size_t size; } Fl_LuaAllocation;

static void validate_bindings(lua_State *lua, int module, const Fl_ScriptProgram *program);

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
    if (r->cancel && atomic_load_explicit(r->cancel,memory_order_relaxed)) luaL_error(L,"evaluation cancelled");
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
        s->appearance.stroke_width, s->appearance.font_size, s->crop[0], s->crop[1], s->crop[2], s->crop[3]};
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
static void push_entity(lua_State *L, Fl_ScriptRuntime *r, size_t entity, const Fl_EntityState *state, const double values[FL_PROP_COUNT][4])
{
    lua_createtable(L, 0, 6); int self = lua_gettop(L);
    static const char *const groups[] = {"transform", "appearance", "crop"};
    for (size_t i = 0; i < sizeof(groups) / sizeof(*groups); ++i) {
        lua_newtable(L); lua_setfield(L, self, groups[i]);
    }
    for (int p = 0; p < FL_PROP_COUNT; ++p) {
        const Fl_PropertyDef *definition = fl_property_definition((Fl_AnimProperty)p);
        if (definition->lua_group) lua_getfield(L, self, definition->lua_group);
        push_value(L, p, values[p]); lua_setfield(L, definition->lua_group ? -2 : self, definition->lua_field);
        if (definition->lua_group) lua_pop(L, 1);
    }
    const Fl_Component *text = fl_entity_component(&r->scene->entities[entity], FL_COMPONENT_TEXT);
    if (text) {
        lua_pushlstring(L, state->text_override ? state->text : text->size ? (const char *)text->data : "",
                       state->text_override ? state->text_size : text->size);
        lua_setfield(L, self, "text");
    }
    char id[24]; snprintf(id, sizeof(id), "%llu", (unsigned long long)r->scene->entities[entity].id);
    lua_pushstring(L, id); lua_setfield(L, self, "id");
}

/* Own sampled text independently of the Lua heap, within a bounded frame budget. */
static const char *retain_text(lua_State *L, const char *text, size_t size)
{
    Fl_ScriptRuntime *r = *(Fl_ScriptRuntime **)lua_getextraspace(L);
    if (size >= SCRIPT_TEXT_LIMIT || size + 1 > SCRIPT_TEXT_LIMIT - r->text_bytes)
        luaL_error(L, "sampled text budget exceeded");
    if (!fl_json_utf8_valid(text, size)) luaL_error(L, "sampled text requires UTF-8 without NUL");
    if (r->text_count == r->text_capacity) {
        size_t capacity = r->text_capacity ? r->text_capacity * 2 : 16;
        if (capacity > SCRIPT_TEXT_LIMIT / sizeof(*r->texts)) luaL_error(L, "sampled text allocation limit");
        char **grown = realloc(r->texts, capacity * sizeof(*grown));
        if (!grown) luaL_error(L, "cannot allocate sampled text storage");
        r->texts = grown; r->text_capacity = capacity;
    }
    char *copy = malloc(size + 1);
    if (!copy) luaL_error(L, "cannot allocate sampled text");
    memcpy(copy, text, size); copy[size] = 0;
    r->texts[r->text_count++] = copy; r->text_bytes += size + 1;
    return copy;
}

/* Read all outputs first so invalid/undeclared writes cannot partially affect the frame. */
static void read_self(lua_State *L, int self, Fl_EntityState *output, const Fl_Component *original_text)
{
    double after[FL_PROP_COUNT][4] = {{0}};
    for (int p = 0; p < FL_PROP_COUNT; ++p) {
        const Fl_PropertyDef *definition = fl_property_definition((Fl_AnimProperty)p);
        bool nested = definition->lua_group != NULL;
        if (nested) {
            lua_getfield(L, self, definition->lua_group);
            luaL_checktype(L, -1, LUA_TTABLE);
        }
        lua_getfield(L, nested ? -1 : self, definition->lua_field);
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
    bool replace_text = false;
    size_t text_size = 0;
    const char *text = NULL;
    if (original_text) {
        lua_getfield(L, self, "text");
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "text must be a string");
        text = lua_tolstring(L, -1, &text_size);
        replace_text = text_size != original_text->size || (text_size && memcmp(text, original_text->data, text_size));
    }
    const char *retained = replace_text ? retain_text(L, text, text_size) : NULL;
    for (int p=0;p<FL_PROP_COUNT;++p) fl_entity_state_apply(output,(Fl_AnimProperty)p,after[p]);
    output->text_override = replace_text;
    output->text = retained; output->text_size = replace_text ? text_size : 0;
    if (original_text) lua_pop(L, 1);
}

/* Push bounded JSON values into a fresh Lua context; no native pointers cross the boundary. */
static void push_json(lua_State *L, const cJSON *v, unsigned depth)
{
    if (depth>16) luaL_error(L,"parameter nesting limit exceeded");
    if (cJSON_IsObject(v) || cJSON_IsArray(v)) {
        lua_newtable(L); bool array=cJSON_IsArray(v); int index=1;
        for (const cJSON *item=v->child;item;item=item->next) {
            push_json(L,item,depth+1);
            if (array) lua_rawseti(L,-2,index++); else lua_setfield(L,-2,item->string);
        }
    } else if (cJSON_IsString(v)) lua_pushstring(L,v->valuestring);
    else if (cJSON_IsNumber(v)) lua_pushnumber(L,v->valuedouble);
    else if (cJSON_IsBool(v)) lua_pushboolean(L,cJSON_IsTrue(v));
    else lua_pushnil(L);
}

/* Copy a bounded declarative parameter value into runtime-owned metadata. */
static cJSON *parameter_metadata_value(lua_State *L, int index, size_t *budget)
{
    index=lua_absindex(L,index);
    if (lua_isnil(L,index)) return cJSON_CreateNull();
    if (lua_isboolean(L,index)) return cJSON_CreateBool(lua_toboolean(L,index));
    if (lua_type(L,index)==LUA_TNUMBER) {
        double n=lua_tonumber(L,index); if (!isfinite(n)) luaL_error(L,"nonfinite parameter metadata");
        return cJSON_CreateNumber(n);
    }
    if (lua_type(L,index)==LUA_TSTRING) {
        size_t size; const char *text=lua_tolstring(L,index,&size);
        if (size>*budget || !fl_json_utf8_valid(text,size)) luaL_error(L,"parameter metadata budget or UTF-8 error");
        *budget-=size; return cJSON_CreateString(text);
    }
    if (lua_istable(L,index)) {
        size_t size=lua_rawlen(L,index);
        if (size!=2 && size!=4) luaL_error(L,"parameter default must be a scalar or vector");
        double values[4];
        for (size_t i=0;i<size;++i) {
            lua_rawgeti(L,index,(lua_Integer)i+1);
            if (lua_type(L,-1)!=LUA_TNUMBER || !isfinite(lua_tonumber(L,-1))) luaL_error(L,"invalid parameter vector default");
            values[i]=lua_tonumber(L,-1); lua_pop(L,1);
        }
        cJSON *array=cJSON_CreateArray();
        for (size_t i=0;array && i<size;++i) {
            cJSON *value=cJSON_CreateNumber(values[i]);
            if (!value || !cJSON_AddItemToArray(array,value)) {cJSON_Delete(value); cJSON_Delete(array); return NULL;}
        }
        return array;
    }
    luaL_error(L,"unsupported parameter metadata value"); return NULL;
}

/* Retain introspection data even when an instance's current parameter values are invalid. */
static void capture_parameter_schema(lua_State *L, int module, Fl_ScriptProgram *program)
{
    if (program->schema_ready) return;
    cJSON_Delete(program->parameter_schema); program->parameter_schema=cJSON_CreateObject();
    if (!program->parameter_schema) luaL_error(L,"cannot allocate parameter schema");
    Fl_Json context={0}; size_t budget=FL_SCRIPT_PARAMETER_LIMIT, count=0;
    lua_getfield(L,module,"parameters"); int schema=lua_gettop(L);
    if (lua_isnil(L,schema)) {lua_pop(L,1); program->schema_ready=true; return;}
    luaL_checktype(L,schema,LUA_TTABLE); lua_pushnil(L);
    while (lua_next(L,schema)) {
        if (++count>64 || lua_type(L,-2)!=LUA_TSTRING || !lua_istable(L,-1)) luaL_error(L,"invalid parameter schema");
        size_t length; const char *name=lua_tolstring(L,-2,&length);
        if (!length || length>64 || !fl_json_utf8_valid(name,length)) luaL_error(L,"invalid parameter name");
        int spec=lua_gettop(L); cJSON *item=cJSON_CreateObject(); if (!fl_json_add(&context,program->parameter_schema,name,item)) luaL_error(L,"cannot allocate parameter definition");
        static const char *const fields[]={"type","default","min","max","label","description"};
        for (size_t i=0;i<sizeof(fields)/sizeof(*fields);++i) {
            lua_getfield(L,spec,fields[i]);
            if (!lua_isnil(L,-1)) fl_json_add(&context,item,fields[i],parameter_metadata_value(L,-1,&budget));
            lua_pop(L,1);
        }
        if (context.error[0]) luaL_error(L,"%s",context.error);
        lua_pop(L,1);
    }
    lua_pop(L,1); program->schema_ready=true;
}

/* Resolve once per attachment generation through the same validator used by native controls. */
static void push_parameters(lua_State *L, Fl_ScriptProgram *program)
{
    if (!program->effective_parameters) {
        Fl_Json context={0};
        if (!fl_parameters_resolve(&context,program->parameter_schema,program->parameters,&program->effective_parameters)) luaL_error(L,"%s",context.error);
    }
    push_json(L,program->effective_parameters,0);
}

/* Discard compatible checkpoints only when this instance's simulation inputs change. */
static void clear_simulation(Fl_ScriptRuntime *runtime, Fl_ScriptProgram *program)
{
    runtime->simulation_bytes-=program->latest.size; free(program->latest.bytes); program->latest=(Fl_ScriptSnapshot){0};
    for (size_t i=0;i<SCRIPT_CHECKPOINTS;++i) {
        runtime->simulation_bytes-=program->checkpoints[i].size; free(program->checkpoints[i].bytes);
        program->checkpoints[i]=(Fl_ScriptSnapshot){0};
    }
    program->state_initialized=false; program->checkpoint_cursor=1;
}

/* Replace one snapshot atomically within the evaluator's aggregate state budget. */
static bool store_snapshot(Fl_ScriptRuntime *runtime, Fl_ScriptSnapshot *snapshot,
                            const unsigned char *bytes, size_t size, int64_t step)
{
    if (size>SCRIPT_STATE_LIMIT-(runtime->simulation_bytes-snapshot->size)) return false;
    unsigned char *copy=malloc(size);
    if (!copy) return false;
    memcpy(copy,bytes,size); free(snapshot->bytes);
    runtime->simulation_bytes=runtime->simulation_bytes-snapshot->size+size;
    *snapshot=(Fl_ScriptSnapshot){.bytes=copy,.size=size,.step=step}; return true;
}

/* Inspect optional capabilities once, without assigning different script types. */
static void capture_lifecycle(lua_State *lua, int module, Fl_ScriptProgram *program)
{
    if (program->lifecycle_ready) return;
    const char *names[]={"initialize","step","sample","build"};
    bool *capabilities[]={&program->has_initialize,&program->has_step,&program->has_sample,&program->has_build};
    for (size_t i=0;i<4;++i) {
        lua_getfield(lua,module,names[i]);
        if (!lua_isnil(lua,-1) && !lua_isfunction(lua,-1)) luaL_error(lua,"%s must be a function",names[i]);
        *capabilities[i]=lua_isfunction(lua,-1); lua_pop(lua,1);
    }
    if (!program->has_initialize && !program->has_step && !program->has_sample && !program->has_build)
        luaL_error(lua,"behavior module requires build(ctx), initialize(ctx), step(ctx) or sample(ctx)");
    lua_getfield(lua,module,"step_hz");
    lua_Integer hz=120;
    if (!lua_isnil(lua,-1)) {
        int integer=0; hz=lua_tointegerx(lua,-1,&integer);
        if (!integer || hz<1 || hz>240 || FL_TICKS_PER_SECOND%(uint64_t)hz) luaL_error(lua,"step_hz must divide 120000 and be within 1..240");
    }
    lua_pop(lua,1); program->step_ticks=FL_TICKS_PER_SECOND/(Fl_Tick)hz;
    program->lifecycle_ready=true;
}

/* Exact resource IDs remain independent of floating-point precision and asset names/paths. */
static Fl_Id import_id(lua_State *lua, int index)
{
    if (lua_isinteger(lua,index)) {
        lua_Integer value=lua_tointeger(lua,index); if (value>0) return (Fl_Id)value;
    } else if (lua_type(lua,index)==LUA_TSTRING) {
        size_t size=0; const char *text=lua_tolstring(lua,index,&size); uint64_t id=0;
        if (size && size<=20) {
            for (size_t i=0;i<size;++i) {
                unsigned digit=(unsigned char)text[i]-'0';
                if (digit>9 || id>(UINT64_MAX-digit)/10) return (Fl_Id)luaL_error(lua,"imports require positive exact Lua asset IDs");
                id=id*10+digit;
            }
            if (id) return id;
        }
    }
    return (Fl_Id)luaL_error(lua,"imports require positive exact Lua asset IDs");
}

/* Validate declaration keys before iteration; imported values are never arbitrary filesystem modules. */
static void import_alias(lua_State *lua, int index)
{
    size_t size=0; const char *name=lua_type(lua,index)==LUA_TSTRING ? lua_tolstring(lua,index,&size) : NULL;
    if (!name || !size || size>64 || !fl_json_utf8_valid(name,size)) luaL_error(lua,"import aliases require bounded UTF-8 names");
}

/* Resolve a fresh per-invocation module graph; cached chunks contain no surviving module/global state. */
static void resolve_imports(lua_State *lua, Fl_ScriptRuntime *runtime, int module, const Fl_Asset *asset,
                             int cache, Fl_Id *path, size_t depth)
{
    module=lua_absindex(lua,module); cache=lua_absindex(lua,cache);
    if (depth==SCRIPT_IMPORT_DEPTH) luaL_error(lua,"Lua import nesting exceeds 32 modules");
    path[depth]=asset->id;
    lua_getfield(lua,module,"imports"); int declarations=lua_gettop(lua);
    if (lua_isnil(lua,declarations)) {lua_pop(lua,1); return;}
    luaL_checktype(lua,declarations,LUA_TTABLE); lua_newtable(lua); int resolved=lua_gettop(lua);
    size_t count=0; lua_pushnil(lua);
    while (lua_next(lua,declarations)) {
        import_alias(lua,-2); Fl_Id id=import_id(lua,-1);
        if (++count>SCRIPT_IMPORT_LIMIT) luaL_error(lua,"Lua import budget exceeded");
        bool declared=false;
        for (size_t i=0;i<asset->reference_count;++i) if (asset->references[i]==id) {declared=true; break;}
        if (!declared) luaL_error(lua,"imports must match the compiled asset declarations");
        for (size_t i=0;i<=depth;++i) if (path[i]==id) luaL_error(lua,"recursive Lua import dependency");
        const Fl_Asset *import=fl_project_asset(runtime->scene->project,id);
        if (!import || import->kind!=FL_ASSET_LUA) luaL_error(lua,"missing or incompatible Lua import");
        lua_pushlstring(lua,(const char *)&id,sizeof(id)); lua_rawget(lua,cache);
        if (lua_isnil(lua,-1)) {
            lua_pop(lua,1); size_t position=0;
            while (position<runtime->import_count && runtime->imports[position].asset!=id) ++position;
            if (position==runtime->import_count) {
                if (position==SCRIPT_IMPORT_LIMIT) luaL_error(lua,"Lua compiled import budget exceeded");
                char name[FL_SOURCE_NAME_MAX+2]; snprintf(name,sizeof(name),"=%s",import->name);
                if (luaL_loadbufferx(lua,import->source,(size_t)import->size,name,"t")!=LUA_OK) lua_error(lua);
                runtime->imports[position]=(Fl_ImportChunk){.asset=id,.ref=luaL_ref(lua,LUA_REGISTRYINDEX)}; ++runtime->import_count;
            }
            int top=lua_gettop(lua); lua_rawgeti(lua,LUA_REGISTRYINDEX,runtime->imports[position].ref); int chunk=lua_gettop(lua);
            environment(lua,runtime->scene->entities[runtime->entity].id ^ runtime->programs[runtime->program_index].component ^ id);
            lua_pushvalue(lua,-1); if (!lua_setupvalue(lua,chunk,1)) lua_pop(lua,1);
            lua_pushvalue(lua,chunk); lua_call(lua,0,1); luaL_checktype(lua,-1,LUA_TTABLE);
            int imported_module=lua_gettop(lua); resolve_imports(lua,runtime,imported_module,import,cache,path,depth+1);
            lua_pushlstring(lua,(const char *)&id,sizeof(id)); lua_pushvalue(lua,imported_module); lua_rawset(lua,cache);
            lua_pushvalue(lua,imported_module); lua_replace(lua,top+1); lua_settop(lua,top+1);
        }
        lua_pushvalue(lua,-3); lua_pushvalue(lua,-2); lua_rawset(lua,resolved);
        lua_pop(lua,2);
    }
    if (count!=asset->reference_count) luaL_error(lua,"imports must be static declarations");
    lua_pushvalue(lua,resolved); lua_setfield(lua,module,"imports"); lua_pop(lua,2);
}

/* Reconstruct the module for every invocation; closures and globals never retain hidden state. */
static int load_module(lua_State *lua, Fl_ScriptRuntime *runtime)
{
    if (runtime->cancel && atomic_load_explicit(runtime->cancel,memory_order_relaxed)) luaL_error(lua,"evaluation cancelled");
    runtime->instructions=SCRIPT_INSTRUCTIONS;
    Fl_ScriptProgram *program=&runtime->programs[runtime->program_index];
    if (program->ref==LUA_NOREF) {
        for (size_t i=0;i<runtime->count;++i) if (runtime->programs[i].asset==program->asset && runtime->programs[i].ref!=LUA_NOREF) {
            program->ref=runtime->programs[i].ref; break;
        }
    }
    if (program->ref==LUA_NOREF) {
        char name[FL_SOURCE_NAME_MAX+2]; snprintf(name,sizeof(name),"=%s",runtime->script->name);
        int status=luaL_loadbufferx(lua,runtime->script->source,runtime->script->size,name,"t");
        if (status!=LUA_OK) {program->compile_failed=status==LUA_ERRSYNTAX; return lua_error(lua);}
        program->ref=luaL_ref(lua,LUA_REGISTRYINDEX);
    }
    lua_rawgeti(lua,LUA_REGISTRYINDEX,program->ref); int chunk=lua_gettop(lua);
    environment(lua,runtime->scene->entities[runtime->entity].id ^ (program->component*UINT64_C(0x9e3779b97f4a7c15)));
    lua_pushvalue(lua,-1); if (!lua_setupvalue(lua,chunk,1)) lua_pop(lua,1);
    lua_pushvalue(lua,chunk); lua_call(lua,0,1); int module=lua_gettop(lua);
    if (!lua_istable(lua,module)) luaL_error(lua,"return a behavior module with sample(ctx)");
    lua_newtable(lua); int cache=lua_gettop(lua); Fl_Id path[SCRIPT_IMPORT_DEPTH];
    resolve_imports(lua,runtime,module,runtime->script,cache,path,0); lua_pop(lua,1);
    capture_parameter_schema(lua,module,program); capture_lifecycle(lua,module,program); validate_bindings(lua,module,program);
    return module;
}

typedef enum Fl_CallbackPhase { CALLBACK_INITIALIZE, CALLBACK_STEP, CALLBACK_SAMPLE } Fl_CallbackPhase;

/* Validate declared input types against this attachment; scene references resolve at callback time. */
static void validate_bindings(lua_State *lua, int module, const Fl_ScriptProgram *program)
{
    module=lua_absindex(lua,module); lua_getfield(lua,module,"bindings"); int schema=lua_gettop(lua);
    if (!lua_isnil(lua,schema) && !lua_istable(lua,schema)) luaL_error(lua,"bindings must be a schema table");
    for (const cJSON *binding=program->bindings->child;binding;binding=binding->next) {
        if (lua_isnil(lua,schema)) luaL_error(lua,"undeclared binding %s",binding->string);
        lua_getfield(lua,schema,binding->string);
        if (!lua_istable(lua,-1)) luaL_error(lua,"undeclared binding %s",binding->string);
        lua_getfield(lua,-1,"type");
        const char *type=lua_type(lua,-1)==LUA_TSTRING ? lua_tostring(lua,-1) : NULL;
        if (!type || lua_rawlen(lua,-1)!=strlen(type) || strcmp(type,cJSON_GetObjectItemCaseSensitive(binding,"type")->valuestring))
            luaL_error(lua,"binding type mismatch: %s",binding->string);
        lua_pop(lua,2);
    }
    size_t count=0;
    if (lua_istable(lua,schema)) {
        lua_pushnil(lua);
        while (lua_next(lua,schema)) {
            size_t size=0; const char *name=lua_type(lua,-2)==LUA_TSTRING ? lua_tolstring(lua,-2,&size) : NULL;
            if (++count>FL_SCRIPT_BINDING_COUNT || !name || !size || size>63 || strlen(name)!=size || !fl_json_utf8_valid(name,size) || !lua_istable(lua,-1))
                luaL_error(lua,"invalid binding schema");
            lua_getfield(lua,-1,"type"); const char *type=lua_type(lua,-1)==LUA_TSTRING ? lua_tostring(lua,-1) : NULL;
            if (!type || lua_rawlen(lua,-1)!=strlen(type) || (strcmp(type,"entity") && strcmp(type,"component") && strcmp(type,"region") && strcmp(type,"asset")))
                luaL_error(lua,"invalid binding type: %s",name);
            lua_pop(lua,1); lua_getfield(lua,-1,"optional");
            if (!lua_isnil(lua,-1) && !lua_isboolean(lua,-1)) luaL_error(lua,"binding optional must be boolean");
            if (!lua_toboolean(lua,-1) && !cJSON_GetObjectItemCaseSensitive(program->bindings,name)) luaL_error(lua,"missing binding: %s",name);
            lua_pop(lua,2);
        }
    }
    lua_pop(lua,1);
}

/* Snapshot native inputs at the callback tick; behavior output ordering never changes these reads. */
static void push_bindings(lua_State *lua, Fl_ScriptRuntime *runtime, Fl_Tick tick)
{
    const Fl_ScriptProgram *program=&runtime->programs[runtime->program_index];
    lua_newtable(lua); int table=lua_gettop(lua);
    for (const cJSON *binding=program->bindings->child;binding;binding=binding->next) {
        Fl_Json context={0}; uint64_t id=0,owner=0;
        fl_json_uint(&context,cJSON_GetObjectItemCaseSensitive(binding,"id"),&id);
        const char *type=cJSON_GetObjectItemCaseSensitive(binding,"type")->valuestring;
        if (!strcmp(type,"entity")) {
            const Fl_Entity *entity=fl_scene_entity(runtime->scene,id); Fl_EntityState native;
            if (!entity || !fl_entity_state_sample(runtime->scene,(size_t)(entity-runtime->scene->entities),tick,&native))
                luaL_error(lua,"unresolved entity binding: %s",binding->string);
            double values[FL_PROP_COUNT][4]={{0}}; state_values(&native,values);
            push_entity(lua,runtime,(size_t)(entity-runtime->scene->entities),&native,values);
        } else if (!strcmp(type,"component")) {
            fl_json_uint(&context,cJSON_GetObjectItemCaseSensitive(binding,"entity"),&owner);
            const Fl_Entity *entity=fl_scene_entity(runtime->scene,owner);
            const Fl_Component *component=entity ? fl_entity_component_instance(entity,id) : NULL;
            if (!component) luaL_error(lua,"unresolved component binding: %s",binding->string);
            if (component->size>FL_SCRIPT_PARAMETER_LIMIT) luaL_error(lua,"component binding snapshot budget exceeded: %s",binding->string);
            if (component->type==FL_COMPONENT_ANIMATION) {
                const Fl_Animation *animation=(const Fl_Animation *)component->data; size_t keys=0;
                for (size_t i=0;i<animation->track_count;++i) {
                    if (animation->tracks[i].key_count>512-keys) luaL_error(lua,"component binding key budget exceeded: %s",binding->string);
                    keys+=animation->tracks[i].key_count;
                }
            }
            cJSON_Delete(runtime->binding_output); runtime->binding_output=NULL;
            runtime->binding_output=fl_document_component_write(&context,component);
            if (!runtime->binding_output || context.error[0]) luaL_error(lua,"cannot snapshot component binding: %s",binding->string);
            push_json(lua,runtime->binding_output,0);
            cJSON_Delete(runtime->binding_output); runtime->binding_output=NULL;
        } else if (!strcmp(type,"region")) {
            Fl_Tick start,end;
            if (!fl_scene_beat_range(runtime->scene,id,&start,&end)) luaL_error(lua,"unresolved region binding: %s",binding->string);
            lua_newtable(lua);
            lua_pushinteger(lua,start); lua_setfield(lua,-2,"start_tick");
            lua_pushinteger(lua,end); lua_setfield(lua,-2,"end_tick");
        } else {
            const Fl_Asset *asset=fl_project_asset(runtime->scene->project,id);
            if (!asset) luaL_error(lua,"unresolved asset binding: %s",binding->string);
            lua_newtable(lua); lua_pushstring(lua,asset->name); lua_setfield(lua,-2,"name");
            lua_pushinteger(lua,asset->kind); lua_setfield(lua,-2,"kind");
        }
        char exact[24]; snprintf(exact,sizeof(exact),"%llu",(unsigned long long)id);
        lua_pushstring(lua,exact); lua_setfield(lua,-2,"id");
        lua_setfield(lua,table,binding->string);
    }
}

/* Build inputs at this callback's actual media time; only sampling sees previous evaluated layers. */
static int push_context(lua_State *lua, Fl_ScriptRuntime *runtime, Fl_CallbackPhase phase, Fl_Tick local_tick, int64_t step)
{
    Fl_ScriptProgram *program=&runtime->programs[runtime->program_index];
    Fl_EntityState native, *requested=runtime->state;
    Fl_Tick composition=runtime->tick;
    if (phase!=CALLBACK_SAMPLE) {
        if (!fl_time_mapping_composition(runtime->scene,&runtime->mapping,local_tick,runtime->time.cycle,&composition) ||
            !fl_entity_state_sample(runtime->scene,runtime->entity,composition,&native)) luaL_error(lua,"cannot resolve fixed-step inputs");
        runtime->state=&native;
    }
    double values[FL_PROP_COUNT][4]={{0}}; state_values(runtime->state,values);
    push_entity(lua,runtime,runtime->entity,runtime->state,values); int self=lua_gettop(lua); runtime->state=requested;
    lua_newtable(lua); int context=lua_gettop(lua);
    lua_pushvalue(lua,self); lua_setfield(lua,context,"entity");
    lua_newtable(lua); int time=lua_gettop(lua);
    lua_pushinteger(lua,local_tick); lua_setfield(lua,time,"tick");
    lua_pushnumber(lua,(double)local_tick/FL_TICKS_PER_SECOND); lua_setfield(lua,time,"seconds");
    lua_pushnumber(lua,(double)runtime->time.duration/FL_TICKS_PER_SECOND); lua_setfield(lua,time,"duration");
    lua_pushnumber(lua,(double)local_tick/(double)runtime->time.duration); lua_setfield(lua,time,"progress");
    lua_pushnumber(lua,(double)composition/FL_TICKS_PER_SECOND); lua_setfield(lua,time,"composition_seconds");
    lua_pushinteger(lua,runtime->time.cycle); lua_setfield(lua,time,"cycle");
    lua_pushinteger(lua,step); lua_setfield(lua,time,"step");
    lua_pushnumber(lua,phase==CALLBACK_INITIALIZE || !program->has_step ? 0 : (double)program->step_ticks/FL_TICKS_PER_SECOND); lua_setfield(lua,time,"dt");
    lua_pushnumber(lua,phase==CALLBACK_SAMPLE && program->has_step ? (double)(local_tick%program->step_ticks)/program->step_ticks : 0);
    lua_setfield(lua,time,"alpha");
    lua_pushstring(lua,phase==CALLBACK_SAMPLE ? runtime->time.phase==FL_TIME_BEFORE ? "before" : runtime->time.phase==FL_TIME_AFTER ? "after" : "active" : "active");
    lua_setfield(lua,time,"phase"); lua_setfield(lua,context,"time");
    push_parameters(lua,program); lua_setfield(lua,context,"params");
    push_bindings(lua,runtime,composition); lua_setfield(lua,context,"bindings");
    if (program->latest.bytes) {
        char error[256];
        if (!fl_lua_value_push(lua,program->latest.bytes,program->latest.size,error)) luaL_error(lua,"%s",error);
    } else lua_newtable(lua);
    lua_setfield(lua,context,"state");
    return context;
}

/* Seed each isolated module environment without relying on ambient time or previous callback history. */
static void seed_environment(lua_State *lua, int environment_index, uint64_t seed)
{
    environment_index=lua_absindex(lua,environment_index);
    lua_getfield(lua,environment_index,"math"); lua_getfield(lua,-1,"randomseed");
    lua_pushinteger(lua,(lua_Integer)(seed&INT64_MAX)); lua_pushinteger(lua,0); lua_call(lua,2,0); lua_pop(lua,1);
}

/* Counter-based callback streams include helper modules, so imported step noise advances reproducibly too. */
static void seed_callback(lua_State *lua, Fl_ScriptRuntime *runtime, int environment_index, Fl_CallbackPhase phase, int64_t step)
{
    Fl_ScriptProgram *program=&runtime->programs[runtime->program_index];
    uint64_t seed=runtime->scene->entities[runtime->entity].id ^ (program->component*UINT64_C(0x9e3779b97f4a7c15));
    if (phase!=CALLBACK_SAMPLE) seed^=(uint64_t)step*UINT64_C(0xbf58476d1ce4e5b9) ^ (uint64_t)runtime->time.cycle*UINT64_C(0x94d049bb133111eb) ^ (uint64_t)(phase+1);
    seed_environment(lua,environment_index,seed);
    for (size_t i=0;i<runtime->import_count;++i) {
        lua_rawgeti(lua,LUA_REGISTRYINDEX,runtime->imports[i].ref);
        const char *name=lua_getupvalue(lua,-1,1);
        if (name) {
            if (!strcmp(name,"_ENV") && lua_istable(lua,-1)) seed_environment(lua,-1,seed ^ runtime->imports[i].asset);
            lua_pop(lua,1);
        }
        lua_pop(lua,1);
    }
}

/* Invoke one capability under the caller's protected boundary, capturing only explicit state for simulation. */
static void invoke_callback(lua_State *lua, Fl_ScriptRuntime *runtime, Fl_CallbackPhase phase, Fl_Tick local_tick, int64_t step)
{
    Fl_ScriptProgram *program=&runtime->programs[runtime->program_index]; int top=lua_gettop(lua);
    int module=load_module(lua,runtime), environment_index=module-1;
    int context=push_context(lua,runtime,phase,local_tick,step);
    lua_getfield(lua,module,"imports"); if (lua_isnil(lua,-1)) {lua_pop(lua,1); lua_newtable(lua);}
    lua_setfield(lua,context,"imports");
    static const char *const names[]={"initialize","step","sample"};
    lua_getfield(lua,module,names[phase]);
    if (!lua_isnil(lua,-1)) {
        if (!lua_isfunction(lua,-1)) luaL_error(lua,"%s must be a function",names[phase]);
        seed_callback(lua,runtime,environment_index,phase,step);
        lua_pushvalue(lua,context); lua_call(lua,1,0);
    } else lua_pop(lua,1);
    if (phase==CALLBACK_SAMPLE) {
        lua_getfield(lua,context,"entity"); int self=lua_gettop(lua);
        Fl_EntityState result=*runtime->state;
        read_self(lua,self,&result,fl_entity_component(&runtime->scene->entities[runtime->entity],FL_COMPONENT_TEXT));
        *runtime->state=result;
    } else {
        lua_getfield(lua,context,"state"); char error[256];
        if (!fl_lua_value_capture(lua,-1,&program->scratch,error)) luaL_error(lua,"%s",error);
        if (!store_snapshot(runtime,&program->latest,program->scratch.data,program->scratch.size,step)) luaL_error(lua,"simulation state budget exceeded");
    }
    lua_settop(lua,top);
}

/* Restore a compatible prefix and advance fixed local steps, with bounded memory and cooperative cancellation. */
static void simulate(lua_State *lua, Fl_ScriptRuntime *runtime)
{
    Fl_ScriptProgram *program=&runtime->programs[runtime->program_index];
    if (!program->state_initialized || program->simulation_cycle!=runtime->time.cycle) {
        clear_simulation(runtime,program); program->simulation_cycle=runtime->time.cycle;
        invoke_callback(lua,runtime,CALLBACK_INITIALIZE,0,0);
        if (!store_snapshot(runtime,&program->checkpoints[0],program->latest.bytes,program->latest.size,0)) luaL_error(lua,"simulation state budget exceeded");
        program->state_initialized=true;
    }
    if (!program->has_step) return;
    int64_t target=runtime->time.tick/program->step_ticks;
    if (program->latest.step>target) {
        const Fl_ScriptSnapshot *nearest=&program->checkpoints[0];
        for (size_t i=1;i<SCRIPT_CHECKPOINTS;++i)
            if (program->checkpoints[i].bytes && program->checkpoints[i].step<=target && program->checkpoints[i].step>nearest->step) nearest=&program->checkpoints[i];
        if (!store_snapshot(runtime,&program->latest,nearest->bytes,nearest->size,nearest->step)) luaL_error(lua,"simulation state budget exceeded");
    }
    if (target-program->latest.step>SCRIPT_REPLAY_LIMIT) luaL_error(lua,"simulation replay step budget exceeded");
    while (program->latest.step<target) {
        int64_t next=program->latest.step+1;
        invoke_callback(lua,runtime,CALLBACK_STEP,next*program->step_ticks,next);
        if (next%(FL_TICKS_PER_SECOND/program->step_ticks)==0) {
            size_t index=program->checkpoint_cursor;
            if (store_snapshot(runtime,&program->checkpoints[index],program->latest.bytes,program->latest.size,next))
                program->checkpoint_cursor=index+1<SCRIPT_CHECKPOINTS ? index+1 : 1;
        }
    }
}

/* Sampling is independent of transport history and never commits mutations of the explicit state copy. */
static int evaluate(lua_State *lua)
{
    Fl_ScriptRuntime *runtime=*(Fl_ScriptRuntime **)lua_getextraspace(lua);
    Fl_ScriptProgram *program=&runtime->programs[runtime->program_index];
    if (!program->lifecycle_ready) {load_module(lua,runtime); lua_settop(lua,0);}
    push_parameters(lua,program); lua_pop(lua,1);
    if (!runtime->time.active) return 0;
    if (program->has_initialize || program->has_step) simulate(lua,runtime);
    if (program->has_sample) invoke_callback(lua,runtime,CALLBACK_SAMPLE,runtime->time.tick,
        program->has_step ? runtime->time.tick/program->step_ticks : 0);
    return 0;
}

/* Initialize the base function template under allocation protection. */
static int initialize(lua_State *L) {luaopen_base(L); return 0;}

Fl_ScriptRuntime *fl_script_runtime_create(const Fl_Scene *s)
{
    if (!s || s->entity_count > SIZE_MAX / sizeof(Fl_ScriptProgram)) return NULL;
    Fl_ScriptRuntime *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->entity_count=s->entity_count;
    if (s->entity_count==SIZE_MAX || s->entity_count+1>SIZE_MAX/sizeof(*r->starts)) {free(r); return NULL;}
    r->starts=calloc(s->entity_count+1,sizeof(*r->starts));
    if (!r->starts) {free(r); return NULL;}
    for (size_t i=0;i<s->entity_count;++i) {
        for (size_t j=0;j<s->entities[i].component_count;++j) r->count+=s->entities[i].components[j].type==FL_COMPONENT_SCRIPT;
        r->starts[i+1]=r->count;
    }
    if (r->count>SIZE_MAX/sizeof(*r->programs)) {fl_script_runtime_destroy(r); return NULL;}
    r->programs=r->count ? calloc(r->count,sizeof(*r->programs)) : NULL;
    if (r->count && !r->programs) {fl_script_runtime_destroy(r); return NULL;}
    size_t program=0;
    for (size_t i=0;i<s->entity_count;++i) for (size_t j=0;j<s->entities[i].component_count;++j) {
        const Fl_Component *c=&s->entities[i].components[j];
        if (c->type==FL_COMPONENT_SCRIPT) {
            Fl_Script script; Fl_Json context={0};
            if (!fl_script_decode(c,&script)) {fl_script_runtime_destroy(r); return NULL;}
            Fl_ScriptProgram *p=&r->programs[program++]; p->ref=LUA_NOREF; p->component=c->id; p->asset=script.script;
            p->parameters=fl_json_parse(&context,script.parameters,script.parameters_size);
            p->bindings=fl_json_parse(&context,script.bindings,script.bindings_size);
            if (!cJSON_IsObject(p->parameters) || !fl_script_bindings_valid(&context,p->bindings)) {fl_script_runtime_destroy(r); return NULL;}
        }
    }
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
    cJSON_Delete(r->build_output); cJSON_Delete(r->binding_output);
    if (r->lua) lua_close(r->lua);
    if (r->programs) for (size_t i=0;i<r->count;++i) {clear_simulation(r,&r->programs[i]); free(r->programs[i].scratch.data); cJSON_Delete(r->programs[i].parameters); cJSON_Delete(r->programs[i].bindings); cJSON_Delete(r->programs[i].parameter_schema); cJSON_Delete(r->programs[i].effective_parameters);}
    fl_script_runtime_begin_frame(r); free(r->texts);
    free(r->starts); free(r->programs); free(r);
}

void fl_script_runtime_set_cancel(Fl_ScriptRuntime *runtime, const atomic_bool *cancel)
{
    if (runtime) runtime->cancel = cancel;
}

void fl_script_runtime_begin_frame(Fl_ScriptRuntime *r)
{
    if (!r) return;
    for (size_t i = 0; i < r->text_count; ++i) free(r->texts[i]);
    r->text_count = r->text_bytes = 0;
}

/* Run one attachment with its own compiled chunk and diagnostic. */
static bool evaluate_attachment(Fl_ScriptRuntime *r, const Fl_Scene *s, size_t entity, size_t index,
                                Fl_Tick tick, Fl_EntityState *state)
{
    const Fl_Component *component=fl_entity_component_instance(&s->entities[entity],r->programs[index].component);
    Fl_Script script;
    if (!fl_script_decode(component,&script) || !script.enabled) return true;
    const Fl_Asset *source=fl_project_asset(s->project,script.script);
    if (!source || source->kind!=FL_ASSET_LUA) return false;
    r->program_index=index; r->mapping=script.time;
    Fl_ScriptProgram *program=&r->programs[index];
    bool bound=fl_time_mapping_sample(s,&script.time,tick,&r->time);
    if (!bound) r->time=(Fl_TimeSample){.duration=1};
    if (program->compile_failed) return false;
    r->scene = s; r->entity = entity; r->tick = tick; r->state = state; r->script = source;
    r->instructions = SCRIPT_INSTRUCTIONS;
    lua_State *L = r->lua;
    lua_sethook(L, instruction_limit, LUA_MASKCOUNT, SCRIPT_HOOK_INTERVAL);
    lua_pushcfunction(L, evaluate);
    bool ok = lua_pcall(L, 0, 0, 0) == LUA_OK;
    lua_sethook(L, NULL, 0, 0);
    if (!ok) {
        const char *error = lua_tostring(L, -1);
        if (!error) error = "Lua evaluation failed";
        if ((!r->cancel || !atomic_load_explicit(r->cancel,memory_order_relaxed)) && strcmp(program->error, error)) FL_LOG_ERROR("script", "Entity %016llx: %s", (unsigned long long)s->entities[entity].id, error);
        snprintf(program->error, sizeof(program->error), "%s", error);
    } else if (!bound) snprintf(program->error,sizeof(program->error),"Unbound or invalid behavior time region");
    else program->error[0] = 0;
    lua_settop(L, 0);
    if (program->ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, program->ref); lua_pushnil(L); lua_setupvalue(L, -2, 1); lua_pop(L, 1);
    }
    lua_settop(L, 0);
    free(program->scratch.data); program->scratch=(Fl_Binary){0};
    lua_gc(L, LUA_GCCOLLECT);
    r->scene = NULL; r->state = NULL; r->script = NULL;
    return ok && bound;
}

bool fl_script_runtime_evaluate(Fl_ScriptRuntime *r, const Fl_Scene *s, size_t entity, Fl_Tick tick, Fl_EntityState *state)
{
    if (!r || !s || !state || entity>=r->entity_count || entity>=s->entity_count) return false;
    Fl_EntityState candidate=*state;
    for (size_t i=r->starts[entity];i<r->starts[entity+1];++i)
        if (!evaluate_attachment(r,s,entity,i,tick,&candidate)) return false;
    *state=candidate; return true;
}

bool fl_script_check(const char *source, size_t size, char *error, size_t error_size)
{
    lua_State *L = luaL_newstate();
    if (!L) {snprintf(error, error_size, "out of memory"); return false;}
    bool ok = luaL_loadbufferx(L, source, size, "=script", "t") == LUA_OK;
    if (!ok) snprintf(error, error_size, "%s", lua_tostring(L, -1));
    lua_close(L);
    return ok;
}

const char *fl_script_runtime_error(const Fl_ScriptRuntime *r, size_t entity)
{
    if (r && entity<r->entity_count) for (size_t i=r->starts[entity];i<r->starts[entity+1];++i) if (r->programs[i].error[0]) return r->programs[i].error;
    return "";
}

const char *fl_script_runtime_component_error(const Fl_ScriptRuntime *r, Fl_Id component)
{
    for (size_t i=0;r && component && i<r->count;++i) if (r->programs[i].component==component) return r->programs[i].error;
    return "";
}

const cJSON *fl_script_runtime_parameter_schema(const Fl_ScriptRuntime *r, Fl_Id component)
{
    for (size_t i=0;r && component && i<r->count;++i)
        if (r->programs[i].component==component && r->programs[i].schema_ready) return r->programs[i].parameter_schema;
    return NULL;
}

/* Validate declarations inside the same sandbox and budgets used by actual behavior evaluation. */
static int validate_module(lua_State *lua)
{
    Fl_ScriptRuntime *runtime=*(Fl_ScriptRuntime **)lua_getextraspace(lua);
    load_module(lua,runtime);
    push_parameters(lua,&runtime->programs[runtime->program_index]);
    return 0;
}

bool fl_script_validate_scene(const Fl_Scene *scene, const atomic_bool *cancel, char *error, size_t error_size)
{
    if (error && error_size) error[0]=0;
    if (!scene) return false;
    bool attached=false;
    for (size_t i=0; !attached && i<scene->entity_count; ++i)
        for (size_t j=0;j<scene->entities[i].component_count;++j) {
            Fl_Script script;
            if (fl_script_decode(&scene->entities[i].components[j],&script) && script.enabled) {attached=true; break;}
        }
    if (!attached) return true;
    Fl_ScriptRuntime *runtime=fl_script_runtime_create(scene);
    if (!runtime) {if (error && error_size) snprintf(error,error_size,"Cannot allocate behavior validation"); return false;}
    runtime->cancel=cancel;
    lua_State *lua=runtime->lua; bool ok=true;
    for (size_t i=0;ok && i<scene->entity_count;++i) for (size_t j=runtime->starts[i];ok && j<runtime->starts[i+1];++j) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {if (error && error_size) snprintf(error,error_size,"Evaluation cancelled"); ok=false; break;}
        const Fl_Component *component=fl_entity_component_instance(&scene->entities[i],runtime->programs[j].component);
        Fl_Script script;
        if (!fl_script_decode(component,&script) || !script.enabled) continue;
        const Fl_Asset *asset=fl_project_asset(scene->project,script.script);
        if (!asset || asset->kind!=FL_ASSET_LUA) {if (error && error_size) snprintf(error,error_size,"Missing Lua asset"); ok=false; break;}
        runtime->scene=scene; runtime->entity=i; runtime->program_index=j; runtime->script=asset; runtime->instructions=SCRIPT_INSTRUCTIONS;
        lua_sethook(lua,instruction_limit,LUA_MASKCOUNT,SCRIPT_HOOK_INTERVAL); lua_pushcfunction(lua,validate_module);
        ok=lua_pcall(lua,0,0,0)==LUA_OK; lua_sethook(lua,NULL,0,0);
        if (!ok && error && error_size) snprintf(error,error_size,"%s [%llu]: %s",asset->name,(unsigned long long)component->id,
            lua_tostring(lua,-1) ? lua_tostring(lua,-1) : "Behavior validation failed");
        lua_settop(lua,0);
        if (runtime->programs[j].ref!=LUA_NOREF) {
            lua_rawgeti(lua,LUA_REGISTRYINDEX,runtime->programs[j].ref); lua_pushnil(lua);
            if (!lua_setupvalue(lua,-2,1)) lua_pop(lua,1);
        }
        lua_settop(lua,0); lua_gc(lua,LUA_GCCOLLECT);
    }
    fl_script_runtime_destroy(runtime); return ok;
}

/* Build runs under the same VM/instruction boundary as sampling, with canonical authored inputs at tick zero. */
static int build_document(lua_State *lua)
{
    Fl_ScriptRuntime *runtime=*(Fl_ScriptRuntime **)lua_getextraspace(lua);
    int module=load_module(lua,runtime);
    push_parameters(lua,&runtime->programs[runtime->program_index]); lua_pop(lua,1);
    lua_getfield(lua,module,"build");
    if (lua_isnil(lua,-1)) return 0;
    int function=lua_gettop(lua);
    int context=push_context(lua,runtime,CALLBACK_SAMPLE,0,0);
    lua_getfield(lua,module,"imports"); if (lua_isnil(lua,-1)) {lua_pop(lua,1); lua_newtable(lua);}
    lua_setfield(lua,context,"imports");
    lua_pushvalue(lua,function); lua_pushvalue(lua,context); lua_call(lua,1,1);
    if (!lua_istable(lua,-1)) return luaL_error(lua,"build(ctx) must return a typed Flow prototype document");
    Fl_Json json={0}; runtime->build_output=fl_lua_document_capture(lua,-1,&json);
    if (!runtime->build_output) return luaL_error(lua,"%s",json.error);
    return 0;
}

bool fl_script_runtime_build(Fl_ScriptRuntime *runtime, const Fl_Scene *scene, size_t entity, Fl_Id component,
                              const atomic_bool *cancel, cJSON **output, char *error, size_t capacity)
{
    if (output) *output=NULL;
    if (error && capacity) error[0]=0;
    if (!runtime || !scene || entity>=scene->entity_count || !output) return false;
    size_t program=runtime->starts[entity];
    while (program<runtime->starts[entity+1] && runtime->programs[program].component!=component) ++program;
    if (program==runtime->starts[entity+1]) return false;
    const Fl_Component *attachment=fl_entity_component_instance(&scene->entities[entity],component); Fl_Script script;
    if (!fl_script_decode(attachment,&script)) return false;
    if (!script.enabled) return true;
    const Fl_Asset *asset=fl_project_asset(scene->project,script.script);
    if (!asset || asset->kind!=FL_ASSET_LUA) return false;
    Fl_EntityState native; if (!fl_entity_state_sample(scene,entity,0,&native)) return false;
    runtime->scene=scene; runtime->entity=entity; runtime->program_index=program; runtime->script=asset;
    runtime->state=&native; runtime->tick=0; runtime->mapping=fl_time_mapping_default(); runtime->cancel=cancel;
    runtime->time=(Fl_TimeSample){.duration=fl_scene_length(scene),.active=true}; runtime->instructions=SCRIPT_INSTRUCTIONS;
    lua_State *lua=runtime->lua; lua_settop(lua,0); lua_sethook(lua,instruction_limit,LUA_MASKCOUNT,SCRIPT_HOOK_INTERVAL);
    lua_pushcfunction(lua,build_document); bool ok=lua_pcall(lua,0,0,0)==LUA_OK;
    lua_sethook(lua,NULL,0,0); runtime->cancel=NULL; runtime->state=NULL;
    if (!ok && error && capacity) snprintf(error,capacity,"%s [%llu] build: %s",asset->name,(unsigned long long)component,lua_tostring(lua,-1));
    lua_settop(lua,0);
    if (ok) {*output=runtime->build_output; runtime->build_output=NULL;}
    else {cJSON_Delete(runtime->build_output); runtime->build_output=NULL;}
    return ok;
}

typedef struct ImportDiscovery {
    Fl_ScriptRuntime runtime;
    const char *source;
    size_t size,count;
    Fl_Id ids[SCRIPT_IMPORT_LIMIT];
} ImportDiscovery;

/* Execute declarations only; callback bodies and imported modules never run during source discovery. */
static int discover_imports(lua_State *lua)
{
    ImportDiscovery *discovery=lua_touserdata(lua,lua_upvalueindex(1));
    if (luaL_loadbufferx(lua,discovery->source,discovery->size,"=script","t")!=LUA_OK) return lua_error(lua);
    int chunk=lua_gettop(lua); environment(lua,0);
    lua_pushvalue(lua,-1); if (!lua_setupvalue(lua,chunk,1)) lua_pop(lua,1);
    lua_pushvalue(lua,chunk); lua_call(lua,0,1);
    if (!lua_istable(lua,-1)) return 0;
    lua_getfield(lua,-1,"imports"); if (lua_isnil(lua,-1)) return 0;
    luaL_checktype(lua,-1,LUA_TTABLE); int imports=lua_gettop(lua); lua_pushnil(lua);
    while (lua_next(lua,imports)) {
        import_alias(lua,-2);
        if (discovery->count==SCRIPT_IMPORT_LIMIT) return luaL_error(lua,"Lua import budget exceeded");
        discovery->ids[discovery->count++]=import_id(lua,-1); lua_pop(lua,1);
    }
    return 0;
}

bool fl_script_dependencies(const char *source, size_t size, Fl_Id **references, size_t *count,
                             char *error, size_t capacity)
{
    if (error && capacity) error[0]=0;
    if (!references || !count || !source || !size || size>FL_SCRIPT_SOURCE_LIMIT) return false;
    *references=NULL; *count=0; ImportDiscovery discovery={.source=source,.size=size};
    Fl_ScriptRuntime *runtime=&discovery.runtime; runtime->instructions=SCRIPT_INSTRUCTIONS;
    lua_State *lua=lua_newstate(script_allocate,runtime);
    if (!lua) {if (error && capacity) snprintf(error,capacity,"Cannot allocate Lua declaration evaluator"); return false;}
    *(Fl_ScriptRuntime **)lua_getextraspace(lua)=runtime;
    lua_pushcfunction(lua,initialize); bool ok=lua_pcall(lua,0,0,0)==LUA_OK;
    if (ok) {
        lua_sethook(lua,instruction_limit,LUA_MASKCOUNT,SCRIPT_HOOK_INTERVAL);
        lua_pushlightuserdata(lua,&discovery); lua_pushcclosure(lua,discover_imports,1); ok=lua_pcall(lua,0,0,0)==LUA_OK;
    }
    if (!ok && error && capacity) snprintf(error,capacity,"%s",lua_tostring(lua,-1));
    lua_close(lua);
    if (ok && discovery.count) {
        *references=malloc(discovery.count*sizeof(**references));
        if (!*references) {if (error && capacity) snprintf(error,capacity,"Cannot allocate Lua import references"); return false;}
        memcpy(*references,discovery.ids,discovery.count*sizeof(**references)); *count=discovery.count;
    }
    return ok;
}
