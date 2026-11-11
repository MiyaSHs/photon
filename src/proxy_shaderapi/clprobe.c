/* analysis/30 step 1: counting probe for the client's per-prop renderable calls (clprobe.h). */
#include <windows.h>
#include <string.h>
#include "clprobe.h"
#include "log.h"
#include "wrap.h"

enum { NVT = 16, MRI_RENDERABLE = 24 };
enum { S_BONES, S_BOUNDS, S_BOUNDSWS, S_FX, NS };
static const int k_slot[NS] = { 16, 20, 21, 45 };
static const char *const k_name[NS] = { "SetupBones(16)", "GetRenderBounds(20)", "GetRenderBoundsWorldspace(21)", "fx blend(45)" };

typedef int (*BonesFn)(void *, void *, int, int, float);
typedef void (*BoundsFn)(void *, float *, float *);
typedef void (*FxFn)(void *, int);

static int g_on;
static SRWLOCK g_lock = SRWLOCK_INIT;
static void **volatile g_vt[NVT];
static void *g_orig[NVT][NS];
static volatile LONG g_nvt;
static volatile LONG64 g_calls[NVT][NS], g_draws[NVT];
static volatile LONG64 g_frames, g_full;

void photon_clprobe_setup(void) {
    char e[4];
    g_on = GetEnvironmentVariableA("PHOTON_TEST_CLPROBE", e, sizeof e) > 0 && e[0] == '1';
    if (g_on) photon_log("clprobe: TEST PHOTON_TEST_CLPROBE=1: renderable slots 16/20/21/45 counted per class (analysis/30)");
}
int photon_clprobe_on(void) { return g_on; }
void photon_clprobe_frame(void) { if (g_on) InterlockedIncrement64(&g_frames); }

static int vt_index(void *self) {
    void **vt = *(void ***)self;
    LONG n = g_nvt;
    for (LONG i = 0; i < n; i++) if (g_vt[i] == vt) return (int)i;
    return -1;
}

static int hk_bones(void *self, void *out, int maxb, int mask, float t) {
    int i = vt_index(self);
    if (i < 0) return 0;   /* not reachable: only patched tables call here */
    InterlockedIncrement64(&g_calls[i][S_BONES]);
    return ((BonesFn)g_orig[i][S_BONES])(self, out, maxb, mask, t);
}
static void hk_bounds(void *self, float *mins, float *maxs) {
    int i = vt_index(self);
    if (i < 0) return;
    InterlockedIncrement64(&g_calls[i][S_BOUNDS]);
    ((BoundsFn)g_orig[i][S_BOUNDS])(self, mins, maxs);
}
static void hk_boundsws(void *self, float *mins, float *maxs) {
    int i = vt_index(self);
    if (i < 0) return;
    InterlockedIncrement64(&g_calls[i][S_BOUNDSWS]);
    ((BoundsFn)g_orig[i][S_BOUNDSWS])(self, mins, maxs);
}
static void hk_fx(void *self, int a) {
    int i = vt_index(self);
    if (i < 0) return;
    InterlockedIncrement64(&g_calls[i][S_FX]);
    ((FxFn)g_orig[i][S_FX])(self, a);
}
static void *const k_hook[NS] = { (void *)hk_bones, (void *)hk_bounds, (void *)hk_boundsws, (void *)hk_fx };

/* patch one class table: every slot must point into the same module's code, as the table itself */
static void add_vt(void **vt) {
    uintptr_t lo, hi;
    if ((LONG)g_nvt >= NVT) { g_full++; return; }
    if (!photon_module_text(vt[0], &lo, &hi)) return;
    for (int s = 0; s < NS; s++) if ((uintptr_t)vt[k_slot[s]] < lo || (uintptr_t)vt[k_slot[s]] >= hi) return;
    LONG i = g_nvt;
    for (int s = 0; s < NS; s++) g_orig[i][s] = vt[k_slot[s]];
    g_vt[i] = vt;
    MemoryBarrier();
    InterlockedIncrement(&g_nvt);   /* visible to the hooks before any slot points at them */
    DWORD old;
    if (!VirtualProtect(&vt[k_slot[0]], (k_slot[NS - 1] - k_slot[0] + 1) * sizeof(void *), PAGE_READWRITE, &old)) return;
    for (int s = 0; s < NS; s++) InterlockedExchangePointer(&vt[k_slot[s]], k_hook[s]);
    VirtualProtect(&vt[k_slot[0]], (k_slot[NS - 1] - k_slot[0] + 1) * sizeof(void *), old, &old);
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt, &m);
    photon_log("clprobe: renderable vtable %ld at module+0x%llx patched", (long)i, (unsigned long long)((uintptr_t)vt - (uintptr_t)m));
}

void photon_clprobe_draw(const uint8_t *info) {
    if (!g_on || !info) return;
    void *r = *(void *const *)(info + MRI_RENDERABLE);
    if (!r) return;
    int i = vt_index(r);
    if (i < 0) {
        AcquireSRWLockExclusive(&g_lock);
        if ((i = vt_index(r)) < 0) add_vt(*(void ***)r);
        ReleaseSRWLockExclusive(&g_lock);
        if ((i = vt_index(r)) < 0) return;
    }
    InterlockedIncrement64(&g_draws[i]);
}

void photon_clprobe_restore(void) {
    if (!g_on) return;
    for (LONG i = 0; i < g_nvt; i++) {
        void **vt = g_vt[i];
        HMODULE m = NULL;   /* client.dll may be unloaded before the shader API shuts down: then there is nothing to restore */
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt, &m)) continue;
        DWORD old;
        if (!VirtualProtect(&vt[k_slot[0]], (k_slot[NS - 1] - k_slot[0] + 1) * sizeof(void *), PAGE_READWRITE, &old)) continue;
        for (int s = 0; s < NS; s++) if (vt[k_slot[s]] == k_hook[s]) InterlockedExchangePointer(&vt[k_slot[s]], g_orig[i][s]);
        VirtualProtect(&vt[k_slot[0]], (k_slot[NS - 1] - k_slot[0] + 1) * sizeof(void *), old, &old);
    }
}

void photon_clprobe_dump(const char *why) {
    if (!g_on) return;
    double f = g_frames ? (double)g_frames : 1;
    photon_log("clprobe summary (%s): frames %lld, vtables %ld (table full %lld)", why, (long long)g_frames, (long)g_nvt, (long long)g_full);
    for (LONG i = 0; i < g_nvt; i++) {
        double d = g_draws[i] ? (double)g_draws[i] : 1;
        photon_log("clprobe vt %ld: draws/frame %.1f; %s %.1f/frame %.2f/draw, %s %.1f %.2f, %s %.1f %.2f, %s %.1f %.2f", (long)i,
                   g_draws[i] / f, k_name[0], g_calls[i][0] / f, g_calls[i][0] / d, k_name[1], g_calls[i][1] / f, g_calls[i][1] / d,
                   k_name[2], g_calls[i][2] / f, g_calls[i][2] / d, k_name[3], g_calls[i][3] / f, g_calls[i][3] / d);
    }
}
