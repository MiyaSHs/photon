/* Photon CPU CF0: a per-C-function profiler in our lua_shared (PHOTON_L3_CPROF=1). Every C function the game registers through
 * PushCFunction / PushCClosure / PushLuaFunction is pushed as a trampoline closure instead: the game's own upvalues 1..n stay where
 * they were and one more (this function's entry) is added after them, and the trampoline calls the original C function directly, in
 * its own frame, so lua_upvalueindex(1..n) still reads the game's upvalues. Each call's time is exclusive of nested trampolined calls.
 * A Lua error longjmps out of a trampoline without returning; frames left that way are recognised by their C stack address (deeper than
 * the current one) and dropped at the next entry. Names come from walking _G and the registry's metatables when a report is made.
 * PHOTON_L3_CPROF_CALLERS=name,name,... (e.g. ents.FindByClass,Entity:GetOwner): for those functions (named at the reset), each calling
 * Lua line too. Lua: photon_cprof_reset(), photon_cprof_report([n]) (to photon_l3.log and the console). Profiling only: lua_tocfunction and
 * jit.util.funcinfo see the trampoline. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>
#include "iface.h"

typedef struct CEntry {
    lua_CFunction f;
    struct CEntry *next;   /* hash chain */
    uint64_t calls, ticks;
    char name[96];
    struct CCaller *callers;   /* PHOTON_L3_CPROF_CALLERS: the calling Lua lines, when this function is one of the names */
    int want_callers;
} CEntry;
typedef struct CCaller { struct CCaller *next; char where[120]; uint64_t calls, ticks; } CCaller;
static const char *g_callers_env;

int l3_cprof;
static CEntry *g_hash[4096];
static CEntry **g_all;
static size_t g_nall, g_capall;
static double g_tick_ns;

/* the stack of running trampolines: start tick, nested ticks, and the C stack address of the frame */
typedef struct { uint64_t t0, child; void *sp; CEntry *e; } CFrame;
static CFrame g_fs[256];
static int g_depth;

static CEntry *entry_for(lua_CFunction f) {
    size_t h = ((uintptr_t)f >> 4) & 4095;
    for (CEntry *e = g_hash[h]; e; e = e->next) if (e->f == f) return e;
    CEntry *e = (CEntry *)calloc(1, sizeof *e);
    e->f = f;
    e->next = g_hash[h];
    g_hash[h] = e;
    if (g_nall == g_capall) { g_capall = g_capall ? g_capall * 2 : 1024; g_all = (CEntry **)realloc(g_all, g_capall * sizeof *g_all); }
    g_all[g_nall++] = e;
    return e;
}

static void note_caller(lua_State *L, CEntry *e, uint64_t t) {
    lua_Debug ar;
    char w[120] = "?";
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) snprintf(w, sizeof w, "%s:%d", ar.short_src, ar.currentline);
    CCaller *c = e->callers;
    for (; c; c = c->next) if (!strcmp(c->where, w)) break;
    if (!c) { c = (CCaller *)calloc(1, sizeof *c); snprintf(c->where, sizeof c->where, "%s", w); c->next = e->callers; e->callers = c; }
    c->calls++; c->ticks += t;
}

/* one trampoline per upvalue count n (0..8): the entry is always upvalue n+1, the game's own upvalues stay 1..n */
#define TRAMP(N) \
    static int tramp_##N(lua_State *L) { \
        CEntry *e = (CEntry *)lua_touserdata(L, lua_upvalueindex(N + 1)); \
        volatile char here; \
        while (g_depth > 0 && (char *)g_fs[g_depth - 1].sp < (char *)&here) g_depth--;   /* frames a longjmp left behind */ \
        int d = g_depth < 256 ? g_depth++ : -1; \
        uint64_t t0 = __rdtsc(); \
        if (d >= 0) { g_fs[d].t0 = t0; g_fs[d].child = 0; g_fs[d].sp = (void *)&here; g_fs[d].e = e; } \
        int r = e->f(L); \
        uint64_t t = __rdtsc() - t0; \
        if (d >= 0) { \
            g_depth = d; \
            e->ticks += t - g_fs[d].child; \
            if (d > 0) g_fs[d - 1].child += t; \
        } \
        e->calls++; \
        if (e->want_callers) note_caller(L, e, t); \
        return r; \
    }
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3) TRAMP(4) TRAMP(5) TRAMP(6) TRAMP(7) TRAMP(8)
static const lua_CFunction k_tramps[9] = { tramp_0, tramp_1, tramp_2, tramp_3, tramp_4, tramp_5, tramp_6, tramp_7, tramp_8 };

/* push f with its n upvalues (already on the stack) as a trampoline closure, or as itself when it cannot be wrapped */
void cprof_pushcclosure(lua_State *L, lua_CFunction f, int n) {
    if (!f || n < 0 || n > 8) { lua_pushcclosure(L, f, n); return; }
    CEntry *e = entry_for(f);
    lua_pushlightuserdata(L, e);
    lua_pushcclosure(L, k_tramps[n], n + 1);
}

static int is_tramp(lua_CFunction f) { for (int i = 0; i < 9; i++) if (k_tramps[i] == f) return i + 1; return 0; }

/* name the entry behind the C function at the top of the stack, if it is a trampoline without a name yet */
static void name_top(lua_State *L, const char *prefix, const char *a, const char *sep, const char *b) {
    lua_CFunction cf = lua_tocfunction(L, -1);
    int k = cf ? is_tramp(cf) : 0;
    if (!k) return;
    const char *nm = lua_getupvalue(L, -1, k);   /* upvalue n+1 = the entry */
    if (!nm) return;
    CEntry *e = (CEntry *)lua_touserdata(L, -1);
    lua_settop(L, -2);
    if (e && !e->name[0]) snprintf(e->name, sizeof e->name, "%s%s%s%s", prefix, a, sep, b);
}
static void name_table(lua_State *L, int t, const char *prefix, const char *tname, const char *sep) {
    lua_pushnil(L);
    while (lua_next(L, t)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TFUNCTION)
            name_top(L, prefix, tname, sep, lua_tolstring(L, -2, NULL));
        lua_settop(L, -2);
    }
}
static void name_all(lua_State *L, const char *prefix) {
    int top = lua_gettop(L);
    /* globals: functions, and one level into library tables */
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    int g = lua_gettop(L);
    name_table(L, g, prefix, "", "");
    lua_pushnil(L);
    while (lua_next(L, g)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TTABLE) {
            const char *lib = lua_tolstring(L, -2, NULL);
            name_table(L, lua_gettop(L), prefix, lib, ".");
        }
        lua_settop(L, -2);
    }
    /* metatables in the registry (Entity, Player, Vector, ...): their methods, and their __index tables */
    lua_pushnil(L);
    while (lua_next(L, LUA_REGISTRYINDEX)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TTABLE) {
            const char *mt = lua_tolstring(L, -2, NULL);
            int m = lua_gettop(L);
            name_table(L, m, prefix, mt, ":");
            lua_getfield(L, m, "__index");
            if (lua_type(L, -1) == LUA_TTABLE) name_table(L, lua_gettop(L), prefix, mt, ":");
            lua_settop(L, m);
        }
        lua_settop(L, -2);
    }
    lua_settop(L, top);
}

static int cmp_ticks(const void *a, const void *b) {
    uint64_t x = (*(CEntry *const *)a)->ticks, y = (*(CEntry *const *)b)->ticks;
    return x < y ? 1 : x > y ? -1 : 0;
}
static uint64_t g_reset_tick;
static void name_all(lua_State *L, const char *prefix);
static const char *realm_prefix(lua_State *L) {
    LuaIface *li = *(LuaIface **)((char *)L + 0x78);
    return li && li->realm == 1 ? "sv " : li && li->realm == 2 ? "menu " : "cl ";
}
/* an unnamed C function as the report prints it, "client.dll+0x12bd00" (also matched by PHOTON_L3_CPROF_CALLERS) */
static void addr_name(lua_CFunction f, char *nm, size_t n) {
    HMODULE m = NULL; char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)f, &m)) {
        GetModuleFileNameA(m, mod, sizeof mod);
        char *b = strrchr(mod, '\\'); if (b) memmove(mod, b + 1, strlen(b));
    }
    snprintf(nm, n, "%s+%#llx", mod, (unsigned long long)((char *)(void *)f - (char *)m));
}
static int l_reset(lua_State *L) {
    if (g_callers_env) name_all(L, realm_prefix(L));
    for (size_t i = 0; i < g_nall; i++) {
        CEntry *e = g_all[i];
        e->calls = e->ticks = 0;
        for (CCaller *c = e->callers, *n; c; c = n) { n = c->next; free(c); }
        e->callers = NULL;
        if (g_callers_env) {   /* "cl ents.FindByClass" matches "ents.FindByClass" in the list; unnamed ones by "client.dll+0x..." */
            char an[96];
            const char *bare;
            if (e->name[0]) { bare = strchr(e->name, ' '); bare = bare ? bare + 1 : e->name; }
            else { addr_name(e->f, an, sizeof an); bare = an; }
            size_t n = strlen(bare);
            for (const char *p = g_callers_env; (p = strstr(p, bare)); p += n)
                if ((p == g_callers_env || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) { e->want_callers = 1; break; }
        }
    }
    g_reset_tick = __rdtsc();
    return 0;
}
static int l_report(lua_State *L) {
    int n = (int)luaL_optnumber(L, 1, 40);
    LuaIface *li = *(LuaIface **)((char *)L + 0x78);
    name_all(L, realm_prefix(L));
    CEntry **v = (CEntry **)malloc(g_nall * sizeof *v);
    memcpy(v, g_all, g_nall * sizeof *v);
    qsort(v, g_nall, sizeof *v, cmp_ticks);
    double wall_ms = (double)(__rdtsc() - g_reset_tick) * g_tick_ns / 1e6, total = 0;
    for (size_t i = 0; i < g_nall; i++) total += (double)v[i]->ticks * g_tick_ns / 1e6;
    char line[256];
    snprintf(line, sizeof line, "PHOTON_CPROF window %.0f ms, C functions %.1f ms (%.1f%%), %zu functions seen\n", wall_ms, total,
             wall_ms > 0 ? 100.0 * total / wall_ms : 0.0, g_nall);
    l3_trace("%s", line);
    if (li) ((void (*)(void *, const char *, ...))li->vt[101])(li, "%s", line);
    for (int i = 0; i < n && (size_t)i < g_nall && v[i]->calls; i++) {
        double ms = (double)v[i]->ticks * g_tick_ns / 1e6;
        char nm[96];
        if (v[i]->name[0]) snprintf(nm, sizeof nm, "%s", v[i]->name);
        else addr_name(v[i]->f, nm, sizeof nm);
        snprintf(line, sizeof line, "PHOTON_CPROF %8.2f ms %5.1f%% %9llu calls %7.0f ns/call  %s\n", ms, total > 0 ? 100.0 * ms / total : 0.0,
                 (unsigned long long)v[i]->calls, v[i]->calls ? ms * 1e6 / (double)v[i]->calls : 0.0, nm);
        l3_trace("%s", line);
        if (li) ((void (*)(void *, const char *, ...))li->vt[101])(li, "%s", line);
        for (CCaller *c = v[i]->callers; c; c = c->next) {
            if (c->calls * 100 < v[i]->calls) continue;   /* callers with 1% of the calls or more */
            snprintf(line, sizeof line, "PHOTON_CPROF      from %8.2f ms %9llu calls  %s\n", (double)c->ticks * g_tick_ns / 1e6,
                     (unsigned long long)c->calls, c->where);
            l3_trace("%s", line);
            if (li) ((void (*)(void *, const char *, ...))li->vt[101])(li, "%s", line);
        }
    }
    free(v);
    return 0;
}

/* at CreateInterface: the mode, and the tick length */
void cprof_init(void) {
    const char *c = getenv("PHOTON_L3_CPROF");
    l3_cprof = c && *c == '1';
    if (!l3_cprof) return;
    g_callers_env = getenv("PHOTON_L3_CPROF_CALLERS");
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    uint64_t t0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&b);
    uint64_t t1 = __rdtsc();
    g_tick_ns = ((double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart) / (double)(t1 - t0);
    g_reset_tick = __rdtsc();
}
/* in each state's Init, after the libraries */
void cprof_register(lua_State *L) {
    if (!l3_cprof) return;
    lua_pushcclosure(L, l_reset, 0); lua_setfield(L, LUA_GLOBALSINDEX, "photon_cprof_reset");
    lua_pushcclosure(L, l_report, 0); lua_setfield(L, LUA_GLOBALSINDEX, "photon_cprof_report");
}
