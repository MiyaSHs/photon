/* Photon CPU P1: a cap on the server ticks one frame may run (PHOTON_CPU_TICKCAP=N, setting "photon_cpu_tickcap"; 0 = off, the default).
 * engine.dll's _Host_RunFrame (FUN_1801c55f0) adds host_frametime (up to 0.5 s, its own clamp in FUN_1801c0b90) to host_remainder and
 * runs floor(host_remainder / interval_per_tick) ticks: under load a frame that took long asks for many ticks, each tick takes long,
 * the next frame is longer still, and the game spirals to a few frames a second (AI1, analysis/16: 25 ticks a frame, 2-4 fps). With a
 * cap of N, host_remainder is clamped right after the frame-time filter so that at most N ticks (N+1 with the engine's own tick
 * alignment) run per frame and the backlog beyond is dropped: the game slows down instead of freezing, as with host_timescale.
 * Single player only (a listen server's clients run on real time): applied while the engine's game is single player
 * (DAT_180da0e88, the flag its status line prints as Singleplayer/Multiplayer; not dedicated) and its local server runs (state >= 2).
 * The call to the frame-time filter at engine+0x1c56e0 is redirected to a thunk (5-byte call, hash-checked against the validated
 * build), which calls the filter and then clamps. */
#include <windows.h>
#include <string.h>
#include "photon_cpu.h"

#define CALL_RVA 0x1c56e0
#define SITE_HASH 0x482b0089u   /* FNV-1a of the 64 bytes from CALL_RVA - 16, engine.dll timestamp 0x6adfbd6c */
#define FILTER_RVA 0x1c0b90     /* the frame-time filter (writes host_frametime) */
#define REMAINDER_RVA 0x8b1648  /* double host_remainder */
#define FRAMETIME_RVA 0x8b12e4  /* float host_frametime */
#define INTERVAL_RVA 0x8afd20   /* double interval_per_tick */
#define SVSTATE_RVA 0x825518    /* int, the local server's state (maxplayers refuses changes while it is above 1: running) */
#define MULTI_RVA 0xda0e88      /* byte, the engine's "Multiplayer" (1) / "Singleplayer" (0) flag; +1 is "dedicated" */

static int g_cap;
static uint8_t *g_engine;
void *tickcap_filter;   /* the engine's frame-time filter, called by the thunk (global: the asm names it) */
static uint64_t g_frames, g_capped;
static double g_dropped;

/* the cap in force: the user's (0 = off), or a stricter one the DYN controller engaged (dyn.c steps tickcap4 / tickcap2) */
static int effective_cap(void) {
    int c = g_cap;
    int d = dyn_engaged("tickcap2") ? 2 : dyn_engaged("tickcap4") ? 4 : 0;
    if (d && (c <= 0 || d < c)) c = d;
    return c;
}
void tickcap_after(void) {
    double *rem = (double *)(g_engine + REMAINDER_RVA), ival = *(double *)(g_engine + INTERVAL_RVA);
    float ft = *(float *)(g_engine + FRAMETIME_RVA);
    g_frames++;
    int cap = effective_cap();
    if (cap <= 0 || ival <= 0 || g_engine[MULTI_RVA] || g_engine[MULTI_RVA + 1] || *(int *)(g_engine + SVSTATE_RVA) < 2) return;
    double lim = (double)cap * ival - (double)ft;
    if (*rem > lim) { g_dropped += *rem - lim; *rem = lim; g_capped++; }
}

/* call the filter with every argument register as the engine set it, then clamp */
__asm__(".text\n.p2align 4\ntickcap_thunk:\n"
        "  subq $0x28, %rsp\n"
        "  callq *tickcap_filter(%rip)\n"
        "  callq tickcap_after\n"
        "  addq $0x28, %rsp\n"
        "  retq\n");
extern void tickcap_thunk(void);

void tickcap_init(void) {
    g_cap = cpu_setting("photon_cpu_tickcap", "PHOTON_CPU_TICKCAP", 0);
    /* installed even with the user's cap off: the DYN controller may engage one under overload (one thunk call a frame otherwise) */
    g_engine = (uint8_t *)GetModuleHandleA("engine.dll");
    if (!g_engine) { cpu_log("tickcap: engine.dll not loaded, off"); return; }
    uint8_t *site = g_engine + CALL_RVA;
    uint32_t h = cpu_fnv(site - 16, 64);
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    if (h != SITE_HASH || site[0] != 0xe8 || site + 5 + rel != g_engine + FILTER_RVA) {
        cpu_log("tickcap: engine.dll bytes differ (hash %08x), off", h);
        return;
    }
    tickcap_filter = g_engine + FILTER_RVA;
    int64_t to = (int64_t)((uint8_t *)(void *)tickcap_thunk - (site + 5));
    if (to < INT32_MIN || to > INT32_MAX) {   /* the thunk out of rel32 reach: go through a trampoline near engine.dll */
        uint8_t *tr = NULL;
        for (uintptr_t a = ((uintptr_t)g_engine - 0x10000000) & ~(uintptr_t)0xffff; a < (uintptr_t)g_engine && !tr; a += 0x10000)
            tr = VirtualAlloc((void *)a, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tr) { cpu_log("tickcap: no trampoline near engine.dll, off"); return; }
        tr[0] = 0xff; tr[1] = 0x25; memset(tr + 2, 0, 4);   /* jmp [rip+0] */
        uint64_t t = (uint64_t)(void *)tickcap_thunk;
        memcpy(tr + 6, &t, 8);
        FlushInstructionCache(GetCurrentProcess(), tr, 14);
        to = (int64_t)(tr - (site + 5));
    }
    rel = (int32_t)to;
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    cpu_log("tickcap: installed; the user's cap %d (0 = off; DYN may engage 4 or 2), single player only", g_cap);
}

void tickcap_present(uint64_t frame) {
    if (!g_engine || frame % 600 || !g_capped) return;
    cpu_log("tickcap: %llu of %llu frames capped, %.2f s of game time dropped (multiplayer %d, server state %d)",
            (unsigned long long)g_capped, (unsigned long long)g_frames, g_dropped, g_engine[MULTI_RVA], *(int *)(g_engine + SVSTATE_RVA));
}
