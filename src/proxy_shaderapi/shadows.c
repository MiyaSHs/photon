/* T4.2 Photon sun shadows (analysis/34_shadows_design.md). Stage 0: what the device offers for the design (INTZ / DF24 depth textures,
 * the NULL render-target format, the RESZ resolve) and what the game's current depth-stencil and back buffer are, logged once. */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ptrt.h"
#include "nanite.h"
#include "passtime.h"
#include "shadows.h"
#include "hiz.h"
#include "packs.h"
#include "fog.h"
#include "texfilter.h"
#include "d3dconst.h"
#include "log.h"
#include "dynq.h"
#include "wrap.h"
#include <stdint.h>
#include "frames.h"
#include "dmirror.h"
#include "vlg.h"
#include "mqueue.h"
#include <string.h>
#include "shaders/shpass_vs.h"
#include "shaders/shdebug_ps.h"
#include "shaders/shnull20_ps.h"
#include "shaders/shnull30_ps.h"
#include "shaders/shadow_ps.h"
#include "shaders/shadow_soft_ps.h"
#include "shaders/lampshadow_ps.h"
#include "shaders/l3ratio_ps.h"
#include "shaders/depthcopy_ps.h"
#include "shaders/outline_ps.h"
#include "shaders/ptrt_comp_ps.h"
#include "shaders/taa_ps.h"
#include "shaders/taa_sharpen_ps.h"
#include "shaders/taa_dist_ps.h"
#include "taa.h"
#include "shaders/nanite_merge_ps.h"
#include "l3work.h"
#include "lgwork.h"
#include "p6sun.h"
#include "../ambient/sun_vis.h"

#define FOURCC4(a, b, c, d) ((D3DFORMAT)((DWORD)(a) | ((DWORD)(b) << 8) | ((DWORD)(c) << 16) | ((DWORD)(d) << 24)))
static int g_probed;
enum { DYN_OFF_LEVEL = 5 };       /* UI2: the dynamic-quality level at which Photon's shadows give way to the engine's */
static IDirect3DTexture9 *g_scene_tex;
static IDirect3DSurface9 *g_scene_sfc;
/* stage 1c: the sun shadow map (INTZ depth texture + a NULL-format colour surface of the same size) */
static int g_sm_size = 2048;   /* the live photon_shadow_res (UI1); the map is recreated when it changes */
static IDirect3DTexture9 *g_sm_tex;
static IDirect3DSurface9 *g_sm_sfc, *g_sm_rt;
static int g_sm_failed;
static IDirect3DTexture9 *g_sn_tex;   /* F1: the near cascade (same size; draws through g_sm_rt) */
static IDirect3DSurface9 *g_sn_sfc;
static void sn_release(void) {
    if (g_sn_sfc) { g_sn_sfc->lpVtbl->Release(g_sn_sfc); g_sn_sfc = NULL; }
    if (g_sn_tex) { g_sn_tex->lpVtbl->Release(g_sn_tex); g_sn_tex = NULL; }
}
static void *g_ps20, *g_ps30, *g_sh_vs, *g_sh_ps, *g_sh_soft_ps;
static volatile LONG64 g_sh_passes, g_sh_casters;
static volatile LONG64 g_dbg_passes, g_zc_all, g_zc_nods, g_zc_nobb, g_zc_otherrt;

static volatile LONG64 g_sw_leaves;
static int g_ncls;
static volatile LONG64 g_sun_asked, g_sun_unlit, g_sun_miss;
static volatile LONG64 g_cast_none, g_refreshed;
static volatile LONG64 g_l1_frames, g_l1_frames_any, g_l1_changes, g_l1_ticks;   /* L1a */
static volatile LONG64 g_l1_maps, g_l1_dropped, g_l1_tooclose, g_l1_passes;   /* L1b/c */
static volatile LONG64 g_l3_passes, g_l3_uploads, g_l3_lamp_updates;   /* L3c */
/* L3 model mask: the world-only depth (see depthcopy_ps.hlsl), valid from the copy to the next depth clear */
static IDirect3DTexture9 *g_wd_tex;
static IDirect3DSurface9 *g_wd_sfc;
static void *g_wd_ps;
static int g_wd_valid, g_wd_failed;
static volatile LONG64 g_wd_copies, g_wd_masked_passes;
static volatile LONG64 g_sun_near;   /* F1: frames with the near cascade */
static volatile LONG64 g_l1_sz_full, g_l1_sz_half, g_l1_sz_quarter;   /* F1: lamp map sizes chosen */
static volatile LONG64 g_mv_ends, g_mv_queued;
static int g_on = -1;                          /* PHOTON_SHADOWS=1 (stage 1a, test) */
static IDirect3DSurface9 *g_auto;              /* the device's automatic depth-stencil (one reference of ours, for identity) */
static IDirect3DTexture9 *g_intz;              /* its INTZ stand-in, same size */
static IDirect3DSurface9 *g_intz_sfc;          /* level 0 of g_intz */
static volatile LONG64 g_subst, g_binds_other, g_creates, g_create_fail, g_resets;
static D3DFORMAT g_auto_fmt;
static IDirect3DSurface9 *g_bb;   /* the back buffer (one reference, released before Reset) */
static IDirect3DTexture9 *g_fcopy; static IDirect3DSurface9 *g_fcopy_sfc; static unsigned g_fcopy_w, g_fcopy_h;   /* PT-RT's frame copy */
static float g_pt_sun[12][4]; static int g_pt_sun_ok, g_pt_maps_only;
static IDirect3DTexture9 *g_taa_cur, *g_taa_hist; static IDirect3DSurface9 *g_taa_cur_s, *g_taa_hist_s; static unsigned g_taa_w, g_taa_h;   /* AA */
static float g_taa_prev[4][4], g_taa_prev_eye[3]; static int g_taa_have; static void *g_taa_ps, *g_taa_sh_ps, *g_taa_d_ps; static float g_taa_sharp = -1;
static IDirect3DTexture9 *g_taa_dist; static IDirect3DSurface9 *g_taa_dist_s;   /* last frame's distances (R32F) */
static IDirect3DTexture9 *g_taa_mv; static IDirect3DSurface9 *g_taa_mv_s;   /* the moving models' motion vectors (dmirror velocity pass) */
static int g_taa_mv_on = -1;   /* PHOTON_TAA_MV=0: camera-only reprojection (A/B) */
static int g_vel_ok; static void *g_vel_tex; static unsigned g_vel_w, g_vel_h;   /* this frame's motion vectors (velocity_pass) */
static IDirect3DTexture9 *g_mnrm; static IDirect3DSurface9 *g_mnrm_s; static int g_mnrm_ok;
static IDirect3DTexture9 *g_up_src; static IDirect3DSurface9 *g_up_src_s;   /* UPSCALE: the scaled view's corner, copied */
static IDirect3DTexture9 *g_tu_hist, *g_tu_dist; static IDirect3DSurface9 *g_tu_hist_s, *g_tu_dist_s; static int g_tu_have;   /* UPSCALE: TAAU's history, distances */
static IDirect3DTexture9 *g_fmv, *g_fdep; static IDirect3DSurface9 *g_fmv_s, *g_fdep_s;   /* UPSCALE: FSR 2's full motion vectors, its depth (R32F copy) */   /* PT-RT models: their world normals (velocity_pass) */
static IDirect3DTexture9 *g_wn_tex; static IDirect3DSurface9 *g_wn_sfc; static unsigned g_wn_w, g_wn_h; static int g_wn_ok;
static IDirect3DTexture9 *g_ssr_prev; static IDirect3DSurface9 *g_ssr_prev_s; static unsigned g_ssr_w, g_ssr_h; static int g_ssr_have; static float g_ssr_rows[4][4];   /* PBR: last composited frame */
static IDirect3DTexture9 *g_wpbr_tex; static IDirect3DSurface9 *g_wpbr_sfc;   /* PBR step 2: the world's roughness / metalness (with g_wn_tex) */   /* PT-RT: the world's smooth normals */   /* PT-RT's sun: shadow_pass's constants, kept for the composite */

static void probe(IDirect3DDevice9 *d) {
    IDirect3D9 *d3d = NULL;
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DDISPLAYMODE dm;
    if (FAILED(d->lpVtbl->GetDirect3D(d, &d3d)) || !d3d) return;
    if (FAILED(d->lpVtbl->GetCreationParameters(d, &cp)) || FAILED(d3d->lpVtbl->GetAdapterDisplayMode(d3d, cp.AdapterOrdinal, &dm))) {
        d3d->lpVtbl->Release(d3d);
        return;
    }
    const struct { const char *name; DWORD usage; D3DRESOURCETYPE type; D3DFORMAT fmt; } k[] = {
        { "INTZ depth texture", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC4('I', 'N', 'T', 'Z') },
        { "DF24 depth texture", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC4('D', 'F', '2', '4') },
        { "DF16 depth texture", D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC4('D', 'F', '1', '6') },
        { "NULL render target", D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, FOURCC4('N', 'U', 'L', 'L') },
        { "RESZ render target", D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, FOURCC4('R', 'E', 'S', 'Z') },
        { "R32F render target", D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_R32F },
    };
    char line[512];
    int n = 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        HRESULT hr = d3d->lpVtbl->CheckDeviceFormat(d3d, cp.AdapterOrdinal, cp.DeviceType, dm.Format, k[i].usage, k[i].type, k[i].fmt);
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%s %s", i ? ", " : "", k[i].name, SUCCEEDED(hr) ? "yes" : "no");
    }
    photon_log("shadows: stage 0 caps (adapter %u, display format %d): %s", cp.AdapterOrdinal, (int)dm.Format, line);
    IDirect3DSurface9 *ds = NULL, *bb = NULL;
    D3DSURFACE_DESC dd, bd;
    if (SUCCEEDED(d->lpVtbl->GetDepthStencilSurface(d, &ds)) && ds && SUCCEEDED(ds->lpVtbl->GetDesc(ds, &dd)))
        photon_log("shadows: current depth-stencil %ux%u format %d multisample %d/%lu usage 0x%lx pool %d", dd.Width, dd.Height, (int)dd.Format,
                   (int)dd.MultiSampleType, dd.MultiSampleQuality, dd.Usage, (int)dd.Pool);
    else photon_log("shadows: no depth-stencil bound at the probe");
    if (ds) ds->lpVtbl->Release(ds);
    if (SUCCEEDED(d->lpVtbl->GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb && SUCCEEDED(bb->lpVtbl->GetDesc(bb, &bd)))
        photon_log("shadows: back buffer %ux%u format %d multisample %d", bd.Width, bd.Height, (int)bd.Format, (int)bd.MultiSampleType);
    if (bb) bb->lpVtbl->Release(bb);
    d3d->lpVtbl->Release(d3d);
}

void photon_shadows_frame(void) {
    if (photon_shadows_on() && photon_frame_count() % 1200 == 600) photon_shadows_dump("periodic");
    if (g_probed) return;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return;
    g_probed = 1;
    probe(d);
}

static int g_debug = -1;
static int debug_mode(void) {
    if (g_debug < 0) { char e[4]; g_debug = GetEnvironmentVariableA("PHOTON_SHADOWS_DEBUG", e, sizeof e) > 0 ? e[0] - '0' : 0; }
    return g_debug;
}
/* UI2 dynamic quality: 0 = the preset's own quality (the ceiling); each level one step cheaper: 1 hard edges and one lamp (L1), 2 half distance, 3 half
 * resolution, 4 quarter distance, 5 engine shadows (Photon's off). Set by the DYN controller (dynq.c). */
/* Photon's sun shadows drawn now: the in-game setting (photon_shadows / photon_preset, live) or PHOTON_SHADOWS_DEBUG=3, unless dynamic
 * quality has stepped all the way down to the engine's shadows */
static int drawing(void) {
    if (!photon_shadows_on()) return 0;
    if (debug_mode() == 3) return 1;
    if (photon_ptrt_on()) return 1;   /* PT-RT: the sun map is its sharp sun (maps only, no darkening pass; ptrt_sun below) */
    return photon_vlg_shadows_wanted() && !(photon_dynq_shadow_level() >= DYN_OFF_LEVEL);
}
/* dmirror asks this per draw on the render thread: one lookup per frame instead of the settings lock per draw (0.3% of MatQueue at 100
 * ragdolls, defaults). A setting change takes effect within a frame. */
static volatile int g_drawing_cached = -1;
static volatile unsigned long long g_drawing_frame = ~0ull;
int photon_shadows_drawing(void) {
    unsigned long long f = photon_frame_count();
    if (f != g_drawing_frame || g_drawing_cached < 0) { g_drawing_cached = drawing(); g_drawing_frame = f; }
    return g_drawing_cached;
}
int photon_shadows_on(void) {
    /* the groundwork (INTZ depth stand-in, view tracking) is on by default since UI1 (2026-11-08: same image and frame time, s1_*), so the
     * in-game switch can turn shadows on at any time; PHOTON_SHADOWS=0 turns all of it off */
    if (g_on < 0) { char e[4]; g_on = !(GetEnvironmentVariableA("PHOTON_SHADOWS", e, sizeof e) > 0 && e[0] == '0'); }
    return g_on;
}

static void l1_release(void);
static void l3_release(void);
static void release_all(void) {
    photon_packs_device_lost();
    photon_texf_device_lost();
    if (g_intz_sfc) { g_intz_sfc->lpVtbl->Release(g_intz_sfc); g_intz_sfc = NULL; }
    if (g_intz) { g_intz->lpVtbl->Release(g_intz); g_intz = NULL; }
    if (g_auto) { g_auto->lpVtbl->Release(g_auto); g_auto = NULL; }
    if (g_bb) { g_bb->lpVtbl->Release(g_bb); g_bb = NULL; }
    if (g_fcopy) { g_fcopy_sfc->lpVtbl->Release(g_fcopy_sfc); g_fcopy->lpVtbl->Release(g_fcopy); g_fcopy = NULL; g_fcopy_sfc = NULL; }   /* D3DPOOL_DEFAULT: before Reset */
    if (g_taa_cur) { g_taa_cur_s->lpVtbl->Release(g_taa_cur_s); g_taa_cur->lpVtbl->Release(g_taa_cur); g_taa_cur = NULL; g_taa_cur_s = NULL; }
    if (g_taa_hist) { g_taa_hist_s->lpVtbl->Release(g_taa_hist_s); g_taa_hist->lpVtbl->Release(g_taa_hist); g_taa_hist = NULL; g_taa_hist_s = NULL; g_taa_have = 0; }
    if (g_taa_dist) { g_taa_dist_s->lpVtbl->Release(g_taa_dist_s); g_taa_dist->lpVtbl->Release(g_taa_dist); g_taa_dist = NULL; g_taa_dist_s = NULL; g_taa_have = 0; }
    if (g_taa_mv) { g_taa_mv_s->lpVtbl->Release(g_taa_mv_s); g_taa_mv->lpVtbl->Release(g_taa_mv); g_taa_mv = NULL; g_taa_mv_s = NULL; }
    if (g_mnrm) { g_mnrm_s->lpVtbl->Release(g_mnrm_s); g_mnrm->lpVtbl->Release(g_mnrm); g_mnrm = NULL; g_mnrm_s = NULL; }
    if (g_up_src) { g_up_src_s->lpVtbl->Release(g_up_src_s); g_up_src->lpVtbl->Release(g_up_src); g_up_src = NULL; g_up_src_s = NULL; }
    if (g_tu_hist) { g_tu_hist_s->lpVtbl->Release(g_tu_hist_s); g_tu_hist->lpVtbl->Release(g_tu_hist); g_tu_hist = NULL; g_tu_hist_s = NULL; g_tu_have = 0; }
    if (g_tu_dist) { g_tu_dist_s->lpVtbl->Release(g_tu_dist_s); g_tu_dist->lpVtbl->Release(g_tu_dist); g_tu_dist = NULL; g_tu_dist_s = NULL; }
    if (g_fmv) { g_fmv_s->lpVtbl->Release(g_fmv_s); g_fmv->lpVtbl->Release(g_fmv); g_fmv = NULL; g_fmv_s = NULL; }
    if (g_fdep) { g_fdep_s->lpVtbl->Release(g_fdep_s); g_fdep->lpVtbl->Release(g_fdep); g_fdep = NULL; g_fdep_s = NULL; }
    photon_dmirror_velocity_release();
    if (g_wn_tex) { g_wn_sfc->lpVtbl->Release(g_wn_sfc); g_wn_tex->lpVtbl->Release(g_wn_tex); g_wn_tex = NULL; g_wn_sfc = NULL; }
    if (g_wpbr_tex) { g_wpbr_sfc->lpVtbl->Release(g_wpbr_sfc); g_wpbr_tex->lpVtbl->Release(g_wpbr_tex); g_wpbr_tex = NULL; g_wpbr_sfc = NULL; }
    if (g_ssr_prev) { g_ssr_prev_s->lpVtbl->Release(g_ssr_prev_s); g_ssr_prev->lpVtbl->Release(g_ssr_prev); g_ssr_prev = NULL; g_ssr_prev_s = NULL; g_ssr_have = 0; }
    if (g_scene_tex) { g_scene_tex->lpVtbl->Release(g_scene_tex); g_scene_tex = NULL; }
    if (g_sm_sfc) { g_sm_sfc->lpVtbl->Release(g_sm_sfc); g_sm_sfc = NULL; }
    if (g_sm_tex) { g_sm_tex->lpVtbl->Release(g_sm_tex); g_sm_tex = NULL; }
    if (g_sm_rt) { g_sm_rt->lpVtbl->Release(g_sm_rt); g_sm_rt = NULL; }
    sn_release();
    l1_release();
    l3_release();
    if (g_scene_sfc) { g_scene_sfc->lpVtbl->Release(g_scene_sfc); g_scene_sfc = NULL; }
}

/* The automatic depth-stencil is whatever is bound right after CreateDevice / Reset; dmirror binds on the first frame, when the main
 * depth buffer is the one bound at Present (shaderapi rebinds its cached automatic surface for depth id -1 every frame). */
void photon_shadows_bind(void *dev) {
    if (!photon_shadows_on() || g_intz) return;
    IDirect3DDevice9 *d = dev;
    if (!g_bb) d->lpVtbl->GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_bb);
    /* PHOTON_SHADOWS_SUBST=0: no stand-in (A/B). Note: the engine reads the bound depth back (GetDepthStencilSurface) and rebinds it, so
     * once the stand-in is bound it is what the main view keeps using (the census's "INTZ surface" was the stand-in itself) */
    { char e[4]; if (GetEnvironmentVariableA("PHOTON_SHADOWS_SUBST", e, sizeof e) > 0 && e[0] == '0') return; }
    IDirect3DSurface9 *ds = NULL;
    D3DSURFACE_DESC dd;
    if (FAILED(d->lpVtbl->GetDepthStencilSurface(d, &ds)) || !ds) { photon_log("shadows: no depth-stencil bound: stage 1a off"); return; }
    if (FAILED(ds->lpVtbl->GetDesc(ds, &dd)) || dd.MultiSampleType != D3DMULTISAMPLE_NONE) {
        photon_log("shadows: the depth-stencil is multisampled (%d): INTZ cannot stand in, stage 1a off", (int)dd.MultiSampleType);
        ds->lpVtbl->Release(ds);
        return;
    }
    HRESULT hr = d->lpVtbl->CreateTexture(d, dd.Width, dd.Height, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC4('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &g_intz, NULL);
    if (FAILED(hr) || !g_intz || FAILED(g_intz->lpVtbl->GetSurfaceLevel(g_intz, 0, &g_intz_sfc))) {
        InterlockedIncrement64(&g_create_fail);
        photon_log("shadows: CreateTexture INTZ %ux%u -> hr 0x%lx: stage 1a off", dd.Width, dd.Height, (unsigned long)hr);
        if (g_intz) { g_intz->lpVtbl->Release(g_intz); g_intz = NULL; }
        ds->lpVtbl->Release(ds);
        return;
    }
    g_auto = ds;   /* keeps the reference GetDepthStencilSurface gave */
    if (!g_bb) d->lpVtbl->GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_bb);
    g_auto_fmt = dd.Format;
    InterlockedIncrement64(&g_creates);
    photon_log("shadows: stage 1a: INTZ %ux%u stands in for the automatic depth-stencil %p (format %d)", dd.Width, dd.Height, (void *)ds, (int)dd.Format);
}

/* stage 1a diagnostics: the distinct depth surfaces bound, with counts (Present thread or MatQueue: one writer, the draw thread) */
enum { NDS = 256 };
static void *g_ds_seen[NDS];
static long long g_ds_n[NDS], g_ds_bb[NDS];
static D3DSURFACE_DESC g_ds_desc[NDS];
static void ds_note(void *sfc) {
    for (int i = 0; i < NDS; i++) {
        if (g_ds_seen[i] == sfc) { g_ds_n[i]++; if (g_bb && photon_dmirror_rt0() == (void *)g_bb) g_ds_bb[i]++; return; }
        if (!g_ds_seen[i]) {
            g_ds_seen[i] = sfc; g_ds_n[i] = 1;
            ((IDirect3DSurface9 *)sfc)->lpVtbl->GetDesc((IDirect3DSurface9 *)sfc, &g_ds_desc[i]);
            if (i < 16) photon_log("shadows: depth surface %d first bound: %p %ux%u format %d usage 0x%lx (frame %llu)", i, sfc, g_ds_desc[i].Width,
                       g_ds_desc[i].Height, (int)g_ds_desc[i].Format, g_ds_desc[i].Usage, (unsigned long long)photon_frame_count());
            return;
        }
    }
}
void *photon_shadows_ds(void *sfc) {
    if (photon_shadows_on() && sfc) ds_note(sfc);
    if (g_intz_sfc && sfc && sfc == (void *)g_auto) { InterlockedIncrement64(&g_subst); return g_intz_sfc; }
    if (g_intz_sfc && sfc) InterlockedIncrement64(&g_binds_other);
    return sfc;
}

void photon_shadows_pre_reset(void) {
    if (!photon_shadows_on()) return;
    InterlockedIncrement64(&g_resets);
    release_all();
}
void photon_shadows_post_reset(void *dev, int ok) {
    if (!photon_shadows_on()) return;
    if (ok) photon_shadows_bind(dev);
    else photon_log("shadows: device Reset failed: stage 1a waits for the next bind");
}

void photon_shadows_dump(const char *why) {
    if (!photon_shadows_on()) return;
    photon_log("shadows summary (%s): INTZ creates %lld (failed %lld), automatic depth binds replaced %lld, other depth binds %lld, resets %lld", why,
               (long long)g_creates, (long long)g_create_fail, (long long)g_subst, (long long)g_binds_other, (long long)g_resets);
    photon_log("shadows: dynamic quality (dynq.c): target %d fps, shadow level %d", photon_vlg_target_fps(), photon_dynq_shadow_level());
    photon_log("shadows: stage 2a: sun asked %lld (in the world's shade %lld), casters not asked %lld", (long long)g_sun_asked, (long long)g_sun_unlit,
               (long long)g_sun_miss);
    photon_log("shadows: stage 1d: %d classes, ShadowCastType answered NONE %lld times, entities refreshed %lld", g_ncls, (long long)g_cast_none,
               (long long)g_refreshed);
    photon_log("shadows: L3: uploads %lld, lamp table updates %lld, screen passes %lld (with the model mask %lld; world depth copies %lld)", (long long)g_l3_uploads,
               (long long)g_l3_lamp_updates, (long long)g_l3_passes, (long long)g_wd_masked_passes, (long long)g_wd_copies);
    l3_work_summary("dump");
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        photon_log("shadows: L1 lamps: map sizes full %lld, half %lld, quarter %lld (F1)", (long long)g_l1_sz_full, (long long)g_l1_sz_half, (long long)g_l1_sz_quarter);
        photon_log("shadows: L1 lamps: frames %lld, with a lamp chosen %lld, choice changes %lld, selection %.4f ms per frame, maps %lld, screen passes %lld, casters dropped to fit %lld, lamps too close %lld",
                   (long long)g_l1_frames, (long long)g_l1_frames_any, (long long)g_l1_changes,
                   g_l1_frames ? (double)g_l1_ticks * 1000.0 / (double)f.QuadPart / (double)g_l1_frames : 0.0, (long long)g_l1_maps, (long long)g_l1_passes,
                   (long long)g_l1_dropped, (long long)g_l1_tooclose);
    }
    {
        long long k[5];
        photon_dmirror_cast_skin_stats(k);
        photon_log("shadows: skinned casters: skinned draws seen %lld, left out (off) %lld, dynamic stream 0 (left out) %lld, flex streams zeroed %lld, palette pool full %lld",
                   k[0], k[1], k[2], k[3], k[4]);
    }
    photon_log("shadows: stage 1c: shadow passes %lld, caster draws %lld (this frame's list %d, skipped %lld), frames with the map kept %lld", (long long)g_sh_passes,
               (long long)g_sh_casters, photon_dmirror_ncast(), photon_dmirror_cast_skipped(), photon_dmirror_cast_cached());
    photon_log("shadows: depth clears %lld (no depth bound %lld, no back buffer %lld, another RT0 %lld)", (long long)g_zc_all, (long long)g_zc_nods,
               (long long)g_zc_nobb, (long long)g_zc_otherrt);
    photon_log("shadows: scene depth left %lld times; main view ends %lld (queued %lld)", (long long)g_sw_leaves, (long long)g_mv_ends, (long long)g_mv_queued);
    photon_log("shadows: scene depth %s; debug passes %lld (full-screen draws %lld, refused for an unknown state %lld)", g_scene_tex ? "found" : "not found",
               (long long)g_dbg_passes, photon_dmirror_fs_draws(), photon_dmirror_fs_unknown());
    int nseen = 0;
    while (nseen < NDS && g_ds_seen[nseen]) nseen++;
    photon_log("shadows: %d distinct depth surfaces bound%s", nseen, nseen == NDS ? " (table full)" : "");
    int top[8], ntop = 0;   /* the 8 most bound */
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < nseen; i++) {
            int used = 0;
            for (int j = 0; j < ntop; j++) if (top[j] == i) used = 1;
            if (!used && (best < 0 || g_ds_n[i] > g_ds_n[best])) best = i;
        }
        if (best < 0) break;
        top[ntop++] = best;
    }
    for (int k = 0; k < ntop; k++) {
        int i = top[k];
        photon_log("shadows: depth surface %d %p %ux%u format %d: %lld binds, %lld with the back buffer as RT0%s", i, g_ds_seen[i], g_ds_desc[i].Width,
                   g_ds_desc[i].Height, (int)g_ds_desc[i].Format, g_ds_n[i], g_ds_bb[i], g_ds_seen[i] == (void *)g_auto ? " (the automatic one)" : "");
    }
}

/* --- stage 1b: the main view's depth (GMod's own INTZ depth texture, analysis/34 stage 1a) and the debug pass ------------------------- */
/* g_scene_sfc / g_scene_tex (declared above): the INTZ depth surface bound with the back buffer, and its texture (one reference each) */
static UINT g_scene_w, g_scene_h;
static void *g_fs_vs, *g_fs_ps;

static void scene_note(IDirect3DSurface9 *ds) {
    if (ds == g_scene_sfc || !g_bb) return;
    D3DSURFACE_DESC dd, bd;
    if (FAILED(ds->lpVtbl->GetDesc(ds, &dd)) || dd.Format != FOURCC4('I', 'N', 'T', 'Z')) return;
    if (FAILED(g_bb->lpVtbl->GetDesc(g_bb, &bd)) || dd.Width != bd.Width || dd.Height != bd.Height) return;
    IDirect3DTexture9 *t = NULL;
    if (FAILED(ds->lpVtbl->GetContainer(ds, &IID_IDirect3DTexture9, (void **)&t)) || !t) return;
    if (g_scene_tex) g_scene_tex->lpVtbl->Release(g_scene_tex);
    if (g_scene_sfc) g_scene_sfc->lpVtbl->Release(g_scene_sfc);
    ds->lpVtbl->AddRef(ds);
    g_scene_sfc = ds; g_scene_tex = t; g_scene_w = dd.Width; g_scene_h = dd.Height;
    photon_log("shadows: scene depth = INTZ surface %p (%ux%u), texture %p", (void *)ds, dd.Width, dd.Height, (void *)t);
}

static void view_rect_fix(float m[4][4]);
static int inv4(const float m[4][4], float out[4][4]) {   /* Gauss-Jordan, row-major */
    float a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1.0f : 0.0f);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-12f) return 0;
        if (p != c) for (int j = 0; j < 8; j++) { float t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        float inv = 1.0f / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= inv;
        for (int r = 0; r < 4; r++) if (r != c) { float f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i][j] = a[i][j + 4];
    return 1;
}

static void seg_event(void);
void photon_shadows_zclear(void *ds, void *rt0) {
    g_wd_valid = 0;   /* the world-only depth belongs to the depth just cleared */
    if (!photon_shadows_on()) return;
    debug_mode();
    g_zc_all++;
    if (!ds || !g_bb) return;
    if (rt0 != (void *)g_bb) { g_zc_otherrt++; return; }
    scene_note(ds);
    if (ds == (void *)g_scene_sfc) seg_event();   /* the depth clear ends a segment; the pass runs before the clear */
}

/* The pass runs when the binding leaves the scene depth (GMod never Clear()s it: its depth clears go to a D24S8 surface). Each frame
 * numbers the times the scene depth is left; the one ending the segment with the most draws last frame is the main view. The scene
 * depth is recognised when it is bound: INTZ, back-buffer size, the back buffer as RT0. */
/* A segment is the draws between two events while the scene depth is bound; events are a depth clear of the scene depth, the binding
 * leaving it, and Present. The event that ends the segment with the most draws last frame (the main view) runs the pass. */
static unsigned long long g_ev_frame = ~0ull;
static int g_seg_have;            /* the current segment's matrices are captured */
static float g_seg_m[4][4];       /* transpose(V * P) of the current segment, captured at its first draw */
#define NSEGM 16
static float g_segm[NSEGM][4][4]; /* this frame's segments' matrices, by event index */
static int g_segm_ok[NSEGM];
static int g_ev_idx, g_ev_best = -1, g_ev_best_next = -1;
static long long g_ev_last_draws, g_ev_best_draws_next;
static void run_pass(void);
static void seg_event(void) {
    unsigned long long f = photon_frame_count();
    long long draws = photon_dmirror_draw_count();
    if (f != g_ev_frame) {
        /* last frame: the largest segment is the main view; the pass goes at the end of the LAST segment with the same camera (the main
         * view can be split by other views' render-target switches) */
        int target = g_ev_best_next;
        if (target >= 0 && target < NSEGM && g_segm_ok[target])
            for (int i = g_ev_idx - 1 < NSEGM - 1 ? g_ev_idx - 1 : NSEGM - 1; i > target; i--) {
                if (!g_segm_ok[i]) continue;
                float diff = 0;
                for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) diff += fabsf(g_segm[i][a][b] - g_segm[target][a][b]);
                if (diff < 1e-3f * (1.0f + fabsf(g_segm[target][3][3]))) { target = i; break; }
            }
        g_ev_best = target; g_ev_best_next = -1; g_ev_best_draws_next = -1; g_ev_frame = f; g_ev_idx = 0;
        memset(g_segm_ok, 0, sizeof g_segm_ok);
    }
    long long seg = draws - g_ev_last_draws;
    if (seg > g_ev_best_draws_next) { g_ev_best_draws_next = seg; g_ev_best_next = g_ev_idx; }
    photon_shadows_trace("EVENT seg", g_ev_idx, NULL, (unsigned long)seg | ((unsigned long)g_seg_have << 24) | ((unsigned long)(g_ev_idx == g_ev_best) << 25));
    if (g_ev_idx < NSEGM) { g_segm_ok[g_ev_idx] = g_seg_have; if (g_seg_have) memcpy(g_segm[g_ev_idx], g_seg_m, sizeof g_seg_m); }
    /* the pass now runs at the main view's end from the engine (photon_shadows_main_view_end); the device-level segments stay for the trace */
    g_ev_idx++;
    g_ev_last_draws = draws;
    g_seg_have = 0;
    g_sw_leaves++;
}
void photon_shadows_ds_switch(void *old_ds, void *new_ds, void *rt0) {
    if (!photon_shadows_on()) return;
    debug_mode();
    if (new_ds && g_bb && rt0 == (void *)g_bb) scene_note((IDirect3DSurface9 *)new_ds);
    if (new_ds && new_ds == (void *)g_scene_sfc) { g_ev_last_draws = photon_dmirror_draw_count(); g_seg_have = 0; return; }
    if (old_ds && old_ds == (void *)g_scene_sfc) seg_event();
}
/* PT-RT models: set on the render thread once the main view's end pass ran (the view model and the HUD draw after it: they keep the
 * engine's light, since no composite lights them), cleared at Present */
static volatile int g_main_done;
int photon_shadows_main_done(void) { return g_main_done; }
void *photon_shadows_motion_tex(void) { return g_taa_mv; }
void *photon_shadows_model_nrm_tex(void) { return g_mnrm; }
int photon_shadows_sun_state(float rows[12][4], void **map) {   /* fog.c: this frame's sun shadow-map constants (shadow_ps c0..c11) and map */
    if (!g_pt_sun_ok || !g_sm_tex) return 0;
    memcpy(rows, g_pt_sun, sizeof g_pt_sun); *map = g_sm_tex; return 1;
}
void *photon_shadows_world_nrm_tex(void) { return g_wn_ok ? g_wn_tex : NULL; }   /* SP: the world's smooth normals this frame (PT-RT only), or NULL */
void *photon_shadows_world_pbr_tex(void) { return g_wpbr_tex; }   /* PBR: PT-RT wraps it (the world's roughness / metalness) */   /* PT-RT wraps it (the moving models' motion vectors) */
/* PT-RT models: the draw goes to the main scene's own targets (the back buffer and the scene depth): not a reflection or monitor texture */
int photon_shadows_scene_target(void) {
    return g_scene_sfc && g_bb && photon_dmirror_ds() == (void *)g_scene_sfc && photon_dmirror_rt0() == (void *)g_bb;
}
void photon_shadows_present(void) {   /* the Present hook, before the real Present: the last segment of the frame ends */
    g_main_done = 0;
    photon_dmirror_upscale_arm(NULL, NULL, 0, 0);
    if (!photon_shadows_on() || !g_scene_sfc || photon_dmirror_ds() != (void *)g_scene_sfc) return;
    seg_event();
}
static void run_pass(void) {
    if (!debug_mode() || !g_scene_tex) return;
    unsigned long long f = photon_frame_count();
    {
        /* the shader API's current view and projection (row-vector: clip = (x,y,z,1) * V * P); m = transpose(V * P) has the rows the
         * shader dots with (clip_j = dot(world, m[j])) */
        float m[4][4], iv[5][4];
        memcpy(m, g_seg_m, sizeof m);
        static int mlogs;
        if (mlogs < 4) {
            mlogs++;
            photon_log("shadows: dbg frame %llu event %d: api VP rows %.4f %.4f %.4f %.1f | %.4f %.4f %.4f %.1f | %.4f %.4f %.4f %.1f | %.4f %.4f %.4f %.1f", f,
                       g_ev_idx, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0],
                       m[3][1], m[3][2], m[3][3]);
        }
        if (inv4(m, iv)) {
            iv[4][0] = 64.0f; iv[4][1] = 0.6f; iv[4][2] = g_debug == 2 ? 1.0f : 0.0f; iv[4][3] = 0.0f;
            if (photon_dmirror_fullscreen(k_shpass_vs, k_shdebug_ps, &g_fs_vs, &g_fs_ps, g_scene_tex, (const float (*)[4])iv, 5, 1,
                                          (int)g_scene_w, (int)g_scene_h, g_bb)) g_dbg_passes++;
        }
    }
}

/* PHOTON_SHADOWS_TRACE=N: every render-target and depth bind and every clear of frame N, with the draw count, in the log */
static long long g_trace_frame = -2;
static int g_trace_lines;
void photon_shadows_trace(const char *what, int idx, void *sfc, unsigned long flags) {
    if (g_trace_frame == -2) { char e[16]; g_trace_frame = GetEnvironmentVariableA("PHOTON_SHADOWS_TRACE", e, sizeof e) > 0 ? atoll(e) : -1; }
    if (g_trace_frame < 0 || (long long)photon_frame_count() != g_trace_frame || g_trace_lines >= 400) return;
    g_trace_lines++;
    D3DSURFACE_DESC dd;
    memset(&dd, 0, sizeof dd);
    if (sfc) ((IDirect3DSurface9 *)sfc)->lpVtbl->GetDesc((IDirect3DSurface9 *)sfc, &dd);
    photon_log("shadows trace: draws %lld %s %d %p %ux%u fmt %d flags 0x%lx%s%s%s", photon_dmirror_draw_count(), what, idx, sfc, dd.Width, dd.Height,
               (int)dd.Format, flags, sfc && sfc == (void *)g_bb ? " BACKBUFFER" : "", sfc && sfc == (void *)g_auto ? " AUTO" : "",
               sfc && sfc == (void *)g_intz_sfc ? " STANDIN" : "");
}

void photon_shadows_draw(void *ds) {
    if (g_seg_have || !g_scene_sfc || ds != (void *)g_scene_sfc || (!debug_mode() && !drawing())) return;
    float V[16], P[16];
    if (!photon_vlg_api_matrix(0, V) || !photon_vlg_api_matrix(1, P)) return;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += V[i * 4 + k] * P[k * 4 + j];
            g_seg_m[j][i] = v;
        }
    g_seg_have = 1;
}

/* The main view's PopView (p6.c, main thread): the pass is queued to MatQueue ahead of the engine's pop (or run at once in sync mode), so
 * it runs in stream order with the main view's render target, depth and matrices still current. */
static void shadow_pass(void);
/* TOON (TASKS): outlines on silhouettes, over the main view (outline_ps), when photon_toon is on; independent of the shadows */
static void *g_outline_ps;
static volatile LONG64 g_outline_passes;
static void toon_outline_pass(void) {
    IDirect3DDevice9 *d = photon_d3d_device();
    float V[16], P[16], m[4][4], iv[8][4];
    if (!d || !g_scene_tex || !g_bb || !photon_vlg_api_matrix(0, V) || !photon_vlg_api_matrix(1, P)) return;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += V[i * 4 + k] * P[k * 4 + j];
            m[j][i] = v;
        }
    view_rect_fix(m);
    if (!inv4(m, iv)) return;
    float c[5][4];
    memcpy(c, iv, 4 * sizeof c[0]);
    c[4][0] = 1.0f / (float)g_scene_w; c[4][1] = 1.0f / (float)g_scene_h; c[4][2] = 0.04f; c[4][3] = 0.9f;
    photon_passtime_begin(PT_OUTLINE);
    if (photon_dmirror_fullscreen(k_shpass_vs, k_outline_ps, &g_sh_vs, &g_outline_ps, g_scene_tex, (const float (*)[4])c, 5, 1, (int)g_scene_w, (int)g_scene_h, g_bb))
        g_outline_passes++;
    photon_passtime_end(PT_OUTLINE);
}
/* PT-RT (analysis/43): the main view, kept for passes that run later in the frame (the Vulkan bridge runs at Present, when the matrices
 * may already be the HUD's): the inverse view-projection rows, the eye, the scene depth texture */
static float g_mv_iv[4][4], g_mv_m[4][4], g_mv_cam[3];
static float g_mv_raw[4][4];   /* AA: the main view's rows before view_rect_fix: its draws' own c8..c11 */
static volatile LONG g_mv_seq;
static SRWLOCK g_mv_lock = SRWLOCK_INIT;
/* RSCALE (TASKS): under mat_viewportscale < 1 the engine draws the 3D view into a rect of the back buffer and upscales it at the end, with
 * the same projection. Folding the rect into the main view's rows (clip' = M^-1 clip, M the map from the texture's ndc to the view's)
 * makes every consumer work in the scene texture's own coordinates: reconstruction (the inverse becomes iv M), Nanite's and the world
 * normals' drawing (into the rect), PT-RT's reprojection. Identity at 100%. Applied only to a plausible sub-rect of the scene. */
static void view_rect_fix(float m[4][4]) {
    unsigned v[4];
    if (!g_scene_w || !g_scene_h || !photon_dmirror_viewport(v) || !v[2] || !v[3]) return;
    if (v[0] + v[2] > g_scene_w || v[1] + v[3] > g_scene_h) return;
    float sx = (float)v[2] / (float)g_scene_w, sy = (float)v[3] / (float)g_scene_h, ox = (float)v[0] / (float)g_scene_w, oy = (float)v[1] / (float)g_scene_h;
    if (sx > 0.999f && sy > 0.999f && ox < 0.001f && oy < 0.001f) return;   /* full view */
    if (sx < 0.1f || sy < 0.1f || fabsf(sx - sy) > 0.02f * sx) return;          /* not the scaled 3D view (a 2D panel, a different aspect) */
    float ax = sx - 1 + 2 * ox, ay = 1 - 2 * oy - sy;
    for (int j = 0; j < 4; j++) { m[0][j] = sx * m[0][j] + ax * m[3][j]; m[1][j] = sy * m[1][j] + ay * m[3][j]; }
    static int logged;
    if (!logged) { logged = 1; photon_log("shadows: RSCALE: the 3D view is a %ux%u rect at %u,%u of the %ux%u scene: Photon's passes follow it", v[2], v[3], v[0], v[1], g_scene_w, g_scene_h); }
}
static void snap_view(void) {
    float V[16], P[16], m[4][4], iv[4][4], Vm[4][4], Vi[4][4];
    if (!photon_vlg_api_matrix(0, V) || !photon_vlg_api_matrix(1, P)) return;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += V[i * 4 + k] * P[k * 4 + j];
            m[j][i] = v;
            Vm[i][j] = V[i * 4 + j];
        }
    float raw[4][4]; memcpy(raw, m, sizeof raw);
    view_rect_fix(m);
    float ivx[8][4];
    if (!inv4(m, ivx) || !inv4(Vm, Vi)) return;
    memcpy(iv, ivx, sizeof iv);
    AcquireSRWLockExclusive(&g_mv_lock);
    memcpy(g_mv_iv, iv, sizeof iv);
    memcpy(g_mv_m, m, sizeof m);
    memcpy(g_mv_raw, raw, sizeof raw);
    g_mv_cam[0] = Vi[3][0]; g_mv_cam[1] = Vi[3][1]; g_mv_cam[2] = Vi[3][2];
    static int eyelog = -1;   /* PHOTON_LOG_EYE=1: the main view's eye and forward axis every 100 views (matching a reference camera) */
    if (eyelog < 0) { char e[4]; eyelog = GetEnvironmentVariableA("PHOTON_LOG_EYE", e, sizeof e) > 0 && e[0] == '1'; }
    static long long views;
    if (eyelog && ++views % 100 == 0)
        photon_log("shadows: eye %.2f %.2f %.2f forward %.4f %.4f %.4f (P00 %.4f P11 %.4f)", Vi[3][0], Vi[3][1], Vi[3][2], -Vi[2][0], -Vi[2][1], -Vi[2][2], P[0], P[5]);
    g_mv_seq++;
    ReleaseSRWLockExclusive(&g_mv_lock);
}
long photon_shadows_view(float iv[4][4], float vp[4][4], float cam[3], void **depth, unsigned *w, unsigned *h) {
    AcquireSRWLockShared(&g_mv_lock);
    memcpy(iv, g_mv_iv, sizeof g_mv_iv); memcpy(vp, g_mv_m, sizeof g_mv_m); memcpy(cam, g_mv_cam, sizeof g_mv_cam);
    long seq = g_mv_seq;
    ReleaseSRWLockShared(&g_mv_lock);
    *depth = g_scene_tex; *w = g_scene_w; *h = g_scene_h;
    return seq;
}
/* PT-RT composite (ptrt_comp_ps): world pixels (drawn with flat lightmaps, so holding about their surface colour) x Photon's light x
 * exposure, after the Vulkan pass wrote it; models (the world-only depth copy) keep their own lighting */
static void *g_ptrt_ps;
static void taa_rt(IDirect3DDevice9 *d, IDirect3DTexture9 **t, IDirect3DSurface9 **sf, D3DFORMAT f);
void photon_shadows_ptrt_composite(void *tex, void *sunvis, void *spec, float exposure, const float sun[3], const float sunc[3]) {
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d || !tex || !g_scene_tex || !g_bb) return;
    if (g_fcopy && (g_fcopy_w != g_scene_w || g_fcopy_h != g_scene_h)) {
        g_fcopy_sfc->lpVtbl->Release(g_fcopy_sfc); g_fcopy->lpVtbl->Release(g_fcopy); g_fcopy = NULL; g_fcopy_sfc = NULL;
    }
    if (!g_fcopy) {   /* the frame (the back buffer's format: X8R8G8B8 / A8R8G8B8) copied for the linear-light composite */
        D3DSURFACE_DESC bd; g_bb->lpVtbl->GetDesc(g_bb, &bd);
        if (FAILED(d->lpVtbl->CreateTexture(d, g_scene_w, g_scene_h, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g_fcopy, NULL))) { g_fcopy = NULL; return; }
        g_fcopy->lpVtbl->GetSurfaceLevel(g_fcopy, 0, &g_fcopy_sfc);
        g_fcopy_w = g_scene_w; g_fcopy_h = g_scene_h;
    }
    RECT r = { 0, 0, (LONG)g_scene_w, (LONG)g_scene_h };
    if (FAILED(d->lpVtbl->StretchRect(d, g_bb, &r, g_fcopy_sfc, &r, D3DTEXF_NONE))) return;
    int masked = g_wd_valid && g_wd_tex;
    void *ndep = photon_nanite_on() ? photon_nanite_depth_tex() : NULL;   /* Nanite's props, merged before this in Path traced */
    /* c0 params (x exposure, y masked, z the sun on); c1..c12 the sun maps' constants (shadow_ps.hlsl's c0..c11); c13 the sun's
     * direction (toward it), c14 its colour */
    float c[27][4];
    memset(c, 0, sizeof c);
    c[0][0] = exposure; c[0][1] = masked ? 1.0f : 0.0f; c[0][2] = g_pt_sun_ok ? 1.0f : 0.0f; c[0][3] = sunvis ? 1.0f : 0.0f;
    if (g_pt_sun_ok) memcpy(c[1], g_pt_sun, sizeof g_pt_sun);
    memcpy(c[13], sun, 12); memcpy(c[14], sunc, 12); c[15][0] = g_wn_ok ? 1.0f : 0.0f;
    static int sdbg = -1;
    if (sdbg < 0) { char e[4]; sdbg = GetEnvironmentVariableA("PHOTON_PTRT_SUNDEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    c[15][1] = (float)sdbg;
    static int wdbg = -1;
    if (wdbg < 0) { char e[4]; wdbg = GetEnvironmentVariableA("PHOTON_PTRT_WN_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    c[15][2] = (float)wdbg;
    c[15][3] = ndep ? 1.0f : 0.0f;
    { float iv2[4][4], vp2[4][4], cam[3]; void *dp; unsigned ww, hh; if (photon_shadows_view(iv2, vp2, cam, &dp, &ww, &hh)) memcpy(c[16], cam, 12); }
    c[16][3] = photon_ptrt_models() ? 1.0f : 0.0f;
    c[17][0] = g_mnrm_ok ? 1.0f : 0.0f;   /* the models' own normals (velocity_pass) */
    static int pbrdbg = -1;   /* PHOTON_PBR_DEBUG=1: the world's roughness (red) and metalness (green) instead of the frame */
    if (pbrdbg < 0) { char e[4]; pbrdbg = GetEnvironmentVariableA("PHOTON_PBR_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    c[17][1] = pbrdbg && g_wn_ok && g_wpbr_tex ? 1.0f : 0.0f;
    static int nospec = -1;   /* PHOTON_PBR_SPEC=0: no glossy reflections in the composite (A/B) */
    if (nospec < 0) { char e[4]; nospec = GetEnvironmentVariableA("PHOTON_PBR_SPEC", e, sizeof e) > 0 && e[0] == '0'; }
    c[17][2] = spec && !nospec && g_wn_ok && g_wpbr_tex ? 1.0f : 0.0f;   /* PBR step 3: the glossy reflections (s11) with the world's channels (s10) */
    photon_dmirror_fullscreen_texn(1, tex, 1);
    if (masked) photon_dmirror_fullscreen_texn(2, g_wd_tex, 0);
    photon_dmirror_fullscreen_texn(3, g_fcopy, 0);
    if (sunvis) photon_dmirror_fullscreen_texn(6, sunvis, 1);   /* the world's sun visibility (half size, bilinear) */
    if (g_wn_ok) photon_dmirror_fullscreen_texn(7, g_wn_tex, 0);   /* the world's smooth normals */
    if (ndep) photon_dmirror_fullscreen_texn(8, ndep, 0);   /* where Nanite drew: lit as world */
    if (g_mnrm_ok) photon_dmirror_fullscreen_texn(9, g_mnrm, 0);
    if (c[17][1] > 0.5f || c[17][2] > 0.5f) photon_dmirror_fullscreen_texn(10, g_wpbr_tex, 0);
    if (c[17][2] > 0.5f) photon_dmirror_fullscreen_texn(11, spec, 1);
    /* PBR step 3: screen-space reflections from last frame's composited frame (copied after this pass), through last frame's rows */
    float ssr_vp[4][4], ssr_iv[4][4], ssr_cam[3]; void *ssr_dep; unsigned ssr_w, ssr_h;
    {   /* PT macro variation (c26.y strength, c26.z scale in units): PHOTON_PTRT_MACROVAR="strength scale" (0: off; default 0.12 over 300) */
        static float mv[2] = { -1, 300 };
        if (mv[0] < 0) { char e[32]; mv[0] = 0.12f; if (GetEnvironmentVariableA("PHOTON_PTRT_MACROVAR", e, sizeof e) > 0) sscanf(e, "%f %f", &mv[0], &mv[1]); if (mv[1] < 1) mv[1] = 300; }
        c[26][1] = mv[0]; c[26][2] = mv[1];
    }
    static int nossr = -1;   /* PHOTON_PTRT_SSR=0: no screen-space part in the glossy reflections (A/B) */
    if (nossr < 0) { char e[4]; nossr = GetEnvironmentVariableA("PHOTON_PTRT_SSR", e, sizeof e) > 0 && e[0] == '0'; }
    int ssr = !nossr && c[17][2] > 0.5f && g_ssr_have && g_ssr_prev && photon_shadows_view(ssr_iv, ssr_vp, ssr_cam, &ssr_dep, &ssr_w, &ssr_h);
    if (ssr) {
        memcpy(c[18], ssr_vp, sizeof(float) * 16); memcpy(c[22], g_ssr_rows, sizeof(float) * 16); c[26][0] = 1;
        photon_dmirror_fullscreen_texn(12, g_ssr_prev, 1);
    }
    if (g_pt_sun_ok) { photon_dmirror_fullscreen_texn(4, g_sm_tex, 0); if (g_pt_sun[11][1] > 0.5f && g_sn_tex) photon_dmirror_fullscreen_texn(5, g_sn_tex, 0); }
    photon_dmirror_fullscreen(k_shpass_vs, k_ptrt_comp_ps, &g_sh_vs, &g_ptrt_ps, g_scene_tex, (const float (*)[4])c, 27, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    for (int k = 1; k <= 12; k++) photon_dmirror_fullscreen_texn(k, NULL, 0);
    if (c[17][2] > 0.5f) {   /* this frame's composited frame and rows, for the next frame's screen-space reflections */
        IDirect3DDevice9 *dd = photon_d3d_device();
        D3DSURFACE_DESC bd2; g_bb->lpVtbl->GetDesc(g_bb, &bd2);
        if (g_ssr_prev && (g_ssr_w != g_scene_w || g_ssr_h != g_scene_h)) { g_ssr_prev_s->lpVtbl->Release(g_ssr_prev_s); g_ssr_prev->lpVtbl->Release(g_ssr_prev); g_ssr_prev = NULL; g_ssr_prev_s = NULL; g_ssr_have = 0; }
        if (!g_ssr_prev && dd) { taa_rt(dd, &g_ssr_prev, &g_ssr_prev_s, bd2.Format); g_ssr_w = g_scene_w; g_ssr_h = g_scene_h; }
        float pv[4][4], piv[4][4], pcam[3]; void *pdep; unsigned pw, ph;
        RECT r = { 0, 0, (LONG)g_scene_w, (LONG)g_scene_h };
        if (dd && g_ssr_prev_s && photon_shadows_view(piv, pv, pcam, &pdep, &pw, &ph) && SUCCEEDED(dd->lpVtbl->StretchRect(dd, g_bb, &r, g_ssr_prev_s, &r, D3DTEXF_NONE))) {
            memcpy(g_ssr_rows, pv, sizeof g_ssr_rows); g_ssr_have = 1;
        }
    } else g_ssr_have = 0;
}
/* NANITE N2b: Photon's static props (colour + D3D depth textures) merged into the main view, depth-tested against the scene depth */
static void *g_nanite_ps;
int photon_shadows_nanite_merge(void *col, void *dep) {
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d || !col || !dep || !g_scene_tex || !g_scene_sfc || !g_bb) return 0;
    float none[1][4] = { { 0, 0, 0, 0 } };
    photon_dmirror_fullscreen_texn(1, dep, 0);
    photon_dmirror_fullscreen_depth(g_scene_sfc);
    int ok = photon_dmirror_fullscreen(k_shpass_vs, k_nanite_merge_ps, &g_sh_vs, &g_nanite_ps, col, (const float (*)[4])none, 1, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    photon_dmirror_fullscreen_texn(1, NULL, 0);
    return ok;
}
/* AA: the main view resolved against its history (taa_ps), at its end: after Photon's own passes, before the view model and the HUD */
static void taa_rt(IDirect3DDevice9 *d, IDirect3DTexture9 **t, IDirect3DSurface9 **sf, D3DFORMAT f) {
    if (*t) return;
    if (FAILED(d->lpVtbl->CreateTexture(d, g_scene_w, g_scene_h, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, t, NULL))) { *t = NULL; return; }
    (*t)->lpVtbl->GetSurfaceLevel(*t, 0, sf);
}
static int taau_this_frame(void);
static void taa_pass(void) {
    if (!photon_taa_on()) return;
    if (taau_this_frame()) { photon_taa_main_view_end(); return; }   /* UPSCALE: the temporal upscale resolves this frame (at the engine's upscale) */
    photon_taa_main_view_end();
    static unsigned long long last_frame;
    unsigned long long fnow = photon_frame_count();
    if (fnow - last_frame > 2) g_taa_have = 0;   /* TAA was off (or the view not drawn): the history is stale */
    last_frame = fnow;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d || !g_scene_tex || !g_bb) return;
    float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
    if (!photon_shadows_view(iv, vp, cam, &dep, &w, &h)) return;
    if (g_taa_cur && (g_taa_w != g_scene_w || g_taa_h != g_scene_h)) {   /* a new size: start over */
        g_taa_cur_s->lpVtbl->Release(g_taa_cur_s); g_taa_cur->lpVtbl->Release(g_taa_cur); g_taa_cur = NULL; g_taa_cur_s = NULL;
        g_taa_hist_s->lpVtbl->Release(g_taa_hist_s); g_taa_hist->lpVtbl->Release(g_taa_hist); g_taa_hist = NULL; g_taa_hist_s = NULL; g_taa_have = 0;
        if (g_taa_dist) { g_taa_dist_s->lpVtbl->Release(g_taa_dist_s); g_taa_dist->lpVtbl->Release(g_taa_dist); g_taa_dist = NULL; g_taa_dist_s = NULL; }
    }
    D3DSURFACE_DESC bd; g_bb->lpVtbl->GetDesc(g_bb, &bd);
    taa_rt(d, &g_taa_cur, &g_taa_cur_s, bd.Format); taa_rt(d, &g_taa_hist, &g_taa_hist_s, bd.Format); taa_rt(d, &g_taa_dist, &g_taa_dist_s, D3DFMT_R32F);
    if (!g_taa_cur || !g_taa_hist) return;
    g_taa_w = g_scene_w; g_taa_h = g_scene_h;
    /* this frame's rows without the jitter (clip.x carries jx * w): what the next frame reprojects into */
    float j[2]; photon_taa_jitter_ndc(j);
    float uj[4][4]; memcpy(uj, vp, sizeof uj);
    for (int k = 0; k < 4; k++) { uj[0][k] -= j[0] * vp[3][k]; uj[1][k] -= j[1] * vp[3][k]; }
    RECT r = { 0, 0, (LONG)g_scene_w, (LONG)g_scene_h };
    if (FAILED(d->lpVtbl->StretchRect(d, g_bb, &r, g_taa_cur_s, &r, D3DTEXF_NONE))) return;
    int mv = g_vel_ok;   /* the moving models' own motion (velocity_pass, at the start of this view's end) */
    float c[11][4];
    memcpy(c[0], iv, sizeof(float) * 16);
    memcpy(c[4], g_taa_have ? g_taa_prev : uj, sizeof(float) * 16);
    c[8][0] = 1.0f / (float)g_scene_w; c[8][1] = 1.0f / (float)g_scene_h; c[8][2] = 0.1f; c[8][3] = g_taa_have ? 1.0f : 0.0f;
    c[9][0] = g_taa_prev_eye[0]; c[9][1] = g_taa_prev_eye[1]; c[9][2] = g_taa_prev_eye[2]; c[9][3] = g_taa_have && g_taa_dist ? 1.0f : 0.0f;
    static int mvdbg = -1;
    if (mvdbg < 0) { char e[4]; mvdbg = GetEnvironmentVariableA("PHOTON_TAA_MV_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    static int rjdbg = -1;
    if (rjdbg < 0) { char e[4]; rjdbg = GetEnvironmentVariableA("PHOTON_TAA_REJECT_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    static int bil = -1;   /* PHOTON_TAA_BILINEAR=1: the history read bilinear instead of Catmull-Rom (A/B) */
    if (bil < 0) { char e[4]; bil = GetEnvironmentVariableA("PHOTON_TAA_BILINEAR", e, sizeof e) > 0 && e[0] == '1'; }
    c[10][0] = mv ? 1.0f : 0.0f; c[10][1] = mv && mvdbg ? 1.0f : 0.0f; c[10][2] = (float)rjdbg; c[10][3] = (float)bil;
    photon_dmirror_fullscreen_texn(1, g_taa_cur, 0);
    photon_dmirror_fullscreen_texn(2, g_taa_hist, 1);
    if (g_taa_dist) photon_dmirror_fullscreen_texn(3, g_taa_dist, 0);
    if (mv) photon_dmirror_fullscreen_texn(4, g_vel_tex, 0);
    int ok = photon_dmirror_fullscreen(k_shpass_vs, k_taa_ps, &g_sh_vs, &g_taa_ps, g_scene_tex, (const float (*)[4])c, 11, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    photon_dmirror_fullscreen_texn(1, NULL, 0); photon_dmirror_fullscreen_texn(2, NULL, 0); photon_dmirror_fullscreen_texn(3, NULL, 0);
    if (mv) photon_dmirror_fullscreen_texn(4, NULL, 0);
    if (ok && g_taa_dist_s) {   /* this frame's distances, for the next frame's check */
        float dc[6][4];
        memcpy(dc[0], iv, sizeof(float) * 16);
        dc[4][0] = cam[0]; dc[4][1] = cam[1]; dc[4][2] = cam[2]; dc[4][3] = 1;
        dc[5][0] = 1.0f / (float)g_scene_w; dc[5][1] = 1.0f / (float)g_scene_h; dc[5][2] = dc[5][3] = 0;
        photon_dmirror_fullscreen(k_shpass_vs, k_taa_dist_ps, &g_sh_vs, &g_taa_d_ps, g_scene_tex, (const float (*)[4])dc, 6, 0, (int)g_scene_w, (int)g_scene_h, g_taa_dist_s);
        memcpy(g_taa_prev_eye, cam, sizeof g_taa_prev_eye);
    }
    if (ok && SUCCEEDED(d->lpVtbl->StretchRect(d, g_bb, &r, g_taa_hist_s, &r, D3DTEXF_NONE))) {
        memcpy(g_taa_prev, uj, sizeof uj); g_taa_have = 1;
        /* the display sharpened (the history above stays as resolved); photon_taa_sharpen 0..1, 0 off, default 0.8 */
        g_taa_sharp = photon_vlg_taa_sharpen();   /* photon_taa_sharpen (live; PHOTON_TAA_SHARPEN sets it too) */
        if (g_taa_sharp > 0) {
            float sc[1][4] = { { 1.0f / (float)g_scene_w, 1.0f / (float)g_scene_h, g_taa_sharp, 0 } };
            photon_dmirror_fullscreen(k_shpass_vs, k_taa_sharpen_ps, &g_sh_vs, &g_taa_sh_ps, g_taa_hist, (const float (*)[4])sc, 1, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
        }
    }
}
/* AA / PT-RT: the main view's moving models into a motion-vector target (dmirror.c photon_dmirror_velocity: one draw per model that
 * moved, none in a still scene), first thing at the main view's end, so the path tracer's history and the TAA resolve both follow them.
 * With PT-RT the target is its interop image (photon_ptrt_motion_tex), else g_taa_mv. Last frame's rows are kept here, unjittered. */
static float g_vel_prev[4][4], g_vel_prev_eye[3]; static int g_vel_have; static unsigned long long g_vel_last;
static void velocity_pass(void) {
    g_vel_ok = 0; g_vel_tex = NULL; g_mnrm_ok = 0;
    int pt = photon_ptrt_on();
    if (g_taa_mv_on < 0) { char e[4]; g_taa_mv_on = !(GetEnvironmentVariableA("PHOTON_TAA_MV", e, sizeof e) > 0 && e[0] == '0'); }
    IDirect3DDevice9 *d = photon_d3d_device();
    float iv[4][4], vp[4][4], cam[3], raw[4][4]; void *dep; unsigned w, h;
    if (!(photon_taa_on() || pt) || !g_taa_mv_on || !d || !g_scene_sfc || !photon_shadows_view(iv, vp, cam, &dep, &w, &h)) {
        g_vel_have = 0; photon_ptrt_motion_drawn(0); return;
    }
    unsigned long long f = photon_frame_count();
    if (f - g_vel_last > 2) g_vel_have = 0;   /* a gap: last frame's rows are stale */
    g_vel_last = f;
    AcquireSRWLockShared(&g_mv_lock); memcpy(raw, g_mv_raw, sizeof raw); ReleaseSRWLockShared(&g_mv_lock);
    float j[2]; photon_taa_jitter_ndc(j);
    float uj[4][4]; memcpy(uj, vp, sizeof uj);
    for (int k = 0; k < 4; k++) { uj[0][k] -= j[0] * vp[3][k]; uj[1][k] -= j[1] * vp[3][k]; }
    IDirect3DTexture9 *tex = NULL; IDirect3DSurface9 *sfc = NULL;
    static int ptmv = -1;   /* PHOTON_PTRT_MV=0: the path tracer's history camera-only (the motion for TAA alone) */
    if (ptmv < 0) { char e[4]; ptmv = !(GetEnvironmentVariableA("PHOTON_PTRT_MV", e, sizeof e) > 0 && e[0] == '0'); }
    /* one target for both: PT-RT wraps this texture at its first bind, so it is not re-created on a size change while PT-RT has it */
    if (g_taa_mv && (g_vel_w != g_scene_w || g_vel_h != g_scene_h) && photon_ptrt_motion_tex() != (void *)g_taa_mv) {
        g_taa_mv_s->lpVtbl->Release(g_taa_mv_s); g_taa_mv->lpVtbl->Release(g_taa_mv); g_taa_mv = NULL; g_taa_mv_s = NULL;
    }
    if (!g_taa_mv) { taa_rt(d, &g_taa_mv, &g_taa_mv_s, D3DFMT_A16B16G16R16F); g_vel_w = g_scene_w; g_vel_h = g_scene_h; }
    if (g_vel_w != g_scene_w || g_vel_h != g_scene_h) { g_vel_have = 0; photon_ptrt_motion_drawn(0); return; }
    tex = g_taa_mv; sfc = g_taa_mv_s;
    if (sfc) sfc->lpVtbl->AddRef(sfc);
    /* PT-RT models: every main-view model drawn (not only the moving ones), with its world normal into g_mnrm beside the motion */
    int pm = photon_ptrt_models();
    if (pm && g_mnrm && g_vel_w != g_scene_w && photon_ptrt_model_nrm_tex() != (void *)g_mnrm) { g_mnrm_s->lpVtbl->Release(g_mnrm_s); g_mnrm->lpVtbl->Release(g_mnrm); g_mnrm = NULL; g_mnrm_s = NULL; }
    if (pm && !g_mnrm) taa_rt(d, &g_mnrm, &g_mnrm_s, D3DFMT_A16B16G16R16F);
    int n = sfc ? photon_dmirror_velocity(sfc, pm ? (void *)g_mnrm_s : NULL, pm && g_mnrm_s, g_scene_sfc, (int)g_scene_w, (int)g_scene_h, (const float (*)[4])raw,
                                          (const float (*)[4])(g_vel_have ? g_vel_prev : uj), g_vel_prev_eye, g_vel_have) : -1;
    if (sfc) sfc->lpVtbl->Release(sfc);
    memcpy(g_vel_prev, uj, sizeof uj); memcpy(g_vel_prev_eye, cam, sizeof g_vel_prev_eye); g_vel_have = 1;
    g_vel_ok = n > 0; g_vel_tex = tex; g_mnrm_ok = n > 0 && pm && g_mnrm_s;
    photon_ptrt_model_nrm_drawn(g_mnrm_ok);
    photon_ptrt_motion_drawn(n > 0 && pt && ptmv);
}
/* UPSCALE (analysis/45) U1: the engine's upscale draw replaced by Photon's (photon_upscale 2: the corner stretched bilinear, as the
 * engine's dev/upscale does: the hook's proof, U2 makes it temporal) */
#include "shaders/upscale_ps.h"
static int g_up_on = -1; static void *g_up_ps; static unsigned g_up_vw, g_up_vh;
static long long g_up_draws;
/* U2: Photon TAAU (photon_upscale 1: jittered while the view is scaled): the corner resolved into a full-size history (taau_ps) */
#include "shaders/taau_ps.h"
static void *g_tu_ps;
static float g_tu_prev[4][4], g_tu_prev_eye[3]; static unsigned long long g_tu_last;
static long long g_tu_draws;
static int taau_this_frame(void) {   /* main view's end: the view is scaled and TAAU will take the engine's upscale */
    g_up_on = photon_vlg_upscale();
    unsigned v[4];
    return (g_up_on == 1 || g_up_on == 3) && photon_taa_on() && g_bb && g_scene_w && photon_dmirror_viewport(v) && !v[0] && !v[1] && (v[2] < g_scene_w || v[3] < g_scene_h);
}
static int taau_resolve(IDirect3DDevice9 *d) {
    float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
    if (!photon_shadows_view(iv, vp, cam, &dep, &w, &h)) return 0;
    D3DSURFACE_DESC bd; g_bb->lpVtbl->GetDesc(g_bb, &bd);
    taa_rt(d, &g_tu_hist, &g_tu_hist_s, bd.Format); taa_rt(d, &g_tu_dist, &g_tu_dist_s, D3DFMT_R32F);
    if (!g_tu_hist_s || !g_tu_dist_s) return 0;
    unsigned long long f = photon_frame_count();
    if (f - g_tu_last > 2) g_tu_have = 0;
    g_tu_last = f;
    float j[2]; photon_taa_jitter_ndc(j);
    float uj[4][4]; memcpy(uj, vp, sizeof uj);
    for (int k = 0; k < 4; k++) { uj[0][k] -= j[0] * vp[3][k]; uj[1][k] -= j[1] * vp[3][k]; }
    float sx = (float)g_up_vw / (float)g_scene_w, sy = (float)g_up_vh / (float)g_scene_h;
    float c[11][4];
    memcpy(c[0], iv, sizeof(float) * 16);
    memcpy(c[4], g_tu_have ? g_tu_prev : uj, sizeof(float) * 16);
    c[8][0] = 1.0f / (float)g_scene_w; c[8][1] = 1.0f / (float)g_scene_h; c[8][2] = sx; c[8][3] = sy;
    c[9][0] = g_tu_prev_eye[0]; c[9][1] = g_tu_prev_eye[1]; c[9][2] = g_tu_prev_eye[2]; c[9][3] = g_tu_have ? 1.0f : 0.0f;
    c[10][0] = g_vel_ok ? 1.0f : 0.0f; c[10][1] = 0; c[10][2] = j[0]; c[10][3] = j[1];
    photon_dmirror_fullscreen_texn(1, g_up_src, 0);
    photon_dmirror_fullscreen_texn(2, g_tu_hist, 1);
    photon_dmirror_fullscreen_texn(3, g_tu_dist, 0);
    if (g_vel_ok) photon_dmirror_fullscreen_texn(4, g_vel_tex, 0);
    int ok = photon_dmirror_fullscreen(k_shpass_vs, k_taau_ps, &g_sh_vs, &g_tu_ps, g_scene_tex, (const float (*)[4])c, 11, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    for (int k = 1; k <= 4; k++) photon_dmirror_fullscreen_texn(k, NULL, 0);
    if (!ok) return 0;
    float dc[6][4];   /* this frame's distances per output pixel, for the next frame's check */
    memcpy(dc[0], iv, sizeof(float) * 16);
    dc[4][0] = cam[0]; dc[4][1] = cam[1]; dc[4][2] = cam[2]; dc[4][3] = 1;
    dc[5][0] = 1.0f / (float)g_scene_w; dc[5][1] = 1.0f / (float)g_scene_h; dc[5][2] = sx; dc[5][3] = sy;
    photon_dmirror_fullscreen(k_shpass_vs, k_taa_dist_ps, &g_sh_vs, &g_taa_d_ps, g_scene_tex, (const float (*)[4])dc, 6, 0, (int)g_scene_w, (int)g_scene_h, g_tu_dist_s);
    RECT r = { 0, 0, (LONG)g_scene_w, (LONG)g_scene_h };
    if (FAILED(d->lpVtbl->StretchRect(d, g_bb, &r, g_tu_hist_s, &r, D3DTEXF_NONE))) { g_tu_have = 0; return 1; }
    memcpy(g_tu_prev, uj, sizeof uj); memcpy(g_tu_prev_eye, cam, sizeof g_tu_prev_eye); g_tu_have = 1;
    float sh = photon_vlg_taa_sharpen();
    if (sh > 0) {
        float sc[1][4] = { { 1.0f / (float)g_scene_w, 1.0f / (float)g_scene_h, sh, 0 } };
        photon_dmirror_fullscreen(k_shpass_vs, k_taa_sharpen_ps, &g_sh_vs, &g_taa_sh_ps, g_tu_hist, (const float (*)[4])sc, 1, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    }
    if (++g_tu_draws % 300 == 1) photon_log("shadows: TAAU %lld: %ux%u -> %ux%u, jitter %.5f %.5f", g_tu_draws, g_up_vw, g_up_vh, g_scene_w, g_scene_h, j[0], j[1]);
    return 1;
}
/* U4: AMD FSR 2 (photon_upscale 3): full motion vectors (fullmv_ps: the camera's from the depth, the models' own) and the corner into
 * FSR on the Vulkan bridge (fsr2.c), its result copied into the back buffer */
#include "shaders/fullmv_ps.h"
#include "fsr2.h"
static void *g_fmv_ps; static int g_fsr_have;
static int fsr2_resolve(IDirect3DDevice9 *d) {
    float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
    if (!photon_shadows_view(iv, vp, cam, &dep, &w, &h)) return 0;
    if (!g_fmv) taa_rt(d, &g_fmv, &g_fmv_s, D3DFMT_A16B16G16R16F);
    if (!g_fmv_s) return 0;
    unsigned long long f = photon_frame_count();
    if (f - g_tu_last > 2) g_fsr_have = 0;
    g_tu_last = f;
    float j[2]; photon_taa_jitter_ndc(j);
    float uj[4][4]; memcpy(uj, vp, sizeof uj);
    for (int k = 0; k < 4; k++) { uj[0][k] -= j[0] * vp[3][k]; uj[1][k] -= j[1] * vp[3][k]; }
    float c[9][4];
    memcpy(c[0], iv, sizeof(float) * 16);
    memcpy(c[4], g_fsr_have ? g_tu_prev : uj, sizeof(float) * 16);
    c[8][0] = 1.0f / (float)g_scene_w; c[8][1] = 1.0f / (float)g_scene_h; c[8][2] = g_vel_ok ? 1.0f : 0.0f; c[8][3] = 0;
    if (g_vel_ok) photon_dmirror_fullscreen_texn(1, g_vel_tex, 0);
    int ok = photon_dmirror_fullscreen(k_shpass_vs, k_fullmv_ps, &g_sh_vs, &g_fmv_ps, g_scene_tex, (const float (*)[4])c, 9, 0, (int)g_scene_w, (int)g_scene_h, g_fmv_s);
    photon_dmirror_fullscreen_texn(1, NULL, 0);
    if (!ok) return 0;
    /* the depth as an R32F copy: the scene's INTZ is the engine's bound depth-stencil at this point (its layout changes under DXVK) */
    if (!g_fdep) taa_rt(d, &g_fdep, &g_fdep_s, D3DFMT_R32F);
    float none[1][4] = { { 0, 0, 0, 0 } };
    if (!g_fdep_s || !photon_dmirror_fullscreen(k_shpass_vs, k_depthcopy_ps, &g_sh_vs, &g_wd_ps, g_scene_tex, (const float (*)[4])none, 1, 0, (int)g_scene_w, (int)g_scene_h, g_fdep_s))
        return 0;
    void *out = NULL;
    if (!photon_fsr2_frame(g_up_src, g_fdep, g_fmv, g_scene_w, g_scene_h, g_up_vw, g_up_vh, j[0], j[1], photon_vlg_taa_sharpen(), !g_fsr_have, &out) || !out)
        return 0;
    float cc[1][4] = { { 1.0f, 1.0f, 1.0f / (float)g_scene_w, 1.0f / (float)g_scene_h } };
    photon_dmirror_fullscreen_texn(1, out, 0);
    ok = photon_dmirror_fullscreen(k_shpass_vs, k_upscale_ps, &g_sh_vs, &g_up_ps, g_scene_tex, (const float (*)[4])cc, 1, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    photon_dmirror_fullscreen_texn(1, NULL, 0);
    memcpy(g_tu_prev, uj, sizeof uj); g_fsr_have = 1;
    return ok;
}
static int upscale_cb(void) {   /* render thread, inside the engine's upscale draw: the post-processed corner is in the back buffer */
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d || !g_bb || !g_up_vw) return 0;
    if (!g_up_src) taa_rt(d, &g_up_src, &g_up_src_s, D3DFMT_A8R8G8B8);
    if (!g_up_src_s) return 0;
    RECT r = { 0, 0, (LONG)g_up_vw, (LONG)g_up_vh };
    if (FAILED(d->lpVtbl->StretchRect(d, g_bb, &r, g_up_src_s, &r, D3DTEXF_NONE))) return 0;
    if (g_up_on == 3 && photon_taa_on() && fsr2_resolve(d)) return 1;   /* FSR 2; not available: TAAU below */
    if ((g_up_on == 1 || g_up_on == 3) && photon_taa_on() && taau_resolve(d)) return 1;   /* TAAU; else (bilinear test, or no jitter yet) the plain stretch */
    float c[1][4] = { { (float)g_up_vw / (float)g_scene_w, (float)g_up_vh / (float)g_scene_h, 1.0f / (float)g_scene_w, 1.0f / (float)g_scene_h } };
    photon_dmirror_fullscreen_texn(1, g_up_src, 1);
    int ok = photon_dmirror_fullscreen(k_shpass_vs, k_upscale_ps, &g_sh_vs, &g_up_ps, g_scene_tex, (const float (*)[4])c, 1, 0, (int)g_scene_w, (int)g_scene_h, g_bb);
    photon_dmirror_fullscreen_texn(1, NULL, 0);
    if (ok && ++g_up_draws % 300 == 1) photon_log("shadows: upscale %lld: the engine's upscale replaced (%ux%u -> %ux%u)", g_up_draws, g_up_vw, g_up_vh, g_scene_w, g_scene_h);
    return ok;
}
static void upscale_arm(void) {
    g_up_on = photon_vlg_upscale();
    unsigned v[4];
    if (photon_dmirror_viewport(v)) photon_taa_view_size(v[2], v[3], g_scene_w, g_scene_h);   /* the next frame's jitter: one view pixel */
    if (!g_up_on || !g_bb || !g_scene_w || !photon_dmirror_viewport(v) || (v[2] >= g_scene_w && v[3] >= g_scene_h) || v[0] || v[1]) return;
    g_up_vw = v[2]; g_up_vh = v[3];
    photon_dmirror_upscale_arm(upscale_cb, g_bb, g_scene_w, g_scene_h);
}
static void mq_pass_main(void *obj, void *a, int b);
static void mq_pass(void *obj, void *a, int b) {
    g_main_done = 1;
    if (photon_hiz_on() && g_wd_valid && g_wd_tex) {   /* T4.7b: the world-only depth's far depths for the next frames' culling */
        float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
        if (photon_shadows_view(iv, vp, cam, &dep, &w, &h)) photon_hiz_main_view_end(g_wd_tex, g_scene_w, g_scene_h, (const float (*)[4])vp, cam);
    }
    photon_shadows_trace("MAIN-VIEW-END", 0, NULL, 0);
    mq_pass_main(obj, a, b);
    taa_pass();   /* AA: after every pass of Photon's at the main view's end */
    photon_fog_main_view_end(g_bb, g_scene_tex, g_scene_w, g_scene_h);     /* SP layer 3: volumetric fog (photon_fog) */
    photon_packs_main_view_end(g_bb, g_scene_tex, g_scene_w, g_scene_h);   /* SP layer 1: the shader packs' screen passes, after Photon's own */
    upscale_arm();   /* UPSCALE: the engine's upscale draw (after its post effects) is Photon's when the view is scaled */
}
static void mq_pass_main(void *obj, void *a, int b) {
    (void)obj; (void)a; (void)b;
    snap_view();
    velocity_pass();
    if (photon_ptrt_on()) {   /* PT-RT: the sun maps first (the world's own triangles in them too), for its composite */
        static long wseq;
        float *wt = NULL; unsigned wn = 0;
        if (photon_ptrt_world_copy(&wt, &wn, &wseq)) {
            static int dumped;
            char dir[MAX_PATH], path[MAX_PATH + 16], e[4];
            if (!dumped && GetEnvironmentVariableA("PHOTON_PTRT_WN_DEBUG", e, sizeof e) > 0 && e[0] == '1' && GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir) > 0) {
                dumped = 1; snprintf(path, sizeof path, "%s\\world.bin", dir);
                FILE *f = fopen(path, "wb"); if (f) { fwrite(wt, sizeof(float) * 18, wn, f); fclose(f); }
            }
            float *wp = photon_ptrt_world_pbr_copy(wn), *wx = photon_ptrt_world_ex_copy(wn);
            photon_dmirror_set_world(wt, wp, wx, wn); free(wt); free(wp); free(wx);
        }
        { static long aseq; photon_ptrt_atlas_with(&aseq, photon_dmirror_set_pbr_atlas, NULL); }   /* PBR packs: the map's atlases (pbrpack.c) */
        if (photon_nanite_on()) {   /* Nanite draws the static props, so the engine records none to cast: its own geometry casts */
            static long pseq = -1;
            long q; unsigned pn; const PhotonCastRange *pr; int npr;
            const float *pt = photon_nanite_casters(&pn, &pr, &npr, &q);
            if (pt && q != pseq) { pseq = q; photon_dmirror_set_props(pt, pn, pr, npr); }
        }
        photon_dmirror_cast_cut_leaves(1);
        g_pt_maps_only = 1; shadow_pass(); g_pt_maps_only = 0;
        photon_dmirror_cast_cut_leaves(0);
        /* the world's smooth normals from the main view, for the composite's sun (the depth's own normal facets displacements) */
        g_wn_ok = 0;
        IDirect3DDevice9 *d = photon_d3d_device();
        if (d && g_scene_tex && g_scene_sfc) {
            if (g_wn_tex && (g_wn_w != g_scene_w || g_wn_h != g_scene_h)) {
                g_wn_sfc->lpVtbl->Release(g_wn_sfc); g_wn_tex->lpVtbl->Release(g_wn_tex); g_wn_tex = NULL; g_wn_sfc = NULL;
                if (g_wpbr_tex) { g_wpbr_sfc->lpVtbl->Release(g_wpbr_sfc); g_wpbr_tex->lpVtbl->Release(g_wpbr_tex); g_wpbr_tex = NULL; g_wpbr_sfc = NULL; }
            }
            if (!g_wn_tex && SUCCEEDED(d->lpVtbl->CreateTexture(d, g_scene_w, g_scene_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_wn_tex, NULL))) {
                g_wn_tex->lpVtbl->GetSurfaceLevel(g_wn_tex, 0, &g_wn_sfc); g_wn_w = g_scene_w; g_wn_h = g_scene_h;
            }
            if (g_wn_tex && !g_wpbr_tex && SUCCEEDED(d->lpVtbl->CreateTexture(d, g_scene_w, g_scene_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_wpbr_tex, NULL)))
                g_wpbr_tex->lpVtbl->GetSurfaceLevel(g_wpbr_tex, 0, &g_wpbr_sfc);
            float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
            static int nown = -1;
            if (nown < 0) { char e[4]; nown = GetEnvironmentVariableA("PHOTON_PTRT_NOWN", e, sizeof e) > 0 && e[0] == '1'; }
            if (!nown && g_wn_sfc && photon_shadows_view(iv, vp, cam, &dep, &w, &h))
                g_wn_ok = photon_dmirror_world_normals(g_wn_sfc, g_wpbr_sfc, g_scene_tex, (const float (*)[4])vp, (const float (*)[4])iv, cam, (int)g_scene_w, (int)g_scene_h);
            photon_ptrt_pbr_drawn(g_wn_ok && g_wpbr_sfc);
            static int logged;
            if (++logged % 50 == 20 && logged < 600) {
                photon_log("shadows: PT-RT world normals %s (texture %p, scene depth %p)", g_wn_ok ? "drawn" : "NOT drawn", (void *)g_wn_tex, (void *)g_scene_sfc);
                float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
                photon_shadows_view(iv, vp, cam, &dep, &w, &h);
                for (int r = 0; r < 4; r++) photon_log("shadows: view row %d: %.5f %.5f %.5f %.3f", r, vp[r][0], vp[r][1], vp[r][2], vp[r][3]);
                photon_log("shadows: view eye %.1f %.1f %.1f, scene %ux%u", cam[0], cam[1], cam[2], g_scene_w, g_scene_h);
                D3DSURFACE_DESC bd, sd, nd; IDirect3DSurface9 *ss = NULL;
                memset(&bd, 0, sizeof bd); memset(&sd, 0, sizeof sd); memset(&nd, 0, sizeof nd);
                if (g_bb) g_bb->lpVtbl->GetDesc(g_bb, &bd);
                if (g_scene_tex && SUCCEEDED(g_scene_tex->lpVtbl->GetSurfaceLevel(g_scene_tex, 0, &ss))) { ss->lpVtbl->GetDesc(ss, &sd); ss->lpVtbl->Release(ss); }
                if (g_wn_sfc) g_wn_sfc->lpVtbl->GetDesc(g_wn_sfc, &nd);
                D3DVIEWPORT9 vpt; memset(&vpt, 0, sizeof vpt); d->lpVtbl->GetViewport(d, &vpt);
                photon_log("shadows: sizes: back buffer %ux%u, scene depth %ux%u, normals %ux%u; viewport now %lu,%lu %lux%lu", bd.Width, bd.Height, sd.Width, sd.Height,
                           nd.Width, nd.Height, vpt.X, vpt.Y, vpt.Width, vpt.Height);
            }
        }
    }
    if (photon_ptrt_on() && photon_nanite_on()) {   /* both: Nanite's props merged first (surface colour, into the depth), so PT-RT traces and lights them */
        photon_nanite_set_albedo(1);
        photon_nanite_main_view();
        photon_ptrt_main_view();
    } else {
        photon_ptrt_main_view();   /* PT-RT: its Vulkan pass while this view's depth is still in place */
        photon_nanite_main_view();   /* NANITE: the static props drawn by Photon */
    }
    int toon = photon_vlg_toon();
    if (!drawing() && !toon && !debug_mode()) return;   /* only the snapshot was wanted (PT-RT) */
    if (photon_ptrt_on()) return;   /* PT-RT's composite already holds the sun (no second darkening) */
    if (drawing()) { shadow_pass(); if (toon) toon_outline_pass(); return; }
    if (toon) { toon_outline_pass(); return; }
    float V[16], P[16];
    if (!g_scene_tex || !photon_vlg_api_matrix(0, V) || !photon_vlg_api_matrix(1, P)) return;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += V[i * 4 + k] * P[k * 4 + j];
            g_seg_m[j][i] = v;
        }
    g_seg_have = 1;
    run_pass();
}
void photon_shadows_main_view_end(void) {
    if (!photon_shadows_on()) return;
    debug_mode();
    if (!drawing() && !debug_mode() && !photon_vlg_toon() && !photon_ptrt_on() && !photon_nanite_on() && !photon_taa_on() && !photon_hiz_on() && !photon_packs_listed() && !photon_fog_on()) return;
    g_mv_ends++;
    if (photon_mq_queue(mq_pass, NULL, NULL, 0)) g_mv_queued++;
    else mq_pass(NULL, NULL, 0);
}

/* --- stage 1c: one cascade of sun shadows from this frame's casters ---------------------------------------------------------------- */
static int sm_create(IDirect3DDevice9 *d, int size) {
    if (g_sm_tex && size != g_sm_size) {   /* a new resolution from the in-game page */
        if (g_sm_sfc) { g_sm_sfc->lpVtbl->Release(g_sm_sfc); g_sm_sfc = NULL; }
        if (g_sm_rt) { g_sm_rt->lpVtbl->Release(g_sm_rt); g_sm_rt = NULL; }
        g_sm_tex->lpVtbl->Release(g_sm_tex); g_sm_tex = NULL;
        sn_release();
        g_sm_failed = 0;
    }
    g_sm_size = size;
    if (g_sm_tex) return 1;
    if (g_sm_failed) return 0;
    HRESULT h1 = d->lpVtbl->CreateTexture(d, g_sm_size, g_sm_size, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC4('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &g_sm_tex, NULL);
    HRESULT h2 = SUCCEEDED(h1) ? g_sm_tex->lpVtbl->GetSurfaceLevel(g_sm_tex, 0, &g_sm_sfc) : E_FAIL;
    HRESULT h3 = SUCCEEDED(h2) ? d->lpVtbl->CreateRenderTarget(d, g_sm_size, g_sm_size, FOURCC4('N', 'U', 'L', 'L'), D3DMULTISAMPLE_NONE, 0, FALSE, &g_sm_rt, NULL) : E_FAIL;
    if (FAILED(h3)) {
        g_sm_failed = 1;
        photon_log("shadows: shadow map %d^2 INTZ + NULL RT failed (hr 0x%lx 0x%lx 0x%lx): stage 1c off", g_sm_size, (unsigned long)h1, (unsigned long)h2, (unsigned long)h3);
        if (g_sm_sfc) { g_sm_sfc->lpVtbl->Release(g_sm_sfc); g_sm_sfc = NULL; }
        if (g_sm_tex) { g_sm_tex->lpVtbl->Release(g_sm_tex); g_sm_tex = NULL; }
        return 0;
    }
    if (FAILED(d->lpVtbl->CreateTexture(d, g_sm_size, g_sm_size, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC4('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &g_sn_tex, NULL)) ||
        FAILED(g_sn_tex->lpVtbl->GetSurfaceLevel(g_sn_tex, 0, &g_sn_sfc))) { sn_release(); photon_log("shadows: near cascade map failed: one cascade"); }
    photon_dmirror_cast_invalidate();
    photon_log("shadows: shadow map %d^2 created (photon_shadow_res)%s", g_sm_size, g_sn_tex ? ", with the near cascade" : "");
    return 1;
}
static void norm3(float v[3]) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void cross3(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
/* --- L1a (analysis/35): which map lamps get a real-time shadow this frame. Device thread.
 * A lamp qualifies with dynamic casters inside its reach (a spot: inside its outer cone, widened by the caster radius); dynamic = filed by
 * this frame's DrawModelExecute (stage 2a's table) or skinned; static props are left out, their shadows are already in the lightmap.
 * Score = the sum over its casters of luminance / attenuation(d); a lamp chosen last frame keeps its slot unless beaten 1.5x; lamps under a
 * tenth of the best score are dropped. */
enum { L1_MAX = 4 };
static const float L1_CASTER_R = 32.0f;
static const float L1_FIXTURE = 32.0f;   /* casters this close to a lamp are taken for its fixture model (a cube face would be all its shadow) */   /* a crate ~25, a standing NPC from its pelvis ~40 */
static int g_l1_pick[L1_MAX], g_l1_n;   /* worldlight indices */
static LgLamp g_l1_lamp[L1_MAX];         /* their records this frame */
static float g_l1_score[L1_MAX];
static int g_l1_ncast[L1_MAX];
static int caster_dynamic(const float t[3]);
static void l1_select(void) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    int want = photon_vlg_light_shadows(NULL);
    if (want > 1 && photon_dynq_shadow_level() >= 1) want = 1;   /* UI2: the first step down also keeps one lamp */
    const LgLamp *L = NULL;
    int nl = want ? lg_work_lamps(&L, NULL) : 0;
    int nc = nl ? photon_dmirror_ncast() : 0;
    float score[LG_LAMPS_MAX] = { 0 };
    int cnt[LG_LAMPS_MAX] = { 0 };
    for (int c = 0; c < nc; c++) {
        float p[3];
        int skinned = photon_dmirror_caster_pos(c, p);
        if (skinned < 0 || (!skinned && !caster_dynamic(p))) continue;   /* < 0: kept for the motion vectors only */
        for (int l = 0; l < nl; l++) {
            float v[3] = { p[0] - L[l].origin[0], p[1] - L[l].origin[1], p[2] - L[l].origin[2] };
            float dd = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (dd > L[l].reach + L1_CASTER_R || dd < L1_FIXTURE) continue;   /* the lamp's own fixture model sits at it */
            if (L[l].type == LG_EMIT_SPOT && dd > L1_CASTER_R) {
                float cosa = (v[0] * L[l].dir[0] + v[1] * L[l].dir[1] + v[2] * L[l].dir[2]) / dd;
                float widen = L1_CASTER_R / dd;   /* sin of the caster's half angle, as a cosine margin */
                if (cosa < L[l].stopdot2 - widen) continue;
            }
            float att = L[l].att[0] + L[l].att[1] * dd + L[l].att[2] * dd * dd;
            float lum = 0.2126f * L[l].color[0] + 0.7152f * L[l].color[1] + 0.0722f * L[l].color[2];
            score[l] += lum / (att > 1e-3f ? att : 1e-3f);
            cnt[l]++;
        }
    }
    for (int l = 0; l < nl; l++)
        for (int k = 0; k < g_l1_n; k++) if (g_l1_pick[k] == L[l].index) score[l] *= 1.5f;   /* hysteresis */
    int pick[L1_MAX], np = 0, pl[L1_MAX];
    float ps[L1_MAX];
    int pc[L1_MAX];
    for (int l = 0; l < nl; l++) {
        if (!(score[l] > 0)) continue;
        int at = np;
        while (at > 0 && ps[at - 1] < score[l]) at--;
        if (at >= want) continue;
        int last = np < want ? np : want - 1;
        for (int k = last; k > at; k--) { pick[k] = pick[k - 1]; ps[k] = ps[k - 1]; pc[k] = pc[k - 1]; pl[k] = pl[k - 1]; }
        pick[at] = L[l].index; ps[at] = score[l]; pc[at] = cnt[l]; pl[at] = l;
        if (np < want) np++;
    }
    while (np > 1 && ps[np - 1] < 0.1f * ps[0]) np--;   /* a lamp under a tenth of the best one's score would cast a faint shadow for a full map's cost */
    int changed = np != g_l1_n;
    for (int k = 0; k < np && !changed; k++) changed = pick[k] != g_l1_pick[k];
    g_l1_changes += changed;
    g_l1_n = np;
    for (int k = 0; k < np; k++) g_l1_lamp[k] = L[pl[k]];
    memcpy(g_l1_pick, pick, sizeof(int) * (size_t)np); memcpy(g_l1_score, ps, sizeof(float) * (size_t)np); memcpy(g_l1_ncast, pc, sizeof(int) * (size_t)np);
    g_l1_frames++;
    g_l1_frames_any += np > 0;
    QueryPerformanceCounter(&t1);
    g_l1_ticks += t1.QuadPart - t0.QuadPart;
    static unsigned long long llog;
    if (want && photon_frame_count() >= llog + 600) {
        llog = photon_frame_count();
        char b[256];
        int o = 0;
        for (int k = 0; k < np && o < 200; k++) o += snprintf(b + o, sizeof b - (size_t)o, " #%d (%.3g, %d casters)", pick[k], ps[k], pc[k]);
        photon_log("shadows: L1 lamps: %d wanted, %d candidates, %d casters; chosen:%s", want, nl, nc, np ? b : " none");
    }
}

/* --- L3c (analysis/37): the re-baked lamp shadows, from l3work's pack: textures, the lamp table per style change, the pass ----------- */
static struct {
    L3Ready *r;
    IDirect3DVolumeTexture9 *l1, *l2, *at;
    IDirect3DTexture9 *bl, *lt;
    void *ps;
    int failed, lt_rows;
    float styles_seen[4096];
} g_l3;
static void wd_release(void) {
    if (g_wd_sfc) { g_wd_sfc->lpVtbl->Release(g_wd_sfc); g_wd_sfc = NULL; }
    if (g_wd_tex) { g_wd_tex->lpVtbl->Release(g_wd_tex); g_wd_tex = NULL; }
    g_wd_valid = 0;
}
static void l3_release(void) {
    wd_release();
    if (g_l3.l1) { g_l3.l1->lpVtbl->Release(g_l3.l1); g_l3.l1 = NULL; }
    if (g_l3.l2) { g_l3.l2->lpVtbl->Release(g_l3.l2); g_l3.l2 = NULL; }
    if (g_l3.at) { g_l3.at->lpVtbl->Release(g_l3.at); g_l3.at = NULL; }
    if (g_l3.bl) { g_l3.bl->lpVtbl->Release(g_l3.bl); g_l3.bl = NULL; }
    if (g_l3.lt) { g_l3.lt->lpVtbl->Release(g_l3.lt); g_l3.lt = NULL; }
    g_l3.lt_rows = 0;
}
/* a DEFAULT volume texture from memory: a SYSTEMMEM twin, LockBox, rows copied, UpdateTexture */
static IDirect3DVolumeTexture9 *l3_volume(IDirect3DDevice9 *d, int w, int h, int dp, D3DFORMAT fmt, int bpp, const void *src) {
    IDirect3DVolumeTexture9 *sys = NULL, *def = NULL;
    if (FAILED(d->lpVtbl->CreateVolumeTexture(d, (UINT)w, (UINT)h, (UINT)dp, 1, 0, fmt, D3DPOOL_SYSTEMMEM, &sys, NULL))) return NULL;
    D3DLOCKED_BOX lb;
    if (SUCCEEDED(sys->lpVtbl->LockBox(sys, 0, &lb, NULL, 0))) {
        for (int z = 0; z < dp; z++)
            for (int y = 0; y < h; y++)
                memcpy((char *)lb.pBits + (size_t)z * lb.SlicePitch + (size_t)y * lb.RowPitch, (const char *)src + ((size_t)z * h + y) * (size_t)w * bpp, (size_t)w * bpp);
        sys->lpVtbl->UnlockBox(sys, 0);
        if (SUCCEEDED(d->lpVtbl->CreateVolumeTexture(d, (UINT)w, (UINT)h, (UINT)dp, 1, 0, fmt, D3DPOOL_DEFAULT, &def, NULL)) &&
            FAILED(d->lpVtbl->UpdateTexture(d, (IDirect3DBaseTexture9 *)sys, (IDirect3DBaseTexture9 *)def))) { def->lpVtbl->Release(def); def = NULL; }
    }
    sys->lpVtbl->Release(sys);
    return def;
}
static IDirect3DTexture9 *l3_tex2d(IDirect3DDevice9 *d, int w, int h, const float *src) {   /* A32B32G32R32F */
    IDirect3DTexture9 *sys = NULL, *def = NULL;
    if (FAILED(d->lpVtbl->CreateTexture(d, (UINT)w, (UINT)h, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, NULL))) return NULL;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(sys->lpVtbl->LockRect(sys, 0, &lr, NULL, 0))) {
        for (int y = 0; y < h; y++) memcpy((char *)lr.pBits + (size_t)y * lr.Pitch, src + (size_t)y * w * 4, (size_t)w * 16);
        sys->lpVtbl->UnlockRect(sys, 0);
        if (SUCCEEDED(d->lpVtbl->CreateTexture(d, (UINT)w, (UINT)h, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &def, NULL)) &&
            FAILED(d->lpVtbl->UpdateTexture(d, (IDirect3DBaseTexture9 *)sys, (IDirect3DBaseTexture9 *)def))) { def->lpVtbl->Release(def); def = NULL; }
    }
    sys->lpVtbl->Release(sys);
    return def;
}
static void l3_lamp_table(IDirect3DDevice9 *d) {   /* rebuilt when any lamp's style scale moved */
    const L3Ready *r = g_l3.r;
    int n = r->nlights < 4096 ? r->nlights : 4096, changed = !g_l3.lt;
    for (int i = 0; i < n; i++) {
        float sc = photon_lg_style_scale(r->lights[i].style);
        if (sc != g_l3.styles_seen[i]) { g_l3.styles_seen[i] = sc; changed = 1; }
    }
    if (!changed || n <= 0) return;
    float *t = calloc((size_t)n * 16, sizeof *t);
    if (!t) return;
    for (int i = 0; i < n; i++) {
        const LgWorldLight *w = &r->lights[i];
        float *q = t + (size_t)i * 16;
        float lum = 0.2126f * w->intensity[0] + 0.7152f * w->intensity[1] + 0.0722f * w->intensity[2];
        q[0] = w->origin[0]; q[1] = w->origin[1]; q[2] = w->origin[2]; q[3] = w->type == LG_EMIT_SPOT ? 2.0f : 1.0f;
        q[4] = lum * g_l3.styles_seen[i]; q[5] = w->stopdot; q[6] = w->stopdot2;
        q[8] = w->normal[0]; q[9] = w->normal[1]; q[10] = w->normal[2]; q[11] = w->exponent > 0 ? w->exponent : 1.0f;
        float c = w->constant_attn, l = w->linear_attn, qq = w->quadratic_attn;
        if (c == 0 && l == 0 && qq == 0) c = 1;
        q[12] = c; q[13] = l; q[14] = qq;
    }
    if (g_l3.lt) { g_l3.lt->lpVtbl->Release(g_l3.lt); g_l3.lt = NULL; }
    g_l3.lt = l3_tex2d(d, 4, n, t);
    g_l3.lt_rows = g_l3.lt ? n : 0;
    g_l3_lamp_updates++;
    free(t);
}
static void l3_take(IDirect3DDevice9 *d) {
    L3Ready *nr = l3_work_take();
    if (!nr) return;
    l3_release();
    l3_ready_free(g_l3.r);
    g_l3.r = nr;
    memset(g_l3.styles_seen, 0, sizeof g_l3.styles_seen);
    const L3Pack *p = nr->pack;
    if (!p) return;
    g_l3.l1 = l3_volume(d, p->l1_dim[0], p->l1_dim[1], p->l1_dim[2], D3DFMT_L16, 2, p->l1);
    g_l3.l2 = l3_volume(d, L3_PX * L3_SB, L3_PY * L3_SB, p->pz * L3_SB, D3DFMT_L16, 2, p->l2);
    g_l3.at = l3_volume(d, L3_AX * L3_B, L3_AY * L3_B, p->az * L3_B, D3DFMT_A8R8G8B8, 4, p->atlas);
    g_l3.bl = l3_tex2d(d, L3_LW, p->lamp_rows, p->lamps);
    int ok = g_l3.l1 && g_l3.l2 && g_l3.at && g_l3.bl;
    g_l3_uploads++;
    photon_log("shadows: L3 %s: %d bricks uploaded%s (l1 %dx%dx%d, l2 %dx%dx%d, atlas %dx%dx%d)", nr->level, p->nbricks, ok ? "" : " FAILED (pass off)", p->l1_dim[0],
               p->l1_dim[1], p->l1_dim[2], L3_PX * L3_SB, L3_PY * L3_SB, p->pz * L3_SB, L3_AX * L3_B, L3_AY * L3_B, p->az * L3_B);
    if (!ok) l3_release();
}
static void l3_pass(IDirect3DDevice9 *d, const float (*inv)[4]) {
    l3_take(d);
    if (!g_l3.r || !g_l3.r->pack || !g_l3.at) return;
    l3_lamp_table(d);
    if (!g_l3.lt) return;
    const L3Pack *p = g_l3.r->pack;
    static int dbg = -1;
    static float bounce = -1;
    if (dbg < 0) {
        char e[16];
        dbg = GetEnvironmentVariableA("PHOTON_L3_DEBUG", e, sizeof e) > 0 && e[0] == '1';
        bounce = GetEnvironmentVariableA("PHOTON_L3_BOUNCE", e, sizeof e) > 0 ? (float)atof(e) : 0.15f;
    }
    float c[11][4];
    memset(c, 0, sizeof c);
    memcpy(c, inv, 4 * sizeof c[0]);
    float V = (float)p->voxel;
    static int brighten = -1;
    if (brighten < 0) { char e[4]; brighten = GetEnvironmentVariableA("PHOTON_L3_BRIGHTEN", e, sizeof e) > 0 && e[0] == '1'; }
    c[10][0] = (float)brighten;
    static int thin = -1;
    if (thin < 0) { char e[4]; thin = !(GetEnvironmentVariableA("PHOTON_L3_THIN", e, sizeof e) > 0 && e[0] == '0'); }
    c[10][2] = (float)thin;
    c[4][0] = 1.0f / V; c[4][1] = (float)L3_DMAX * V; c[4][2] = V * 0.35f; c[4][3] = 8.0f;   /* sharp: a third of a voxel; blurry: the lightmap's 16-unit luxel */
    c[5][0] = (float)p->sb_min[0]; c[5][1] = (float)p->sb_min[1]; c[5][2] = (float)p->sb_min[2]; c[5][3] = (float)g_l3.lt_rows;
    c[6][0] = (float)p->l1_dim[0]; c[6][1] = (float)p->l1_dim[1]; c[6][2] = (float)p->l1_dim[2];
    c[7][0] = (float)(L3_PX * L3_SB); c[7][1] = (float)(L3_PY * L3_SB); c[7][2] = (float)(p->pz * L3_SB);
    c[8][0] = (float)(L3_AX * L3_B); c[8][1] = (float)(L3_AY * L3_B); c[8][2] = (float)(p->az * L3_B); c[8][3] = (float)p->lamp_rows;
    c[9][0] = bounce; c[9][1] = 0.02f; c[9][2] = 1.0f; c[9][3] = (float)dbg;
    if (g_wd_valid && g_wd_tex) { c[10][1] = 1.0f; photon_dmirror_fullscreen_texn(6, g_wd_tex, 0); g_wd_masked_passes++; }
    photon_dmirror_fullscreen_texn(1, g_l3.l1, 0);
    photon_dmirror_fullscreen_texn(2, g_l3.l2, 0);
    photon_dmirror_fullscreen_texn(3, g_l3.at, 1);
    photon_dmirror_fullscreen_texn(4, g_l3.bl, 0);
    photon_dmirror_fullscreen_texn(5, g_l3.lt, 0);
    if (photon_dmirror_fullscreen(k_shpass_vs, k_l3ratio_ps, &g_sh_vs, &g_l3.ps, g_scene_tex, (const float (*)[4])c, 11, 2, (int)g_scene_w, (int)g_scene_h, g_bb))
        g_l3_passes++;
}

/* vlg.c, draw thread, before a draw of one of Photon's model pairs (VLG / bump / skin: props, static props, characters). The first such
 * draw on the scene depth after a depth clear copies that depth: the world drawn so far, without models. Only while L3 has a pack. */
void photon_shadows_model_draw(void) {
    if (g_wd_valid || g_wd_failed || !g_scene_tex || !g_bb) return;
    if (!(g_l3.at && g_l3.r && g_l3.r->pack) && !photon_ptrt_on() && !photon_hiz_on()) return;   /* wanted by L3, PT-RT's composite (world pixels only) and T4.7b's Hi-Z */
    if (photon_dmirror_ds() != (void *)g_scene_sfc || photon_dmirror_rt0() != (void *)g_bb) return;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return;
    if (!g_wd_tex) {
        if (FAILED(d->lpVtbl->CreateTexture(d, g_scene_w, g_scene_h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &g_wd_tex, NULL)) ||
            FAILED(g_wd_tex->lpVtbl->GetSurfaceLevel(g_wd_tex, 0, &g_wd_sfc))) {
            wd_release(); g_wd_failed = 1;
            photon_log("shadows: L3 model mask: R32F %ux%u render target failed: the re-bake corrects model pixels too", g_scene_w, g_scene_h);
            return;
        }
    }
    float none[1][4] = { { 0, 0, 0, 0 } };
    if (photon_dmirror_fullscreen(k_shpass_vs, k_depthcopy_ps, &g_sh_vs, &g_wd_ps, g_scene_tex, (const float (*)[4])none, 1, 0, (int)g_scene_w, (int)g_scene_h, g_wd_sfc)) {
        g_wd_valid = 1; g_wd_copies++;
    }
}

/* --- L1b/L1c (analysis/35): each chosen lamp's map, aimed at its casters, in a tile of one INTZ atlas; then one screen pass for all ---- */
enum { L1_NCAST = 8192 };
static IDirect3DTexture9 *g_l1_tex;
static IDirect3DSurface9 *g_l1_sfc, *g_l1_rt;
static int g_l1_size, g_l1_failed;
static void *g_l1_ps;
static unsigned char g_l1_sel[L1_NCAST], g_l1_fsel[L1_NCAST];   /* the lamp's casters; those of the view being drawn */
static void l1_release(void) {
    if (g_l1_sfc) { g_l1_sfc->lpVtbl->Release(g_l1_sfc); g_l1_sfc = NULL; }
    if (g_l1_rt) { g_l1_rt->lpVtbl->Release(g_l1_rt); g_l1_rt = NULL; }
    if (g_l1_tex) { g_l1_tex->lpVtbl->Release(g_l1_tex); g_l1_tex = NULL; }
}
static int l1_atlas(IDirect3DDevice9 *d, int size) {   /* 4 x 4 tiles of size (an aimed lamp takes one, a cube lamp one per face with casters) */
    if (g_l1_tex && size != g_l1_size) { l1_release(); g_l1_failed = 0; }
    g_l1_size = size;
    if (g_l1_tex) return 1;
    if (g_l1_failed) return 0;
    HRESULT h1 = d->lpVtbl->CreateTexture(d, 4 * size, 4 * size, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC4('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &g_l1_tex, NULL);
    HRESULT h2 = SUCCEEDED(h1) ? g_l1_tex->lpVtbl->GetSurfaceLevel(g_l1_tex, 0, &g_l1_sfc) : E_FAIL;
    HRESULT h3 = SUCCEEDED(h2) ? d->lpVtbl->CreateRenderTarget(d, 4 * size, 4 * size, FOURCC4('N', 'U', 'L', 'L'), D3DMULTISAMPLE_NONE, 0, FALSE, &g_l1_rt, NULL) : E_FAIL;
    if (FAILED(h3)) {
        g_l1_failed = 1;
        photon_log("shadows: L1 lamp atlas %d^2 failed (hr 0x%lx 0x%lx 0x%lx): lamp shadows off", 4 * size, (unsigned long)h1, (unsigned long)h2, (unsigned long)h3);
        l1_release();
        return 0;
    }
    photon_dmirror_cast_invalidate();
    photon_log("shadows: L1 lamp atlas %d^2 created (photon_light_shadow_res %d)", 4 * size, size);
    return 1;
}
static float l1_kfloor(void) {   /* K's floor: the ambient part no lamp accounts for (PHOTON_L1_K, tuning) */
    static float k = -1;
    if (k < 0) { char e[16]; k = GetEnvironmentVariableA("PHOTON_L1_K", e, sizeof e) > 0 ? (float)atof(e) : 0.02f; if (!(k > 0)) k = 0.02f; }
    return k;
}
static float lamp_at(const LgLamp *l, const float p[3]) {   /* a lamp's unshadowed luminance at p (no N.L: a direction-free estimate) */
    float v[3] = { p[0] - l->origin[0], p[1] - l->origin[1], p[2] - l->origin[2] };
    float d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (d > l->reach) return 0;
    float spot = 1;
    if (l->type == LG_EMIT_SPOT && d > 0) {
        float c = (v[0] * l->dir[0] + v[1] * l->dir[1] + v[2] * l->dir[2]) / d;
        if (c <= l->stopdot2) return 0;
        if (c < l->stopdot) spot = powf((c - l->stopdot2) / fmaxf(l->stopdot - l->stopdot2, 1e-4f), l->exponent > 0 ? l->exponent : 1.0f);
    }
    float att = l->att[0] + l->att[1] * d + l->att[2] * d * d;
    return (0.2126f * l->color[0] + 0.7152f * l->color[1] + 0.0722f * l->color[2]) * spot / fmaxf(att, 1e-3f);
}
static int l1_mark(const LgLamp *lp, int nc) {   /* this lamp's casters as l1_select counts them; returns how many */
    int n = 0;
    for (int c = 0; c < nc; c++) {
        float p[3];
        int skinned = photon_dmirror_caster_pos(c, p);
        g_l1_sel[c] = 0;
        if (skinned < 0 || (!skinned && !caster_dynamic(p))) continue;
        float v[3] = { p[0] - lp->origin[0], p[1] - lp->origin[1], p[2] - lp->origin[2] };
        float dd = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (dd > lp->reach + L1_CASTER_R || dd < L1_FIXTURE) continue;
        if (lp->type == LG_EMIT_SPOT && dd > L1_CASTER_R && (v[0] * lp->dir[0] + v[1] * lp->dir[1] + v[2] * lp->dir[2]) / dd < lp->stopdot2 - L1_CASTER_R / dd) continue;
        g_l1_sel[c] = 1;
        n++;
    }
    return n;
}
/* one view of a lamp into atlas tile `tile`: rows from basis (x, y, z) at E with half-angle tangent t, near nr, far fr; the casters g_l1_fsel marks */
static int l1_view(int tile, int res, int sz, const float E[3], const float x[3], const float y[3], const float z[3], float t, float nr, float fr, float rows[4][4]) {
    float q = fr / (fr - nr);
    for (int k = 0; k < 3; k++) { rows[0][k] = x[k] / t; rows[1][k] = y[k] / t; rows[2][k] = z[k] * q; rows[3][k] = z[k]; }
    rows[0][3] = -dot3(x, E) / t; rows[1][3] = -dot3(y, E) / t; rows[2][3] = (-dot3(z, E) - nr) * q; rows[3][3] = -dot3(z, E);
    PhotonCastOpts o = { (tile & 3) * res, (tile >> 2) * res, sz, g_l1_fsel, 0, 1 + tile };   /* F1: sz <= res, in the tile's corner */
    if (photon_dmirror_cast_opts(g_l1_sfc, g_l1_rt, &o, (const float (*)[4])rows, k_shnull20_ps, k_shnull30_ps, &g_ps20, &g_ps30) <= 0) return 0;
    g_l1_maps++;
    return 1;
}
/* F1 (TASKS): a lamp's map size from its casters' size on screen (projected diameter in pixels, from the real projection, so zoom counts):
 * full tile, half or quarter; 1.25x hysteresis per lamp so the size does not flicker; a change clears the at-rest cache */
enum { L1_SZ_SLOTS = 4096 };
static unsigned char g_l1_szlvl[L1_SZ_SLOTS];   /* per worldlight: 0 unknown, 1 full, 2 half, 3 quarter */
static int l1_size(int lamp, int res, float px) {
    int want = px * 1.5f >= res * 0.5f ? 1 : px * 1.5f >= res * 0.25f ? 2 : 3;
    unsigned char *lv = lamp >= 0 && lamp < L1_SZ_SLOTS ? &g_l1_szlvl[lamp] : NULL;
    int cur = lv && *lv ? *lv : want;
    if (want < cur) cur = want;   /* grow at once */
    else if (want > cur) {        /* shrink only with margin */
        float lim = cur == 1 ? res * 0.5f : res * 0.25f;
        if (px * 1.5f * 1.25f < lim) cur = want;
    }
    if (lv && *lv && *lv != cur) photon_dmirror_cast_invalidate();
    if (lv) *lv = (unsigned char)cur;
    if (cur == 1) g_l1_sz_full++; else if (cur == 2) g_l1_sz_half++; else g_l1_sz_quarter++;
    return res >> (cur - 1);
}
static void l1_pass(IDirect3DDevice9 *d, const float (*inv)[4], float strength, const float cam[3], float pxscale) {
    int res = 512;
    int want = photon_vlg_light_shadows(&res);
    if (!want || g_l1_n <= 0) return;
    int nc = photon_dmirror_ncast();
    if (nc <= 0 || nc > L1_NCAST || !l1_atlas(d, res)) return;
    float c[8 + 10 * L1_MAX][4];
    memset(c, 0, sizeof c);
    memcpy(c, inv, 4 * sizeof c[0]);
    int used = 0, tiles = 0;
    for (int i = 0; i < g_l1_n; i++) {
        const LgLamp *lp = &g_l1_lamp[i];
        if (l1_mark(lp, nc) <= 0) continue;
        float E[3] = { lp->origin[0], lp->origin[1], lp->origin[2] };
        static float pos[L1_NCAST][3];
        static int idx[L1_NCAST];
        int n = 0;
        for (int k = 0; k < nc; k++) if (g_l1_sel[k]) { photon_dmirror_caster_pos(k, pos[n]); idx[n] = k; n++; }
        /* the casters' sphere */
        float C[3] = { 0, 0, 0 }, R = 0, dc = 0;
        for (int k = 0; k < n; k++) { C[0] += pos[k][0]; C[1] += pos[k][1]; C[2] += pos[k][2]; }
        C[0] /= n; C[1] /= n; C[2] /= n;
        for (int k = 0; k < n; k++) {
            float r = sqrtf((pos[k][0] - C[0]) * (pos[k][0] - C[0]) + (pos[k][1] - C[1]) * (pos[k][1] - C[1]) + (pos[k][2] - C[2]) * (pos[k][2] - C[2]));
            if (r > R) R = r;
        }
        R += L1_CASTER_R;
        {
            float v[3] = { C[0] - E[0], C[1] - E[1], C[2] - E[2] };
            dc = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }
        static int force_cube = -1;
        if (force_cube < 0) { char e[4]; force_cube = GetEnvironmentVariableA("PHOTON_L1_FORCE_CUBE", e, sizeof e) > 0 && e[0] == '1'; }   /* A/B of the cube path */
        float dcam = sqrtf((C[0] - cam[0]) * (C[0] - cam[0]) + (C[1] - cam[1]) * (C[1] - cam[1]) + (C[2] - cam[2]) * (C[2] - cam[2]));
        float px = 2.0f * R / fmaxf(dcam - R, 16.0f) * pxscale;   /* the casters' sphere across, in screen pixels */
        int sz = l1_size(lp->index, res, px);
        int aimed = !force_cube && dc > 0 && R / dc <= 0.966f;   /* fits one cone of 150 degrees */
        int spotcone = !force_cube && !aimed && lp->type == LG_EMIT_SPOT && lp->stopdot2 >= 0.259f;   /* a spot's own cone of at most 150 degrees covers all it lights */
        static unsigned long long flog;
        if (photon_frame_count() >= flog + 600) {
            flog = photon_frame_count();
            photon_log("shadows: L1 lamp #%d (%s) at %.0f %.0f %.0f reach %.0f: %d casters, sphere %.0f %.0f %.0f r %.0f at %.0f (%s); its light there %.3g", lp->index,
                       lp->type == LG_EMIT_SPOT ? "spot" : "point", E[0], E[1], E[2], lp->reach, n, C[0], C[1], C[2], R, dc,
                       aimed ? "aimed" : spotcone ? "spot cone" : "cube", lamp_at(lp, C));
        }
        float (*b)[4] = &c[8 + 10 * used];
        memcpy(g_l1_fsel, g_l1_sel, (size_t)nc);
        if (aimed || spotcone) {
            if (tiles >= 16) break;
            float z[3], t, nr, fr;
            if (aimed) {
                z[0] = (C[0] - E[0]) / dc; z[1] = (C[1] - E[1]) / dc; z[2] = (C[2] - E[2]) / dc;
                t = tanf(asinf(R / dc)) * 1.05f;
                nr = dc - R - 8.0f; fr = dc + R + 8.0f;
            } else {
                z[0] = lp->dir[0]; z[1] = lp->dir[1]; z[2] = lp->dir[2];
                norm3(z);
                t = tanf(acosf(lp->stopdot2 < 0.999f ? lp->stopdot2 : 0.999f)) * 1.05f;
                nr = 4.0f; fr = lp->reach + L1_CASTER_R;
            }
            if (nr < 1.0f) nr = 1.0f;
            float up[3] = { 0, 0, 1 };
            if (fabsf(z[2]) > 0.99f) { up[0] = 1; up[2] = 0; }
            float x[3], y[3];
            cross3(up, z, x); norm3(x);
            cross3(z, x, y); norm3(y);
            float rows[4][4];
            if (!l1_view(tiles, res, sz, E, x, y, z, t, nr, fr, rows)) continue;
            memcpy(b[0], rows, sizeof rows);
            b[5][3] = nr; b[6][3] = fr; b[8][3] = 0;
            b[9][0] = 0.25f * (float)(tiles & 3); b[9][1] = 0.25f * (float)(tiles >> 2);
            tiles++;
        } else {
            /* cube: six 90-degree faces (tangent 1.05: they overlap a little), only those with casters drawn; bases as the shader's */
            static const float FZ[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            static const float FX[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 } };
            static const float FY[6][3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
            float nearest = 1e30f;
            for (int k = 0; k < n; k++) {
                float w[3] = { pos[k][0] - E[0], pos[k][1] - E[1], pos[k][2] - E[2] };
                float dk = sqrtf(dot3(w, w));
                if (dk < nearest) nearest = dk;
            }
            const float t = 1.05f, fr = lp->reach + L1_CASTER_R, r = L1_CASTER_R;
            float nr = nearest - 2.0f * r;   /* from the nearest caster's near side: nothing nearer is drawn, and depth precision goes further */
            if (nr < 4.0f) nr = 4.0f;
            int faces = 0;
            for (int f = 0; f < 6; f++) {
                b[f >> 1][(f & 1) * 2] = -1.0f; b[f >> 1][(f & 1) * 2 + 1] = 0;
                int any = 0;
                memset(g_l1_fsel, 0, (size_t)nc);
                for (int k = 0; k < n; k++) {
                    float w[3] = { pos[k][0] - E[0], pos[k][1] - E[1], pos[k][2] - E[2] };
                    float dz = dot3(w, FZ[f]);
                    if (dz + r <= nr) continue;
                    if (fabsf(dot3(w, FX[f])) - r > t * (dz + r) || fabsf(dot3(w, FY[f])) - r > t * (dz + r)) continue;
                    g_l1_fsel[idx[k]] = 1;
                    any = 1;
                }
                if (!any || tiles >= 16) continue;
                float rows[4][4];
                if (!l1_view(tiles, res, sz, E, FX[f], FY[f], FZ[f], t, nr, fr, rows)) continue;
                b[f >> 1][(f & 1) * 2] = 0.25f * (float)(tiles & 3); b[f >> 1][(f & 1) * 2 + 1] = 0.25f * (float)(tiles >> 2);
                tiles++; faces++;
            }
            if (!faces) continue;
            b[3][0] = b[3][1] = b[3][2] = b[3][3] = 0;
            b[5][3] = nr; b[6][3] = fr; b[8][3] = 1;
            b[9][0] = 0; b[9][1] = t;
        }
        /* K: "the rest of the light" where this lamp's shadows fall, from the frame's other candidate lamps at the casters' centre (no
         * visibility: lamps behind walls overcount, which only makes the shadow lighter) plus a floor for the ambient part */
        float kk = l1_kfloor();
        {
            const LgLamp *all = NULL;
            int na = lg_work_lamps(&all, NULL);
            for (int j = 0; j < na; j++) if (all[j].index != lp->index) kk += lamp_at(&all[j], C);
        }
        b[4][0] = E[0]; b[4][1] = E[1]; b[4][2] = E[2]; b[4][3] = lp->reach;
        b[5][0] = 0.2126f * lp->color[0] + 0.7152f * lp->color[1] + 0.0722f * lp->color[2]; b[5][1] = kk;
        b[6][0] = lp->dir[0]; b[6][1] = lp->dir[1]; b[6][2] = lp->dir[2];
        b[7][0] = lp->att[0]; b[7][1] = lp->att[1]; b[7][2] = lp->att[2]; b[7][3] = lp->type == LG_EMIT_SPOT ? 1.0f : 0.0f;
        b[8][0] = lp->stopdot; b[8][1] = lp->stopdot2; b[8][2] = lp->exponent > 0 ? lp->exponent : 1.0f;
        b[9][2] = 0.25f * (float)sz / (float)res; b[9][3] = 1.0f / (float)(4 * res);
        used++;
    }
    if (!used) return;
    static int dbg = -1;
    if (dbg < 0) { char e[4]; dbg = GetEnvironmentVariableA("PHOTON_L1_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    int soft = 0, sres;
    float sdist, sstr;
    photon_vlg_shadow_settings(&sres, &sdist, &soft, &sstr);
    if (photon_dynq_shadow_level() >= 1) soft = 0;   /* UI2's first step: hard edges for the lamps too */
    /* the darkness slider is relative for the lamps: its preset value (0.55-0.6, below 1 because the sun pass does not model the sky's
     * light in shade) means full strength here, as K already stands for the rest of the light; lower values weaken them in step */
    float ls = strength / 0.55f;
    c[4][0] = ls > 1 ? 1 : ls; c[4][1] = (float)soft; c[4][2] = (float)used; c[4][3] = (float)dbg;
    photon_dmirror_fullscreen_tex2(g_l1_tex);
    if (photon_dmirror_fullscreen(k_shpass_vs, k_lampshadow_ps, &g_sh_vs, &g_l1_ps, g_scene_tex, (const float (*)[4])c, 8 + 10 * used, 1, (int)g_scene_w, (int)g_scene_h, g_bb))
        g_l1_passes++;
}

/* PT-RT's sun: the maps and the darkening pass's constants (c0..c11 as shadow_ps.hlsl takes them), kept for the composite */
static void shadow_pass(void) {
    IDirect3DDevice9 *d = photon_d3d_device();
    g_pt_sun_ok = 0;
    if (!g_pt_maps_only) l1_select();
    int res = 2048, soft = 0;
    float H = 1024.0f, strength = 0.55f;
    photon_vlg_shadow_settings(&res, &H, &soft, &strength);
    int full = res, used = res, lvl = photon_dynq_shadow_level();
    if (lvl >= 1) soft = 0;
    if (lvl >= 2) H = H * 0.5f < 512 ? 512 : H * 0.5f;
    if (lvl >= 3 && used > 1024) used /= 2;
    if (lvl >= 4) H = H * 0.5f < 384 ? 384 : H * 0.5f;
    if (!d || !g_scene_tex || !g_bb || !sm_create(d, full)) return;
    static int casc_env = -1;
    if (casc_env < 0) { char e[4]; casc_env = !(GetEnvironmentVariableA("PHOTON_SUN_CASCADE", e, sizeof e) > 0 && e[0] == '0'); }
    int cascade = casc_env && g_sn_tex != NULL;
    float Hn0 = H * 0.25f;
    if (cascade && used >= 1024) used /= 2;   /* the far cascade at half size: near full + far quarter = 1.25x the old pixels */
    static int last_used;
    if (used != last_used) { last_used = used; photon_dmirror_cast_invalidate(); }   /* a resolution step: the kept map is the wrong size */
    /* the main view: transpose(V*P) rows and its inverse; the camera position is the inverse view's translation */
    float V[16], P[16], m[4][4], iv[8][4], Vi[4][4], Vm[4][4];
    if (!photon_vlg_api_matrix(0, V) || !photon_vlg_api_matrix(1, P)) return;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += V[i * 4 + k] * P[k * 4 + j];
            m[j][i] = v;
            Vm[i][j] = V[i * 4 + j];
        }
    view_rect_fix(m);
    if (!inv4(m, iv) || !inv4(Vm, Vi)) return;
    float cam[3] = { Vi[3][0], Vi[3][1], Vi[3][2] };   /* row-vector view: the inverse's last row is the eye */
    photon_passtime_begin(PT_L3);
    l3_pass(d, (const float (*)[4])iv);   /* L3 first: it corrects the lightmapped frame, before any shadow darkens it */
    photon_passtime_end(PT_L3);
    photon_passtime_begin(PT_L1);
    if (!g_pt_maps_only) l1_pass(d, (const float (*)[4])iv, strength, cam, fabsf(P[5]) * (float)g_scene_h * 0.5f);   /* the lamps do not need a sun; P[5]: the projection's y scale */
    photon_passtime_end(PT_L1);
    long serial = 0;
    const SunMap *sm = lg_work_sun(&serial);
    if (!sm) return;
    SunInfo si;
    sun_map_info(sm, &si);
    if (!si.has_sun) return;
    /* the sun's orthographic box: centred on the camera, half size H, depth along the light over the map's extent */
    float zd[3] = { -si.sun_dir[0], -si.sun_dir[1], -si.sun_dir[2] };   /* the light travels this way: depth grows along it */
    norm3(zd);
    float up[3] = { 0, 0, 1 };
    if (fabsf(zd[2]) > 0.99f) { up[0] = 1; up[2] = 0; }
    float x[3], y[3];
    cross3(up, zd, x); norm3(x);
    cross3(zd, x, y); norm3(y);
    /* texel snapping: move the centre in whole shadow-map texels along x and y, so static shadows do not shimmer as the camera moves */
    float texel = 2.0f * H / (float)used;
    float cx = floorf(dot3(cam, x) / texel) * texel, cy = floorf(dot3(cam, y) / texel) * texel;
    float span = 0;
    for (int k = 0; k < 3; k++) span += fabsf(si.world_max[k] - si.world_min[k]);
    if (span < 1) span = 32768.0f;
    float dz0 = dot3(cam, zd) - span;   /* depth 0 at the camera's projection minus the map's size along the light */
    float L[3][4];
    for (int k = 0; k < 3; k++) { L[0][k] = x[k] / H; L[1][k] = y[k] / H; L[2][k] = zd[k] / (2.0f * span); }
    L[0][3] = -cx / H; L[1][3] = -cy / H; L[2][3] = -dz0 / (2.0f * span);
    float light4[4][4] = { { L[0][0], L[0][1], L[0][2], L[0][3] }, { L[1][0], L[1][1], L[1][2], L[1][3] }, { L[2][0], L[2][1], L[2][2], L[2][3] }, { 0, 0, 0, 1 } };
    static unsigned long long glog;
    if (photon_frame_count() >= glog + 600) {   /* the geometry check (tools/shadow_geo.py) reads these */
        glog = photon_frame_count();
        photon_log("shadows: geo sun_dir %.5f %.5f %.5f cam %.2f %.2f %.2f main rows %.6f %.6f %.6f %.4f | %.6f %.6f %.6f %.4f | %.6f %.6f %.6f %.4f | %.6f %.6f %.6f %.4f",
                   si.sun_dir[0], si.sun_dir[1], si.sun_dir[2], cam[0], cam[1], cam[2], m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                   m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
    }
    photon_passtime_begin(PT_CAST);
    int n = photon_dmirror_cast_all(g_sm_sfc, g_sm_rt, used, (const float (*)[4])light4, k_shnull20_ps, k_shnull30_ps, &g_ps20, &g_ps30);
    photon_passtime_end(PT_CAST);
    g_sh_casters += n;
    if (n <= 0) return;
    /* F1: the near cascade, a quarter of the box at the full map size (4x sharper near the camera); the far map above is drawn at half size
     * when the cascade is on (the corner trick), so memory grows by a quarter, not double. PHOTON_SUN_CASCADE=0: one cascade (A/B). */
    float N[3][4];
    int near_on = 0;
    if (cascade) {
        float Hn = Hn0, texn = 2.0f * Hn / (float)full;
        float cxn = floorf(dot3(cam, x) / texn) * texn, cyn = floorf(dot3(cam, y) / texn) * texn;
        for (int k = 0; k < 3; k++) { N[0][k] = x[k] / Hn; N[1][k] = y[k] / Hn; N[2][k] = zd[k] / (2.0f * span); }
        N[0][3] = -cxn / Hn; N[1][3] = -cyn / Hn; N[2][3] = -dz0 / (2.0f * span);
        float near4[4][4] = { { N[0][0], N[0][1], N[0][2], N[0][3] }, { N[1][0], N[1][1], N[1][2], N[1][3] }, { N[2][0], N[2][1], N[2][2], N[2][3] }, { 0, 0, 0, 1 } };
        PhotonCastOpts o = { 0, 0, full, NULL, 1, 17 };
        photon_passtime_begin(PT_NEAR);
        near_on = photon_dmirror_cast_opts(g_sn_sfc, g_sm_rt, &o, (const float (*)[4])near4, k_shnull20_ps, k_shnull30_ps, &g_ps20, &g_ps30) > 0;
        photon_passtime_end(PT_NEAR);
    }
    /* the darkening pass: c0..c3 inverse main view, c4 params, c5..c7 light rows */
    iv[4][0] = 4.0f / (2.0f * span); iv[4][1] = strength;   /* bias: 4 world units along the light */ iv[4][2] = 1.0f / (float)g_sm_size; iv[4][3] = (float)used / (float)g_sm_size;
    memcpy(iv[5], L[0], 16); memcpy(iv[6], L[1], 16); memcpy(iv[7], L[2], 16);
    float ivx[12][4];
    memcpy(ivx, iv, sizeof(float) * 32);
    memset(ivx[8], 0, sizeof(float) * 16);
    if (near_on) {
        memcpy(ivx[8], N[0], 16); memcpy(ivx[9], N[1], 16); memcpy(ivx[10], N[2], 16);
        ivx[11][0] = iv[4][0]; ivx[11][1] = 1; ivx[11][2] = 1.0f / (float)g_sm_size; ivx[11][3] = (float)full / (float)g_sm_size;
        photon_dmirror_fullscreen_texn(2, g_sn_tex, 0);
        g_sun_near++;
    }
    if (g_pt_maps_only) {   /* PT-RT: keep what the composite needs; no darkening */
        memcpy(g_pt_sun, ivx, sizeof g_pt_sun);
        g_pt_sun_ok = 1;
        photon_dmirror_fullscreen_texn(2, NULL, 0);   /* the near map was bound for a darkening pass that does not run */
        return;
    }
    memcpy(g_pt_sun, ivx, sizeof g_pt_sun); g_pt_sun_ok = 1;   /* also kept for the volumetric fog's sun shafts (fog.c) */
    photon_passtime_begin(PT_SCREEN);
    photon_dmirror_fullscreen_tex2(g_sm_tex);
    if (photon_dmirror_fullscreen(k_shpass_vs, soft ? k_shadow_soft_ps : k_shadow_ps, &g_sh_vs, soft ? &g_sh_soft_ps : &g_sh_ps, g_scene_tex, (const float (*)[4])ivx, 12, 1, (int)g_scene_w, (int)g_scene_h, g_bb))
        g_sh_passes++;
    photon_passtime_end(PT_SCREEN);
}

/* --- stage 1d: the engine's RTT shadows off for the props Photon shadows (analysis/34) -----------------------------------------------
 * C_BaseEntity::CreateShadow (run at creation and on network updates) asks the renderable's ShadowCastType (slot 31) and destroys an
 * existing RTT shadow when it answers SHADOWS_NONE; the shadow manager never asks again on its own. So, for the classes whose draws
 * Photon casts (CPhysicsProp for now: never skinned), slot 31 of their renderable table answers 0 while Photon shadows are on (UI3: also
 * while photon_shadow_props is off, and for characters while photon_shadow_characters is off), and once
 * after a class is patched every existing entity of it gets OnDataChanged(DATA_UPDATE_DATATABLE_CHANGED) (networkable slot 5), which
 * re-runs that check. Main thread only. Interface slots from the SDK (IClientRenderable 0 GetIClientUnknown; IClientUnknown 4
 * GetClientNetworkable, 5 GetClientRenderable (IHandleEntity's virtual destructor is slot 0); IClientNetworkable 2 GetClientClass, 5 OnDataChanged; ClientClass +0x10 network name;
 * VClientEntityList003 0 GetClientNetworkable, 6 GetHighestEntityIndex). */
enum { NCLS = 32, RSLOT_SHADOWCAST = 31, KIND_PROP = 1, KIND_CHAR = 2 };
typedef int (*ShadowCastFn)(void *);
static void **g_cls_vt[NCLS];
static ShadowCastFn g_cls_orig[NCLS];
static int g_cls_kind[NCLS];
static int g_refresh_pending;
static int g_sc_props = 1, g_sc_chars = 1;   /* UI3, main thread copy (photon_shadows_frame_3d_done) */
static volatile int g_sc_static;              /* photon_shadow_static, read by the device thread */
static int skinned_env(void);
static int hk_shadowcast(void *self) {
    void **vt = *(void ***)self;
    for (int i = 0; i < g_ncls; i++)
        if (g_cls_vt[i] == vt) {
            int none = g_cls_kind[i] == KIND_PROP ? (!g_sc_props || drawing()) : (!g_sc_chars || (drawing() && skinned_env()));
            if (none) { g_cast_none++; return 0; }
            return g_cls_orig[i](self);
        }
    return 0;
}
static const char *class_name(void *renderable) {
    void *unk = ((void *(*)(void *))(*(void ***)renderable)[0])(renderable);
    if (!unk) return NULL;
    void *net = ((void *(*)(void *))(*(void ***)unk)[4])(unk);   /* IHandleEntity's destructor is slot 0: GetClientNetworkable is 4 */
    if (!net) return NULL;
    const uint8_t *cc = ((void *(*)(void *))(*(void ***)net)[2])(net);
    if (!cc) return NULL;
    return *(const char *const *)(cc + 0x10);
}
/* UI3: a renderable table's kind, decided once at its first draw. Characters by network class name; client-only ragdolls (no class of
 * their own: the name is an ancestor's, CBaseAnimating) by their model having bones (studiohdr_t numbones at +0x9C; DrawModelState_t
 * starts with the studiohdr_t pointer). */
static int classify(const char *name, const uint8_t *state) {
    if (!name) name = "";
    if (!strcmp(name, "CPhysicsProp")) return KIND_PROP;
    if (!strncmp(name, "CNPC_", 5) || !strncmp(name, "CAI_", 4) || strstr(name, "Player") || strstr(name, "Ragdoll")) return KIND_CHAR;
    const uint8_t *hdr = state ? *(const uint8_t *const *)state : NULL;
    if ((!*name || !strcmp(name, "CBaseAnimating")) && hdr && *(const int *)(hdr + 0x9C) > 1) return KIND_CHAR;
    return 0;
}
static void sun_note_dme(const uint8_t *info, int cast);
/* T4.2 skinned casters (characters): default on, PHOTON_SHADOWS_SKINNED=0 off (then characters keep the engine's shadow). Read on the
 * device thread per skinned draw: the UI3 switch is the main thread's copy (a stale frame only costs one frame of characters' shadows). */
static int skinned_env(void) {
    static int v = -1;
    if (v < 0) { char e[4]; v = !(GetEnvironmentVariableA("PHOTON_SHADOWS_SKINNED", e, sizeof e) > 0 && e[0] == '0'); }
    return v;
}
int photon_shadows_skinned(void) { return skinned_env() && g_sc_chars; }
void photon_shadows_dme(const uint8_t *state, const uint8_t *info) {   /* p6.c's DrawModelExecute hook */
    int draw = drawing();
    if (!info || (!draw && g_sc_props && g_sc_chars)) return;
    void *r = *(void *const *)(info + 24);
    void **vt = r ? *(void ***)r : NULL;
    int kind = -1;
    for (int i = 0; vt && i < g_ncls; i++) if (g_cls_vt[i] == vt) { kind = g_cls_kind[i]; break; }
    static void **skip[128]; static int nskip;   /* seen and not ours: not asked again */
    for (int i = 0; vt && kind < 0 && i < nskip; i++) if (skip[i] == vt) kind = 0;
    if (vt && kind < 0) {
        kind = 0;
        uintptr_t lo, hi;
        if (g_ncls < NCLS && photon_module_text(vt[0], &lo, &hi) && (uintptr_t)vt[RSLOT_SHADOWCAST] >= lo && (uintptr_t)vt[RSLOT_SHADOWCAST] < hi) {
            const char *name = class_name(r);
            kind = classify(name, state);
            DWORD old;
            if (kind && VirtualProtect(&vt[RSLOT_SHADOWCAST], sizeof(void *), PAGE_READWRITE, &old)) {
                g_cls_orig[g_ncls] = (ShadowCastFn)vt[RSLOT_SHADOWCAST];
                g_cls_vt[g_ncls] = vt;
                g_cls_kind[g_ncls] = kind;
                g_ncls++;
                InterlockedExchangePointer(&vt[RSLOT_SHADOWCAST], (void *)hk_shadowcast);
                VirtualProtect(&vt[RSLOT_SHADOWCAST], sizeof(void *), old, &old);
                g_refresh_pending = 1;
                photon_log("shadows: stage 1d: %s renderable table %p (%s): ShadowCastType switched by the settings", name ? name : "(no class)", (void *)vt,
                           kind == KIND_PROP ? "prop" : "character");
            } else kind = 0;
        }
        if (!kind && nskip < 128) skip[nskip++] = vt;
    }
    if (draw) sun_note_dme(info, !(kind == KIND_PROP && !g_sc_props) && !(kind == KIND_CHAR && !g_sc_chars));
}
void photon_shadows_frame_3d_done(void) {   /* p6.c, main thread, after the frame's 3D views: re-ask the patched classes' entities */
    photon_packs_poll();   /* SP layer 1: is a shader pack listed (keeps the main view's end running for it) */
    static int was = -1;
    int p, c;
    photon_vlg_shadow_casters(&p, &c);
    g_sc_static = (p & 2) != 0 || photon_ptrt_on();   /* PT-RT: the map's props (trees) cast too: its sun is fully Photon's (PT-PROPS) */
    p &= 1;
    g_sc_props = p; g_sc_chars = c;
    int now = drawing() | p << 1 | c << 2;
    if (was >= 0 && now != was && g_ncls > 0) {
        g_refresh_pending = 1;
        photon_log("shadows: settings changed (sun shadows %s, props cast %s, characters cast %s): the engine's shadows re-asked", now & 1 ? "on" : "off",
                   p ? "on" : "off", c ? "on" : "off");
    }
    was = now;
    if (!g_refresh_pending) return;
    g_refresh_pending = 0;
    HMODULE cl = GetModuleHandleA("client.dll");
    void *(*ci)(const char *, int *) = cl ? (void *(*)(const char *, int *))(void *)GetProcAddress(cl, "CreateInterface") : NULL;
    void *el = ci ? ci("VClientEntityList003", NULL) : NULL;
    if (!el) return;
    void **evt = *(void ***)el;
    int hi = ((int (*)(void *))evt[6])(el);
    long n = 0;
    for (int i = 0; i <= hi && i < 65536; i++) {
        void *net = ((void *(*)(void *, int))evt[0])(el, i);
        if (!net) continue;
        void *unk = ((void *(*)(void *))(*(void ***)net)[0])(net);
        void *ren = unk ? ((void *(*)(void *))(*(void ***)unk)[5])(unk) : NULL;   /* GetClientRenderable */
        if (!ren) continue;
        void **rv = *(void ***)ren;
        int ours = 0;
        for (int k = 0; k < g_ncls; k++) if (g_cls_vt[k] == rv) ours = 1;
        if (!ours) continue;
        ((void (*)(void *, int))(*(void ***)net)[5])(net, 1);   /* OnDataChanged(DATA_UPDATE_DATATABLE_CHANGED): re-runs CreateShadow */
        n++;
    }
    g_refreshed += n;
    photon_log("shadows: stage 1d: %ld existing entities refreshed (their engine shadows re-asked)", n);
}

/* --- stage 2a: indoor props do not cast. The main thread asks P6's sun (photon_p6sun_light: the engine's own sun trace for a model at its
 * origin) at each DrawModelExecute and files the answer under the model-to-world translation; the device thread looks a caster up by its
 * c58..c60 translation (the same matrix) and leaves it out when the world already shades it from the sun. Lock-free: one writer (main),
 * one reader (MatQueue), a slot holds key + answer and a stale read only costs one frame of one prop's shadow. */
enum { SUNH = 4096 };
typedef struct { volatile uint64_t key; volatile int lit; } SunEnt;
static SunEnt g_sunh[SUNH];
static uint64_t sun_key(const float t[3]) {
    uint32_t u[3];
    memcpy(u, t, sizeof u);
    uint64_t k = ((uint64_t)u[0] * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)u[1] * 0xc2b2ae3d27d4eb4full) ^ ((uint64_t)u[2] * 0x165667b19e3779f9ull);
    return k ? k : 1;
}
/* skinned casters (their c58 is a bone, not the entity's origin): the same answers filed by 64-unit cell, so a character's pelvis finds its
 * entity's origin in its own cell or the one below. Bit 0: a lit entity was there this frame or last, bit 1: one in shade. */
enum { SUNC = 4096 };
typedef struct { volatile uint64_t key; volatile int bits; volatile unsigned frame; } SunCell;
static SunCell g_sunc[SUNC];
static uint64_t cell_key(int x, int y, int z) {
    uint64_t k = ((uint64_t)(uint32_t)x * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)(uint32_t)y * 0xc2b2ae3d27d4eb4full) ^ ((uint64_t)(uint32_t)z * 0x165667b19e3779f9ull);
    return k ? k : 1;
}
static void sun_note_cell(const float t[3], int lit) {
    int x = (int)floorf(t[0] / 64.0f), y = (int)floorf(t[1] / 64.0f), z = (int)floorf(t[2] / 64.0f);
    uint64_t k = cell_key(x, y, z);
    SunCell *c = &g_sunc[k & (SUNC - 1)];
    unsigned f = (unsigned)photon_frame_count();
    if (c->key != k || f - c->frame > 1) { c->key = k; c->bits = 0; }
    c->bits |= lit ? 1 : 2;
    c->frame = f;
}
int photon_shadows_skinned_lit(const float p[3]) {   /* dmirror, device thread: 0 = only shaded entities near this skinned caster */
    int x = (int)floorf(p[0] / 64.0f), y = (int)floorf(p[1] / 64.0f), z = (int)floorf(p[2] / 64.0f), bits = 0;
    unsigned f = (unsigned)photon_frame_count();
    for (int dz = 0; dz >= -1; dz--) {
        uint64_t k = cell_key(x, y, z + dz);
        const SunCell *c = &g_sunc[k & (SUNC - 1)];
        if (c->key == k && f - c->frame <= 2) bits |= c->bits;
    }
    return !(bits == 2);
}
static void sun_note_dme(const uint8_t *info, int cast) {   /* cast 0: UI3 switched this caster's kind off */
    const float *m = *(const float *const *)(info + 40);   /* pModelToWorld: matrix3x4, translation in [0][3], [1][3], [2][3] */
    if (!m) return;
    float t[3] = { m[3], m[7], m[11] };
    unsigned char desc[88];
    photon_p6sun_light(t, desc);
    int lit = cast && photon_p6sun_state() != 0;   /* only a definite "in the world's shade" leaves a caster out */
    uint64_t k = sun_key(t);
    SunEnt *e = &g_sunh[k & (SUNH - 1)];
    e->lit = lit;
    e->key = k;
    sun_note_cell(t, photon_p6sun_state() != 0);
    g_sun_asked++;
    if (!lit) g_sun_unlit++;
}
static int caster_dynamic(const float t[3]) {   /* L1: a DrawModelExecute filed this translation (an entity, not a static prop) */
    uint64_t k = sun_key(t);
    return g_sunh[k & (SUNH - 1)].key == k;
}
int photon_shadows_caster_lit(const float m34[3][4]) {   /* dmirror, device thread: 0 = the world shades this caster from the sun, or it is a static prop */
    float t[3] = { m34[0][3], m34[1][3], m34[2][3] };
    uint64_t k = sun_key(t);
    const SunEnt *e = &g_sunh[k & (SUNH - 1)];
    static int gate = -1;
    if (gate < 0) { char v[4]; gate = !(GetEnvironmentVariableA("PHOTON_SHADOWS_INDOOR", v, sizeof v) > 0 && v[0] == '0'); }   /* 0: stage 2a off (A/B) */
    if (!gate) return 1;
    if (e->key != k) { g_sun_miss++; return g_sc_static; }   /* not a DrawModelExecute draw: a static prop; photon_shadow_static decides (off: the map's own bake, its author's look) */
    return e->lit;
}
