/* Photon CPU L3: CLuaInterface slots 55-91 (garrysmod_common ILuaInterface, first part), each from the decompile of GMod's x86-64
 * lua_shared (the function per slot is in modules/cpu/luajit/clua_slots.txt). ILuaObject and ILuaGameCallback are the game's; they are
 * called through their vtables at the byte offsets the original uses (iface.h). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "iface.h"
#include "luajit.h"

#define L_ (self->L)

/* tier0 (imported by name from tier0.dll, as the original does) */
extern void tier0_Error(const char *fmt, ...);
extern void tier0_Warning(const char *fmt, ...);
extern int tier0_ThreadInMainThread(void);

/* defined in the other parts */
int li_Top(LuaIface *self);
void li_Pop(LuaIface *self, int n);
void li_SetState(LuaIface *self, lua_State *L);
int li_ReferenceCreate(LuaIface *self);
void li_PushNumber(LuaIface *self, double v);
double li_GetNumber(LuaIface *self, int i);
int li_GetType(LuaIface *self, int i);
const char *li_GetTypeName(LuaIface *self, int t);
const char *li_GetString(LuaIface *self, int i, unsigned int *outlen);
int li_GetBool(LuaIface *self, int i);
void li_PushString(LuaIface *self, const char *s, unsigned int len);
void li_SetTable(LuaIface *self, int i);
void li_CreateTable(LuaIface *self);
void li_ThrowError(LuaIface *self, const char *s);
void li_CreateMetaTableType(LuaIface *self, const char *name, int type);
int li_CallFunctionProtected(LuaIface *self, int nargs, int nres, int show_error);
extern void *photon_lua_error_record;   /* the CLuaError the last failed call filled (iface_more.c) */
extern int photon_AdvancedLuaErrorReporter(lua_State *L);
extern void photon_gmod_openlibs(lua_State *L);
void li_RunThreadedCalls(LuaIface *self);

/* LuaJIT patch 0007: around each __gc (which runs on LuaJIT's VM thread, where GMod's C finalizers SetState to), keep the interface
 * on the state it was on */
extern void *(*lj_gmod_gc_hook)(void *luabase, void *restore, int after);
static void *gc_hook(void *luabase, void *restore, int after) {
    LuaIface *li = (LuaIface *)luabase;
    if (!after) return li->L;
    li->L = (lua_State *)restore;
    return NULL;
}

static int panic_fn(lua_State *L) {
    tier0_Error("Lua Panic! Something went horribly wrong!\n\n\"%s\"\n", lua_tolstring(L, -1, NULL));
    return 0;
}

static const char *const k_hooknames[] = {
#include "hooknames.inc"
};

/* CLuaInterface::DoStackCheck (FUN_1800094a0): at a call boundary the Lua stack must be empty; a leak is a fatal error */
void li_DoStackCheck(LuaIface *self) {
    int top = li_Top(self);
    if (!top) return;
    char buf[4096];
    size_t n = 0;
    buf[0] = 0;
    for (int i = 1; i <= top && n < sizeof buf - 128; i++) {
        const char *s = li_GetString(self, i, NULL);
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%d: %s (%s)\n", i, li_GetTypeName(self, li_GetType(self, i)), s ? s : "UNKNOWN");
    }
    tier0_Error("%s Lua Stack Leak [%i]!\n%s", self->realm == 1 ? "Server" : "Client", top, buf);
}

void *li_CreateObject(LuaIface *self);
void li_DestroyObject(LuaIface *self, void *obj);

int li_Init(LuaIface *self, void *callback, int unused) {                                       /* 55 (bool) */
    (void)unused;
    self->callback = callback;
    self->global_obj = li_CreateObject(self);
    memset(self->temp, 0, sizeof self->temp);
    memset(self->meta_obj, 0, sizeof self->meta_obj);
    self->next_type = 44;
    memset(self->ret, 0, sizeof self->ret);
    self->temp_i = 0;
    lua_State *L = luaL_newstate();
    if (!L) { tier0_Error("Unable to create new Lua State! Couldn't alloc memory?\n"); return 0; }
    *(void **)((char *)L + 0x78) = self;   /* L->luabase: GMod's lua_State field at +0x78 (LuaJIT patch 0003) */
    self->mainL = L;
    li_SetState(self, L);
    lua_atpanic(L, panic_fn);
    lj_gmod_gc_hook = gc_hook;
    lua_pushcclosure(L, photon_AdvancedLuaErrorReporter, 0);
    self->errref = li_ReferenceCreate(self);
    /* the FPU precision check: a value that survives only at double precision */
    li_PushNumber(self, 1437217655.0);
    if (li_GetNumber(self, -1) != 1437217655.0) {
        luaJIT_setmode(L, 0, LUAJIT_MODE_OFF);
        tier0_Warning("Lua detected bad FPU precision! Prepare for weirdness!\n");
    }
    li_Pop(self, 1);
    {   /* PHOTON_L3_NOJIT=1: the JIT compiler off (diagnostics) */
        const char *nj = getenv("PHOTON_L3_NOJIT");
        if (nj && *nj == '1') luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    }
    li_DoStackCheck(self);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lobj_setfromstack(self->global_obj, -1);
    li_Pop(self, 1);
    /* the gamemode hook-name table, keys 1..317 */
    lua_createtable(L, 0, 0);
    for (int i = 0; i < (int)(sizeof k_hooknames / sizeof k_hooknames[0]); i++) {
        lua_pushinteger(L, i + 1);
        lua_pushstring(L, k_hooknames[i]);
        lua_settable(L, -3);
    }
    self->hooknames_obj = li_CreateObject(self);
    lobj_setfromstack(self->hooknames_obj, -1);
    li_Pop(self, 1);
    if (self->callback) VCALL(self->callback, LCB_INTERFACECREATED, void, self);
    /* Global()->SetMember("VERSION", 1234.0f), then BRANCH = "unknown" (ILuaObject slots as the original calls them) */
    ((void (*)(void *, const char *, float))(*(void ***)self->global_obj)[0x90 / 8])(self->global_obj, "VERSION", 1234.0f);
    ((void (*)(void *, const char *, const char *))(*(void ***)self->global_obj)[0x80 / 8])(self->global_obj, "BRANCH", "unknown");
    photon_gmod_openlibs(L);
    { extern void cprof_register(lua_State *L); cprof_register(L); }   /* PHOTON_L3_CPROF (cprof.c) */
    li_DoStackCheck(self);
    lua_getfield(L, LUA_GLOBALSINDEX, "debug");
    lua_pushnil(L); lua_setfield(L, -2, "setlocal");
    lua_pushnil(L); lua_setfield(L, -2, "setupvalue");
    lua_pushnil(L); lua_setfield(L, -2, "upvalueid");
    lua_pushnil(L); lua_setfield(L, -2, "upvaluejoin");
    lua_settop(L, -2);
    return 1;
}
void li_Shutdown(LuaIface *self) {                                                               /* 56 */
    for (size_t i = 0; i < self->nthreaded; i++) VCALL(self->threaded[i], LTC_ONSHUTDOWN, void);
    free(self->threaded);
    self->threaded = NULL;
    self->nthreaded = self->capthreaded = 0;
    for (int i = 0; i < 32; i++) if (self->temp[i]) { li_DestroyObject(self, self->temp[i]); self->temp[i] = NULL; }
    li_DestroyObject(self, self->global_obj);
    self->global_obj = NULL;
    lua_close(self->L);
    self->L = NULL;
}
void li_Cycle(LuaIface *self) {                                                                  /* 57 */
    memset(self->ret, 0, sizeof self->ret);
    li_DoStackCheck(self);
    li_RunThreadedCalls(self);
}
void *li_Global(LuaIface *self) { return self->global_obj; }                                    /* 58 */
void *li_NewTemporaryObject(LuaIface *self);
void *li_GetObject(LuaIface *self, int i) {                                                      /* 59 */
    void *o = li_NewTemporaryObject(self);
    lobj_setfromstack(o, i);
    return o;
}
void li_PushLuaObject(LuaIface *self, void *obj) {                                               /* 60 */
    if (!obj) { lua_pushnil(L_); return; }
    lobj_push(obj);
}
void li_PushLuaFunction(LuaIface *self, lua_CFunction f) {                                       /* 61 */
    extern int l3_cprof; extern void cprof_pushcclosure(lua_State *L, lua_CFunction f, int n);
    if (l3_cprof) cprof_pushcclosure(L_, f, 0); else lua_pushcclosure(L_, f, 0);
}
void li_Error(LuaIface *self, const char *err);
void li_LuaError(LuaIface *self, const char *err, int index) {                                   /* 62 */
    if (index != -1) { luaL_argerror(L_, index, err); return; }
    li_Error(self, err);   /* the original tail-calls slot 100 with "%s" and err */
}
void li_TypeError(LuaIface *self, const char *name, int index) { luaL_typerror(L_, index, name); }   /* 63 */
static int call_failed(LuaIface *self) {
    VCALL(self->callback, LCB_LUAERROR, void, photon_lua_error_record);
    return 0;
}
int li_CallInternal(LuaIface *self, int nargs, int nres) {                                       /* 64 (bool) */
    l3_check_entry(self, self->L, self->mainL, "CallInternal");
    if (li_GetType(self, -nargs - 1) != LUA_TFUNCTION) tier0_Error("Lua tried to call non functions");
    if (!tier0_ThreadInMainThread()) tier0_Error("Calling Lua function in a thread other than main!\n");
    if (nres > 4) tier0_Error("[CLuaInterface::Call] Expecting more returns than possible");
    memset(self->ret, 0, sizeof self->ret);
    for (int i = 0; i < nres; i++) self->ret[i] = li_NewTemporaryObject(self);
    if (!li_CallFunctionProtected(self, nargs, nres, 0)) return call_failed(self);
    for (int i = 0; i < nres; i++) {
        if (!self->ret[i]) self->ret[i] = li_NewTemporaryObject(self);
        lobj_setfromstack(self->ret[i], -i - 1);
    }
    if (nres > 0) li_Pop(self, nres);
    return 1;
}
int li_CallInternalNoReturns(LuaIface *self, int nargs) {                                        /* 65 (bool) */
    if (!li_CallFunctionProtected(self, nargs, 0, 0)) return call_failed(self);
    return 1;
}
int li_CallInternalGetBool(LuaIface *self, int nargs) {                                          /* 66 (bool) */
    if (!li_CallFunctionProtected(self, nargs, 1, 0)) return call_failed(self);
    if (li_GetType(self, -1) != LUA_TBOOLEAN) { li_Pop(self, 1); return 0; }
    int b = li_GetBool(self, -1);
    li_Pop(self, 1);
    return b;
}
const char *li_CallInternalGetString(LuaIface *self, int nargs) {                                /* 67 */
    if (!li_CallFunctionProtected(self, nargs, 1, 0)) { call_failed(self); return NULL; }
    if (li_GetType(self, -1) != LUA_TSTRING) { li_Pop(self, 1); return NULL; }
    const char *s = li_GetString(self, -1, NULL);   /* as the original: the string may be collected once popped */
    li_Pop(self, 1);
    return s;
}
int li_CallInternalGet(LuaIface *self, int nargs, void *obj) {                                   /* 68 (bool) */
    if (!li_CallFunctionProtected(self, nargs, 1, 0)) return call_failed(self);
    lobj_setfromstack(obj, -1);
    li_Pop(self, 1);
    return 1;
}
void li_NewGlobalTable(LuaIface *self, const char *name) {                                       /* 69 */
    lua_createtable(L_, 0, 0);
    lua_setfield(L_, LUA_GLOBALSINDEX, name);
}
void *li_NewTemporaryObject(LuaIface *self) {                                                    /* 70 */
    self->temp_i++;
    if (self->temp_i > 31) self->temp_i = 0;
    if (!self->temp[self->temp_i]) self->temp[self->temp_i] = li_CreateObject(self);
    else VCALL(self->temp[self->temp_i], LOBJ_UNREFERENCE, void);
    return self->temp[self->temp_i];
}
int li_isUserData(LuaIface *self, int i) { return lua_type(L_, i) == LUA_TUSERDATA; }            /* 71 (bool) */
void *li_GetMetaTableObject_idx(LuaIface *self, int i) {                                         /* 72: GetMetaTableObject(int index) */
    if (!lua_getmetatable(L_, i)) return NULL;
    void *o = li_NewTemporaryObject(self);
    lobj_setfromstack(o, -1);
    li_Pop(self, 1);
    return o;
}
void *li_GetMetaTableObject_name(LuaIface *self, const char *name, int type) {                   /* 73: GetMetaTableObject(name, type) */
    lua_getfield(L_, LUA_REGISTRYINDEX, name);
    if (li_GetType(self, -1) != LUA_TTABLE) {
        li_Pop(self, 1);
        if (type == -1) return NULL;
        li_CreateMetaTableType(self, name, type);
    }
    void *o = li_NewTemporaryObject(self);
    lobj_setfromstack(o, -1);
    li_Pop(self, 1);
    return o;
}
void *li_GetReturn(LuaIface *self, int i) {                                                      /* 74 */
    if ((unsigned)i > 3) tier0_Error("Tried to get return higher than max");
    if (!self->ret[i]) tier0_Error("Error: Calling GetReturn on an invalid return! Check code!!\n");
    return self->ret[i];
}
int li_IsServer(LuaIface *self) { return self->realm == 1; }                                     /* 75 */
int li_IsClient(LuaIface *self) { return self->realm == 0; }                                     /* 76 */
int li_IsMenu(LuaIface *self) { return self->realm == 2; }                                       /* 77 */
void li_DestroyObject(LuaIface *self, void *obj) { VCALL(self->callback, LCB_DESTROYOBJECT, void, obj); }   /* 78 */
void *li_CreateObject(LuaIface *self) { return VCALL(self->callback, LCB_CREATEOBJECT, void *); }          /* 79 */
/* 80-85: the SetMember overload group, slot order as MSVC laid it out (assigned by behaviour) */
static int lobj_istable(void *o) { return (int)(uint8_t)VCALL(o, LOBJ_ISTABLE, uint64_t); }
void li_SetMember_str_obj(LuaIface *self, void *table, const char *key, void *value) {           /* 80 */
    if (!lobj_istable(table)) return;
    lobj_push(table);
    li_PushString(self, key, 0);
    if (value) lobj_push(value); else lua_pushnil(L_);
    li_SetTable(self, -3);
    li_Pop(self, 1);
}
void li_SetMember_str(LuaIface *self, void *table, const char *key) {                            /* 81: value from the stack top */
    if (!lobj_istable(table)) return;
    lobj_push(table);
    li_PushString(self, key, 0);
    lua_pushvalue(L_, -3);
    li_SetTable(self, -3);
    li_Pop(self, 2);
}
void li_SetMember_num_obj(LuaIface *self, void *table, float key, void *value) {                 /* 82 */
    lobj_push(table);
    li_PushNumber(self, (double)key);
    if (value) lobj_push(value); else lua_pushnil(L_);
    li_SetTable(self, -3);
    li_Pop(self, 1);
}
void li_SetMember_num(LuaIface *self, void *table, float key) {                                  /* 83: value from the stack top */
    lobj_push(table);
    li_PushNumber(self, (double)key);
    lua_pushvalue(L_, -3);
    li_SetTable(self, -3);
    li_Pop(self, 2);
}
void li_SetMember_obj_obj(LuaIface *self, void *table, void *key, void *value) {                 /* 84 */
    if (!lobj_istable(table)) return;
    lobj_push(table);
    lobj_push(key);
    if (value) lobj_push(value); else lua_pushnil(L_);
    li_SetTable(self, -3);
    li_Pop(self, 1);
}
void *li_GetNewTable(LuaIface *self) {                                                           /* 85 */
    li_CreateTable(self);
    void *o = li_GetObject(self, -1);
    li_Pop(self, 1);
    return o;
}
void li_SetType(LuaIface *self, unsigned char realm) { self->realm = realm; }                    /* 86 */
void li_PushLong(LuaIface *self, long n) { lua_pushnumber(L_, (double)n); }                      /* 87 */
int li_GetFlags(LuaIface *self, int i) {                                                         /* 88 */
    /* the original walks the value's ILuaObject: a table ORs every value's GetInt, anything else is its own GetInt. Done here directly
     * on the Lua value (ILuaObject::GetInt is (int) of the number). */
    if (lua_type(L_, i) == LUA_TTABLE) {
        int flags = 0, t = i < 0 ? lua_gettop(L_) + i + 1 : i;
        lua_pushnil(L_);
        while (lua_next(L_, t)) { flags |= (int)lua_tonumber(L_, -1); lua_settop(L_, -2); }
        return flags;
    }
    return (int)lua_tonumber(L_, i);
}
int li_FindOnObjectsMetaTable(LuaIface *self, int obj, int key) {                                /* 89 (bool) */
    if (!lua_getmetatable(L_, obj)) return 0;
    lua_pushvalue(L_, key);
    lua_gettable(L_, -2);
    if (lua_type(L_, -1) != LUA_TNIL) return 1;   /* as the original: leaves the metatable and the value on the stack */
    li_Pop(self, 1);
    return 0;
}
