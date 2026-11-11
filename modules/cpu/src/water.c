/* Photon CPU C4a (analysis/03): render the water reflection and refraction views only when a water surface can be in view.
 *
 * The engine reports a "visible water volume" (IVRenderView slot 10, R_GetVisibleFogVolume FUN_1800efc10) whenever the eye's BSP leaf has
 * the compile-time "may see water" flag, without looking at the water surfaces themselves. The client then renders a full reflection and
 * refraction (CAboveWaterView), even when no water is on screen: 43% of the 400-prop frame on gm_construct's field.
 *
 * The engine already leaves water outside the frustum and the PVS (analysis/03 §8). What remains is water in view but behind geometry,
 * which only the GPU can tell: Photon Render's W1 (host version 2, water_pixels) sums the depth-test samples of every main-view water draw
 * per frame, answered 1-2 frames late. C4b (analysis/03 §9): the client's CViewRender::RenderView (vtable slot 6) pushes the view being
 * rendered; after the engine's GetVisibleFogVolume, when the eye is above water, the eye is the main view's origin, the last 2 answered
 * frames drew water but passed no water pixel, and the camera has barely moved over the last frames, the volume is reported as -1:
 * DetermineWaterRenderInfo (client FUN_1802f8f80) then builds no water view. Unknown evidence (no water drawn, or a draw not measured)
 * never skips. The worst case, agreed with Photon Render: water revealed by moving geometry while the camera stands still shows the previous
 * reflection for 1-2 frames, until the next answer. The user decides before it can be a default.
 *
 * PHOTON_CPU_WATER=1 turns it on (off by default until its gates pass); PHOTON_CPU_PROBE_WATER=1 logs the decisions every 600 frames;
 * the runtime flag garrysmod/data/photon_cpu_water_off.txt (photon_cpu_flag water_off 1) turns it off within a run. */
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "photon_cpu.h"

enum { RVA_GVFV = 0x147ea0, RVA_GVFV_IMPL = 0xefc10,
       RVA_CVIEWRENDER_VT = 0x6e6350, RVA_RENDERVIEW = 0x302ba0, RVA_MAIN_RV_CALL = 0x2e5899, SLOT_GVFV = 10, SLOT_RENDERVIEW = 6 };
static const DWORD k_engine_ts = 0x6adfbd6c, k_client_ts = 0x6adfbed6;
/* FNV-1a of the code each hook depends on (validated build): the slot-10 forwarder (11 bytes, includes the jump to R_GetVisibleFogVolume),
 * R_GetVisibleFogVolume's first 256 bytes (they hold the worldbrush, leaf and water-data offsets), RenderView's first 64 bytes */
static const uint32_t k_hash_fwd = 0xa3b110fc, k_hash_impl = 0x908eda68, k_hash_rv = 0x40f93764;

typedef uint64_t (*GvfvFn)(void *self, uint64_t eye, uint64_t info);
typedef uint64_t (*RenderViewFn)(void *self, uint64_t view, uint64_t clear, uint64_t what);
static GvfvFn g_gvfv;
static RenderViewFn g_renderview;
static int g_state, g_on, g_probe, g_clip;   /* g_clip: PHOTON_CPU_WATERCLIP (waterclip.c, C4c) */
static uintptr_t g_engine;
typedef void (*Push3DFn)(void *self, uint64_t view, uint64_t flags, uint64_t rt, uint64_t frustum);
static Push3DFn g_push_next;
typedef uint64_t (*BuildWLFn)(void *self, uint64_t list, uint64_t info, uint64_t force_leaf, uint64_t vis, uint64_t shadow, uint64_t refl);
static BuildWLFn g_bwl_next;   /* whatever IVRenderView slot 12 held (the CPU probe or the engine) */   /* whatever IVRenderView slot 38 held (Photon Render may chain there too) */
static void *g_rv;
static void **g_rv_vt;
static int g_clip_vol = -1, g_clip_eyein;
static uint64_t g_clip_vol_frame;
static const void *g_main_setup;   /* the main screen view's CViewSetup while it renders */
static volatile int g_off_flag;

/* the views being rendered (main thread; nested for Lua render.RenderView) */
typedef struct { float origin[3], angles[3], fov; int w, h, main; } View;
static uintptr_t g_main_ret;   /* return address of CViewRender::Render's one RenderView call: the main screen view */
enum { VSTACK = 8 };
static View g_views[VSTACK];
static int g_nviews;

static LONG64 st_calls, st_reported, st_skipped, st_kept_visible, st_kept_eyein, st_kept_noview, st_kept_mismatch;

static uint64_t hook_renderview(void *self, uint64_t view, uint64_t clear, uint64_t what) {
    int pushed = 0;
    if (GetCurrentThreadId() == cpu_host()->main_thread && g_nviews < VSTACK && view) {
        const char *v = (const char *)(uintptr_t)view;
        View *t = &g_views[g_nviews];
        memcpy(t->origin, v + 0x40, 12);
        memcpy(t->angles, v + 0x4c, 12);
        memcpy(&t->fov, v + 0x38, 4);
        t->w = *(const int *)(v + 0x10);
        t->h = *(const int *)(v + 0x18);
        t->main = (uintptr_t)__builtin_return_address(0) == g_main_ret;
        if (t->main && g_nviews == 0) g_main_setup = v;
        g_nviews++;
        pushed = 1;
    }
    uint64_t r = g_renderview(self, view, clear, what);
    if (pushed) { g_nviews--; if (!g_nviews) g_main_setup = NULL; }
    return r;
}

/* C4c: after a Push3DView inside the main screen view, waterclip.c narrows a water view's culling frustum */
/* C4c: a water view's world list built with the engine's own frustum trim (waterclip.c), when the client passes no override data */
static uint64_t hook_bwl(void *self, uint64_t list, uint64_t info, uint64_t force_leaf, uint64_t vis, uint64_t shadow, uint64_t refl) {
    if (g_clip == 1 && !g_off_flag && !vis && GetCurrentThreadId() == cpu_host()->main_thread) {
        const void *v = waterclip_visdata();
        if (v) vis = (uint64_t)(uintptr_t)v;
    }
    return g_bwl_next(self, list, info, force_leaf, vis, shadow, refl);
}

static void hook_push(void *self, uint64_t view, uint64_t flags, uint64_t rt, uint64_t frustum) {
    waterclip_cancel();   /* a pending trim belongs to the view pushed just before; any new push ends it */
    g_push_next(self, view, flags, rt, frustum);
    if (!g_clip || g_off_flag || GetCurrentThreadId() != cpu_host()->main_thread || g_nviews != 1 || !g_views[0].main || !g_main_setup) return;
    int vol = g_clip_vol_frame == cpu_host()->frame_count() ? g_clip_vol : -1;
    waterclip_push(g_clip, g_engine, g_rv, g_rv_vt, g_main_setup, vol, g_clip_eyein, (const void *)(uintptr_t)view,
                   (void *)(uintptr_t)rt, (void *)(uintptr_t)frustum);
}

/* camera stillness: the main view's pose over the last frames (main thread) */
enum { NPOSE = 3 };
static float g_pose[NPOSE][6];
static int g_npose, g_pose_i;
static uint64_t g_pose_frame;
static void note_pose(const View *v) {
    uint64_t f = cpu_host()->frame_count();
    if (g_npose && f == g_pose_frame) return;   /* one pose a frame */
    g_pose_frame = f;
    memcpy(g_pose[g_pose_i], v->origin, 12);
    memcpy(g_pose[g_pose_i] + 3, v->angles, 12);
    g_pose_i = (g_pose_i + 1) % NPOSE;
    if (g_npose < NPOSE) g_npose++;
}
static int camera_still(const View *v) {
    if (g_npose < NPOSE) return 0;
    for (int i = 0; i < NPOSE; i++) {
        for (int k = 0; k < 3; k++) if (fabsf(g_pose[i][k] - v->origin[k]) > 4.0f) return 0;
        for (int k = 0; k < 3; k++) {
            float d = fabsf(g_pose[i][3 + k] - v->angles[k]);
            if (d > 180.0f) d = 360.0f - d;
            if (d > 1.0f) return 0;
        }
    }
    return 1;
}

/* W1 evidence: consecutive answered frames that drew water but passed no water pixel */
static int g_hidden_streak;
static uint64_t g_last_answer;
static LONG64 st_unknown;
static void update_evidence(void) {
    const PhotonCpuHost *h = cpu_host();
    uint64_t f = 0, samples = 0;
    uint32_t draws = 0;
    if (h->version < 2 || !h->water_pixels || !h->water_pixels(&f, &draws, &samples) || f == g_last_answer) return;
    g_last_answer = f;
    if (draws > 0 && samples == 0) g_hidden_streak++;
    else { g_hidden_streak = 0; if (draws == 0 || samples == UINT64_MAX) st_unknown++; }
}

static uint64_t hook_gvfv(void *self, uint64_t eye, uint64_t info) {
    uint64_t r = g_gvfv(self, eye, info);
    if (GetCurrentThreadId() != cpu_host()->main_thread || !info || !eye) return r;
    st_calls++;
    int *in = (int *)(uintptr_t)info;
    int vol = in[0];
    if (g_nviews == 1 && g_views[0].main) {   /* C4c: the volume the main view's water views will be drawn for */
        g_clip_vol = vol;
        g_clip_eyein = *((const unsigned char *)in + 8);
        g_clip_vol_frame = cpu_host()->frame_count();
    }
    if (!g_on && !g_probe) return r;   /* C4b off: nothing else to do */
    if (vol < 0) return r;
    st_reported++;
    if (*((const unsigned char *)in + 8)) { st_kept_eyein++; return r; }
    if (g_nviews != 1 || !g_views[0].main) { st_kept_noview++; return r; }   /* W1's evidence is the main screen view's only */
    const View *v = &g_views[g_nviews - 1];
    const float *e = (const float *)(uintptr_t)eye;
    if (e[0] != v->origin[0] || e[1] != v->origin[1] || e[2] != v->origin[2] || v->w <= 0 || v->h <= 0 || !(v->fov > 1 && v->fov < 179)) {
        st_kept_mismatch++;
        return r;
    }
    note_pose(v);
    update_evidence();
    if (g_hidden_streak < 2 || !camera_still(v)) { st_kept_visible++; return r; }
    st_skipped++;                        /* in probe-only mode (or with the runtime flag) a would-skip: nothing changes */
    if (g_on && !g_off_flag) in[0] = -1;
    return r;
}

static int patch_slot(void **slot, void *expect, void *hook) {
    DWORD old;
    if (*slot != expect || !VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return 0;
    InterlockedCompareExchangePointer(slot, hook, expect);
    VirtualProtect(slot, sizeof(void *), old, &old);
    return 1;
}

static int install(void) {
    uintptr_t elo, ehi, clo, chi;
    if (!cpu_module_range("engine.dll", &elo, &ehi) || !cpu_module_range("client.dll", &clo, &chi)) return 0;
    const IMAGE_NT_HEADERS64 *ne = (const IMAGE_NT_HEADERS64 *)(elo + ((const IMAGE_DOS_HEADER *)elo)->e_lfanew);
    const IMAGE_NT_HEADERS64 *nc = (const IMAGE_NT_HEADERS64 *)(clo + ((const IMAGE_DOS_HEADER *)clo)->e_lfanew);
    if (ne->FileHeader.TimeDateStamp != k_engine_ts || nc->FileHeader.TimeDateStamp != k_client_ts) {
        cpu_log("photon_cpu water: engine/client build is not the validated one: off");
        return -1;
    }
    if (cpu_fnv((const void *)(elo + RVA_GVFV), 11) != k_hash_fwd || cpu_fnv((const void *)(elo + RVA_GVFV_IMPL), 256) != k_hash_impl ||
        cpu_fnv((const void *)(clo + RVA_RENDERVIEW), 64) != k_hash_rv) {
        cpu_log("photon_cpu water: code differs from the validated build: off");
        return -1;
    }
    typedef void *(*CreateInterfaceFn)(const char *, int *);
    CreateInterfaceFn ci = (CreateInterfaceFn)(void *)GetProcAddress((HMODULE)elo, "CreateInterface");
    void *rv = ci ? ci("VEngineRenderView014", NULL) : NULL;
    void **ivt = rv ? *(void ***)rv : NULL;
    void **cvt = (void **)(clo + RVA_CVIEWRENDER_VT);
    if (!ivt || ivt[SLOT_GVFV] != (void *)(elo + RVA_GVFV) || cvt[SLOT_RENDERVIEW] != (void *)(clo + RVA_RENDERVIEW)) {
        cpu_log("photon_cpu water: vtable slots are not the validated ones: off");
        return -1;
    }
    g_gvfv = (GvfvFn)(void *)(elo + RVA_GVFV);
    g_renderview = (RenderViewFn)(void *)(clo + RVA_RENDERVIEW);
    if (memcmp((const void *)(clo + RVA_MAIN_RV_CALL), "\xff\x50\x30", 3) != 0) {   /* call [rax+0x30] in CViewRender::Render */
        cpu_log("photon_cpu water: CViewRender::Render's RenderView call is not the validated one: off");
        return -1;
    }
    g_main_ret = clo + RVA_MAIN_RV_CALL + 3;
    DeleteFileA("garrysmod\\data\\photon_cpu_water_off.txt");
    if (!patch_slot(&cvt[SLOT_RENDERVIEW], (void *)g_renderview, (void *)hook_renderview) ||
        !patch_slot(&ivt[SLOT_GVFV], (void *)g_gvfv, (void *)hook_gvfv)) {
        cpu_log("photon_cpu water: could not patch the slots: off");
        return -1;
    }
    g_engine = elo;
    g_rv = rv;
    g_rv_vt = ivt;
    if (g_clip) {   /* IVRenderView slot 38 (Push3DView, a shared hook point: chain whatever is there) and slot 43 (OverrideViewFrustum) */
        void *cur = ivt[38];
        if (ivt[43] != (void *)(elo + 0x1482a0) || !cur) { cpu_log("photon_cpu waterclip: IVRenderView slots 38/43 not as validated: off"); g_clip = 0; }
        else {
            g_push_next = (Push3DFn)cur;
            g_bwl_next = (BuildWLFn)ivt[12];
            if (!patch_slot(&ivt[38], cur, (void *)hook_push) || !patch_slot(&ivt[12], (void *)g_bwl_next, (void *)hook_bwl)) {
                cpu_log("photon_cpu waterclip: could not patch slots 38/12: off"); g_clip = 0; }
            else cpu_log("photon_cpu waterclip: %s (IVRenderView slot 38 wrapped, chained to %p)", g_clip == 1 ? "on" : "probe only", cur);
        }
    }
    if (cpu_host()->version < 2 || !cpu_host()->water_pixels) {
        cpu_log("photon_cpu water: the proxy has no water_pixels (host version %d): off", cpu_host()->version);
        return -1;
    }
    cpu_log("photon_cpu water: %s (CViewRender slot 6 and IVRenderView slot 10 wrapped; evidence from Photon Render's water_pixels)",
            g_on ? "on" : "probe only");
    return 1;
}

void water_present(uint64_t frame) {
    static int enabled = -1;
    static uint64_t f0;
    static ULONGLONG t0;
    if (enabled < 0) {
        char e[4];
        g_on = GetEnvironmentVariableA("PHOTON_CPU_WATER", e, sizeof e) > 0 && e[0] == '1';
        g_probe = GetEnvironmentVariableA("PHOTON_CPU_PROBE_WATER", e, sizeof e) > 0 && e[0] == '1';
        g_clip = GetEnvironmentVariableA("PHOTON_CPU_WATERCLIP", e, sizeof e) > 0 ? (e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0) : 0;
        enabled = g_on || g_probe || g_clip;
    }
    if (!enabled || g_state < 0) return;
    if (g_state == 0) {
        int r = install();
        if (r != 0) { g_state = r; f0 = frame; t0 = GetTickCount64(); }
        return;
    }
    if (frame % 30 == 0) {
        int off = GetFileAttributesA("garrysmod\\data\\photon_cpu_water_off.txt") != INVALID_FILE_ATTRIBUTES;
        if (off != g_off_flag) { g_off_flag = off; cpu_log("photon_cpu water: %s at frame %llu (runtime flag)", off ? "off" : "on", (unsigned long long)frame); }
    }
    if ((frame - f0 < 600 && GetTickCount64() - t0 < 15000) || frame == f0) return;   /* 600 frames or 15 s (the VM) */
    if (g_clip) waterclip_report();
    if (!g_probe) { f0 = frame; t0 = GetTickCount64(); return; }
    double n = (double)(frame - f0);
    f0 = frame;
    t0 = GetTickCount64();
    cpu_log("photon_cpu water: per frame %.2f queries, %.2f with water reported, %.2f skipped%s, kept: %.2f seen or moving, %.2f eye in water, "
            "%.2f no view, %.2f other; unknown answers %.2f", st_calls / n, st_reported / n, st_skipped / n, g_on && !g_off_flag ? "" : " (would skip: probe only)",
            st_kept_visible / n, st_kept_eyein / n,
            st_kept_noview / n, st_kept_mismatch / n, st_unknown / n);
    st_calls = st_reported = st_skipped = st_kept_visible = st_kept_eyein = st_kept_noview = st_kept_mismatch = st_unknown = 0;
}
