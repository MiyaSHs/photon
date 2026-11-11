/* Photon CPU DYN: the CPU side of the frame-rate target (modules/cpu TASKS DYN; the controller is Photon Render's dynq.c).
 * - The steps: named ways to make frames cheaper on the CPU, each relative to the user's own settings (engaging one never makes things
 *   more expensive than what the user chose). Default order, cheapest quality loss first (the user's photon_dyn_order reorders them in
 *   the controller): loot (remove dropped loot), debris (remove gameplay-spawned physics debris), corpses64/32/16 (keep at most that
 *   many corpses), corpsedraw (corpses and loot drawn only nearby), tickcap4/2 (the P1 tick cap engaged).
 * - The engaged mask: set by the controller (dyn_set), read by the Lua modules through the gmsv_/gmcl_photon_cpu binary module
 *   (photon_cpu_dyn_mask, exported) and by tickcap.c.
 * - The main thread's busy time per frame: its CPU cycles (QueryThreadCycleTime: waits do not count) between two Presents, for the
 *   controller's CPU-bound / GPU-bound decision.
 * Safety, not a step: the removers only ever take gameplay-spawned things (photon_cpu_corpses.lua). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "photon_cpu.h"

static const char *const k_steps[] = { "loot", "debris", "corpses64", "corpses32", "corpses16", "corpsedraw", "tickcap4", "tickcap2",
                                       "gpu:renderscale1", "gpu:renderscale2" };   /* gpu:: Photon Render's side, applied here */
enum { NSTEPS = sizeof k_steps / sizeof k_steps[0] };
static volatile LONG g_mask;
double dyn_saving(int step);
double physgov_ms_per_tick(void);
double physgov_ticks_per_s(void);

int dyn_steps(const char *const **names) { *names = k_steps; return NSTEPS; }
__declspec(dllexport) int photon_cpu_dyn_steps(const char *const **names) { return dyn_steps(names); }   /* for the Lua module */
void dyn_set(int step, int engaged) {
    if (step < 0 || step >= NSTEPS) return;
    LONG bit = 1L << step, old, nw;
    do { old = g_mask; nw = engaged ? (old | bit) : (old & ~bit); } while (InterlockedCompareExchange(&g_mask, nw, old) != old);
}
/* tests: PHOTON_CPU_DYN_FORCE=<mask> engages those steps from the start (no controller needed) */
__declspec(dllexport) int photon_cpu_dyn_mask(void) {
    static int forced = -2;
    if (forced == -2) { char e[16]; forced = GetEnvironmentVariableA("PHOTON_CPU_DYN_FORCE", e, sizeof e) > 0 ? atoi(e) : -1; }
    return forced >= 0 ? (int)g_mask | forced : (int)g_mask;
}
/* what the steps could act on, reported by the server Lua (photon_cpu_corpses.lua, once a second, through the binary module): dyn_available
 * for the controller (host contract v4) */
static volatile LONG g_n_loot = -1, g_n_debris = -1, g_n_corpses = -1;
static volatile ULONGLONG g_counts_at;
static volatile LONG g_aw_total, g_aw_loot, g_aw_debris, g_aw_c[3];   /* awake physics objects: all, in loot, in debris, in the corpses
                                                                         * corpses64/32/16 would remove */
__declspec(dllexport) void photon_cpu_dyn_counts(int loot, int debris, int corpses) {
    g_n_loot = loot; g_n_debris = debris; g_n_corpses = corpses;
    g_counts_at = GetTickCount64();
}
__declspec(dllexport) void photon_cpu_dyn_awake(int total, int loot, int debris, int c64, int c32, int c16) {
    g_aw_total = total; g_aw_loot = loot; g_aw_debris = debris; g_aw_c[0] = c64; g_aw_c[1] = c32; g_aw_c[2] = c16;
}

/* the renderscale steps: available by default since Photon Render's passes follow the engine's scaled 3D view (RSCALE, photon-94,
 * 2026-11-10); photon_cpu_renderscale=0 (setting or env) keeps them out */
static int renderscale_allowed(void) {
    static int v = -1;
    if (v < 0) v = cpu_setting("photon_cpu_renderscale", "PHOTON_CPU_RENDERSCALE", 1) != 0;
    return v;
}
int dyn_available(int step) {
    if (step < 0 || step >= NSTEPS) return 0;
    if (step == 8 || step == 9) return renderscale_allowed();
    /* corpsedraw: never helped in any laptop DYN run (2026-11-10, every check "did not help", median +0 to +2.9 ms; a hidden corpse
     * still sets up its bones), and each failed check held the CPU side 10-20 s; out of DYN unless photon_cpu_dyn_corpsedraw=1. The menu's
     * corpse draw distance is not affected. */
    if (step == 5) {
        static int v = -1;
        if (v < 0) v = cpu_setting("photon_cpu_dyn_corpsedraw", "PHOTON_CPU_DYN_CORPSEDRAW", 0) == 1;
        if (!v) return 0;
    }
    if (g_n_corpses < 0 || GetTickCount64() - g_counts_at > 5000) return 1;   /* not reported (or stale): try it */
    switch (step) {
    case 0: return g_n_loot > 0;                                  /* loot */
    case 1: return g_n_debris > 0;                                /* debris */
    case 2: return g_n_corpses > 64;                              /* corpses64 */
    case 3: return g_n_corpses > 32;                              /* corpses32 */
    case 4: return g_n_corpses > 16;                              /* corpses16 */
    case 5: return g_n_corpses + g_n_loot > 0;                    /* corpsedraw */
    case 8: case 9: return renderscale_allowed();                 /* gpu:renderscale1/2 (photon_cpu_renderscale=0: never) */
    default: return 1;                                            /* tickcap4/2 */
    }
}

int dyn_engaged(const char *name) {
    for (int i = 0; i < NSTEPS; i++) if (!strcmp(k_steps[i], name)) return (g_mask >> i) & 1;
    return 0;
}

/* the main thread's CPU time per frame: its cycles between Presents (QueryThreadCycleTime: exact, real Windows); where that is not
 * available (Wine/Proton: not implemented) its kernel + user time from GetThreadTimes (coarse there, ~10 ms steps). Either way it is
 * reported as the mean per frame over windows of at least 0.5 s, clamped to the window's wall time, skipping windows with a hitch */
static HANDLE g_main;
static ULONG64 g_last_cycles;
static volatile double g_busy_ms;
static double g_cycles_per_ms;
static int g_use_times = -1;   /* -1 undecided, 0 cycles, 1 thread times */
static ULONG64 g_t_last;   /* 100 ns units */
static LARGE_INTEGER g_win_start, g_qpf;
static int g_win_frames;
static volatile double g_interval_ms;   /* the frame interval over the same window, for the log */
static ULONG64 thread_time(void) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(g_main, &c, &e, &k, &u)) return 0;
    return ((ULONG64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONG64)u.dwHighDateTime << 32 | u.dwLowDateTime);
}
void dyn_present(uint64_t frame) {
    static ULONGLONG last_log;
    ULONGLONG tl = GetTickCount64();
    if (tl - last_log >= 10000 && (last_log = tl, frame)) cpu_log("dyn: main thread busy %.2f ms a frame, interval %.2f ms (%s), engaged mask %ld", g_busy_ms, g_interval_ms,
                                           g_use_times == 1 ? "thread times" : g_use_times == 0 ? "cycles" : "-", (long)g_mask);
    if (tl == last_log && frame) {   /* the same 10 s: the expected savings (dyn_saving), to check them against what each step measures */
        char b[512]; int n = 0;
        for (int k = 0; k < NSTEPS && n < (int)sizeof b - 40; k++) n += snprintf(b + n, sizeof b - n, " %s %.2f", k_steps[k], dyn_saving(k));
        cpu_log("dyn: expected savings (ms a frame):%s; physics %.2f ms a tick, %.1f ticks/s", b, physgov_ms_per_tick(), physgov_ticks_per_s());
    }
    if (!g_main) {
        g_main = OpenThread(THREAD_QUERY_INFORMATION, FALSE, cpu_host()->main_thread);
        g_cycles_per_ms = cpu_tsc_mhz() * 1000.0;
        QueryPerformanceFrequency(&g_qpf);
        if (!g_main) return;
    }
    /* this frame's main-thread time, in ms, from the cycles where they exist, else from the thread times */
    double busy = -1;
    if (g_use_times != 1) {
        ULONG64 c = 0;
        if (QueryThreadCycleTime(g_main, &c) && c) {
            if (g_last_cycles && c > g_last_cycles && g_cycles_per_ms > 0) busy = (double)(c - g_last_cycles) / g_cycles_per_ms;
            g_last_cycles = c;
            g_use_times = 0;
        } else if (g_use_times == -1) {
            g_use_times = 1;
            cpu_log("dyn: QueryThreadCycleTime unavailable, main thread time from GetThreadTimes (0.5 s means)");
        }
    }
    if (g_use_times == 1) {
        ULONG64 t = thread_time();
        if (g_t_last && t >= g_t_last) busy = (double)(t - g_t_last) / 1e4;
        g_t_last = t;
    }
    /* windows of >= 0.5 s: the mean per frame, clamped to the wall time (a thread is never busier than the clock), and a window with a
     * frame over 250 ms (a load or streaming hitch) is dropped, as the controller drops those frames */
    static LARGE_INTEGER last;
    static int bad;
    static double win_busy;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!last.QuadPart || !g_win_start.QuadPart) { last = now; g_win_start = now; g_win_frames = 0; win_busy = 0; bad = 0; return; }
    double frame_ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    last = now;
    if (frame_ms > 250.0) bad = 1;
    if (busy >= 0) win_busy += busy;
    g_win_frames++;
    double win_ms = (double)(now.QuadPart - g_win_start.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    if (win_ms >= 500.0) {
        if (!bad && g_win_frames > 0) {
            if (win_busy > win_ms) win_busy = win_ms;
            g_busy_ms = win_busy / (double)g_win_frames;
            g_interval_ms = win_ms / (double)g_win_frames;
        }
        g_win_start = now; g_win_frames = 0; win_busy = 0; bad = 0;
    }
}
double dyn_main_busy_ms(void) { return g_busy_ms; }

/* dyn_saving (host contract v5): the ms per frame engaging the step is expected to save now, from what is measured: the server's physics
 * per tick (physgov's timing of Simulate) times the share of the awake physics objects the step would remove, times the server ticks a
 * frame; for the tick cap, the main thread's busy time of the ticks it would drop; for corpsedraw a rough per-model draw cost. 0: nothing
 * to save; -1: not measured (the controller falls back to the user's order for it). The controller corrects wrong estimates by measuring each step it engages. */
double physgov_ms_per_tick(void);
double physgov_ticks_per_s(void);
double dyn_saving(int step) {
    if (step < 0 || step >= NSTEPS || !dyn_available(step)) return 0;
    double interval = g_interval_ms > 0 ? g_interval_ms : 16.7;
    double ticks_per_frame = physgov_ticks_per_s() * interval / 1000.0;
    double phys = physgov_ms_per_tick() * ticks_per_frame;        /* server physics ms a frame */
    double total = g_aw_total > 0 ? (double)g_aw_total : 0;
    double share = 0;
    /* physics not measured (governor off, server.dll not patched) or no awake census (no binary module): unknown, not "nothing" */
    if (step <= 4 && (physgov_ticks_per_s() <= 0 || (g_aw_total <= 0 && g_n_corpses < 0))) return -1;
    if ((step == 6 || step == 7) && physgov_ticks_per_s() <= 0) return -1;
    /* Only claim a saving the measurement explains (laptop v5 run, 2026-11-10): with corpses not colliding with each other physics was
     * ~0.8 ms a tick, so physics-based estimates were ~0 while removing corpses still helped a lot (their bones, drawing, networking);
     * the controller then skipped the steps that worked. Below a 25% physics share of the main thread: unknown (-1), the user's order. */
    if (step <= 4 && (g_busy_ms <= 0 || phys < 0.25 * g_busy_ms)) return -1;
    switch (step) {
    case 0: share = total ? g_aw_loot / total : 0; return phys * share;
    case 1: share = total ? g_aw_debris / total : 0; return phys * share;
    case 2: case 3: case 4: share = total ? g_aw_c[step - 2] / total : 0; return phys * share;
    case 5: return -1;   /* corpse drawing: no measurement yet (a fixed per-model guess made the controller pick it, and it never helped) */
    case 6: case 7: return -1;   /* the tick cap: catch-up ticks come in bursts, an average per frame cannot see them (it read 0 while the
                                  * cap was the first step that helped) */
    default: return 0;   /* gpu:renderscale: estimated by Photon Render from its pass times */
    }
}

/* gpu:renderscale steps (Photon Render's request, applied by the client Lua through the binary module): mat_viewportscale /
 * mat_viewportupscale set through the ConVar's own IConVar::SetValue(float), so the engine's change path runs (a write into the fields
 * is ignored for these: Photon Render's finding). GMod blocks both from Lua's RunConsoleCommand. Main thread (Lua), these two names
 * only. GMod's IConVar (the subobject at +0x30, 8 slots, CS:GO's layout) has four SetValue overloads in slots 0-3, forwarding to the
 * parent ConVar's primary slots 17 / 16 / 15 / 14; client.dll's ConVar vtable (0x180784a08 + continuation 0x180784a60) has 14
 * InternalSetValue(const char *), 15 InternalSetFloatValue(float), 16 (int), 17 (Color): so SetValue(float) is IConVar slot 2 (slot 1,
 * the int overload, read a garbage int and crashed). Every call is checked by reading the value back; a mismatch turns this off. The ConVar objects are found as Photon Render's cvar.c finds them (name string, pointer, layout). */
static float rs_get(void *cv) {
    void *par = *(void **)((uint8_t *)cv + 0x38);
    uint32_t fu; memcpy(&fu, (uint8_t *)par + 0x54, 4); fu ^= (uint32_t)(uintptr_t)par;
    float f; memcpy(&f, &fu, 4); return f;
}
static void *rs_find(const char *name) {
    HMODULE m = GetModuleHandleA("client.dll");
    if (!m) return NULL;
    const uint8_t *base = (const uint8_t *)m;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t nl = strlen(name) + 1;
    for (int si = 0; si < nt->FileHeader.NumberOfSections; si++) {
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_READ) || (sec[si].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))) continue;
        const uint8_t *a = base + sec[si].VirtualAddress, *e = a + sec[si].Misc.VirtualSize;
        for (const uint8_t *p = a; p + nl <= e; p++) {
            if (memcmp(p, name, nl) || (p > a && p[-1])) continue;
            uint64_t want = (uint64_t)(uintptr_t)p;
            for (int sj = 0; sj < nt->FileHeader.NumberOfSections; sj++) {
                if (!(sec[sj].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                const uint8_t *b = base + sec[sj].VirtualAddress, *f = b + sec[sj].Misc.VirtualSize;
                for (const uint8_t *q = b + 0x18; q + 8 <= f; q += 8) {
                    uint64_t v; memcpy(&v, q, 8);
                    if (v != want) continue;
                    void *cv = (void *)(q - 0x18);
                    /* a real ConVar of client.dll: its two vtables are ConVar's (primary RVA 0x784a08, IConVar RVA 0x784ab8) */
                    void *vt0 = *(void **)cv, *vt1 = *(void **)((uint8_t *)cv + 0x30);
                    cpu_log("dyn: renderscale: %s candidate %p, vtables +%#llx / +%#llx", name, cv, (unsigned long long)((uint8_t *)vt0 - base),
                            (unsigned long long)((uint8_t *)vt1 - base));
                    if ((uint8_t *)vt0 == base + 0x784a08 && (uint8_t *)vt1 == base + 0x784ab8) return cv;
                }
            }
        }
    }
    return NULL;
}
static int rs_set(void *cv, float v) {
    void *icv = (uint8_t *)cv + 0x30;
    void (*setf)(void *, float) = (void (*)(void *, float))(*(void ***)icv)[2];
    setf(icv, v);
    float got = rs_get(cv);
    return got > v - 0.001f && got < v + 0.001f;
}
__declspec(dllexport) int photon_cpu_set_viewport(float scale, float upscale) {
    static int state;   /* 0 untried, 1 working, -1 off */
    static void *cv_s, *cv_u;
    if (state < 0 || GetCurrentThreadId() != cpu_host()->main_thread) return 0;
    if (!state) {
        cv_s = rs_find("mat_viewportscale"); cv_u = rs_find("mat_viewportupscale");
        if (!cv_s || !cv_u) { state = -1; cpu_log("dyn: renderscale: ConVars not found, off"); return 0; }
        state = 1;
    }
    if (scale < 0.1f || scale > 1.0f) return 0;
    if (!rs_set(cv_u, upscale) || !rs_set(cv_s, scale)) {
        state = -1;
        cpu_log("dyn: renderscale: SetValue did not take (read back %.3f / %.3f), off", rs_get(cv_s), rs_get(cv_u));
        return 0;
    }
    return 1;
}
