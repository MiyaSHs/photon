/* Photon CPU, phase 2 E1: where the Lua runs interpreted. GMod's lua_shared is LuaJIT 2.1.0-beta3 with jit.attach disabled, but it
 * exports the built-in sampling profiler (luaJIT_profile_start / _stop / _dumpstack). With PHOTON_CPU_PROBE_JIT=1, the load probe
 * (lua_load.c, which sees each Lua state as it loads its first chunk) registers one global C function in every state:
 *   photon_cpu_jitprof("start")   profile this state (one state at a time: the profiler is process-wide), 1 ms samples, line level
 *   photon_cpu_jitprof("stop")    stop and log the report (photon_proxy.log, "jitprof:" lines)
 * Each sample has the VM state: N compiled (machine code), I interpreted, C in a C function, G garbage collector, J the JIT compiler.
 * The report gives the share of each state and the top source lines with their split, so the interpreted hot spots (where traces
 * abort or never start) can be read off and matched with the code. Measurement only; inert without the variable. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

typedef void (*ProfileCb)(void *data, void *L, int samples, int vmstate);
typedef void (*ProfileStart_t)(void *L, const char *mode, ProfileCb cb, void *data);
typedef void (*ProfileStop_t)(void *L);
typedef const char *(*DumpStack_t)(void *L, const char *fmt, int depth, size_t *len);
typedef int (*CFunction)(void *L);
typedef void (*PushCClosure_t)(void *L, CFunction fn, int n);
typedef void (*SetField_t)(void *L, int idx, const char *k);
typedef const char *(*ToLString_t)(void *L, int idx, size_t *len);

static ProfileStart_t p_start;
static ProfileStop_t p_stop;
static DumpStack_t p_dump;
static PushCClosure_t p_pushcclosure;
static SetField_t p_setfield;
static ToLString_t p_tolstring;
static int g_on = -1;

#define LUA_GLOBALSINDEX (-10002)
#define NLOC 8192
enum { S_N, S_I, S_C, S_G, S_J, S_NSTATE };
static const char k_states[S_NSTATE] = { 'N', 'I', 'C', 'G', 'J' };
typedef struct { char loc[120]; uint32_t n[S_NSTATE]; } Loc;
static Loc *g_locs;
static uint64_t g_tot[S_NSTATE], g_samples, g_overflow;
static void *g_active;
static void *g_registered[8];

static int state_index(int vmstate) {
    for (int i = 0; i < S_NSTATE; i++) if (k_states[i] == vmstate) return i;
    return S_C;
}

static void cb(void *data, void *L, int samples, int vmstate) {
    (void)data;
    int s = state_index(vmstate);
    g_tot[s] += (uint64_t)samples;
    g_samples += (uint64_t)samples;
    size_t len = 0;
    const char *where = p_dump(L, "pl", 1, &len);   /* "file:line" of the running Lua function (the caller, in a C function) */
    char key[120];
    size_t n = len < sizeof key - 1 ? len : sizeof key - 1;
    memcpy(key, where ? where : "?", where ? n : 1);
    key[where ? n : 1] = 0;
    uint32_t h = cpu_fnv(key, strlen(key)) & (NLOC - 1);
    for (int probe = 0; probe < NLOC; probe++, h = (h + 1) & (NLOC - 1)) {
        Loc *l = &g_locs[h];
        if (!l->loc[0]) { strcpy(l->loc, key); l->n[s] += (uint32_t)samples; return; }
        if (!strcmp(l->loc, key)) { l->n[s] += (uint32_t)samples; return; }
    }
    g_overflow += (uint64_t)samples;
}

static uint32_t loc_total(const Loc *l) { uint32_t t = 0; for (int i = 0; i < S_NSTATE; i++) t += l->n[i]; return t; }
static int cmp_loc(const void *a, const void *b) { uint32_t x = loc_total(a), y = loc_total(b); return x < y ? 1 : x > y ? -1 : 0; }

static void report(void) {
    if (!g_samples) { cpu_log("jitprof: no samples"); return; }
    cpu_log("jitprof: %llu samples: compiled %.1f%%, interpreted %.1f%%, C functions %.1f%%, GC %.1f%%, JIT compiler %.1f%%"
            " (%llu not tallied)", (unsigned long long)g_samples, 100.0 * g_tot[S_N] / g_samples, 100.0 * g_tot[S_I] / g_samples,
            100.0 * g_tot[S_C] / g_samples, 100.0 * g_tot[S_G] / g_samples, 100.0 * g_tot[S_J] / g_samples,
            (unsigned long long)g_overflow);
    qsort(g_locs, NLOC, sizeof(Loc), cmp_loc);
    for (int i = 0; i < 60 && g_locs[i].loc[0]; i++) {
        const Loc *l = &g_locs[i];
        cpu_log("jitprof:   %5.2f%%  N %5u I %5u C %5u G %5u J %4u  %s", 100.0 * loc_total(l) / g_samples, l->n[S_N], l->n[S_I],
                l->n[S_C], l->n[S_G], l->n[S_J], l->loc);
    }
}

static int lua_jitprof(void *L) {
    const char *cmd = p_tolstring(L, 1, NULL);
    if (!cmd) return 0;
    if (!strcmp(cmd, "start") && !g_active) {
        if (!g_locs) g_locs = calloc(NLOC, sizeof(Loc));
        if (!g_locs) return 0;
        memset(g_locs, 0, sizeof(Loc) * NLOC);
        memset(g_tot, 0, sizeof g_tot);
        g_samples = g_overflow = 0;
        p_start(L, "li1", cb, NULL);
        g_active = L;
        cpu_log("jitprof: started on state %p", L);
    } else if (!strcmp(cmd, "stop") && g_active) {
        p_stop(g_active);
        if (g_active != L) cpu_log("jitprof: stopped from state %p (started on %p)", L, g_active);
        g_active = NULL;
        report();
    } else cpu_log("jitprof: '%s' ignored on state %p (active %p)", cmd, L, g_active);
    return 0;
}

/* called by the load probe before each luaL_loadbufferx on the main thread: register the function once per state */
void jitprof_register(void *L) {
    if (g_on < 0) {
        const char *e = getenv("PHOTON_CPU_PROBE_JIT");
        g_on = e && *e == '1';
        if (g_on) {
            HMODULE m = GetModuleHandleA("lua_shared.dll");
            p_start = (ProfileStart_t)(void *)GetProcAddress(m, "luaJIT_profile_start");
            p_stop = (ProfileStop_t)(void *)GetProcAddress(m, "luaJIT_profile_stop");
            p_dump = (DumpStack_t)(void *)GetProcAddress(m, "luaJIT_profile_dumpstack");
            p_pushcclosure = (PushCClosure_t)(void *)GetProcAddress(m, "lua_pushcclosure");
            p_setfield = (SetField_t)(void *)GetProcAddress(m, "lua_setfield");
            p_tolstring = (ToLString_t)(void *)GetProcAddress(m, "lua_tolstring");
            if (!p_start || !p_stop || !p_dump || !p_pushcclosure || !p_setfield || !p_tolstring) {
                cpu_log("jitprof: lua_shared exports missing, off");
                g_on = 0;
            }
        }
    }
    if (!g_on || !L) return;
    int i;
    for (i = 0; i < 8 && g_registered[i]; i++) if (g_registered[i] == L) return;
    if (i == 8) return;
    g_registered[i] = L;
    p_pushcclosure(L, lua_jitprof, 0);
    p_setfield(L, LUA_GLOBALSINDEX, "photon_cpu_jitprof");
    cpu_log("jitprof: photon_cpu_jitprof registered in state %p", L);
}
