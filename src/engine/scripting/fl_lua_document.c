#include "fl_lua_document.h"
#include <math.h>
#include <string.h>

#define DOCUMENT_NODES 200000u
#define DOCUMENT_BYTES (16u*1024u*1024u)
#define DOCUMENT_DEPTH 32u

typedef struct Capture {Fl_Json *context; size_t nodes,bytes; const void *path[DOCUMENT_DEPTH];} Capture;

/* Empty native collection fields have an unambiguous schema even though Lua has one table type. */
static bool collection(const char *name)
{
    static const char *const names[]={"entities","components","beats","clips","tracks","keys","overrides"};
    for (size_t i=0;name && i<sizeof(names)/sizeof(*names);++i) if (!strcmp(name,names[i])) return true;
    return false;
}

/* Account for JSON node/string storage before allocation; every rejected tree is released by its caller. */
static bool budget(Capture *capture, size_t bytes)
{
    if (++capture->nodes>DOCUMENT_NODES || bytes>DOCUMENT_BYTES-capture->bytes) return fl_json_fail(capture->context,"Generated document budget exceeded");
    capture->bytes+=bytes; return true;
}

/* Raw table traversal cannot execute metamethods; reserve Lua stack capacity before any native allocation. */
static cJSON *value(lua_State *lua, int index, const char *name, Capture *capture, size_t depth)
{
    index=lua_absindex(lua,index);
    if (!budget(capture,sizeof(cJSON))) return NULL;
    int type=lua_type(lua,index); cJSON *out=NULL;
    if (type==LUA_TNIL) out=cJSON_CreateNull();
    else if (type==LUA_TBOOLEAN) out=cJSON_CreateBool(lua_toboolean(lua,index));
    else if (type==LUA_TNUMBER) {
        double number=lua_tonumber(lua,index);
        if (!isfinite(number)) {fl_json_fail(capture->context,"Generated document numbers must be finite"); return NULL;}
        if (lua_isinteger(lua,index) && (lua_tointeger(lua,index)>INT64_C(9007199254740991) || lua_tointeger(lua,index)<-INT64_C(9007199254740991))) out=fl_json_i64(lua_tointeger(lua,index));
        else out=cJSON_CreateNumber(number);
    } else if (type==LUA_TSTRING) {
        size_t size=0; const char *text=lua_tolstring(lua,index,&size);
        if (!budget(capture,size+1) || !fl_json_utf8_valid(text,size)) {fl_json_fail(capture->context,"Generated document strings require UTF-8 without NUL"); return NULL;}
        out=cJSON_CreateString(text);
    } else if (type==LUA_TTABLE) {
        if (depth==DOCUMENT_DEPTH) {fl_json_fail(capture->context,"Generated document nesting limit exceeded"); return NULL;}
        const void *identity=lua_topointer(lua,index);
        for (size_t i=0;i<depth;++i) if (capture->path[i]==identity) {fl_json_fail(capture->context,"Generated document contains a cycle"); return NULL;}
        capture->path[depth]=identity; size_t entries=0,length=lua_rawlen(lua,index); bool numeric=false,strings=false,valid=true;
        int top=lua_gettop(lua); lua_pushnil(lua);
        while (lua_next(lua,index)) {
            int key=lua_type(lua,-2);
            if (key==LUA_TNUMBER && lua_isinteger(lua,-2) && lua_tointeger(lua,-2)>0 && (uint64_t)lua_tointeger(lua,-2)<=length) numeric=true;
            else if (key==LUA_TSTRING) strings=true;
            else valid=false;
            ++entries; lua_pop(lua,1);
            if (entries>DOCUMENT_NODES) {valid=false; break;}
        }
        lua_settop(lua,top);
        if (!valid || (numeric && strings) || (numeric && entries!=length)) {fl_json_fail(capture->context,"Generated document requires dense arrays or string-keyed objects"); return NULL;}
        bool array=numeric || (!entries && collection(name)); out=array ? cJSON_CreateArray() : cJSON_CreateObject();
        if (!out) {fl_json_fail(capture->context,"Cannot allocate generated document"); return NULL;}
        if (array) for (size_t i=0;i<length;++i) {
            lua_rawgeti(lua,index,(lua_Integer)i+1); cJSON *child=value(lua,-1,NULL,capture,depth+1); lua_pop(lua,1);
            if (!fl_json_add(capture->context,out,NULL,child)) break;
        } else {
            lua_pushnil(lua);
            while (!capture->context->error[0] && lua_next(lua,index)) {
                size_t size=0; const char *key=lua_tolstring(lua,-2,&size);
                if (!key || !fl_json_utf8_valid(key,size) || !budget(capture,size+1)) fl_json_fail(capture->context,"Invalid generated document field name");
                else fl_json_add(capture->context,out,key,value(lua,-1,key,capture,depth+1));
                lua_pop(lua,1);
            }
            lua_settop(lua,top);
        }
        if (capture->context->error[0]) {cJSON_Delete(out); return NULL;}
    } else {fl_json_fail(capture->context,"Generated document cannot contain functions or runtime handles"); return NULL;}
    if (!out) fl_json_fail(capture->context,"Cannot allocate generated document"); return out;
}

cJSON *fl_lua_document_capture(lua_State *lua, int index, Fl_Json *context)
{
    if (!lua || !context) return NULL;
    if (!lua_checkstack(lua,4*DOCUMENT_DEPTH+16)) {fl_json_fail(context,"Cannot reserve generated document stack"); return NULL;}
    Capture capture={.context=context}; return value(lua,index,NULL,&capture,0);
}
