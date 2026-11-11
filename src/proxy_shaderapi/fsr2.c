/* UPSCALE U4 (analysis/45): FSR 2.2 on the Vulkan bridge. See fsr2.h. */
#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include "fsr2.h"
#include "log.h"
#ifdef PHOTON_FSR2
#include "vkbridge.h"
#include "fsr2/fsr2_glue.h"
static PhotonVkImage g_col, g_dep, g_mv, g_out;
static void *g_col_t, *g_dep_t, *g_mv_t;
static PhotonVkImage *g_imgs[4] = { &g_col, &g_dep, &g_mv, &g_out };
static PhotonVkPass g_pass;
static int g_reg, g_failed;
static unsigned g_dw, g_dh;
static struct { float jx, jy, mvx, mvy, sharp, ms; unsigned rw, rh; int reset; } g_f;
static LARGE_INTEGER g_last;
static long long g_frames;
static void logline(const char *m) { photon_log("%s", m); }
static int setup(void *u) { (void)u; return 1; }
static void img(const PhotonVkImage *i, PhotonFsr2Img *o) { o->img = i->img; o->view = i->view; o->w = i->w; o->h = i->h; o->fmt = i->format; o->aspect = i->aspect; }
static void record(VkCommandBuffer cb, void *u) {
    (void)u;
    PhotonFsr2Img c, d, m, o;
    img(&g_col, &c); img(&g_dep, &d); img(&g_mv, &m); img(&g_out, &o);
    photon_fsr2_dispatch(cb, &c, &d, &m, &o, g_f.jx, g_f.jy, g_f.mvx, g_f.mvy, g_f.rw, g_f.rh, g_f.sharp, g_f.ms, g_f.reset, 7.0f, 30000.0f, 1.3f);
}
static int wrap(void *t, void **have, PhotonVkImage *im) {
    if (t == *have && im->img) return 1;
    if (!photon_vk_wrap(t, im)) return 0;
    *have = t;
    return 1;
}
int photon_fsr2_frame(void *color, void *depth, void *motion, unsigned dw, unsigned dh, unsigned rw, unsigned rh, float jx, float jy,
                      float sharp, int reset, void **out) {
    if (g_failed) return 0;
    if (!g_reg) {   /* registering brings the bridge up at the next Present */
        g_pass = (PhotonVkPass){ "fsr2", setup, record, NULL, g_imgs, 4, NULL, 0, 1 };
        photon_vk_add_pass(&g_pass); g_reg = 1;
        return 0;
    }
    const PhotonVk *vk = photon_vk();
    if (!vk || !photon_vk_available()) return 0;
    if (!wrap(color, &g_col_t, &g_col) || !wrap(depth, &g_dep_t, &g_dep) || !wrap(motion, &g_mv_t, &g_mv)) {
        g_failed = 1; photon_log("fsr2: an input could not be wrapped: off (TAAU instead)"); return 0;
    }
    if (!g_out.tex || g_dw != dw || g_dh != dh) {
        if (!photon_vk_image(dw, dh, D3DFMT_A16B16G16R16F, 0, &g_out)) { g_failed = 1; photon_log("fsr2: no output image: off"); return 0; }
        if (!photon_fsr2_init(photon_vk_gipa(), vk->instance, vk->phys, vk->dev, vk->get_proc, dw, dh, dw, dh, logline)) {
            g_failed = 1; photon_log("fsr2: the context failed: off (TAAU instead)"); return 0;
        }
        g_dw = dw; g_dh = dh; reset = 1;
    }
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    g_f.ms = g_last.QuadPart ? (float)((double)(now.QuadPart - g_last.QuadPart) * 1000.0 / (double)fq.QuadPart) : 16.7f;
    g_last = now;
    /* FSR's jitter is in render pixels, y down (its projection offset is 2 x / w, -2 y / h); the motion vectors are target uv offsets: a
     * target pixel is a render pixel (the view is the target's corner), so x the target size gives pixels */
    g_f.jx = jx * (float)rw * 0.5f; g_f.jy = -jy * (float)rh * 0.5f;
    g_f.mvx = (float)dw; g_f.mvy = (float)dh;
    g_f.rw = rw; g_f.rh = rh; g_f.sharp = sharp; g_f.reset = reset;
    g_pass.enabled = 1;
    photon_vk_run(&g_pass);
    g_pass.enabled = 0;
    if (++g_frames % 300 == 1) photon_log("fsr2: frame %lld: %ux%u -> %ux%u, jitter %.3f %.3f px", g_frames, rw, rh, dw, dh, g_f.jx, g_f.jy);
    *out = g_out.tex;
    return 1;
}
#else
int photon_fsr2_frame(void *color, void *depth, void *motion, unsigned dw, unsigned dh, unsigned rw, unsigned rh, float jx, float jy,
                      float sharp, int reset, void **out) {
    (void)color; (void)depth; (void)motion; (void)dw; (void)dh; (void)rw; (void)rh; (void)jx; (void)jy; (void)sharp; (void)reset; (void)out;
    return 0;
}
#endif
