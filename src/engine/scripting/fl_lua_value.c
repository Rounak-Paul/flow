#include "fl_lua_value.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(lua_Integer)==8 && sizeof(lua_Number)==8,"explicit state requires Lua's 64-bit numeric backend");
enum { VALUE_NIL, VALUE_FALSE, VALUE_TRUE, VALUE_INTEGER, VALUE_NUMBER, VALUE_STRING, VALUE_ARRAY, VALUE_OBJECT };
typedef struct ValueContext {Fl_Binary *bytes; size_t nodes; char *error;} ValueContext;

/* Record the first codec failure without unwinding owned native storage. */
static bool fail(ValueContext *context, const char *message)
{
    if (!context->error[0]) snprintf(context->error,256,"%s",message);
    return false;
}

/* Bound byte writes before the stream can allocate beyond the explicit-state budget. */
static bool write_bytes(ValueContext *context, const void *data, size_t size)
{
    if (size>FL_LUA_VALUE_LIMIT-context->bytes->size) return fail(context,"explicit state byte limit exceeded");
    fl_binary_write(context->bytes,data,size);
    return !context->bytes->failed || fail(context,"cannot allocate explicit state");
}

/* Use the engine's canonical little-endian word encoding. */
static bool write_word(ValueContext *context, uint64_t value)
{
    unsigned char bytes[8];
    for (unsigned i=0;i<8;++i) bytes[i]=(unsigned char)(value>>(8*i));
    return write_bytes(context,bytes,sizeof(bytes));
}

/* Store strings by length, including binary buffers and embedded NUL. */
static bool write_string(lua_State *lua, int index, ValueContext *context)
{
    size_t size; const char *text=lua_tolstring(lua,index,&size);
    return write_word(context,size) && write_bytes(context,text,size);
}

/* Traverse raw values; metatables cannot execute or smuggle hidden Lua state into checkpoints. */
static bool capture(lua_State *lua, int index, ValueContext *context, unsigned depth)
{
    if (depth>FL_LUA_VALUE_DEPTH || ++context->nodes>FL_LUA_VALUE_NODES) return fail(context,"explicit state nesting or value limit exceeded");
    if (!lua_checkstack(lua,6)) return fail(context,"cannot grow explicit state stack");
    index=lua_absindex(lua,index);
    unsigned char type;
    switch (lua_type(lua,index)) {
    case LUA_TNIL: type=VALUE_NIL; return write_bytes(context,&type,1);
    case LUA_TBOOLEAN: type=lua_toboolean(lua,index) ? VALUE_TRUE : VALUE_FALSE; return write_bytes(context,&type,1);
    case LUA_TNUMBER: {
        uint64_t bits;
        if (lua_isinteger(lua,index)) {lua_Integer value=lua_tointeger(lua,index); memcpy(&bits,&value,8); type=VALUE_INTEGER;}
        else {lua_Number value=lua_tonumber(lua,index); if (!isfinite(value)) return fail(context,"explicit state requires finite numbers"); memcpy(&bits,&value,8); type=VALUE_NUMBER;}
        return write_bytes(context,&type,1) && write_word(context,bits);
    }
    case LUA_TSTRING: type=VALUE_STRING; return write_bytes(context,&type,1) && write_string(lua,index,context);
    case LUA_TTABLE: break;
    default: return fail(context,"explicit state cannot contain functions, threads or native handles");
    }
    size_t count=0, maximum=0; bool array=false, object=false;
    lua_pushnil(lua);
    while (lua_next(lua,index)) {
        ++count; lua_pop(lua,1);
        if (count>FL_LUA_VALUE_NODES) {lua_pop(lua,1); return fail(context,"explicit state table limit exceeded");}
        if (lua_type(lua,-1)==LUA_TSTRING) object=true;
        else if (lua_isinteger(lua,-1) && lua_tointeger(lua,-1)>0 && (uint64_t)lua_tointeger(lua,-1)<=FL_LUA_VALUE_NODES) {
            array=true; size_t key=(size_t)lua_tointeger(lua,-1); if (key>maximum) maximum=key;
        } else {lua_pop(lua,1); return fail(context,"explicit state tables require string keys or dense array indices");}
    }
    if ((array && object) || (array && maximum!=count)) return fail(context,"explicit state tables cannot mix keys or contain array holes");
    type=array ? VALUE_ARRAY : VALUE_OBJECT;
    if (!write_bytes(context,&type,1) || !write_word(context,count)) return false;
    if (array) {
        for (size_t i=1;i<=count;++i) {
            lua_rawgeti(lua,index,(lua_Integer)i); bool ok=capture(lua,-1,context,depth+1); lua_pop(lua,1);
            if (!ok) return false;
        }
    } else {
        lua_pushnil(lua);
        while (lua_next(lua,index)) {
            bool ok=write_string(lua,-2,context) && capture(lua,-1,context,depth+1);
            lua_pop(lua,1);
            if (!ok) {lua_pop(lua,1); return false;}
        }
    }
    return true;
}

bool fl_lua_value_capture(lua_State *lua, int index, Fl_Binary *out, char error[256])
{
    error[0]=0; out->size=out->cursor=0; out->failed=false;
    int top=lua_gettop(lua); ValueContext context={.bytes=out,.error=error};
    bool ok=capture(lua,index,&context,0); lua_settop(lua,top); return ok;
}

/* Read one checked byte from immutable checkpoint storage. */
static bool read_tag(ValueContext *context, unsigned char *tag)
{
    if (context->bytes->cursor>=context->bytes->size) return fail(context,"truncated explicit state");
    *tag=context->bytes->data[context->bytes->cursor++]; return true;
}

/* Push a length-delimited string only after its complete payload is known to exist. */
static bool push_string(lua_State *lua, ValueContext *context)
{
    uint64_t size=fl_binary_get_u64(context->bytes);
    if (context->bytes->failed || size>context->bytes->size-context->bytes->cursor) return fail(context,"truncated explicit state string");
    lua_pushlstring(lua,(const char *)context->bytes->data+context->bytes->cursor,(size_t)size);
    context->bytes->cursor+=(size_t)size; return true;
}

/* Reconstruct values into a fresh callback environment, never a serialized Lua heap. */
static bool push(lua_State *lua, ValueContext *context, unsigned depth)
{
    if (depth>FL_LUA_VALUE_DEPTH || ++context->nodes>FL_LUA_VALUE_NODES) return fail(context,"invalid explicit state structure");
    if (!lua_checkstack(lua,6)) return fail(context,"cannot grow explicit state stack");
    unsigned char tag; if (!read_tag(context,&tag)) return false;
    switch (tag) {
    case VALUE_NIL: lua_pushnil(lua); return true;
    case VALUE_FALSE: case VALUE_TRUE: lua_pushboolean(lua,tag==VALUE_TRUE); return true;
    case VALUE_INTEGER: case VALUE_NUMBER: {
        uint64_t bits=fl_binary_get_u64(context->bytes);
        if (context->bytes->failed) return fail(context,"truncated explicit state number");
        if (tag==VALUE_INTEGER) {lua_Integer value; memcpy(&value,&bits,8); lua_pushinteger(lua,value);}
        else {lua_Number value; memcpy(&value,&bits,8); if (!isfinite(value)) return fail(context,"nonfinite explicit state"); lua_pushnumber(lua,value);}
        return true;
    }
    case VALUE_STRING: return push_string(lua,context);
    case VALUE_ARRAY: case VALUE_OBJECT: {
        uint64_t count=fl_binary_get_u64(context->bytes);
        if (context->bytes->failed || count>FL_LUA_VALUE_NODES) return fail(context,"invalid explicit state table");
        lua_createtable(lua,tag==VALUE_ARRAY ? (int)count : 0,tag==VALUE_OBJECT ? (int)count : 0);
        int table=lua_gettop(lua);
        for (uint64_t i=0;i<count;++i) {
            if (tag==VALUE_OBJECT && !push_string(lua,context)) return false;
            if (!push(lua,context,depth+1)) return false;
            if (tag==VALUE_ARRAY) lua_rawseti(lua,table,(lua_Integer)i+1); else lua_rawset(lua,table);
        }
        return true;
    }
    default: return fail(context,"unknown explicit state value");
    }
}

bool fl_lua_value_push(lua_State *lua, const unsigned char *bytes, size_t size, char error[256])
{
    error[0]=0; int top=lua_gettop(lua);
    Fl_Binary stream={.data=(unsigned char *)bytes,.size=size}; ValueContext context={.bytes=&stream,.error=error};
    bool ok=size<=FL_LUA_VALUE_LIMIT && push(lua,&context,0) && stream.cursor==stream.size;
    if (!ok) {if (!error[0]) snprintf(error,256,"invalid explicit state size"); lua_settop(lua,top);}
    return ok;
}
