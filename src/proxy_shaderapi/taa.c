/* AA: the TAA jitter. See taa.h. Runs on the thread that renders (the material system's queue thread in queued mode). */
#include <windows.h>
#include <math.h>
#include "taa.h"
#include "dmirror.h"
#include "log.h"
#include "vlg.h"

static int g_on = -1, g_mode = -1, g_active;
static unsigned g_frame;
static float g_j[2];                                 /* this frame's jitter, NDC */
static long long g_jittered, g_skipped;

/* photon_taa, read once per Present (photon_taa_present) into a cached flag: the hooks (LoadMatrix runs ~180 times a frame, at times
 * inside the engine's own resets) never take vlg's settings lock */
static volatile int g_on_cached;
int photon_taa_on(void) { return g_on_cached; }
static unsigned g_view_w, g_view_h, g_target_w, g_target_h;
static void refresh_on(void) {
    /* photon_taa, or Photon's temporal upscale while the 3D view is scaled (its jitter is the upscale's samples) */
    int up = photon_vlg_upscale();
    int on = photon_vlg_taa() != 0 || ((up == 1 || up == 3) && g_view_w && g_target_w && (g_view_w < g_target_w || g_view_h < g_target_h));
    if (on != g_on) { g_on = on; photon_log("taa: %s", on ? "on" : "off"); }
    g_on_cached = on;
    if (!on) g_active = 0;
}

static float halton(unsigned i, unsigned b) {
    float f = 1, r = 0;
    while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

void photon_taa_matrix_mode(int mode) { g_mode = mode; }

/* MATERIAL_PROJECTION = 1; a perspective projection (row-major, row-vector: column 3 = (0, 0, +-1, 0)) gets the offset on its third
 * row's x / y (clip.x += z * jx * m11 with w = z * m11: ndc.x += jx) */
void photon_taa_load_matrix(float m[16]) {
    if (!g_active || g_mode != 1 || !photon_taa_on()) return;
    if (m[3] != 0 || m[7] != 0 || fabsf(fabsf(m[11]) - 1.0f) > 1e-6f || m[15] != 0) {   /* Source's is right-handed: m11 = -1 (w = -z) */
        static int persp_logs;
        if (m[15] == 0 && persp_logs++ < 3) photon_log("taa: perspective-like projection not taken (m15 0): %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                                       m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        g_skipped++;
        return;
    }
    m[8] += g_j[0] * m[11]; m[9] += g_j[1] * m[11];   /* clip w = z m11: ndc += j */
    g_jittered++;
}

/* UPSCALE: the main view's own size (scaled below the target when upscaling): the jitter is one of its pixels, and the sequence grows
 * with the upscale (8 x (target / view)^2 samples, so every output pixel is covered) */
void photon_taa_view_size(unsigned w, unsigned h, unsigned tw, unsigned th) { g_view_w = w; g_view_h = h; g_target_w = tw; g_target_h = th; }
void photon_taa_present(void) {
    refresh_on();
    if (!photon_taa_on()) return;
    g_frame++;
    unsigned v[4] = { 0, 0, 0, 0 };
    float w = 960, h = 540;
    if (g_view_w && g_view_h) { w = (float)g_view_w; h = (float)g_view_h; }
    else if (photon_dmirror_viewport(v) && v[2] && v[3]) { w = (float)v[2]; h = (float)v[3]; }
    unsigned len = 8;
    if (g_view_w && g_target_w && g_view_w < g_target_w) {
        float r = (float)g_target_w / (float)g_view_w;
        len = (unsigned)(8.0f * r * r + 0.5f); if (len > 64) len = 64;
    }
    /* Halton (2, 3) over len frames, in pixels [-0.5, 0.5], to NDC (y up) */
    unsigned i = (unsigned)(g_frame % len) + 1;
    g_j[0] = (halton(i, 2) - 0.5f) * 2.0f / w;
    g_j[1] = (halton(i, 3) - 0.5f) * 2.0f / h;
    g_active = 1;
}

void photon_taa_main_view_end(void) {
    g_active = 0;
    static long long ends;
    if (++ends % 100 == 50) photon_log("taa: main view %lld: projections jittered %lld, skipped (not perspective) %lld", ends, g_jittered, g_skipped);
}

void photon_taa_jitter_ndc(float j[2]) {
    if (photon_taa_on()) { j[0] = g_j[0]; j[1] = g_j[1]; } else j[0] = j[1] = 0;
}
