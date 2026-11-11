/* T4.7 ceiling (test only, PHOTON_TEST_CULL): how many model draws are hidden, and what skipping them saves. See cull.h. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cull.h"
#include "d3dconst.h"
#include "frames.h"
#include "log.h"
#include "dmirror.h"

/* ModelRenderInfo_t (x64): entity_index at 68, instance (ModelInstanceHandle_t, uint16) at 84; flags at 64, the model at 32 */
enum { MRI_MODEL = 32, MRI_M2W = 40, MRI_FLAGS = 64, MRI_ENTITY = 68, MRI_INSTANCE = 84 };
enum { NSLOT = 8192 };

typedef struct {
    uint64_t key;
    IDirect3DQuery9 *q;
    uint64_t q_frame;      /* the frame the pending query was issued in, 0 = none pending */
    int hidden;            /* the last answered query drew no sample */
    int box;               /* T4.7a: the pending query is the bounding box (the model was skipped), not the model's own draw */
    int queued;            /* T4.7a: its box waits in g_q for the end of the frame */
    uint64_t seen;         /* the last frame this key was drawn or skipped */
} Slot;

static int g_mode;                     /* 0 off, 1 measure, N >= 2 skip hidden draws, re-testing every Nth frame */
static int g_real, g_showbox;          /* T4.7a PHOTON_CULL=1: hidden models skipped and re-tested by box queries; PHOTON_CULL_SHOWBOX=1 debug view */
enum { RETEST = 8 };                   /* a visible model's own draw is queried every RETEST frames (staggered by slot) */
/* T4.7a: boxes of skipped models, queried in photon_cull_pre_present when the frame's depth is complete */
enum { NQ = 4096 };
typedef struct { int slot; float m34[3][4], vp[4][4], mins[3], maxs[3]; void *ds; } Pending;
static Pending g_q[NQ];
static int g_nq;
static long long g_r_ds_changed;
static long long g_r_skipped, g_r_boxes, g_r_box_fail, g_r_draw_q, g_r_revealed, g_r_hidden_new, g_r_nobounds;
/* the engine's IVModelInfo::GetModelRenderBounds (VModelInfoClient006 slot 7, engine.dll+0x1e3080 in build 26229091; analysis/25) */
typedef void (*BoundsFn)(void *, const void *, float *, float *);
static void *g_mi;
static BoundsFn g_bounds;
enum { NB = 1024 };
static struct { uintptr_t model; float mins[3], maxs[3]; } g_bcache[NB];
static int bounds_of(uintptr_t model, float mins[3], float maxs[3]) {
    if (!g_bounds || !model) return 0;
    size_t h = (size_t)((model >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (NB - 1);
    if (g_bcache[h].model == model) { memcpy(mins, g_bcache[h].mins, 12); memcpy(maxs, g_bcache[h].maxs, 12); return 1; }
    g_bounds(g_mi, (const void *)model, mins, maxs);
    if (!(maxs[0] > mins[0] && maxs[1] > mins[1] && maxs[2] > mins[2])) return 0;
    g_bcache[h].model = model; memcpy(g_bcache[h].mins, mins, 12); memcpy(g_bcache[h].maxs, maxs, 12);
    return 1;
}
static void bind_bounds(void);
int photon_cull_bounds(const void *model, float mins[3], float maxs[3]) {   /* T4.7b (hiz.c): a model's render bounds, cached */
    static int tried;
    if (!g_bounds && !tried) { tried = 1; bind_bounds(); }
    return bounds_of((uintptr_t)model, mins, maxs);
}
static void bind_bounds(void) {
    HMODULE en = GetModuleHandleA("engine.dll");
    typedef void *(*CiFn)(const char *, int *);
    CiFn ci = en ? (CiFn)(void *)GetProcAddress(en, "CreateInterface") : NULL;
    void *mi = ci ? ci("VModelInfoClient006", NULL) : NULL;
    if (!mi) { photon_log("cull: no VModelInfoClient006: PHOTON_CULL off"); g_real = 0; return; }
    void *fn = (*(void ***)mi)[7];
    if ((uintptr_t)fn != (uintptr_t)en + 0x1e3080) {
        photon_log("cull: VModelInfoClient006 slot 7 is engine+0x%llx, not GetModelRenderBounds (+0x1e3080): PHOTON_CULL off",
                   (unsigned long long)((uintptr_t)fn - (uintptr_t)en));
        g_real = 0; return;
    }
    g_mi = mi; g_bounds = (BoundsFn)fn;
}
static Slot g_slot[NSLOT];
static IDirect3DDevice9 *g_qdev;       /* the device the queries belong to */
static int g_active;                   /* a query is open around the current DrawModelExecute */
static long long g_draws, g_queried, g_answers, g_hidden_answers, g_skipped, g_busy, g_full, g_create_fail, g_frames;
static long long g_f_draws, g_f_hidden;   /* this frame: keys asked, keys hidden by the last answer (for the per-frame mean) */
static double g_sum_hidden_frac;
static uint64_t g_last_frame;

static uint64_t key_of(const uint8_t *info) {
    int ent = *(const int *)(info + MRI_ENTITY);
    unsigned inst = *(const uint16_t *)(info + MRI_INSTANCE);
    uint64_t model = *(const uintptr_t *)(info + MRI_MODEL);
    unsigned flags = *(const uint32_t *)(info + MRI_FLAGS);
    return ((uint64_t)(uint32_t)ent << 32 | inst << 16 | (flags & 0xffff)) ^ (model >> 4) * 0x9E3779B97F4A7C15ull;
}
static Slot *slot_of(uint64_t key) {
    size_t h = (size_t)((key ^ key >> 31) * 0xBF58476D1CE4E5B9ull >> 51) & (NSLOT - 1);
    for (int i = 0; i < 32; i++, h = (h + 1) & (NSLOT - 1)) {
        Slot *s = &g_slot[h];
        if (s->key == key) return s;
        if (!s->key) { s->key = key; return s; }
    }
    return NULL;
}

static void release_all(void) {
    for (int i = 0; i < NSLOT; i++) if (g_slot[i].q) { IDirect3DQuery9_Release(g_slot[i].q); g_slot[i].q = NULL; g_slot[i].q_frame = 0; }
    g_qdev = NULL;
}

void photon_cull_setup(void) {
    char e[16];
    DWORD n = GetEnvironmentVariableA("PHOTON_TEST_CULL", e, sizeof e);
    g_mode = n > 0 && n < sizeof e ? atoi(e) : 0;
    if (g_mode < 0) g_mode = 0;
    g_real = GetEnvironmentVariableA("PHOTON_CULL", e, sizeof e) > 0 && e[0] == '1';
    g_showbox = GetEnvironmentVariableA("PHOTON_CULL_SHOWBOX", e, sizeof e) > 0 && e[0] == '1';
    if (g_real) { g_mode = 0; photon_log("cull: PHOTON_CULL=1: hidden models are skipped and re-tested with bounding-box queries (needs PHOTON_BATCH)%s",
                                          g_showbox ? "; PHOTON_CULL_SHOWBOX=1: every model's box is drawn (debug view)" : ""); }
    if (g_mode)
        photon_log("cull: TEST PHOTON_TEST_CULL=%d: an occlusion query around every model draw; %s", g_mode,
                   g_mode == 1 ? "measure only" : "draws hidden at the last answer are skipped, re-tested every Nth frame (artefacts expected)");
}
int photon_cull_on(void) { return g_mode != 0 || g_real; }

static int real_begin(const uint8_t *info);
int photon_cull_begin(const uint8_t *info) {
    if (g_real) return real_begin(info);
    if (!g_mode || !info || g_active) return 0;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return 0;
    if (d != g_qdev) { if (g_qdev) release_all(); g_qdev = d; }
    uint64_t f = photon_frame_count();
    if (f != g_last_frame) {
        if (g_f_draws) { g_sum_hidden_frac += (double)g_f_hidden / (double)g_f_draws; g_frames++; }
        g_f_draws = g_f_hidden = 0;
        g_last_frame = f;
    }
    g_draws++;
    Slot *s = slot_of(key_of(info));
    if (!s) { g_full++; return 0; }
    if (s->q && s->q_frame) {   /* the answer to the last query, if the GPU has it (never flushed for) */
        DWORD px = 0;
        HRESULT r = IDirect3DQuery9_GetData(s->q, &px, sizeof px, 0);
        if (r == S_OK) { s->hidden = px == 0; s->q_frame = 0; g_answers++; g_hidden_answers += s->hidden; }
        else if (r != S_FALSE) s->q_frame = 0;   /* device lost or an error: forget it */
    }
    if (s->seen != f) { g_f_draws++; g_f_hidden += s->hidden; }
    s->seen = f;
    if (g_mode >= 2 && s->hidden && f % (uint64_t)g_mode != 0) { g_skipped++; return -1; }
    if (s->q_frame) { g_busy++; return 0; }   /* still pending: no new query this time */
    if (!s->q && FAILED(IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_OCCLUSION, &s->q))) { s->q = NULL; g_create_fail++; return 0; }
    if (FAILED(IDirect3DQuery9_Issue(s->q, D3DISSUE_BEGIN))) return 0;
    s->q_frame = f;
    g_active = 1;
    g_queried++;
    return (int)(s - g_slot) + 1;
}
void photon_cull_end(int h) {
    if (h <= 0 || !g_active) return;
    g_active = 0;
    IDirect3DQuery9_Issue(g_slot[h - 1].q, D3DISSUE_END);
}
/* T4.7a: a hidden model is not drawn; its box is queried instead, and the model is drawn again from the visit after a box shows samples.
 * A visible model's own draw is queried every RETEST frames; no samples make it hidden. */
static int real_begin(const uint8_t *info) {
    if (!info || g_active) return 0;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d || !photon_dmirror_bound()) return 0;
    if (!g_bounds && g_real) { bind_bounds(); if (!g_real) return 0; photon_dmirror_on_zclear(photon_cull_pre_present); }
    if (d != g_qdev) { if (g_qdev) release_all(); g_qdev = d; }
    uint64_t f = photon_frame_count();
    g_draws++;
    const float (*m34)[4] = *(const float (*const *)[4])(info + MRI_M2W);
    float mins[3], maxs[3];
    int hb = m34 && bounds_of(*(const uintptr_t *)(info + MRI_MODEL), mins, maxs);
    if (!hb) g_r_nobounds++;
    if (g_showbox && hb) photon_dmirror_box_query(NULL, m34, mins, maxs, 0.0f, 1, NULL);
    Slot *s = slot_of(key_of(info));
    if (!s) { g_full++; return 0; }
    if (s->q && s->q_frame) {
        DWORD px = 0;
        HRESULT r = IDirect3DQuery9_GetData(s->q, &px, sizeof px, 0);
        if (r == S_OK) {
            int was = s->hidden;
            s->hidden = px == 0;
            if (was && !s->hidden) g_r_revealed++;
            if (!was && s->hidden) g_r_hidden_new++;
            s->q_frame = 0;
            g_answers++;
        } else if (r != S_FALSE) { s->q_frame = 0; s->hidden = 0; }
    }
    s->seen = f;
    if (s->hidden && hb) {   /* skipped: the box is queued, and queried before Present against the whole frame's depth */
        g_r_skipped++;
        if (!s->q_frame && !s->queued) {
            if (g_nq == NQ) { s->hidden = 0; g_r_box_fail++; return 0; }
            if (!s->q && FAILED(IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_OCCLUSION, &s->q))) { s->q = NULL; g_create_fail++; s->hidden = 0; return 0; }
            Pending *p = &g_q[g_nq++];
            p->slot = (int)(s - g_slot);
            memcpy(p->m34, m34, sizeof p->m34);
            memcpy(p->vp, photon_dmirror_viewproj(), sizeof p->vp);
            memcpy(p->mins, mins, sizeof p->mins); memcpy(p->maxs, maxs, sizeof p->maxs);
            p->ds = photon_dmirror_ds();
            s->queued = 1;
        }
        return -1;
    }
    s->hidden = 0;
    if (s->q_frame || ((f + (uint64_t)(s - g_slot)) % RETEST) != 0 || !hb) return 0;
    if (!s->q && FAILED(IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_OCCLUSION, &s->q))) { s->q = NULL; g_create_fail++; return 0; }
    if (FAILED(IDirect3DQuery9_Issue(s->q, D3DISSUE_BEGIN))) return 0;
    s->q_frame = f; s->box = 0;
    g_active = 1;
    g_r_draw_q++;
    return (int)(s - g_slot) + 1;
}

/* Before a depth clear (dmirror.c's Clear hook: Source clears depth between views and before the view model and 2D work, so by Present the
 * frame's depth is gone) and before the real Present: the queued boxes against the depth surface they were drawn with (a box whose
 * surface is no longer bound is dropped, and its model drawn again) */
void photon_cull_pre_present(void) {
    if (!g_real || !g_nq) return;
    void *ds = photon_dmirror_ds();
    uint64_t f = photon_frame_count();
    for (int i = 0; i < g_nq; i++) {
        Pending *p = &g_q[i];
        Slot *s = &g_slot[p->slot];
        s->queued = 0;
        if (p->ds != ds) { s->hidden = 0; g_r_ds_changed++; continue; }
        float sz = (p->maxs[0] - p->mins[0]) + (p->maxs[1] - p->mins[1]) + (p->maxs[2] - p->mins[2]);
        if (s->q && photon_dmirror_box_query(s->q, (const float (*)[4])p->m34, p->mins, p->maxs, 1.0f + 0.02f * sz, 0, (const float (*)[4])p->vp)) {
            s->q_frame = f; s->box = 1; g_r_boxes++;
        } else { s->hidden = 0; g_r_box_fail++; }
    }
    g_nq = 0;
}

void photon_cull_device_lost(void) { if (g_mode) release_all(); }

void photon_cull_dump(const char *why) {
    if (g_real) {
        photon_log("cull (%s): PHOTON_CULL model draws %lld: skipped %lld (%.1f%%), box queries %lld (could not run %lld), own-draw queries %lld,"
                   " answers %lld; became hidden %lld, revealed by a box %lld; boxes dropped (depth surface changed) %lld; no bounds %lld, slot table full %lld, query creation failed %lld",
                   why ? why : "?", g_draws, g_r_skipped, g_draws ? 100.0 * g_r_skipped / g_draws : 0.0, g_r_boxes, g_r_box_fail, g_r_draw_q, g_answers,
                   g_r_hidden_new, g_r_revealed, g_r_ds_changed, g_r_nobounds, g_full, g_create_fail);
        return;
    }
    if (!g_mode) return;
    photon_log("cull (%s): model draws %lld, queried %lld (still pending at the next draw %lld), answers %lld, hidden %lld (%.1f%%); skipped %lld;"
               " mean hidden share per frame %.1f%% over %lld frames; slot table full %lld, query creation failed %lld",
               why ? why : "?", g_draws, g_queried, g_busy, g_answers, g_hidden_answers, g_answers ? 100.0 * g_hidden_answers / g_answers : 0.0, g_skipped,
               g_frames ? 100.0 * g_sum_hidden_frac / g_frames : 0.0, g_frames, g_full, g_create_fail);
}
