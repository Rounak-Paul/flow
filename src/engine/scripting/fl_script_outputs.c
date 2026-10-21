#include "fl_script_outputs.h"
#include "schema/fl_properties.h"
#include "project/fl_project.h"
#include <lauxlib.h>
#include <string.h>

/* Resolve an exact property spelling, including the variable-length text channel. */
static int output_property(const char *name)
{
    if (!strcmp(name,"text")) return FL_SCRIPT_TEXT_PROPERTY;
    for (int p=0;p<FL_PROP_COUNT;++p) if (!strcmp(name,fl_property_name((Fl_AnimProperty)p))) return p;
    return -1;
}

static size_t capture(lua_State *lua, int module, const Fl_Scene *scene, size_t entity,
                                  const cJSON *bindings, bool sample, Fl_ScriptTarget targets[FL_SCRIPT_TARGET_LIMIT], bool reads)
{
    module=lua_absindex(lua,module); memset(targets,0,FL_SCRIPT_TARGET_LIMIT*sizeof(*targets));
    strcpy(targets[0].name,"self"); targets[0].entity=entity; size_t count=1;
    lua_getfield(lua,module,reads ? "reads" : "outputs"); int outputs=lua_gettop(lua);
    if (lua_isnil(lua,outputs)) {
        if (sample && !reads) for (int p=0;p<FL_SCRIPT_OUTPUT_COUNT;++p) targets[0].modes[p]=FL_OUTPUT_REPLACE;
        lua_pop(lua,1); return count;
    }
    luaL_checktype(lua,outputs,LUA_TTABLE); lua_pushnil(lua);
    while (lua_next(lua,outputs)) {
        size_t length; const char *name=lua_type(lua,-2)==LUA_TSTRING ? lua_tolstring(lua,-2,&length) : NULL;
        if (!name || !length || length>=64 || strlen(name)!=length || !fl_json_utf8_valid(name,length) || !lua_istable(lua,-1))
            luaL_error(lua,"outputs require named target/property tables");
        size_t at=0;
        if (strcmp(name,"self")) {
            if (count==FL_SCRIPT_TARGET_LIMIT) luaL_error(lua,"output target budget exceeded");
            at=count++; strcpy(targets[at].name,name); targets[at].entity=SIZE_MAX;
            const cJSON *binding=cJSON_GetObjectItemCaseSensitive(bindings,name);
            const cJSON *type=binding ? cJSON_GetObjectItemCaseSensitive(binding,"type") : NULL;
            if (!cJSON_IsString(type) || strcmp(type->valuestring,"entity")) luaL_error(lua,"output target requires an entity binding: %s",name);
            Fl_Json context={0}; uint64_t id;
            if (!fl_json_uint(&context,cJSON_GetObjectItemCaseSensitive(binding,"id"),&id)) luaL_error(lua,"invalid output binding: %s",name);
            const Fl_Entity *target=fl_scene_entity(scene,id);
            if (target) targets[at].entity=(size_t)(target-scene->entities);
            for (size_t i=0;target && i<at;++i) if (targets[i].entity==targets[at].entity)
                luaL_error(lua,"aliased output targets: %s",name);
        }
        int fields=lua_gettop(lua); lua_pushnil(lua); size_t properties=0;
        while (lua_next(lua,fields)) {
            size_t size; const char *property=lua_type(lua,-2)==LUA_TSTRING ? lua_tolstring(lua,-2,&size) : NULL;
            if (++properties>FL_SCRIPT_OUTPUT_COUNT || !property || strlen(property)!=size) luaL_error(lua,"invalid output property");
            int p=output_property(property);
            const char *mode=reads ? lua_isboolean(lua,-1) && lua_toboolean(lua,-1) ? "replace" : NULL : lua_type(lua,-1)==LUA_TSTRING ? lua_tostring(lua,-1) : NULL;
            if (p<0 || !mode || (!reads && lua_rawlen(lua,-1)!=strlen(mode))) luaL_error(lua,"invalid output declaration");
            Fl_OutputMode blend=!strcmp(mode,"replace") ? FL_OUTPUT_REPLACE : !strcmp(mode,"add") ? FL_OUTPUT_ADD : !strcmp(mode,"multiply") ? FL_OUTPUT_MULTIPLY : FL_OUTPUT_NONE;
            if (!blend || (p==FL_SCRIPT_TEXT_PROPERTY && blend!=FL_OUTPUT_REPLACE)) luaL_error(lua,"unsupported output blend: %s",property);
            if (!sample) luaL_error(lua,"output declarations require sample capability");
            targets[at].modes[p]=blend; lua_pop(lua,1);
        }
        lua_pop(lua,1);
    }
    lua_pop(lua,1); return count;
}

size_t fl_script_outputs_capture(lua_State *lua, int module, const Fl_Scene *scene, size_t entity,
                                  const cJSON *bindings, bool sample, Fl_ScriptTarget targets[FL_SCRIPT_TARGET_LIMIT])
{
    return capture(lua,module,scene,entity,bindings,sample,targets,false);
}

size_t fl_script_reads_capture(lua_State *lua, int module, const Fl_Scene *scene, size_t entity,
                                const cJSON *bindings, bool sample, Fl_ScriptTarget targets[FL_SCRIPT_TARGET_LIMIT])
{
    return capture(lua,module,scene,entity,bindings,sample,targets,true);
}
