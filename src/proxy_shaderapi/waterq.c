/* W1: occlusion queries around water draws, summed per frame. See waterq.h. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include "waterq.h"
#include "d3dconst.h"
#include "frames.h"
#include "log.h"
#include "mqueue.h"

enum { NF = 8, NQW = 32 };   /* frames in flight; water draws queried per frame (beyond that the frame counts as water seen) */
typedef struct {
    uint64_t frame;          /* 0 = free */
    IDirect3DQuery9 *q[NQW];
    int nq, answered, overflow, failed;
    uint32_t draws;
    uint64_t samples;
} Rec;
static Rec g_rec[NF];
static IDirect3DDevice9 *g_dev;
static int g_active = -1;    /* -1: not decided yet; 0 idle; 1 on */
static int g_log;
static Rec *g_open;          /* the record of the query between begin and end */
static SRWLOCK g_pub_lock = SRWLOCK_INIT;
static uint64_t g_pub_frame, g_pub_samples;
static uint32_t g_pub_draws;
static volatile LONG g_wanted;
static int g_views_known, g_main_view;   /* draw thread: photon_waterq_view was ever called; the top view is the main screen view */
static volatile LONG g_possible;
static long long g_n_draws, g_n_other, g_n_frames, g_n_seen, g_n_overflow, g_n_failed, g_n_dropped;

static void release_all(void) {
    for (int i = 0; i < NF; i++) {
        for (int k = 0; k < NQW; k++) if (g_rec[i].q[k]) { IDirect3DQuery9_Release(g_rec[i].q[k]); g_rec[i].q[k] = NULL; }
        g_rec[i].frame = 0;
    }
    g_open = NULL;
}

static int active(void) {
    if (g_active < 0) {
        char e[4];
        g_log = GetEnvironmentVariableA("PHOTON_WATERQ", e, sizeof e) > 0 && e[0] == '1';
        if (g_log) g_wanted = 1;
        g_active = 0;
    }
    if (!g_active && g_wanted) { g_active = 1; photon_log("waterq: on (%s)", g_log ? "PHOTON_WATERQ=1" : "asked by photon_cpu"); }
    return g_active;
}

void photon_waterq_set_possible(void) { InterlockedExchange(&g_possible, 1); }
int photon_waterq_possible(void) { char e[4]; return g_possible || (GetEnvironmentVariableA("PHOTON_WATERQ", e, sizeof e) > 0 && e[0] == '1'); }

static void set_view(void *obj, void *a, int main) { (void)obj; (void)a; g_views_known = 1; g_main_view = main; }
void photon_waterq_view(int main) {
    static int env = -1;
    if (env < 0) { char e[4]; env = GetEnvironmentVariableA("PHOTON_WATERQ", e, sizeof e) > 0 && e[0] == '1'; if (env) InterlockedExchange(&g_wanted, 1); }
    if (!g_wanted) return;
    if (!photon_mq_queue(set_view, NULL, NULL, main)) set_view(NULL, NULL, main);
}

void photon_waterq_begin(void) {
    if (!active()) return;
    if (g_views_known && !g_main_view) { g_n_other++; return; }
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return;
    if (d != g_dev) { if (g_dev) release_all(); g_dev = d; }
    uint64_t f = photon_frame_count() + 1;   /* the frame being drawn: frame_count advances at its Present */
    Rec *r = &g_rec[f % NF];
    if (r->frame != f) {
        if (r->frame && r->answered < r->nq) g_n_dropped++;   /* never answered within NF frames: it simply is not published */
        r->frame = f; r->nq = r->answered = r->overflow = r->failed = 0; r->draws = 0; r->samples = 0;
    }
    r->draws++;
    g_n_draws++;
    if (r->nq == NQW) { r->overflow = 1; return; }
    IDirect3DQuery9 **q = &r->q[r->nq];
    if (!*q && FAILED(IDirect3DDevice9_CreateQuery(d, D3DQUERYTYPE_OCCLUSION, q))) { *q = NULL; r->failed = 1; return; }
    if (FAILED(IDirect3DQuery9_Issue(*q, D3DISSUE_BEGIN))) { r->failed = 1; return; }
    g_open = r;
}

void photon_waterq_end(void) {
    Rec *r = g_open;
    if (!r) return;
    g_open = NULL;
    if (FAILED(IDirect3DQuery9_Issue(r->q[r->nq], D3DISSUE_END))) { r->failed = 1; return; }
    r->nq++;
}

void photon_waterq_present(void) {
    if (g_active != 1) return;
    uint64_t now = photon_frame_count();   /* frames up to and including this one are complete */
    uint64_t best = 0; uint32_t bd = 0; uint64_t bs = 0; int bo = 0, bf = 0;
    /* frames that were drawn without any water draw have no record: the just-finished frame is published with draws 0 if it has none */
    Rec *cur = &g_rec[now % NF];
    if (cur->frame != now) { cur->frame = now; cur->nq = cur->answered = cur->overflow = cur->failed = 0; cur->draws = 0; cur->samples = 0; }
    for (int i = 0; i < NF; i++) {
        Rec *r = &g_rec[i];
        if (!r->frame || r->frame > now) continue;
        while (r->answered < r->nq) {
            DWORD px = 0;
            HRESULT h = IDirect3DQuery9_GetData(r->q[r->answered], &px, sizeof px, 0);
            if (h == S_FALSE) break;
            if (h != S_OK) { r->failed = 1; px = 1; }
            r->samples += px;
            r->answered++;
        }
        if (r->answered == r->nq && r->frame > best) {
            best = r->frame; bd = r->draws; bo = r->overflow; bf = r->failed;
            bs = (r->overflow || r->failed) ? UINT64_MAX : r->samples;   /* unmeasured draws count as water seen */
        }
    }
    if (best) {
        AcquireSRWLockExclusive(&g_pub_lock);
        if (best > g_pub_frame) {
            g_pub_frame = best; g_pub_draws = bd; g_pub_samples = bs;
            g_n_frames++;
            if (bd && bs) g_n_seen++;
            g_n_overflow += bo; g_n_failed += bf;
            int state = !bd ? 0 : bs ? 2 : 1;   /* no water drawn / drawn but hidden / seen */
            static int last = -1, nlog;
            if (g_log && state != last && nlog < 200) {
                nlog++;
                photon_log("waterq: frame %llu: %s (draws %u, samples %llu)", best, state == 0 ? "no water drawn" : state == 1 ? "water drawn, all hidden" : "water seen",
                           bd, bs);
            }
            last = state;
        }
        ReleaseSRWLockExclusive(&g_pub_lock);
        for (int i = 0; i < NF; i++)   /* published and older: free */
            if (g_rec[i].frame && g_rec[i].frame <= best && g_rec[i].answered == g_rec[i].nq) g_rec[i].frame = 0;
    }
    if (g_log && now % 600 == 0)
        photon_log("waterq: frame %llu: water draws %lld (outside the main view, not counted: %lld); frames published %lld, with water seen %lld (unmeasured: overflow %lld, failed %lld),"
                   " frames dropped %lld; last: frame %llu, draws %u, samples %llu", now, g_n_draws, g_n_other, g_n_frames, g_n_seen, g_n_overflow, g_n_failed,
                   g_n_dropped, g_pub_frame, g_pub_draws, g_pub_samples);
}

void photon_waterq_device_lost(void) { if (g_active == 1) release_all(); }

int photon_waterq_last(uint64_t *frame, uint32_t *draws, uint64_t *samples) {
    if (!g_wanted) InterlockedExchange(&g_wanted, 1);
    AcquireSRWLockShared(&g_pub_lock);
    uint64_t f = g_pub_frame; uint32_t d = g_pub_draws; uint64_t s = g_pub_samples;
    ReleaseSRWLockShared(&g_pub_lock);
    if (!f) return 0;
    if (frame) *frame = f;
    if (draws) *draws = d;
    if (samples) *samples = s;
    return 1;
}
