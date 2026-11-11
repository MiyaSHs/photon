/* Photon CPU L3: the replacement lua_shared's CLuaInterface (analysis/15). One C object with an explicit 133-slot vtable in the order of
 * GMod's x86-64 lua_shared (modules/cpu/luajit/clua_slots.txt). The game DLLs (MSVC) call it through that vtable with the Win64
 * convention (this in rcx), so plain C functions taking the object first are ABI-identical to its C++ methods.
 * Layout: the original object is 0x9b0 bytes. Only +0x00 (vtable) and +0x08 (lua_State*) are read from outside: binary modules read
 * LUA->state. The other offsets below are the original's, kept so a reader of the decompile can follow along. */
#ifndef PHOTON_LUA_IFACE_H
#define PHOTON_LUA_IFACE_H
#include <stdint.h>
#include <stddef.h>
#include "lua.h"
#include "lauxlib.h"

typedef struct LuaIface {
    void **vt;                 /* +0x000 */
    lua_State *L;              /* +0x008 */
    int errref;                /* +0x010 registry ref of the pcall error handler (AdvancedLuaErrorReporter) */
    uint8_t pad0[0x38 - 0x14];
    void **threaded;           /* +0x038 ILuaThreadedCall*, in AddThreadedCall order (the original: an MSVC std::list) */
    size_t nthreaded;          /* +0x040 */
    void *ret[4];              /* +0x048 ILuaObject* results of CallInternal (GetReturn) */
    void *temp[32];            /* +0x068 NewTemporaryObject's ring of ILuaObject* */
    uint8_t realm;             /* +0x168 State::CLIENT 0 / SERVER 1 / MENU 2 (SetType) */
    uint8_t pad1[7];
    void *callback;            /* +0x170 ILuaGameCallback* */
    uint8_t pad2[0x198 - 0x178];
    int temp_i;                /* +0x198 the last temporary handed out */
    uint8_t pad3[4];
    void *global_obj;          /* +0x1a0 ILuaObject* for _G */
    void *hooknames_obj;       /* +0x1a8 ILuaObject* for the gamemode hook-name table */
    int next_type;             /* +0x1b0 the next metatable type id (starts at 44, after GMod's built-in types) */
    uint8_t pad4[4];
    void *meta_obj[0xff];      /* +0x1b8 ILuaObject* per metatable type; ends at 0x9b0, the original's size */
    /* ours, past the original's layout */
    char path_id[64];          /* SetPathID / GetPathID */
    char (*paths)[0x104];      /* PushPath / PopPath / GetPath (the original: a CUtlVector at +0x18) */
    int npaths, cappaths;
    size_t capthreaded;
    lua_State *mainL;          /* the state Init made (L follows SetState into coroutines) */
} LuaIface;
/* PHOTON_L3_TRACE=1: diagnostics to photon_l3.log in the game's working directory */
void l3_trace(const char *fmt, ...);
extern int l3_tracing;
void l3_check_entry(void *self, void *L, void *mainL, const char *where);

/* ILuaObject and ILuaGameCallback vtable byte offsets as GMod's lua_shared uses them (from the decompile) */
#define LOBJ_UNREFERENCE 0x010
#define LOBJ_GETINT 0x030
#define LOBJ_ISNIL 0x110
#define LOBJ_ISTABLE 0x118
#define LCB_CREATEOBJECT 0x00
#define LCB_DESTROYOBJECT 0x08
#define LCB_LUAERROR 0x28
#define LCB_INTERFACECREATED 0x30
/* ILuaThreadedCall: Init 0, IsDone 1, Done(ILuaInterface*) 2, OnShutdown 3 */
#define LTC_ISDONE 0x08
#define LTC_DONE 0x10
#define LTC_ONSHUTDOWN 0x18
#define VCALL(obj, off, ret, ...) ((ret (*)())(*(void ***)(obj))[(off) / 8])(obj, ##__VA_ARGS__)

/* GMod userdata: { data pointer, type }. Vector and Angle carry their 12 bytes inline at +0x10 (0x20 bytes). */
typedef struct GModUserData { void *data; uint8_t type; } GModUserData;

/* ILuaObject methods as GMod's lua_shared calls them (vtable byte offsets from the decompile; MSVC orders the overloads differently
 * from garrysmod_common's declaration, so these come from the binary, not the header). */
#define LOBJ_SETFROMSTACK 0x008
#define LOBJ_PUSH 0x108
static inline void lobj_setfromstack(void *obj, int i) { ((void (*)(void *, int))(*(void ***)obj)[LOBJ_SETFROMSTACK / 8])(obj, i); }
static inline void lobj_push(void *obj) { ((void (*)(void *))(*(void ***)obj)[LOBJ_PUSH / 8])(obj); }

LuaIface *photon_iface_new(uint8_t realm);
void photon_iface_free(LuaIface *self);
extern void *photon_iface_vtable[133];
#endif
