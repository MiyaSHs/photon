/* Photon CPU C4 probe (PHOTON_CPU_PROBE_VIEWS=1): which views build world lists, and what each costs.
 *
 * The client's CRendering3dView::BuildWorldRenderLists (FUN_1802f8a10) asks the engine through IVRenderView slot 12 (BuildWorldLists),
 * unless its world-list cache (r_worldlistcache, default 1) already holds a list for that view. Slot 12 of VEngineRenderView014's vtable is
 * a 23-byte forwarder in the validated build (engine+0x147ec0). The probe swaps that one slot (Photon Render hooks slots 37/38/40 of the
 * same table: never the whole table) and counts per caller stack: calls per frame and microseconds per frame. Every argument is passed
 * through as a raw 64-bit value (arguments 5-7 travel on the stack: analysis/02 §5).
 * Off unless the env var is set; on any mismatch with the validated build it logs one line and does nothing. */
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include "photon_cpu.h"

enum { SLOT_BUILDWORLDLISTS = 12, RVA_BWL = 0x147ec0, NREC = 64, DEPTH = 4, WINDOW = 600, WINDOW_MS = 15000 };
static const DWORD k_engine_timestamp = 0x6adfbd6c;
static const uint32_t k_hash_bwl = 0x652a0b98;   /* FNV-1a of the 32 bytes at engine+0x147ec0 */

typedef uint64_t (*BwlFn)(void *self, uint64_t list, uint64_t info, uint64_t force_leaf, uint64_t vis, uint64_t shadow, uint64_t water);
static BwlFn g_orig;
static int g_state;   /* 0 not tried, 1 on, -1 off */

typedef struct { volatile LONG64 key; void *fr[DEPTH]; volatile LONG64 calls, cycles, shadow; } Rec;
static Rec g_rec[NREC];

static Rec *rec_for(void *const *fr) {
    uint64_t hk = 1469598103934665603ull;
    for (int i = 0; i < DEPTH; i++) hk = (hk ^ (uint64_t)(uintptr_t)fr[i]) * 1099511628211ull;
    LONG64 key = (LONG64)(hk | 1);
    unsigned h = (unsigned)(hk % NREC);
    for (int i = 0; i < NREC; i++, h = (h + 1) % NREC) {
        if (g_rec[h].key == key) return &g_rec[h];
        if (!g_rec[h].key && InterlockedCompareExchange64(&g_rec[h].key, key, 0) == 0) {
            for (int d = 0; d < DEPTH; d++) g_rec[h].fr[d] = fr[d];
            return &g_rec[h];
        }
        if (g_rec[h].key == key) return &g_rec[h];
    }
    return NULL;
}

static uint64_t probe_bwl(void *self, uint64_t list, uint64_t info, uint64_t force_leaf, uint64_t vis, uint64_t shadow, uint64_t water) {
    void *fr[DEPTH];
    cpu_caller_stack(fr, DEPTH);
    unsigned long long t0 = __rdtsc();
    uint64_t r = g_orig(self, list, info, force_leaf, vis, shadow, water);
    unsigned long long dt = __rdtsc() - t0;
    Rec *p = rec_for(fr);
    if (p) {
        InterlockedIncrement64(&p->calls);
        InterlockedAdd64(&p->cycles, (LONG64)dt);
        if (shadow & 0xff) InterlockedIncrement64(&p->shadow);
    }
    return r;
}

static int install(void) {
    uintptr_t lo, hi;
    if (!cpu_module_range("engine.dll", &lo, &hi)) return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(lo + ((const IMAGE_DOS_HEADER *)lo)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != k_engine_timestamp) {
        cpu_log("probe views: engine.dll timestamp 0x%lx is not the validated build: off", nt->FileHeader.TimeDateStamp);
        return -1;
    }
    typedef void *(*CreateInterfaceFn)(const char *, int *);
    CreateInterfaceFn ci = (CreateInterfaceFn)(void *)GetProcAddress((HMODULE)lo, "CreateInterface");
    void *rv = ci ? ci("VEngineRenderView014", NULL) : NULL;
    if (!rv) { cpu_log("probe views: no VEngineRenderView014: off"); return -1; }
    void **vt = *(void ***)rv;
    void **slot = &vt[SLOT_BUILDWORLDLISTS];
    uintptr_t fn = (uintptr_t)*slot;
    if (fn != lo + RVA_BWL || cpu_fnv((const void *)fn, 32) != k_hash_bwl) {
        cpu_log("probe views: IVRenderView slot 12 is not the validated BuildWorldLists: off");
        return -1;
    }
    g_orig = (BwlFn)fn;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return -1;
    InterlockedCompareExchangePointer(slot, (void *)probe_bwl, (void *)fn);
    VirtualProtect(slot, sizeof(void *), old, &old);
    cpu_log("probe views: on, IVRenderView slot 12 (BuildWorldLists) wrapped");
    return 1;
}

void probe_views_present(uint64_t frame) {
    static int enabled = -1;
    static uint64_t f0;
    static ULONGLONG t0;
    if (enabled < 0) {
        char e[4];
        enabled = GetEnvironmentVariableA("PHOTON_CPU_PROBE_VIEWS", e, sizeof e) > 0 && e[0] == '1';
    }
    if (!enabled || g_state < 0) return;
    if (g_state == 0) {
        int r = install();
        if (r != 0) { g_state = r; f0 = frame; t0 = GetTickCount64(); }
        return;
    }
    if (frame - f0 < WINDOW && GetTickCount64() - t0 < WINDOW_MS) return;
    if (frame == f0) return;
    double frames = (double)(frame - f0), mhz = cpu_tsc_mhz();
    f0 = frame;
    t0 = GetTickCount64();
    LONG64 tc = 0, tcy = 0;
    for (int i = 0; i < NREC; i++) { tc += g_rec[i].calls; tcy += g_rec[i].cycles; }
    cpu_log("probe views: frame %llu: %.2f BuildWorldLists calls, %.3f ms per frame", (unsigned long long)frame, tc / frames,
            tcy / frames / mhz / 1000.0);
    for (int i = 0; i < NREC; i++) {
        Rec *p = &g_rec[i];
        LONG64 c = InterlockedExchange64(&p->calls, 0), cy = InterlockedExchange64(&p->cycles, 0), sh = InterlockedExchange64(&p->shadow, 0);
        if (!c) continue;
        char a[300], b[300], d[300], e[300];
        cpu_log("probe views:   %.2f calls (%.0f%% shadow depth), %.3f ms per frame <- %s <- %s <- %s <- %s", c / frames, 100.0 * sh / c,
                cy / frames / mhz / 1000.0, cpu_where(p->fr[0], a, sizeof a), cpu_where(p->fr[1], b, sizeof b),
                cpu_where(p->fr[2], d, sizeof d), cpu_where(p->fr[3], e, sizeof e));
    }
}
