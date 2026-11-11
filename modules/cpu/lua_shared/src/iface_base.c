/* Photon CPU L3: CLuaInterface slots 0-54 (garrysmod_common ILuaBase), each from GMod's x86-64 lua_shared (decompile of the function in
 * the slot named in clua_slots.txt). Every method works on self->L, which Call/PCall restore afterwards (a coroutine may have switched
 * it), exactly as the original does. */
#include <string.h>
#include "iface.h"

#define L_ (self->L)

static const char *const k_typenames[44] = {
    "nil", "bool", "lightuserdata", "number", "string", "table", "function", "userdata", "thread", "entity", "vector", "angle",
    "physobj", "save", "restore", "damageinfo", "effectdata", "movedata", "recipientfilter", "usercmd", "vehicle", "material", "panel",
    "particle", "particleemitter", "texture", "usermsg", "convar", "mesh", "matrix", "sound", "pixelvishandle", "dlight", "video",
    "file", "locomotion", "path", "navarea", "soundhandle", "navladder", "particlesystem", "projectedtexture", "physcollide",
    "surfaceinfo" };

int li_Top(LuaIface *self) { return lua_gettop(L_); }                                            /* 0 */
void li_Push(LuaIface *self, int i) { lua_pushvalue(L_, i); }                                    /* 1 */
void li_Pop(LuaIface *self, int n) {                                                             /* 2 */
    if (lua_gettop(L_) < n) { extern void tier0_Error(const char *, ...); tier0_Error("Lua stack over-popped!\n"); }
    lua_settop(L_, -n - 1);
}
void li_GetTable(LuaIface *self, int i) { lua_gettable(L_, i); }                                 /* 3 */
void li_GetField(LuaIface *self, int i, const char *k) { lua_getfield(L_, i, k); }               /* 4 */
void li_SetField(LuaIface *self, int i, const char *k) { lua_setfield(L_, i, k); }               /* 5 */
void li_CreateTable(LuaIface *self) { lua_createtable(L_, 0, 0); }                               /* 6 */
void li_SetTable(LuaIface *self, int i) { lua_settable(L_, i); }                                 /* 7 */
void li_SetMetaTable(LuaIface *self, int i) { lua_setmetatable(L_, i); }                         /* 8 */
int li_GetMetaTable(LuaIface *self, int i) { return lua_getmetatable(L_, i) != 0; }              /* 9 (bool) */
void li_SetState(LuaIface *self, lua_State *L);
void li_Call(LuaIface *self, int nargs, int nres) {                                              /* 10 */
    lua_State *L = L_;
    lua_call(L, nargs, nres);
    li_SetState(self, L);
}
int li_PCall(LuaIface *self, int nargs, int nres, int errfunc) {                                 /* 11 */
    lua_State *L = L_;
    int r = lua_pcall(L, nargs, nres, errfunc);
    li_SetState(self, L);
    return r;
}
int li_Equal(LuaIface *self, int a, int b) { return lua_equal(L_, a, b); }                       /* 12 */
int li_RawEqual(LuaIface *self, int a, int b) { return lua_rawequal(L_, a, b); }                 /* 13 */
void li_Insert(LuaIface *self, int i) { lua_insert(L_, i); }                                     /* 14 */
void li_Remove(LuaIface *self, int i) { lua_remove(L_, i); }                                     /* 15 */
int li_Next(LuaIface *self, int i) { return lua_next(L_, i); }                                   /* 16 */
void *li_NewUserdata(LuaIface *self, unsigned int size) { return lua_newuserdata(L_, size); }    /* 17 */
void li_ThrowError(LuaIface *self, const char *s) { luaL_error(L_, "%s", s); }                   /* 18 */
int li_GetType(LuaIface *self, int i);
const char *li_GetTypeName(LuaIface *self, int t);
void li_TypeError(LuaIface *self, const char *name, int i);
void li_CheckType(LuaIface *self, int i, int t) {                                                /* 19 */
    if (li_GetType(self, i) != t) li_TypeError(self, li_GetTypeName(self, t), i);
}
void li_ArgError(LuaIface *self, int arg, const char *msg) { luaL_argerror(L_, arg, msg); }      /* 20 */
void li_RawGet(LuaIface *self, int i) { lua_rawget(L_, i); }                                     /* 21 */
void li_RawSet(LuaIface *self, int i) { lua_rawset(L_, i); }                                     /* 22 */
const char *li_GetString(LuaIface *self, int i, unsigned int *outlen) {                         /* 23 */
    size_t len = 0;
    const char *s = lua_tolstring(L_, i, &len);
    if (outlen) *outlen = (unsigned int)len;
    return s;
}
double li_GetNumber(LuaIface *self, int i) { return lua_tonumber(L_, i); }                       /* 24 */
int li_GetBool(LuaIface *self, int i) { return lua_toboolean(L_, i) != 0; }                      /* 25 */
lua_CFunction li_GetCFunction(LuaIface *self, int i) { return lua_tocfunction(L_, i); }          /* 26 */
void *li_GetUserdata(LuaIface *self, int i) { return lua_touserdata(L_, i); }                    /* 27 */
void li_PushNil(LuaIface *self) { lua_pushnil(L_); }                                             /* 28 */
void li_PushString(LuaIface *self, const char *s, unsigned int len) {                           /* 29 */
    if (!s) s = "";
    if (len) lua_pushlstring(L_, s, len); else lua_pushstring(L_, s);
}
void li_PushNumber(LuaIface *self, double v) { lua_pushnumber(L_, v); }                          /* 30 */
void li_PushBool(LuaIface *self, int b) { lua_pushboolean(L_, (uint8_t)b); }                     /* 31 */
extern int l3_cprof;
extern void cprof_pushcclosure(lua_State *L, lua_CFunction f, int n);
void li_PushCFunction(LuaIface *self, lua_CFunction f) {                                          /* 32 */
    if (l3_cprof) cprof_pushcclosure(L_, f, 0); else lua_pushcclosure(L_, f, 0);
}
void li_PushCClosure(LuaIface *self, lua_CFunction f, int n) {                                   /* 33 */
    if (l3_cprof) cprof_pushcclosure(L_, f, n); else lua_pushcclosure(L_, f, n);
}
void li_PushUserdata(LuaIface *self, void *p) { lua_pushlightuserdata(L_, p); }                  /* 34 */
int li_ReferenceCreate(LuaIface *self) { return luaL_ref(L_, LUA_REGISTRYINDEX); }               /* 35 */
void li_ReferenceFree(LuaIface *self, int r) { luaL_unref(L_, LUA_REGISTRYINDEX, r); }           /* 36 */
void li_ReferencePush(LuaIface *self, int r) { lua_rawgeti(L_, LUA_REGISTRYINDEX, r); }          /* 37 */
void li_PushSpecial(LuaIface *self, int which) {                                                 /* 38 */
    switch (which) {
    case 0: lua_pushvalue(L_, LUA_GLOBALSINDEX); return;      /* SPECIAL_GLOB */
    case 1: lua_pushvalue(L_, LUA_ENVIRONINDEX); return;      /* SPECIAL_ENV */
    case 2: lua_pushvalue(L_, LUA_REGISTRYINDEX); return;     /* SPECIAL_REG */
    default: lua_pushnil(L_); return;                         /* the original tail-calls PushNil (slot 28) */
    }
}
int li_IsType(LuaIface *self, int i, int t) {                                                    /* 39 (bool) */
    int lt = lua_type(L_, i);
    if (lt == t) return 1;
    if (lt == LUA_TUSERDATA && t > LUA_TUSERDATA) {
        GModUserData *u = (GModUserData *)lua_touserdata(L_, i);
        return u ? u->type == (uint8_t)t : 0;
    }
    return 0;
}
int li_GetType(LuaIface *self, int i) {                                                          /* 40 */
    int lt = lua_type(L_, i);
    if (lt >= 1 && lt <= 8 && lt != LUA_TUSERDATA) return lt;
    if (lt == LUA_TUSERDATA) {
        GModUserData *u = (GModUserData *)lua_touserdata(L_, i);
        if (!u || u->type < 9) return LUA_TUSERDATA;
        return u->type;
    }
    return 0;   /* nil and none */
}
const char *li_GetTypeName(LuaIface *self, int t) {                                              /* 41 */
    (void)self;
    if (t < 0) return "none";
    if (t > 43) return "unknown";
    return k_typenames[t];
}
void *li_CreateObject(LuaIface *self);
void li_CreateMetaTableType(LuaIface *self, const char *name, int type) {                        /* 42 */
    if (luaL_newmetatable_type(L_, name, type) && (unsigned)type < 0xff) {
        if (!self->meta_obj[type]) self->meta_obj[type] = li_CreateObject(self);
        lobj_setfromstack(self->meta_obj[type], -1);
    }
}
const char *li_CheckString(LuaIface *self, int i) { return luaL_checklstring(L_, i, NULL); }     /* 43 */
double li_CheckNumber(LuaIface *self, int i) { return luaL_checknumber(L_, i); }                 /* 44 */
int li_ObjLen(LuaIface *self, int i) { return (int)lua_objlen(L_, i); }                          /* 45 */
static float g_zero_angle[3], g_zero_vector[3];
const float *li_GetAngle(LuaIface *self, int i) {                                                /* 46 */
    GModUserData *u = (GModUserData *)lua_touserdata(L_, i);
    if (!u || !u->data || u->type != 11) { memset(g_zero_angle, 0, sizeof g_zero_angle); return g_zero_angle; }
    return (const float *)u->data;
}
const float *li_GetVector(LuaIface *self, int i) {                                               /* 47 */
    GModUserData *u = (GModUserData *)lua_touserdata(L_, i);
    if (!u || !u->data || u->type != 10) { memset(g_zero_vector, 0, sizeof g_zero_vector); return g_zero_vector; }
    return (const float *)u->data;
}
int li_PushMetaTable(LuaIface *self, int type);
static void push_inline3(LuaIface *self, const float *v, uint8_t type) {
    uint8_t *u = (uint8_t *)lua_newuserdata(L_, 0x20);
    memcpy(u + 0x10, v, 12);
    ((GModUserData *)u)->data = u + 0x10;
    ((GModUserData *)u)->type = type;
    if (li_PushMetaTable(self, type)) lua_setmetatable(L_, -2);
}
void li_PushAngle(LuaIface *self, const float *a) { push_inline3(self, a, 11); }                 /* 48 */
void li_PushVector(LuaIface *self, const float *v) { push_inline3(self, v, 10); }                /* 49 */
void li_SetState(LuaIface *self, lua_State *L) {                                                 /* 50 */
    if (l3_tracing && L && *(void **)((char *)L + 0x78) != self)
        l3_trace("SetState: L %p has luabase %p, not this interface %p (main L %p)", (void *)L, *(void **)((char *)L + 0x78), (void *)self, (void *)self->mainL);
    self->L = L;
    { extern void *l3_last_L; l3_last_L = L; }
}
int li_CreateMetaTable(LuaIface *self, const char *name) {                                       /* 51 */
    int type = self->next_type;
    if (luaL_newmetatable_type(L_, name, type)) {
        if (!self->meta_obj[type]) self->meta_obj[type] = li_CreateObject(self);
        lobj_setfromstack(self->meta_obj[type], -1);
        self->next_type++;
        return type;
    }
    lua_getfield(L_, -1, "MetaID");
    double id = lua_tonumber(L_, -1);
    lua_settop(L_, -2);
    return (int)id;
}
int li_PushMetaTable(LuaIface *self, int type) {                                                 /* 52 (bool) */
    if ((unsigned)type < 0xff && self->meta_obj[type]) { lobj_push(self->meta_obj[type]); return 1; }
    return 0;
}
void li_PushUserType(LuaIface *self, void *data, int type) {                                     /* 53 */
    GModUserData *u = (GModUserData *)lua_newuserdata(L_, sizeof *u);
    u->data = data;
    u->type = (uint8_t)type;
    if (li_PushMetaTable(self, type)) lua_setmetatable(L_, -2);
}
void li_SetUserType(LuaIface *self, int i, void *data) {                                         /* 54 */
    GModUserData *u = (GModUserData *)lua_touserdata(L_, i);
    if (u) u->data = data;
}
