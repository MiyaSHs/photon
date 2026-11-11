/* P6 (analysis/21 §3.1, analysis/15c §3): skip the engine's per-model light setup for draws Photon lights itself. On the laptop that setup is
 * 21-27% of a busy frame (analysis/21, P6 ceiling). Stage 1, behind PHOTON_P6=1:
 * - the engine's CModelRender (VEngineModel016) gets a private copy of its vtable with slot 20 (DrawModelExecute) replaced;
 * - eligibility is learned per (model, skin, body): its first draws run unsuppressed while vlg.c reports every RenderPass inside the call,
 *   and a model whose passes were all Photon-lit becomes eligible;
 * - an eligible draw fully inside the ambient volume runs with [CModelRender+0x48] = 1 (the byte Lua's SuppressEngineLighting sets), so the
 *   engine returns from its light setup at once; Lua's own value is restored afterwards;
 * - inside a suppressed draw vlg.c ignores the stale engine lights and cube (photon_p6_suppressed); a non-Photon pass inside one revokes the
 *   model for good (counted).
 * Learning needs RenderPass on the drawing thread, so stage 1 works in sync mode (mat_queue_mode 0) only; in queued mode no model becomes
 * eligible and nothing changes. The sun (no engine directional light reaches a suppressed draw) comes from p6sun.c: a suppressed draw asks it once,
 * at its lighting origin, and vlg.c puts the answer into the light block (photon_p6_sun). */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "p6.h"
#include "p6sun.h"
#include "ambvol.h"
#include "log.h"
#include "wrap.h"
#include "dmirror.h"
#include "vlg.h"
#include "cubemap.h"
#include "cull.h"
#include "hiz.h"
#include "clprobe.h"
#include "shadows.h"
#include "mqueue.h"
#include "waterq.h"
#include "lodwork.h"
#include "l4work.h"

enum { SLOT_DME = 20, SLOT_SUPPRESS = 23, OFF_SUPPRESS = 0x48 };
/* ModelRenderInfo_t (x64): origin 0, angles 12, pRenderable 24, pModel 32, pModelToWorld 40, pLightingOffset 48, pLightingOrigin 56,
 * flags 64, entity_index 68, skin 72, body 76 (analysis/15c: flags at +0x40) */
enum { MRI_MODEL = 32, MRI_M2W = 40, MRI_LORIGIN = 56, MRI_FLAGS = 64, MRI_SKIN = 72, MRI_BODY = 76 };
/* STUDIO_* flags of passes that never take the engine's lighting or must stay stock: wireframe, vcollide wireframe, SSAO and shadow depth */
#define P6_SKIP_FLAGS (0x20u | 0x100u | 0x08000000u | 0x40000000u)

typedef void (*DmeFn)(void *self, void *state, const uint8_t *info, void *bones);
/* IStudioRender (VStudioRender025): the two calls the engine's light setup ends with (analysis/05 §2.4): SetAmbientLightColors (+0xa0) and
 * SetLocalLights (+0xb0). A suppressed call pushes a near-black cube and no lights through them, so the shader API's light state says
 * "ambient light" and stock picks the lit VS combo (DYNAMIC_LIGHT); Photon's volume and grid then supply the light (the cube is unused at
 * volume weight 1). Without this the combo is stale from the previous draw and often unlit (laptop run p6_idx: dynamic index 0x1). */
enum { SR_SLOT_AMBIENT = 0xa0 / 8, SR_SLOT_LOCALLIGHTS = 0xb0 / 8 };
typedef void (*SrAmbientFn)(void *self, const float *cube);
typedef void (*SrLightsFn)(void *self, int n, const void *lights);
static void *g_sr;
static SrAmbientFn g_sr_ambient;
static SrLightsFn g_sr_lights;
static const float k_dim_cube[6][4] = { { 1e-3f, 1e-3f, 1e-3f, 0 }, { 1e-3f, 1e-3f, 1e-3f, 0 }, { 1e-3f, 1e-3f, 1e-3f, 0 },
                                        { 1e-3f, 1e-3f, 1e-3f, 0 }, { 1e-3f, 1e-3f, 1e-3f, 0 }, { 1e-3f, 1e-3f, 1e-3f, 0 } };

static long long g_hiz_skipped;   /* T4.7b */
static int g_want, g_hook, g_bound, g_failed;   /* g_hook: the DrawModelExecute hook is wanted (P6, or the T4.7 cull ceiling) */
/* PHOTON_TEST_DUPSKIP=1 (T4.5 stage 1, analysis/23): a suppressed draw whose (model, skin, body, flags) was already drawn this frame is skipped,
 * so the frame shows the instancing ceiling (the image is wrong: duplicates vanish) */
static int g_dupskip;
static unsigned g_dup_frame;
static volatile LONG64 g_dup_skipped;
typedef struct { uintptr_t model; int skin, body; unsigned flags, frame; } DupKey;
/* T4.5 stage 2b (PHOTON_BATCH=replay, dmirror.c): per frame and view, a group per (model, skin, body, flags); its first copy is drawn by the
 * engine and recorded, later copies are replayed with their own model matrix and skip the engine */
enum { NGROUP = 512 };
typedef struct { uintptr_t model; int skin, body; unsigned flags, frame, view; unsigned char *rec, *rl; PhotonCubeCtx cc; } Group;   /* rl: vlg.c's relight context, cc: cubemap.c's pick */
static Group g_grp[NGROUP];
static unsigned char *g_rec_pool;
static int g_rec_used;
static unsigned g_rec_frame;
static volatile LONG64 g_replayed, g_grp_full;
static DupKey g_dup[8192];
static void **g_real_vt, **g_vt;
static void *g_obj;
static DmeFn g_dme;
static DWORD g_draw_tid;                 /* the thread that runs RenderPass (vlg.c reports it); learning needs it equal to ours */

/* per draw, on the thread that draws: the main thread in sync mode; in queued mode (P6Q) MatQueue, set by q_begin / q_end (thread-local, so
 * the main thread's next calls do not touch what MatQueue is drawing) */
static __thread int t_mode;              /* 0 none, 1 learning, 2 suppressed */
static __thread int t_ours, t_other, t_why[5];   /* why a pass was not Photon-lit: P6_PASS_* */
static __thread int t_idx = -1;            /* the VS dynamic index of the last unlit pass */
static __thread float t_origin[3];         /* the suppressed draw's lighting origin: the engine's SetLightingOrigin is part of the skipped setup */
static __thread int t_skinned;             /* a pass of this suppressed call had a skinned VS combo (vlg.c) */
static __thread int t_sun;                 /* the suppressed draw is sunlit: t_sun_desc is the directional LightDesc_t the engine would have set */
static __thread unsigned char t_sun_desc[88];
static volatile LONG64 g_sunlit;
static int g_norelight;
/* PHOTON_TEST_RELIGHTCHECK=1: one replayable copy in 64 is drawn by the engine instead, its device state recorded, and the c32..c55 and c64
 * that vlg.c uploaded for it compared with what photon_vlg_relight computed for it */
static int g_relcheck;
static int g_mqtest;   /* PHOTON_TEST_MQ=1: a counting call queued around every model draw (mqueue.c) */
static unsigned g_chk_tick;
static unsigned char *g_chk_rec;
static float g_chk_psc[65][4];
static int g_chk_ntex, g_chk_slot[2];
static void *g_chk_tex[2];
static volatile LONG64 g_chk_n, g_chk_ok, g_chk_bad, g_chk_texbad, g_cube_fallback;
static float g_chk_maxd;
static void relight_compare(void) {
    const float (*got)[4] = photon_dmirror_rec_psc(g_chk_rec);
    float md = 0;
    void *const *gt = photon_dmirror_rec_tex(g_chk_rec);
    for (int i = 0; i < g_chk_ntex; i++)
        if (gt[g_chk_slot[i]] != g_chk_tex[i] && g_chk_texbad++ < 6)
            photon_log("p6 relight check: copy at (%.1f %.1f %.1f): sampler %d has texture %p, the replay's cubemap pick %p", t_origin[0], t_origin[1],
                       t_origin[2], g_chk_slot[i], gt[g_chk_slot[i]], g_chk_tex[i]);
    for (int r = 32; r <= 64; r++) {
        if (r > 58 && r < 64) continue;   /* c57/c58: the cubemap blend plane and switch */
        if (r == 57 && g_chk_psc[58][0] == 0.0f) continue;   /* the plane is not read with the blend off */
        for (int k = 0; k < 4; k++) { float d = got[r][k] - g_chk_psc[r][k]; if (d < 0) d = -d; if (d > md) md = d; }
    }
    g_chk_n++;
    if (md > g_chk_maxd) g_chk_maxd = md;
    if (md <= 1e-4f) { g_chk_ok++; return; }
    if (g_chk_bad++ < 6)
        photon_log("p6 relight check: copy at (%.1f %.1f %.1f) differs by %g: c32 %g %g c33 %g %g %g c64 %g %g %g | relight c32 %g %g c33 %g %g %g c64 %g %g %g",
                   t_origin[0], t_origin[1], t_origin[2], md, got[32][0], got[32][1], got[33][0], got[33][1], got[33][2], got[64][0], got[64][1], got[64][2],
                   g_chk_psc[32][0], g_chk_psc[32][1], g_chk_psc[33][0], g_chk_psc[33][1], g_chk_psc[33][2], g_chk_psc[64][0], g_chk_psc[64][1], g_chk_psc[64][2]);
}
static __thread int t_call_draws;          /* device draws so far in the current suppressed call (dmirror.c) */

/* eligibility, open addressing on (model, skin, body) */
enum { NKEY = 4096, LEARN_OK = 3 };
typedef struct { uintptr_t model; int skin, body; signed char state; unsigned char seen; volatile signed char rep; } Key;   /* state 0 learning, 1 eligible, -1 never; rep (P6Q replay): 0 unknown, 1 its recordings are replayable, -1 not */
static Key g_keys[NKEY];
static volatile LONG64 g_draws, g_suppressed, g_learning, g_revoked, g_not_covered, g_lua_suppressed, g_skip_flags, g_keys_eligible, g_keys_never;

/* model_t: FileNameHandle_t (8 bytes) then char szName[260] (public SDK 2013 gl_model_private.h layout, x64) */
static volatile LONG g_name_logs;
static void note_key(const Key *k, const char *what) {
    if (InterlockedIncrement(&g_name_logs) <= 96)
        photon_log("p6: %s: model %p skin %d body %d (this call: ours %d, other %d: stock shader %d, ours unlit %d, override route %d,"
                   " lighting/volume/grid off %d; unlit VS dynamic index 0x%x)", what, (void *)k->model, k->skin, k->body, t_ours, t_other, t_why[P6_PASS_STOCK],
                   t_why[P6_PASS_UNLIT], t_why[P6_PASS_ROUTE], t_why[P6_PASS_OFF], t_idx);
}

static Key *key_of(uintptr_t model, int skin, int body) {
    size_t h = (size_t)((model >> 4) * 0x9E3779B97F4A7C15ull ^ (uint64_t)skin * 0x85EBCA6Bu ^ (uint64_t)body * 0xC2B2AE35u) & (NKEY - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (NKEY - 1)) {
        Key *k = &g_keys[h];
        if (k->model == model && k->skin == skin && k->body == body) return k;
        if (!k->model) { k->model = model; k->skin = skin; k->body = body; return k; }
    }
    return NULL;   /* table crowded: the model stays stock */
}

/* --- P6Q step 3: the main thread's view instance (analysis/27). IVRenderView (VEngineRenderView014) slots 37 / 38 Push3DView, 39 Push2DView,
 * 40 PopView, checked by the internal call each forwards to. Every push gets a new instance number, a pop returns to the parent's: groups
 * keyed by it never cross views, as dmirror.c's view generation in sync mode. Main thread only. */
static unsigned g_vcount, g_vst[64];
static int g_vdepth;
static unsigned view_inst(void) { return g_vdepth > 0 ? g_vst[g_vdepth - 1] : 0; }
typedef void (*ViewFn)(void *, void *, void *, void *, void *, void *);
static ViewFn g_push3a, g_push3b, g_push2, g_pop;
static void view_push(void) { unsigned v = ++g_vcount; if (g_vdepth < 64) g_vst[g_vdepth] = v; g_vdepth++; }
static void view_pop(void) { if (g_vdepth > 0) g_vdepth--; }
static unsigned char g_v3d[64], g_vscreen[64];
/* W1: tell waterq whether the view now on top is the outermost screen 3D view (after every push and pop) */
static void water_view(void) { int i = g_vdepth - 1; photon_waterq_view(i == 0 && g_v3d[0] && g_vscreen[0]); }
/* T4.2 (analysis/34): which 3D view is the main one. Per pushed view: 3D or not, drawn to the screen (no render target) or not, and the
 * DrawModelExecute calls made while it is on top. The screen 3D views of a frame are numbered in pop order; the one with the most model
 * draws is the main view, and next frame its PopView (same number) triggers shadows.c's pass. A frame's 3D part ends at a 2D push at the
 * top level (the HUD). Main thread only. */
static unsigned g_vdme[64];
static int g_screen_ord, g_main_ord = -1, g_best_ord = -1;
static unsigned g_best_dme;
static float g_vsetup[64][8];   /* fov, origin xyz, zNear, zFar, width, height (CViewSetup +0x38, +0x40, +0x58, +0x5c, +0x10, +0x18) */
static unsigned g_vlog_frames;
static void view_mark(int is3d, void *rt, const uint8_t *vs) {
    int i = g_vdepth - 1;
    if (i >= 0 && i < 64) {
        g_v3d[i] = (unsigned char)is3d; g_vscreen[i] = rt == NULL; g_vdme[i] = 0;
        if (vs && is3d) {
            const float *f = (const float *)(vs + 0x38), *o = (const float *)(vs + 0x40), *z = (const float *)(vs + 0x58);
            g_vsetup[i][0] = *f; g_vsetup[i][1] = o[0]; g_vsetup[i][2] = o[1]; g_vsetup[i][3] = o[2]; g_vsetup[i][4] = z[0]; g_vsetup[i][5] = z[1];
            g_vsetup[i][6] = (float)*(const int *)(vs + 0x10); g_vsetup[i][7] = (float)*(const int *)(vs + 0x18);
        }
    }
}
static void frame_3d_done(void) {
    g_vlog_frames++;
    photon_l4_main_tick();   /* L4: a path-traced bake applied between frames' 3D work (main thread) */
    photon_shadows_frame_3d_done();
    if (g_best_ord >= 0) g_main_ord = g_best_ord;
    g_best_ord = -1; g_best_dme = 0; g_screen_ord = 0;
}
/* T4.2: the shadow pass belongs after the main view's world and translucents but before the viewmodel, which GMod draws as a nested
 * screen 3D view with zNear 1 (the main view's is 3): run it just before that push; the main view's pop runs it when there was none */
static int g_shadow_done;
static void push3_shadow(void *rt, const uint8_t *vs) {
    if (g_vdepth == 0) { g_shadow_done = 0; return; }   /* a top-level 3D push: a new main view */
    if (g_vdepth == 1 && !g_shadow_done && rt == NULL && vs && *(const float *)(vs + 0x58) < 2.0f) {
        g_shadow_done = 1;
        photon_shadows_main_view_end();
    }
}
static void hk_push3a(void *s, void *a, void *b, void *c, void *d, void *e) { push3_shadow(c, a); view_push(); view_mark(1, c, a); water_view(); g_push3a(s, a, b, c, d, e); }
static void hk_push3b(void *s, void *a, void *b, void *c, void *d, void *e) { push3_shadow(c, a); view_push(); view_mark(1, c, a); water_view(); g_push3b(s, a, b, c, d, e); }
static void hk_push2(void *s, void *a, void *b, void *c, void *d, void *e) {
    if (g_vdepth == 0) frame_3d_done();
    view_push(); view_mark(0, c, NULL); water_view(); g_push2(s, a, b, c, d, e);
}
static void hk_pop(void *s, void *a, void *b, void *c, void *d, void *e) {
    int i = g_vdepth - 1;
    if (i >= 0 && i < 64 && g_v3d[i] && g_vscreen[i]) {
        int ord = g_screen_ord++;
        if ((g_vlog_frames % 600) == 300 && ord < 8)
            photon_log("p6 views: screen 3D view %d: model draws %u, fov %.1f origin %.1f %.1f %.1f znear %.2f zfar %.0f %gx%g (main last frame %d)", ord, g_vdme[i],
                       g_vsetup[i][0], g_vsetup[i][1], g_vsetup[i][2], g_vsetup[i][3], g_vsetup[i][4], g_vsetup[i][5], g_vsetup[i][6], g_vsetup[i][7], g_main_ord);
        if (g_vdme[i] > g_best_dme || g_best_ord < 0) { g_best_dme = g_vdme[i]; g_best_ord = ord; }
        /* the main view is the outermost screen 3D view: the 3D skybox, the water reflection / refraction and the viewmodel are pushed inside
         * it (p6 views log, 2026-11-08); its pop comes after them, before the engine's pop, with the main view's state still current */
        if (i == 0 && !g_shadow_done) photon_shadows_main_view_end();
        if (i == 0) g_shadow_done = 1;
    }
    view_pop(); g_pop(s, a, b, c, d, e); water_view();
}
static int has_bytes(const void *code, const uint8_t *pat, size_t n) {
    const uint8_t *c = code;
    for (size_t i = 0; i + n <= 48; i++) if (memcmp(c + i, pat, n) == 0) return 1;
    return 0;
}
static int bind_views(HMODULE en, void *(*ci)(const char *, int *)) {
    void *rv = ci("VEngineRenderView014", NULL);
    if (!rv) return 0;
    void **vt = *(void ***)rv;
    uintptr_t lo, hi;
    if (!photon_module_text(vt[0], &lo, &hi)) return 0;
    for (int i = 37; i <= 40; i++) if ((uintptr_t)vt[i] < lo || (uintptr_t)vt[i] >= hi) return 0;
    static const uint8_t j_b8[] = { 0xff, 0xa2, 0xb8, 0, 0, 0 }, c_b8[] = { 0xff, 0x92, 0xb8, 0, 0, 0 }, j_c8[] = { 0xff, 0xa2, 0xc8, 0, 0, 0 },
                         j_d0[] = { 0xff, 0xa0, 0xd0, 0, 0, 0 };
    if (!has_bytes(vt[37], j_b8, 6) || !has_bytes(vt[38], c_b8, 6) || !has_bytes(vt[39], j_c8, 6) || !has_bytes(vt[40], j_d0, 6)) {
        photon_log("p6q: IVRenderView slots 37-40 are not Push3DView / Push2DView / PopView in this build: queued replay off"); return 0;
    }
    size_t n = 0;
    while (n < 256 && (uintptr_t)vt[n] >= lo && (uintptr_t)vt[n] < hi) n++;
    void **copy = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(void *));
    if (!copy) return 0;
    copy[0] = vt[-1];
    memcpy(copy + 1, vt, n * sizeof(void *));
    g_push3a = (ViewFn)vt[37]; g_push3b = (ViewFn)vt[38]; g_push2 = (ViewFn)vt[39]; g_pop = (ViewFn)vt[40];
    copy[1 + 37] = (void *)hk_push3a; copy[1 + 38] = (void *)hk_push3b; copy[1 + 39] = (void *)hk_push2; copy[1 + 40] = (void *)hk_pop;
    InterlockedExchangePointer((PVOID volatile *)rv, copy + 1);
    (void)en;
    photon_log("p6q: IVRenderView %p: Push3DView / Push2DView / PopView hooked (view instances for queued replay)", rv);
    return 1;
}
static int g_views;   /* bind_views succeeded */

/* --- P6Q (analysis/27): queued mode. DrawModelExecute runs on the main thread, its draws later on MatQueue; the decision, the light push
 * and the sun stay on the main thread, and q_begin / q_end (queued around the engine's queued draw) set and evaluate the per-draw state
 * on MatQueue. Payloads live in a ring the main thread fills; MatQueue reads each about a frame later. */
static int g_p6q;   /* PHOTON_P6Q=1 (test) */
enum { NPAY = 32768 };
struct QGroup;
typedef struct { Key *k; float origin[3]; int sun; unsigned char desc[88]; struct QGroup *g; unsigned seq; float m34[3][4]; } Pay;
static Pay *g_pay;
static unsigned g_npay;
static volatile LONG64 g_q_learning, g_q_suppressed, g_q_fail, g_q_revoked;
static Pay *pay_new(void) {
    if (!g_pay) g_pay = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Pay) * NPAY);
    return g_pay ? &g_pay[g_npay++ & (NPAY - 1)] : NULL;
}
static void q_begin(void *obj, void *a, int mode) {
    (void)a;
    const Pay *p = obj;
    t_mode = mode; t_ours = t_other = 0; memset(t_why, 0, sizeof t_why); t_call_draws = 0; t_skinned = 0;
    memcpy(t_origin, p->origin, sizeof t_origin);
    t_sun = p->sun;
    memcpy(t_sun_desc, p->desc, sizeof t_sun_desc);
}
static void q_end(void *obj, void *a, int mode) {
    (void)a;
    Pay *p = obj;
    Key *k = p->k;
    if (mode == 1 && t_mode == 1) {
        if (t_other) { k->state = -1; g_keys_never++; note_key(k, "never (a stock pass, queued)"); }
        else if (t_ours && ++k->seen >= LEARN_OK && k->state == 0) { k->state = 1; g_keys_eligible++; note_key(k, "eligible (queued)"); }
    }
    if (mode == 2 && t_mode == 2 && photon_dmirror_replay_on()) {   /* every suppressed draw teaches the cubemap handle -> texture map */
        PhotonCubeCtx cc;
        photon_cube_replay_take(&cc);
        photon_cube_learn(&cc, photon_dmirror_cur_tex());
    }
    if (mode == 2 && t_mode == 2 && t_other && k->state > 0) {
        k->state = -1; g_revoked++; g_q_revoked++; g_keys_never++; note_key(k, "revoked (a stock pass inside a suppressed draw, queued)");
    }
    t_mode = 0;
}

/* --- P6Q step 3: replay in queued mode (analysis/27). Main thread: per view instance the first copy of a group runs the engine between
 * qr_begin / qr_end (recorded on MatQueue); later copies of a group whose model is known replayable queue qr_replay instead of the engine.
 * Group slots come from a ring; a queued call whose slot was reused since (sequence stamp) does nothing and is counted. */
typedef struct QGroup { Key *k; unsigned flags, vinst, seq; volatile int ok; unsigned mq_view; unsigned char *rec, *rl; PhotonCubeCtx cc; } QGroup;
enum { NQG = 2048, NQH = 4096 };
static QGroup *g_qg;
static unsigned g_nqg, g_qseq;
static struct { Key *k; unsigned flags, vinst, slot; } g_qh[NQH];
static volatile LONG64 g_qr_recorded, g_qr_bad, g_qr_replayed, g_qr_stale, g_qr_norec, g_qr_viewmis, g_qr_learn_ok, g_qr_learn_no;
static QGroup *qgroup(Key *k, unsigned flags, int *created) {
    unsigned v = view_inst();
    size_t h = (size_t)(((uintptr_t)k >> 4) * 0x9E3779B97F4A7C15ull ^ (uint64_t)flags * 31 ^ (uint64_t)v * 7919) & (NQH - 1);
    for (int i = 0; i < 16; i++, h = (h + 1) & (NQH - 1)) {
        if (g_qh[h].k == k && g_qh[h].flags == flags && g_qh[h].vinst == v) {
            QGroup *g = &g_qg[g_qh[h].slot];
            if (g->k == k && g->vinst == v) { *created = 0; return g; }
        }
        if (g_qh[h].vinst != v || !g_qh[h].k) {   /* a slot of an older view instance (or free): take it */
            if (!g_qg) {
                size_t rs = (photon_dmirror_rec_size() + 15) & ~(size_t)15, rls = (photon_vlg_relight_size() + 15) & ~(size_t)15;
                g_qg = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(QGroup) * NQG);
                unsigned char *pool = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (rs + rls) * NQG);
                if (!g_qg || !pool) return NULL;
                for (int n = 0; n < NQG; n++) { g_qg[n].rec = pool + (rs + rls) * (size_t)n; g_qg[n].rl = g_qg[n].rec + rs; }
            }
            unsigned slot = g_nqg++ & (NQG - 1);
            QGroup *g = &g_qg[slot];
            g->k = k; g->flags = flags; g->vinst = v; g->seq = ++g_qseq; g->ok = 0;
            g_qh[h].k = k; g_qh[h].flags = flags; g_qh[h].vinst = v; g_qh[h].slot = slot;
            *created = 1;
            return g;
        }
    }
    return NULL;
}
static void qr_begin(void *obj, void *a, int seq) {
    QGroup *g = obj;
    (void)a;
    if (g->seq != (unsigned)seq) { g_qr_stale++; return; }
    photon_dmirror_record_begin(g->rec);
    photon_vlg_relight_take(g->rl);   /* clears vlg's and cubemap's contexts: only this call's pass may fill them */
    photon_cube_replay_take(&g->cc);
}
static void qr_end(void *obj, void *a, int seq) {
    QGroup *g = obj;
    (void)a;
    if (g->seq != (unsigned)seq) { g_qr_stale++; return; }
    photon_dmirror_record_end();
    photon_vlg_relight_take(g->rl);
    photon_cube_replay_take(&g->cc);
    if (t_skinned) photon_dmirror_rec_drop(g->rec);
    int ok = photon_dmirror_rec_ok(g->rec);
    if (ok) { photon_cube_learn(&g->cc, photon_dmirror_rec_tex(g->rec)); g_qr_recorded++; }
    else g_qr_bad++;
    g->mq_view = photon_dmirror_view_gen();
    g->ok = ok;
    if (ok && g->k->rep == 0) { g->k->rep = 1; g_qr_learn_ok++; }
    else if (!ok && g->k->rep >= 0) { if (g->k->rep > 0) note_key(g->k, "queued replay: a recording failed, replay off for this model"); g->k->rep = -1; g_qr_learn_no++; }
}
static void qr_replay(void *obj, void *a, int seq) {
    Pay *p = obj;
    QGroup *g = p->g;
    (void)a;
    if (g->seq != (unsigned)seq) { g_qr_stale++; return; }
    if (!g->ok) { g_qr_norec++; return; }   /* the copy is lost for this frame (the model is barred from now on) */
    if (g->mq_view != photon_dmirror_view_gen()) g_qr_viewmis++;
    float psc[65][4];
    const float (*pp)[4] = NULL;
    memcpy(psc, photon_dmirror_rec_psc(g->rec), sizeof psc);
    if (!g_norelight && photon_vlg_relight(g->rl, p->origin, p->sun ? p->desc : NULL, psc)) pp = (const float (*)[4])psc;
    int ntex = 0, slot[2];
    void *tex[2];
    if (!g_norelight && !photon_cube_replay(&g->cc, p->origin, &ntex, slot, tex, psc)) ntex = 0;   /* no texture learned: the recorded one */
    if (photon_dmirror_replay(g->rec, (const float (*)[4])p->m34, pp, ntex, slot, tex)) g_qr_replayed++;
}

/* PHOTON_TEST_RELIGHTCHECK=1 in queued mode: the main thread sends one replayable copy in 64 through the engine; on MatQueue qr_chkb computes
 * what qr_replay would have uploaded for it and records the engine's draw, qr_chke compares (relight_compare, as in sync mode) */
static volatile int g_qchk_active;
static void qr_chkb(void *obj, void *a, int seq) {
    Pay *p = obj;
    QGroup *g = p->g;
    (void)a;
    g_qchk_active = 0;
    if (g->seq != (unsigned)seq || !g->ok || !g_chk_rec) return;
    memcpy(g_chk_psc, photon_dmirror_rec_psc(g->rec), sizeof g_chk_psc);
    g_chk_ntex = 0;
    if (!photon_vlg_relight(g->rl, p->origin, p->sun ? p->desc : NULL, g_chk_psc)) return;
    if (!photon_cube_replay(&g->cc, p->origin, &g_chk_ntex, g_chk_slot, g_chk_tex, g_chk_psc)) g_chk_ntex = 0;
    photon_dmirror_record_begin(g_chk_rec);
    PhotonCubeCtx z;
    photon_cube_replay_take(&z);
    g_qchk_active = 1;
}
static void qr_chke(void *obj, void *a, int seq) {
    (void)obj; (void)a; (void)seq;
    if (!g_qchk_active) return;
    g_qchk_active = 0;
    photon_dmirror_record_end();
    PhotonCubeCtx cc;
    photon_cube_replay_take(&cc);
    if (photon_dmirror_rec_ok(g_chk_rec)) { photon_cube_learn(&cc, photon_dmirror_rec_tex(g_chk_rec)); relight_compare(); }
}

static void dme_p6(void *self, void *state, const uint8_t *info, void *bones) {
    uint8_t *flag = (uint8_t *)self + OFF_SUPPRESS;
    g_draws++;
    if (!info || *flag) { if (info && *flag) g_lua_suppressed++; g_dme(self, state, info, bones); return; }
    if (*(const uint32_t *)(info + MRI_FLAGS) & P6_SKIP_FLAGS) { g_skip_flags++; g_dme(self, state, info, bones); return; }
    Key *k = key_of(*(const uintptr_t *)(info + MRI_MODEL), *(const int *)(info + MRI_SKIN), *(const int *)(info + MRI_BODY));
    int queued = g_draw_tid != GetCurrentThreadId();   /* the draws run on another thread: the material queue (P6Q) */
    if (!k || k->state < 0 || (queued && !g_p6q)) { g_dme(self, state, info, bones); return; }
    if (queued) {
        Pay *p = pay_new();
        const float *lo = *(const float *const *)(info + MRI_LORIGIN);
        if (!lo) lo = (const float *)info;
        int mode = k->state == 0 ? 1 : 2;
        if (!p || (mode == 2 && !photon_amb_covers(lo))) { if (mode == 2) g_not_covered++; g_dme(self, state, info, bones); return; }
        p->k = k;
        memcpy(p->origin, lo, sizeof p->origin);
        p->sun = mode == 2 ? photon_p6sun_light(lo, p->desc) : 0;
        QGroup *rec = NULL;   /* step 3: this copy records its group on MatQueue */
        if (mode == 2 && g_views && photon_dmirror_replay_on() && k->rep >= 0) {
            int created = 0;
            QGroup *g = qgroup(k, *(const uint32_t *)(info + MRI_FLAGS), &created);
            const float (*m34)[4] = *(const float (*const *)[4])(info + MRI_M2W);
            if (g && !created && k->rep == 1 && m34) {   /* a later copy of a replayable group in this view: no engine call */
                memcpy(p->m34, m34, sizeof p->m34);
                p->g = g; p->seq = g->seq;
                if (g_relcheck && (++g_chk_tick & 63) == 0 &&
                    (g_chk_rec || (g_chk_rec = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, photon_dmirror_rec_size())))) {
                    int qchk = photon_mq_queue(q_begin, p, NULL, 2) && photon_mq_queue(qr_chkb, p, NULL, (int)g->seq);   /* the engine draws it, checked */
                    if (qchk) {
                        g_suppressed++; g_q_suppressed++; g_sunlit += p->sun;
                        if (g_sr) { g_sr_ambient(g_sr, &k_dim_cube[0][0]); g_sr_lights(g_sr, 0, NULL); }
                        *flag = 1;
                        g_dme(self, state, info, bones);
                        *flag = 0;
                        if (!photon_mq_queue(qr_chke, p, NULL, 0) || !photon_mq_queue(q_end, p, NULL, 2)) g_q_fail++;
                        return;
                    }
                }
                if (photon_mq_queue(qr_replay, p, NULL, (int)g->seq)) { g_replayed++; g_sunlit += p->sun; return; }
            }
            if (g && created) rec = g;
        }
        if (!photon_mq_queue(q_begin, p, NULL, mode)) { g_q_fail++; g_dme(self, state, info, bones); return; }
        if (rec && !photon_mq_queue(qr_begin, rec, NULL, (int)rec->seq)) rec = NULL;
        if (mode == 1) { g_learning++; g_q_learning++; g_dme(self, state, info, bones); }
        else {
            g_suppressed++; g_q_suppressed++; g_sunlit += p->sun;
            if (g_sr) { g_sr_ambient(g_sr, &k_dim_cube[0][0]); g_sr_lights(g_sr, 0, NULL); }
            *flag = 1;
            g_dme(self, state, info, bones);
            *flag = 0;
        }
        if (rec && !photon_mq_queue(qr_end, rec, NULL, (int)rec->seq)) g_q_fail++;
        if (!photon_mq_queue(q_end, p, NULL, mode)) g_q_fail++;
        return;
    }
    if (k->state == 0) {   /* learning: run as stock, look at what the passes were */
        g_learning++;
        t_mode = 1; t_ours = t_other = 0; memset(t_why, 0, sizeof t_why);
        g_dme(self, state, info, bones);
        t_mode = 0;
        if (t_other) { k->state = -1; g_keys_never++; note_key(k, "never (a stock pass)"); }
        else if (t_ours && ++k->seen >= LEARN_OK) { k->state = 1; g_keys_eligible++; note_key(k, "eligible"); }
        return;
    }
    const float *lo = *(const float *const *)(info + MRI_LORIGIN);
    if (!lo) lo = (const float *)info;   /* origin; the engine would add pLightingOffset, which rigid props don't carry */
    if (!photon_amb_covers(lo)) { g_not_covered++; g_dme(self, state, info, bones); return; }
    memcpy(t_origin, lo, sizeof t_origin);
    if (g_dupskip) {
        unsigned fl = *(const uint32_t *)(info + MRI_FLAGS);
        size_t h = (size_t)((k->model >> 4) * 0x9E3779B97F4A7C15ull ^ (uint64_t)k->skin * 31 ^ (uint64_t)k->body * 131 ^ fl) & 8191;
        for (int i = 0; i < 32; i++, h = (h + 1) & 8191) {
            DupKey *d = &g_dup[h];
            if (d->frame != g_dup_frame) { d->model = k->model; d->skin = k->skin; d->body = k->body; d->flags = fl; d->frame = g_dup_frame; break; }
            if (d->model == k->model && d->skin == k->skin && d->body == k->body && d->flags == fl) { g_dup_skipped++; return; }
        }
    }
    Group *grp = NULL;
    int chk = 0;
    if (photon_dmirror_replay_on()) {
        unsigned fl = *(const uint32_t *)(info + MRI_FLAGS), view = photon_dmirror_view_gen();
        size_t h = (size_t)((k->model >> 4) * 0x9E3779B97F4A7C15ull ^ (uint64_t)k->skin * 31 ^ (uint64_t)k->body * 131 ^ fl ^ (uint64_t)view * 7919) & (NGROUP - 1);
        if (g_rec_frame != g_dup_frame) { g_rec_frame = g_dup_frame; g_rec_used = 0; }
        for (int i = 0; i < 16; i++, h = (h + 1) & (NGROUP - 1)) {
            Group *gq = &g_grp[h];
            if (gq->frame == g_dup_frame && gq->model == k->model && gq->skin == k->skin && gq->body == k->body && gq->flags == fl && gq->view == view) {
                grp = gq; break;
            }
            if (gq->frame != g_dup_frame) {   /* a free slot this frame: a new group, recorded by this copy */
                size_t rs = (photon_dmirror_rec_size() + 15) & ~(size_t)15, rls = (photon_vlg_relight_size() + 15) & ~(size_t)15;
                if (!g_rec_pool) g_rec_pool = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (rs + rls) * NGROUP);
                if (!g_rec_pool || g_rec_used >= NGROUP) { g_grp_full++; break; }
                gq->model = k->model; gq->skin = k->skin; gq->body = k->body; gq->flags = fl; gq->view = view; gq->frame = g_dup_frame;
                gq->rec = g_rec_pool + (rs + rls) * (size_t)g_rec_used++;
                gq->rl = gq->rec + rs;
                grp = gq;
                photon_dmirror_record_begin(gq->rec);
                photon_vlg_relight_take(gq->rl);   /* clears vlg's and cubemap's contexts: only this call's pass may fill them */
                photon_cube_replay_take(&gq->cc);
                break;
            }
        }
        const float (*m34)[4] = *(const float (*const *)[4])(info + MRI_M2W);
        if (grp && photon_dmirror_rec_ok(grp->rec) && m34) {
            /* this copy's own lighting: the sun at its origin and the block's origin-dependent part (vlg.c), in a copy of the recorded c0..c64 */
            float psc[65][4];
            const float (*pp)[4] = NULL;
            memcpy(psc, photon_dmirror_rec_psc(grp->rec), sizeof psc);
            int sun = g_norelight ? 0 : photon_p6sun_light(t_origin, t_sun_desc);
            if (!g_norelight && photon_vlg_relight(grp->rl, t_origin, sun ? t_sun_desc : NULL, psc)) pp = (const float (*)[4])psc;
            int ntex = 0, slot[2];
            void *tex[2];
            int cube_ok = g_norelight || photon_cube_replay(&grp->cc, t_origin, &ntex, slot, tex, psc);
            if (!g_chk_rec) g_chk_rec = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, photon_dmirror_rec_size());
            if (!cube_ok && g_chk_rec) {
                chk = 2;   /* the copy's cubemap texture is not known yet: the engine draws it, recorded, and cubemap.c learns from it */
                g_cube_fallback++;
            } else if (pp && g_relcheck && (++g_chk_tick & 63) == 0 && g_chk_rec) {
                memcpy(g_chk_psc, psc, sizeof psc);   /* the engine draws this copy below, recorded into g_chk_rec */
                g_chk_ntex = ntex; memcpy(g_chk_slot, slot, sizeof slot); memcpy(g_chk_tex, tex, sizeof tex);
                chk = 1;
            } else if (cube_ok && photon_dmirror_replay(grp->rec, m34, pp, ntex, slot, tex)) { g_replayed++; g_sunlit += sun; return; }
        }
        if (grp && photon_dmirror_rec_ok(grp->rec)) grp = NULL;   /* replay failed or no matrix: the engine draws it, nothing recorded */
        if (chk) { photon_dmirror_record_begin(g_chk_rec); PhotonCubeCtx z; photon_cube_replay_take(&z); }
    }
    g_suppressed++;
    t_sun = photon_p6sun_light(t_origin, t_sun_desc);
    g_sunlit += t_sun;
    t_mode = 2; t_ours = t_other = 0; memset(t_why, 0, sizeof t_why); t_call_draws = 0; t_skinned = 0;
    if (g_sr) { g_sr_ambient(g_sr, &k_dim_cube[0][0]); g_sr_lights(g_sr, 0, NULL); }
    *flag = 1;
    g_dme(self, state, info, bones);
    *flag = 0;
    t_mode = 0;
    if (grp) {
        photon_dmirror_record_end();
        photon_vlg_relight_take(grp->rl);
        photon_cube_replay_take(&grp->cc);
        if (t_skinned) photon_dmirror_rec_drop(grp->rec);
        if (photon_dmirror_rec_ok(grp->rec)) photon_cube_learn(&grp->cc, photon_dmirror_rec_tex(grp->rec));
    }
    if (chk) {
        photon_dmirror_record_end();
        PhotonCubeCtx cc;
        photon_cube_replay_take(&cc);
        if (photon_dmirror_rec_ok(g_chk_rec)) { photon_cube_learn(&cc, photon_dmirror_rec_tex(g_chk_rec)); if (chk == 1) relight_compare(); }
    }
    if (!grp && !chk && photon_dmirror_replay_on()) {   /* every other suppressed draw teaches the cubemap handle -> texture map too */
        PhotonCubeCtx cc;
        photon_cube_replay_take(&cc);
        photon_cube_learn(&cc, photon_dmirror_cur_tex());
    }
    if (t_other) { k->state = -1; g_revoked++; g_keys_never++; note_key(k, "revoked (a stock pass inside a suppressed draw)"); }
}

/* The hook: P6 (when wanted) inside the T4.7 cull ceiling's query (when on); the ceiling sees every model draw on the draw thread except the
 * flagged ones (shadow depth and the like) */
static void hook_dme(void *self, void *state, const uint8_t *info, void *bones) {
    int h = 0;
    if (g_vdepth > 0 && g_vdepth <= 64) g_vdme[g_vdepth - 1]++;   /* T4.2: model draws per view (main thread) */
    photon_shadows_dme(state, info);   /* T4.2 stage 1d: the RTT switch for the classes Photon shadows */
    if (g_mqtest) photon_mq_test_draw(0);
    photon_clprobe_draw(info);
    if (info) photon_lod_note_model(*(const void *const *)(info + 32));   /* F3: ModelRenderInfo_t pModel (+32, cull.c MRI_MODEL) */
    /* T4.7b: a draw of the outermost screen 3D view (not the skybox, water views, viewmodel, shadow depth, monitors) hidden behind the world */
    static long long hz_all, hz_main;
    if (info && photon_hiz_on() && ++hz_all % 20000 == 10000)
        photon_log("hiz: DrawModelExecute %lld, in the main view %lld, skipped %lld (view depth now %d, 3d %d, screen %d)", hz_all, hz_main, g_hiz_skipped,
                   g_vdepth, g_vdepth > 0 ? g_v3d[g_vdepth - 1] : -1, g_vdepth > 0 ? g_vscreen[g_vdepth - 1] : -1);
    if (info && photon_hiz_on() && g_vdepth == 1 && g_v3d[0] && g_vscreen[0] && !(*(const uint32_t *)(info + MRI_FLAGS) & P6_SKIP_FLAGS) && ++hz_main) {
        const void *mdl = *(const void *const *)(info + 32), *m2w = *(const void *const *)(info + 40);
        float mn[3], mx[3];
        static long long nob;
        int hb = mdl && m2w && photon_cull_bounds(mdl, mn, mx);
        if (!hb && ++nob % 20000 == 1) photon_log("hiz: no bounds (%lld; model %p, matrix %p)", nob, mdl, m2w);
        int hid = hb && photon_hiz_occluded(mn, mx, (const float (*)[4])m2w);
        {   /* for photon-a0's C3d decision: animated models (studiohdr numbones > 1: NPCs, ragdolls, players) apart from rigid props */
            static long long t_anim, h_anim, t_rigid, h_rigid;
            const uint8_t *hdr = state ? *(const uint8_t *const *)state : NULL;   /* DrawModelState_t m_pStudioHdr */
            int anim = hdr && *(const uint32_t *)hdr == 0x54534449u && *(const int *)(hdr + 156) > 1;   /* "IDST", numbones */
            if (hb) { if (anim) { t_anim++; h_anim += hid; } else { t_rigid++; h_rigid += hid; } }
            if (hb && (t_anim + t_rigid) % 20000 == 0)
                photon_log("hiz: by kind: animated (NPCs, ragdolls, players) %lld tests, %lld hidden (%.1f%%); rigid %lld tests, %lld hidden (%.1f%%)",
                           t_anim, h_anim, t_anim ? 100.0 * (double)h_anim / (double)t_anim : 0.0, t_rigid, h_rigid, t_rigid ? 100.0 * (double)h_rigid / (double)t_rigid : 0.0);
        }
        if (hb) photon_hiz_note_entity(*(const int *)(info + 68), hid);   /* ModelRenderInfo_t entity_index (cull.c MRI_ENTITY) */
        if (hid) { g_hiz_skipped++; return; }
    }
    if (photon_cull_on() && info && !(*(const uint32_t *)(info + MRI_FLAGS) & P6_SKIP_FLAGS) && g_draw_tid == GetCurrentThreadId()) {
        h = photon_cull_begin(info);
        if (h < 0) return;
    }
    if (g_want) dme_p6(self, state, info, bones);
    else g_dme(self, state, info, bones);
    photon_cull_end(h);
    if (g_mqtest) photon_mq_test_draw(1);
}

/* vlg.c, from RenderPass on the draw thread: one pass of ours (lit by Photon) or not */
void photon_p6_pass(int code, int vsidx) {
    g_draw_tid = GetCurrentThreadId();
    if (!t_mode) return;
    if (code == P6_PASS_LIT) t_ours++;
    else { t_other++; if (code > 0 && code < 5) t_why[code]++; if (code == P6_PASS_UNLIT) t_idx = vsidx; }
}
int photon_p6_suppressed(void) { return t_mode == 2; }
void photon_p6_skinned(void) { if (t_mode == 2) t_skinned = 1; }
int photon_p6_is_skinned(void) { return t_skinned; }
int photon_p6_call_draws_inc(void) { return t_call_draws++; }
void photon_p6_origin(float o[3]) { memcpy(o, t_origin, sizeof t_origin); }
int photon_p6_sun(void *desc) { if (t_mode != 2 || !t_sun) return 0; memcpy(desc, t_sun_desc, sizeof t_sun_desc); return 1; }


static int bind_engine(void) {
    HMODULE en = GetModuleHandleA("engine.dll");
    if (!en) return 0;
    typedef void *(*CiFn)(const char *, int *);
    CiFn ci = (CiFn)(void *)GetProcAddress(en, "CreateInterface");
    void *mr = ci ? ci("VEngineModel016", NULL) : NULL;
    if (!mr) return 0;
    void **vt = *(void ***)mr;
    uintptr_t lo, hi;
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt[0], &m) || m != en) {
        photon_log("p6: VEngineModel016's vtable is not engine.dll's: off"); return -1;
    }
    /* the suppress byte: SuppressEngineLighting (slot 23) starts mov [rcx+0x48],dl (analysis/15c) */
    const uint8_t *s = (const uint8_t *)vt[SLOT_SUPPRESS];
    if (!(s[0] == 0x88 && s[1] == 0x51 && s[2] == OFF_SUPPRESS)) {
        photon_log("p6: slot %d starts %02x %02x %02x, not mov [rcx+0x48],dl: off (different build)", SLOT_SUPPRESS, s[0], s[1], s[2]); return -1;
    }
    HMODULE sm = GetModuleHandleA("studiorender.dll");
    CiFn sci = sm ? (CiFn)(void *)GetProcAddress(sm, "CreateInterface") : NULL;
    void *sr = sci ? sci("VStudioRender025", NULL) : NULL;
    if (sr) {
        void **svt = *(void ***)sr;
        uintptr_t slo, shi;
        if (photon_module_text(svt[0], &slo, &shi) && (uintptr_t)svt[SR_SLOT_AMBIENT] >= slo && (uintptr_t)svt[SR_SLOT_AMBIENT] < shi &&
            (uintptr_t)svt[SR_SLOT_LOCALLIGHTS] >= slo && (uintptr_t)svt[SR_SLOT_LOCALLIGHTS] < shi) {
            g_sr = sr; g_sr_ambient = (SrAmbientFn)svt[SR_SLOT_AMBIENT]; g_sr_lights = (SrLightsFn)svt[SR_SLOT_LOCALLIGHTS];
        }
    }
    if (!g_sr) photon_log("p6: no VStudioRender025 light calls: suppressed draws keep a stale light state");
    size_t n = 0;
    if (!photon_module_text(vt[0], &lo, &hi)) return -1;
    while (n < 256 && (uintptr_t)vt[n] >= lo && (uintptr_t)vt[n] < hi) n++;
    if (n <= SLOT_SUPPRESS) { photon_log("p6: CModelRender vtable has %zu slots: off", n); return -1; }
    void **copy = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(void *));
    if (!copy) return -1;
    copy[0] = vt[-1];
    memcpy(copy + 1, vt, n * sizeof(void *));
    g_real_vt = vt; g_vt = copy + 1; g_obj = mr;
    g_dme = (DmeFn)vt[SLOT_DME];
    g_vt[SLOT_DME] = (void *)hook_dme;
    if ((g_p6q && photon_dmirror_replay_on_wanted()) || photon_shadows_on() || photon_waterq_possible()) g_views = bind_views(en, ci);
    InterlockedExchangePointer((PVOID volatile *)mr, g_vt);   /* the private table is complete before this one store */
    photon_log("p6: TEST PHOTON_P6=1: CModelRender %p, DrawModelExecute hooked (vtable copy, %zu slots); suppress byte +0x%x verified", mr, n,
               OFF_SUPPRESS);
    return 1;
}

int photon_p6_wanted(void) { return g_want; }

void photon_p6_setup(void) {
    char e[8];
    /* on by default (2026-11-07); PHOTON_P6=0 turns P6 off, and with it the queued path and replay's use of it */
    g_want = !(GetEnvironmentVariableA("PHOTON_P6", e, sizeof e) > 0 && e[0] == '0');
    photon_p6sun_setup(g_want);
    g_dupskip = g_want && GetEnvironmentVariableA("PHOTON_TEST_DUPSKIP", e, sizeof e) > 0 && e[0] == '1';
    g_relcheck = GetEnvironmentVariableA("PHOTON_TEST_RELIGHTCHECK", e, sizeof e) > 0 && e[0] == '1';
    g_p6q = !(GetEnvironmentVariableA("PHOTON_P6Q", e, sizeof e) > 0 && e[0] == '0');   /* on by default; PHOTON_P6Q=0 off */
    g_norelight = GetEnvironmentVariableA("PHOTON_TEST_NORELIGHT", e, sizeof e) > 0 && e[0] == '1';   /* A/B: replayed copies keep the first copy's lighting */
    photon_cull_setup();
    photon_clprobe_setup();
    g_mqtest = GetEnvironmentVariableA("PHOTON_TEST_MQ", e, sizeof e) > 0 && e[0] == '1';
    g_hook = g_want || photon_cull_on() || g_mqtest || photon_clprobe_on() || photon_hiz_on();
    if (g_dupskip) photon_log("p6: TEST PHOTON_TEST_DUPSKIP=1: duplicate suppressed draws are skipped (instancing ceiling; the image is wrong)");
}

void photon_p6_frame(void) {
    g_dup_frame++;
    photon_clprobe_frame();
    if (!g_hook || g_bound || g_failed) return;
    int r = bind_engine();
    if (r > 0) g_bound = 1; else if (r < 0) g_failed = 1;
}

void photon_p6_restore(void) {
    photon_clprobe_restore();
    if (g_obj && *(void ***)g_obj == g_vt) InterlockedExchangePointer((PVOID volatile *)g_obj, g_real_vt);
}

void photon_p6_dump(const char *why) {
    photon_cull_dump(why);
    photon_clprobe_dump(why);
    photon_mq_dump(why);
    if (!g_want) return;
    photon_p6sun_dump(why);
    photon_log("p6 summary (%s): bound %d; draws %lld, suppressed %lld (sunlit %lld), learning %lld, not covered by the volume %lld, Lua-suppressed %lld,"
               " skipped by flags %lld; duplicates skipped (PHOTON_TEST_DUPSKIP) %lld; replayed (PHOTON_BATCH=replay) %lld, group table full %lld;"
               " models eligible %lld, never %lld (revoked after a stock pass inside a suppressed draw %lld)", why,
               g_bound, (long long)g_draws, (long long)g_suppressed, (long long)g_sunlit, (long long)g_learning, (long long)g_not_covered,
               (long long)g_lua_suppressed, (long long)g_skip_flags, (long long)g_dup_skipped, (long long)g_replayed, (long long)g_grp_full,
               (long long)g_keys_eligible, (long long)g_keys_never, (long long)g_revoked);
    if (g_p6q)
        photon_log("p6 queued (P6Q, %s): learning %lld, suppressed %lld, queue failures %lld, revoked %lld", why, (long long)g_q_learning,
                   (long long)g_q_suppressed, (long long)g_q_fail, (long long)g_q_revoked);
    if (g_p6q && g_views)
        photon_log("p6 queued replay (%s): views %u; groups recorded %lld (not replayable %lld), replays %lld; stale calls %lld, replays without a"
                   " recording %lld, view mismatches %lld; models replayable %lld, barred %lld", why, g_vcount, (long long)g_qr_recorded,
                   (long long)g_qr_bad, (long long)g_qr_replayed, (long long)g_qr_stale, (long long)g_qr_norec, (long long)g_qr_viewmis,
                   (long long)g_qr_learn_ok, (long long)g_qr_learn_no);
    if (g_relcheck)
        photon_log("p6 relight check (%s): copies checked %lld, equal %lld, differ %lld; largest difference %g; cubemap textures differing %lld", why,
                   (long long)g_chk_n, (long long)g_chk_ok, (long long)g_chk_bad, g_chk_maxd, (long long)g_chk_texbad);
    if (photon_dmirror_replay_on() || g_replayed)
        photon_log("p6 replay: copies drawn by the engine because their cubemap texture was not learned yet %lld", (long long)g_cube_fallback);
}
