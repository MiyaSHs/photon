/* Photon CPU L3: our lua_shared.dll's CLuaShared. GMod's original DLL is kept beside us as lua_shared_gmod.dll and does everything
 * that is not Lua: the file cache (LoadFile, Mount*, FindScripts...), CLuaConVars, Init/Shutdown. Our CLuaShared forwards its 19
 * slots to the original's object, except CreateLuaInterface / CloseLuaInterface / GetLuaInterface, which make and track our
 * CLuaInterface. Those are also stored in the original's realm array (+0x78 + realm*8), so its own methods that walk the interfaces
 * (GetStackTraces, the cache) see ours. The original's own LuaJIT is never used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "iface.h"

#define ORIG_DLL "lua_shared_gmod.dll"
#define ORIG_CONVARS_RVA 0xba000   /* the original's static CLuaConVars (FUN_1800069b0 returns it) */

typedef struct Shared { void **vt; void *orig; } Shared;
static Shared g_shared;
static HMODULE g_orig;
static void *(*g_orig_create)(const char *, int *);

/* exported variables the original exports too; refreshed from the original's after its Init */
void *g_pFullFileSystem_export;
void *cvar_export;

static HMODULE self_module(void) {
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)self_module, &m);
    return m;
}
static int load_orig(void) {
    if (g_orig) return 1;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self_module(), path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    if (n + sizeof ORIG_DLL > MAX_PATH) return 0;
    memcpy(path + n, ORIG_DLL, sizeof ORIG_DLL);
    g_orig = LoadLibraryA(path);
    if (!g_orig) return 0;
    g_orig_create = (void *(*)(const char *, int *))(void *)GetProcAddress(g_orig, "CreateInterface");
    return g_orig_create != NULL;
}
static void **orig_var(const char *name) { return g_orig ? (void **)(void *)GetProcAddress(g_orig, name) : NULL; }
static void sync_exports(void) {
    void **fs = orig_var("?g_pFullFileSystem@@3PEAVIFileSystem@@EA"), **cv = orig_var("cvar");
    if (fs) g_pFullFileSystem_export = *fs;
    if (cv) cvar_export = *cv;
}

void *photon_orig_shared(void) { return g_shared.orig; }
void *photon_orig_convars(void) { return (uint8_t *)g_orig + ORIG_CONVARS_RVA; }
void **photon_filesystem(void) {
    void **fs = orig_var("?g_pFullFileSystem@@3PEAVIFileSystem@@EA");
    return fs ? (void **)*fs : NULL;
}

/* forwarding thunks: swap `this` for the original object and jump to the original's slot, every other argument untouched */
#define FWD(n) \
    __asm__(".text\n.p2align 4\nshared_fwd_" #n ":\n" \
            "  movq 8(%rcx), %rcx\n  movq (%rcx), %rax\n  jmpq *" #n "*8(%rax)\n"); \
    extern void shared_fwd_##n(void);
FWD(0) FWD(1) FWD(2) FWD(3) FWD(7) FWD(8) FWD(9) FWD(10) FWD(11) FWD(12) FWD(13) FWD(14) FWD(15) FWD(16) FWD(17) FWD(18)

static void **orig_slot(int n) { return (*(void ***)g_shared.orig)[n]; }

/* 1: Init(CreateInterfaceFn, bool, CSteamAPIContext*, IGet*): the original's, then our copies of its exported globals */
static void shared_Init(Shared *s, void *factory, int b, void *steam, void *get) {
    ((void (*)(void *, void *, int, void *, void *))orig_slot(1))(s->orig, factory, (uint8_t)b, steam, get);
    sync_exports();
}
/* 4: CreateLuaInterface(realm, renew): a zeroed object with the original constructor's fields (FUN_180008020) */
extern void *photon_iface_vtable[133];
static void *shared_CreateLuaInterface(Shared *s, unsigned realm, int renew) {
    (void)renew;
    sync_exports();
    LuaIface *li = (LuaIface *)calloc(1, sizeof *li);
    extern void *photon_iface_vtable_traced[133];
    li->vt = l3_tracing ? photon_iface_vtable_traced : photon_iface_vtable;   /* TRACE: records each call's slot (trace_vt.c) */
    li->errref = -1;
    *(void **)((uint8_t *)s->orig + 0x78 + (realm & 0xff) * 8) = li;
    return li;
}
/* 5: CloseLuaInterface: Shutdown, slot 131, clear the realm slots, and with neither client nor server left, the convars' slot 4 */
void li_Shutdown(LuaIface *self);
void li_FreeInternals(LuaIface *self);
static void shared_CloseLuaInterface(Shared *s, LuaIface *li) {
    li_Shutdown(li);
    li_FreeInternals(li);
    void **arr = (void **)((uint8_t *)s->orig + 0x78);
    for (int i = 0; i < 3; i++) if (arr[i] == li) arr[i] = NULL;
    if (!arr[0] && !arr[1]) { void *cv = photon_orig_convars(); VCALL(cv, 0x20, void); }
    free(li);
}
/* 6: GetLuaInterface(realm) */
static void *shared_GetLuaInterface(Shared *s, unsigned realm) { return *(void **)((uint8_t *)s->orig + 0x78 + (realm & 0xff) * 8); }

static void *g_shared_vt[19] = {
    (void *)shared_fwd_0, (void *)shared_Init, (void *)shared_fwd_2, (void *)shared_fwd_3, (void *)shared_CreateLuaInterface,
    (void *)shared_CloseLuaInterface, (void *)shared_GetLuaInterface, (void *)shared_fwd_7, (void *)shared_fwd_8, (void *)shared_fwd_9,
    (void *)shared_fwd_10, (void *)shared_fwd_11, (void *)shared_fwd_12, (void *)shared_fwd_13, (void *)shared_fwd_14,
    (void *)shared_fwd_15, (void *)shared_fwd_16, (void *)shared_fwd_17, (void *)shared_fwd_18,
};

/* LuaJIT patch 0009: print hands each piece (values, the tabs, the final newline) to L->luabase's slot 101, Msg("%.*s", len, str), as
 * GMod's lua_shared does (FUN_180015070); that reaches the game's console through ILuaGameCallback::Msg */
extern void (*lj_gmod_print_hook)(lua_State *L, const char *s, size_t n);
static void print_hook(lua_State *L, const char *s, size_t n) {
    void *lb = *(void **)((char *)L + 0x78);
    if (!lb) { fwrite(s, 1, n, stdout); return; }
    ((void (*)(void *, const char *, ...))(*(void ***)lb)[0x328 / 8])(lb, "%.*s", (int)n, s);
}

void *CreateInterface(const char *name, int *rc) {
    if (!load_orig()) { if (rc) *rc = 1; return NULL; }
    if (!g_shared.orig) {
        lj_gmod_print_hook = print_hook;
        extern void cprof_init(void);
        cprof_init();
        const char *t = getenv("PHOTON_L3_TRACE");
        l3_tracing = t && *t == '1';
        if (l3_tracing) {
            l3_trace("photon lua_shared: trace on");
            extern void l3_trace_install(void);
            l3_trace_install();
            freopen("photon_l3_stderr.log", "w", stderr);   /* LuaJIT's API-check failures (CHECK=1 builds) go to stderr */
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    if (!strcmp(name, "LUASHARED003")) {
        if (!g_shared.orig) {
            int r = 1;
            g_shared.orig = g_orig_create(name, &r);
            if (!g_shared.orig) { if (rc) *rc = 1; return NULL; }
            g_shared.vt = g_shared_vt;
        }
        if (rc) *rc = 0;
        return &g_shared;
    }
    return g_orig_create(name, rc);
}

/* GMod's link-check symbol and its renamed lua_resume */
void luaJIT_version_2_1_0_beta3(void) {}
int lua_resume_real(lua_State *L, int narg) { return lua_resume(L, narg); }
