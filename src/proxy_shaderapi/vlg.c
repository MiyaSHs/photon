/* T3.4c/T3.4f (Option A) + lighting plan v2 P1/P2: Photon lighting, the proxy side (design, hooks, pairing rule, guards
 * and threading: vlg.h). Pairs: VertexLitGeneric (vlg), bumped VertexLitGeneric (bump) and VertexLitGeneric $phong
 * (skin); c56 envmap occlusion. */
#include <windows.h>
#include <bcrypt.h>
#include <d3d9.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vlg.h"
#include "ptrt.h"
#include "taa.h"
#ifndef PHOTON_VLG_MUTANT_EAGER
#define PHOTON_VLG_MUTANT_EAGER 0   /* 1: the old eager VS rename (test A/B) */
#endif
#include "ambvol.h"
#include "cubemap.h"
#include "p6.h"
#include "cull.h"
#include "waterq.h"
#include "shadows.h"
#include "lights.h"
#include "d3dconst.h"
#include "frames.h"
#include "log.h"
#include "photon_lightblock.h"

/* Test-only build (CMake target proxy_shaderapi_mutant, tests/proxyhost): PHOTON_VLG_MUTANT=1 skips every pair revert
 * and the TakeSnapshot pairing check, so the mock host must see a vs_3_0 meet a stock ps_2_x. It refuses to light
 * anything unless PHOTON_TEST_VLG is set, and scripts/install_proxy.sh never installs it. */
#ifndef PHOTON_VLG_MUTANT
#define PHOTON_VLG_MUTANT 0
#endif

PhotonVlgStats g_photon_vlg = { .version = 11, .reason = "not set up" };
#define S g_photon_vlg
/* Statistics only, bumped on the draw thread: a plain increment (the locked one cost ~20 cycles each, several per draw; laptop profile) */
#define INC(x) (++S.x)

/* --- build 26229091 facts (T1.5 guard) ---------------------------------------------------------------------- */
enum { SH_SETDEFAULT = 0, SH_SETVS = 23, SH_SETPS = 24 };
enum { API_MATRIXMODE = 6, API_LOADMATRIX = 9, API_SETVSIDX = 40, API_GETMAXLIGHTS = 43, API_TAKESNAPSHOT = 109, API_BEGINPASS = 124, API_RENDERPASS = 125,
       API_SETLIGHT = 127, API_SETORIGIN = 128, API_SETAMBIENT = 129, API_SETCUBE = 130, API_CLEARSNAPSHOTS = 170,
       API_DISABLELIGHTS = 207, API_RESET260 = 260 };
typedef struct { int slot; uint32_t rva; } SlotRva;
/* CShaderShadowDX8 vtable 0x18007e250 and the CShaderAPIDx8 ShaderApi030 sub-object vtable 0x18007b4b8 (analysis/14b,
 * 05 §4.3; ClearSnapshots slot 170 = FUN_180011980 and slot 260 = FUN_1800196f0 both call the snapshot reset
 * FUN_18002f100). GetMaxLights (43) is called directly, not hooked. */
static const SlotRva k_sh_rva[] = { { SH_SETDEFAULT, 0x2a2e0 }, { SH_SETVS, 0x2a760 }, { SH_SETPS, 0x2a670 } };
static const SlotRva k_api_rva[] = {
    { API_MATRIXMODE, 0x19220 }, { API_LOADMATRIX, 0x18f70 }, { API_SETVSIDX, 0x204f0 }, { API_GETMAXLIGHTS, 0x17df0 }, { API_TAKESNAPSHOT, 0x20fd0 }, { API_BEGINPASS, 0x11150 },
    { API_RENDERPASS, 0x1a9d0 }, { API_SETLIGHT, 0x1e910 }, { API_SETORIGIN, 0x1eb50 }, { API_SETAMBIENT, 0x1d550 }, { API_SETCUBE, 0x1d5f0 },
    { API_CLEARSNAPSHOTS, 0x11980 }, { API_DISABLELIGHTS, 0x14f80 }, { API_RESET260, 0x196f0 } };
enum { OFF_SH_VSHANDLE = 0x270,      /* CShaderShadowDX8: VS handle from the shader manager (FUN_18002a760) */
       OFF_SH_PSHANDLE = 0x278,      /* CShaderShadowDX8: PS handle from the shader manager (FUN_18002a670) */
       OFF_SH_VSIDX = 0x280, OFF_SH_PSIDX = 0x284,   /* the static index of the same calls */
       OFF_API_D3DLIGHT = 0xf84,     /* ShaderApi030 sub-object: D3DLIGHT9[4] written by SetLight (FUN_18001e910) */
       OFF_API_FFFOG = 0x132d,       /* ShaderApi030 sub-object: bool, D3DRS_FOGENABLE as last applied by FUN_1800166e0
                                      * (= !pixel fog && scene fog); RenderPass FUN_18001a9d0 reads it for FOGVERTEXMODE */
       D3DLIGHT_STRIDE = 0x68 };
_Static_assert(sizeof(D3DLIGHT9) == D3DLIGHT_STRIDE, "D3DLIGHT9 is 0x68 bytes");
/* r_pixelfog: the ConVar object stock itself defines (DAT_1800a2a90, constructed by FUN_180001660 with the name
 * 0x18007b300 and default "1"); its primary vtable is ConVar::vftable 0x1800808f8, whose slot 13 (+0x68) is GetInt
 * FUN_1800047c0 (m_pParent +0x38 == this ? m_nValue +0x58 ^ (uint32)this : parent->GetInt()), the call stock's own
 * GetPixelFogMode FUN_180018040 / FUN_1800166e0 make. dxlevel: caps object [0x18009b008] +0x4f8 (same functions:
 * pixel fog needs dxlevel >= 90). */
enum { RVA_CV_PIXELFOG = 0xa2a90, RVA_CV_VTBL = 0x808f8, RVA_CV_GETINT = 0x47c0, RVA_STR_PIXELFOG = 0x7b300,
       CV_SLOT_GETINT = 13, CV_OFF_NAME = 0x18, RVA_PCAPS = 0x9b008, CAPS_OFF_DXLEVEL = 0x4f8 };
/* The VS dynamic index stock binds at RenderPass: g_pShaderManager ([0x18009c440]) +8 (m_nVertexShaderIndex).
 * VertexLitGeneric's SM2 path sets it with SET_DYNAMIC_VERTEX_SHADER_CMD (vertexlitgeneric_dx9_helper.cpp, vs20),
 * i.e. through the command buffer: ExecuteCommandBuffer (api slot 94, FUN_180015800, decomp :15713) writes mgr+8
 * directly and never calls api slot 40 [confirmed in game, T3.4f A5: 8700 of 8725 marked draws saw no slot 40]. So
 * pre_draw reads mgr+8 unless slot 40 was called on this thread since the last RenderPass (same value then). The
 * pointer is taken only if stock's SetVertexShaderIndex (slot 40, stock+0x204f0) is exactly
 * "mov rax,[rip->0x9c440]; mov [rax+8],edx; ret" (else lighting stays off). */
enum { RVA_FN_SETVSIDX = 0x204f0, RVA_PSHADERMGR = 0x9c440, MGR_OFF_VSIDX = 8 };
static const unsigned char k_setvsidx_code[11] = { 0x48, 0x8b, 0x05, 0x49, 0xbf, 0x07, 0x00, 0x89, 0x50, 0x08, 0xc3 };

/* The pairs GMod's VertexLitGeneric SM2 path draws, and our SM3 substitutes. VCS layouts: vlg (tools/t34_build_vlg20.py,
 * analysis/14c §2) vs20 1536 statics x 384 dynamics, ps20b 31457280 x 12; bump (tools/t34_build_bump20.py) bump_vs20
 * 8 x 24 (dynamics COMPRESSED_VERTS, DOWATERFOG, SKINNING, NUM_LIGHTS: no lighting-mode bits), bump_ps20b 43008 x 20;
 * skin (tools/t34_build_skin20.py) skin_vs20 2 x 48 (static USE_STATIC_CONTROL_FLOW; dynamics COMPRESSED_VERTS x1,
 * DOWATERFOG x2, SKINNING x4, LIGHTING_PREVIEW x8, NUM_LIGHTS x16 0..2: SDK 2015 skin_vs20.fxc, layout = stock's
 * [confirmed, t34_build_skin20 [layout]]), skin_ps20b 86016 x 80. */
enum { VS_NSTATIC = 1536, VS_NDYN = 384, VS_ST_VERTEXCOLOR = 1, VS_ST_HALFLAMBERT = 4,
       VSD_DYNAMIC_LIGHT = 2, VSD_STATIC_LIGHT = 4, VSD_STATIC_LIGHT_LIGHTMAP = 8, VSD_LIGHTING_PREVIEW = 64,
       PS_NDYN = 12, PS_NSTATIC = 31457280,
       BVS_NSTATIC = 8, BVS_NDYN = 24, BPS_NSTATIC = 43008, BPS_NDYN = 20,
       SVS_NSTATIC = 2, SVS_NDYN = 48, SVSD_LIGHTING_PREVIEW = 8, SPS_NSTATIC = 86016, SPS_NDYN = 80 };
typedef struct VcsFile {
    const char *stock, *ours;
    uint32_t nstatic, ndyn;
    uint32_t *ids;                   /* sorted static ids of the .ids file */
    size_t nids;
} VcsFile;
enum { PAIR_VLG, PAIR_BUMP, PAIR_SKIN, NPAIR };
typedef struct Pair {
    const char *tag;
    VcsFile vs, ps;
    int key;                         /* config key that must be != 0 at load for the pair to be used (-1: none) */
    uint8_t snapbit;                 /* PHOTON_VLG_SNAP_* bit of its marked snapshots (besides _OURS) */
    int on;                          /* both files loaded and checked at setup */
} Pair;
enum { K_LIGHTING, K_LIGHT_SCALE, K_AMBIENT_SCALE, K_AMBIENT_FLOOR, K_HALFLAMBERT, K_RANGE, K_DEBUG, K_ENVOCC,
       K_ENVOCC_MIN, K_ENVOCC_MAX, K_ENVOCC_CAL, K_BUMP, K_SKIN, K_AMBIENT, K_CUBE_BLEND, K_CUBE_PICK, K_LIGHTS, K_CLUSTER_K, K_LIGHT_CUTOFF, K_CLUSTER_KS, K_PRESET, K_SHADOWS, K_SH_RES, K_SH_DIST, K_SH_SOFT, K_SH_STRENGTH, K_TARGET_FPS, K_SH_PROPS, K_SH_CHARS, K_L1_LAMPS, K_L1_RES, K_L3, K_L3_RES, K_SH_STATIC, K_AUTO_LOD, K_PATHTRACE, K_PT_RAYS, K_PT_BOUNCES, K_TAA, K_TAA_SHARPEN, K_UPSCALE, K_RENDER_SCALE, K_NANITE, K_CPU_ANIM_STEP,
       K_FOG, K_FOG_DENSITY, K_FOG_HEIGHT, K_FOG_FALLOFF, K_FOG_DIST, K_FOG_R, K_FOG_G, K_FOG_B, K_FOG_SUN, K_FOG_ANISO, K_FOG_STEPS,
       NKEY };
static Pair g_pair[NPAIR] = {
    { "vlg", { "vertexlit_and_unlit_generic_vs20", "photon_vlg_vs20", VS_NSTATIC, VS_NDYN, NULL, 0 },
             { "vertexlit_and_unlit_generic_ps20b", "photon_vlg_ps20b", PS_NSTATIC, PS_NDYN, NULL, 0 }, -1, 0, 0 },
    { "bump", { "vertexlit_and_unlit_generic_bump_vs20", "photon_bump_vs20", BVS_NSTATIC, BVS_NDYN, NULL, 0 },
              { "vertexlit_and_unlit_generic_bump_ps20b", "photon_bump_ps20b", BPS_NSTATIC, BPS_NDYN, NULL, 0 }, K_BUMP,
              PHOTON_VLG_SNAP_BUMP, 0 },
    { "skin", { "skin_vs20", "photon_skin_vs20", SVS_NSTATIC, SVS_NDYN, NULL, 0 },
              { "skin_ps20b", "photon_skin_ps20b", SPS_NSTATIC, SPS_NDYN, NULL, 0 }, K_SKIN, PHOTON_VLG_SNAP_SKIN, 0 } };
/* the pair whose stock VS (PS) name this is, among the pairs in use; kind = index + 1 (0 = none) */
static int vs_kind(const char *name) {
    for (int k = 0; k < NPAIR; k++) if (g_pair[k].on && strcmp(name, g_pair[k].vs.stock) == 0) return k + 1;
    return 0;
}
static int ps_kind(const char *name) {
    for (int k = 0; k < NPAIR; k++) if (g_pair[k].on && strcmp(name, g_pair[k].ps.stock) == 0) return k + 1;
    return 0;
}

/* LightDesc_t, public mathlib/lightdesc.h (88 bytes, analysis/05 §2 item 22). */
typedef struct LightDesc {
    int type;                       /* MATERIAL_LIGHT_DISABLE 0, POINT 1, DIRECTIONAL 2, SPOT 3 */
    float color[3], pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
    float thetadot, phidot;
    unsigned flags;
    float oo_dot, range_sq;
} LightDesc;
_Static_assert(sizeof(LightDesc) == 0x58, "LightDesc_t is 88 bytes");
_Static_assert(offsetof(LightDesc, range) == 40 && offsetof(LightDesc, theta) == 60, "LightDesc_t layout");

/* --- state ----------------------------------------------------------------------------------------------------- */
static uintptr_t g_stock_base;
static int g_api_ready;                     /* ShaderApi030 hooks installed (renames wait for it) */
static volatile uint8_t g_snap[65536];      /* PHOTON_VLG_SNAP_* per snapshot id */
static void *g_sh_prev[64], *g_api_prev[300];
static void *g_cv_pixelfog;                 /* stock's r_pixelfog ConVar, validated in setup */
static char *const volatile *g_pmgr;        /* &g_pShaderManager (stock+0x9c440), validated in setup */
static PhotonWrap *volatile g_sh_obj;       /* the (single) ShaderShadow010 wrapper, for the TakeSnapshot check */

static SRWLOCK g_lock = SRWLOCK_INIT;       /* the config */
/* The light copies (g_lights, g_cube, g_origin) are written by the SetLight/SetAmbientLightCube/SetOrigin hooks and read in RenderPass:
 * all shader API calls of a draw, on the thread that executes draws (main, or MatQueue in queued mode). So they take no lock (the laptop
 * profile counted the per-draw lock pairs at about 2% of the main thread); light_read_check logs once if that assumption ever breaks. */
static volatile DWORD g_light_tid;
static int g_maxl = -1;
static int t_p6_idx = -1;   /* P6 diagnostics: the VS dynamic index of the current pass */
static void light_read_check(void) {
    static volatile LONG logged;
    DWORD t = GetCurrentThreadId(), wt = g_light_tid;
    if (wt && t != wt && InterlockedExchange(&logged, 1) == 0)
        photon_log("vlg: WARNING: light state written on thread %lu and read on %lu: the unlocked light copies assume one draw thread",
                   (unsigned long)wt, (unsigned long)t);
}
static LightDesc g_lights[4];
static float g_cube[6][4];
static float g_origin[3];

static volatile LONG g_dev_on;              /* the device may hold enable = 1 in c32 */
/* c56 (P1 envmap occlusion) as last written to the device by the draw thread; invalid after a snapshot clear (the
 * device-reset path, slot 260) so the next draw of ours writes it again */
static float g_occ_dev[4];
static volatile LONG g_occ_valid;
/* S3: the light block (c32..c55) as last uploaded by the draw thread. c33..c55 are written only by the full upload, so
 * a draw whose block equals it skips the 25-register write (consecutive meshes of one model, props sharing light
 * state); c32 is restored alone if flag_off cleared it since. Invalid after a snapshot clear (as g_occ_valid). */
static PhotonLightBlock g_blk_dev;
static volatile LONG g_blk_valid;
/* P3h, photon_ambient 1 only: c64.y and c59..c62 as last written, with the same invalidation */
static float g_c64_dev[4], g_vol_c_dev[4][4];
static float g_lg_c_dev[4];       /* P5: c63 as last written */
static volatile LONG g_lg_c_valid;
static volatile LONG g_vol_f_valid, g_vol_c_valid;
static int g_vol_draw;              /* the current draw has weight > 0 (draw thread) */

/* Shadow (snapshot) thread: what the shadow holds since SetDefaultState, as set through our hooks. t_{vs,ps}_ours = the
 * pair kind (PAIR_* + 1) whose substitute is in the shadow, 0 = stock; t_{vs,ps}_name/_sidx = the caller's stock name
 * pointer and index (re-sent on a revert). */
static __thread int t_vs_ours, t_ps_ours, t_vs_hl;
static __thread int t_vs_pending;   /* a VS of this pair kind may be renamed once its PS pairs (slot 24): the stock VS is in the shadow meanwhile */
static __thread const char *t_vs_name, *t_ps_name;
static __thread int t_vs_sidx, t_ps_sidx;
/* Draw thread. */
static __thread int t_cur_snap = 0xffff, t_dyn_idx, t_vs_fresh;
static __thread int t_ps_voltest;   /* P3d: the shadow's PS is Photon_DebugVol */
static __thread int t_ps_water;     /* W1: the shadow's PS is a water shader */
static int g_vol_wants;             /* P3d: the current draw is a volume draw (pre_draw sets it, draw thread) */

/* (handle, static index, pair kind) records the shader manager returned for our names (CShaderShadowDX8 +0x270/+0x278
 * and +0x280/+0x284 after the call): the TakeSnapshot check compares the shadow's real handle AND index against them,
 * so a released handle of ours that the manager reuses for another shader only matches if that shader also happens to
 * sit at an index we recorded for that very handle. Append-only between snapshot clears, bounded by the listed statics
 * (the .ids counts; full sets: vlg 48 VS + 2296 PS, bump 4 VS + 246 PS, skin 2 VS + 1032 PS: at most 54 VS and 3574 PS
 * records). load_pairs refuses .ids sets whose VS or PS total exceeds NHANDLES (lighting stays off). */
enum { NHANDLES = 4096 };
typedef struct { uint64_t h; int idx, kind; } HandleRec;
static volatile HandleRec g_hv[NHANDLES], g_hp[NHANDLES];
static volatile LONG g_nhv, g_nhp;
static SRWLOCK g_hlock = SRWLOCK_INIT;
/* 0: handle unknown; 1: this exact (handle, index) is ours (*kind = its pair kind); 2: the handle is one of ours, but
 * never with this index */
static int handle_find(const volatile HandleRec *t, LONG n, uint64_t h, int idx, int *kind) {
    int r = 0;
    for (LONG i = 0; i < n && i < NHANDLES; i++)
        if (t[i].h == h) { if (t[i].idx == idx) { if (kind) *kind = t[i].kind; return 1; } r = 2; }
    return r;
}
static void handle_add(volatile HandleRec *t, volatile LONG *n, uint64_t h, int idx, int kind) {
    if (handle_find(t, *n, h, idx, NULL) == 1) return;
    AcquireSRWLockExclusive(&g_hlock);
    if (handle_find(t, *n, h, idx, NULL) != 1 && *n < NHANDLES) {
        t[*n].h = h; t[*n].idx = idx; t[*n].kind = kind; MemoryBarrier(); InterlockedIncrement(n);
    } else if (*n >= NHANDLES) INC(handle_table_full);
    ReleaseSRWLockExclusive(&g_hlock);
}

/* --- config ------------------------------------------------------------------------------------------------------ */
static const struct { const char *key, *env; float def, lo, hi; int integer; } k_keys[NKEY] = {
    { "photon_lighting", "PHOTON_LIGHTING", 1, 0, 1, 1 },
    { "photon_light_scale", "PHOTON_LIGHT_SCALE", 1, 0, 16, 0 },
    { "photon_ambient_scale", "PHOTON_AMBIENT_SCALE", 1, 0, 16, 0 },
    { "photon_ambient_floor", "PHOTON_AMBIENT_FLOOR", 0, 0, 1, 0 },
    { "photon_halflambert", "PHOTON_HALFLAMBERT", -1, -1, 1, 1 },
    { "photon_range", "PHOTON_RANGE", 0, 0, 1, 1 },
    { "photon_debug", "PHOTON_DEBUG", 0, 0, 3, 1 },   /* 3 (P5): the light grid's count per pixel (photon_lights_ps.h PhotonLightHeat) */
    { "photon_envmap_occlusion", "PHOTON_ENVMAP_OCCLUSION", 1, 0, 1, 0 },
    { "photon_envmap_occlusion_min", "PHOTON_ENVMAP_OCCLUSION_MIN", 0, 0, 1, 0 },
    { "photon_envmap_occlusion_max", "PHOTON_ENVMAP_OCCLUSION_MAX", 1, 0, 4, 0 },
    { "photon_envmap_calibration", "PHOTON_ENVMAP_CALIBRATION", 0.5f, 0.01f, 100, 0 },
    { "photon_bump", "PHOTON_BUMP", 1, 0, 1, 1 },
    { "photon_skin", "PHOTON_SKIN", 1, 0, 1, 1 },
    /* -1 (unset, the default): follows photon_lights, which needs the volume (the folded lights leave the cube for the grid, analysis/18); 0 forces it off */
    { "photon_ambient", "PHOTON_AMBIENT", -1, -1, 1, 1 },
    { "photon_cubemap_blend", "PHOTON_CUBEMAP_BLEND", 1, 0, 1, 1 },   /* P4b: the second cubemap from the pick table, blended per pixel (default on 2026-11-07: laptop cost within noise, steps 100% -> 3%) */
    /* P4b: the visible cubemap from the pick table replaces the engine's. -1 (unset, the default): follows photon_cubemap_blend, because the
     * blend without the pick pops where the engine binds a sample the table holds hidden (analysis/17b); 0 forces it off */
    { "photon_cubemap_pick", "PHOTON_CUBEMAP_PICK", -1, -1, 1, 1 },
    /* P5 (analysis/18_p5_lights.md, lights.h): per-pixel lights from the light grid, on the volume's draws; the grid's lights per cell; the static
     * lights' window cutoff (latched at map load) */
    { "photon_lights", "PHOTON_LIGHTS", 1, 0, 1, 1 },   /* default on 2026-11-07 (user: on by default when the difference is minimal and an improvement) */
    { "photon_cluster_k", "PHOTON_CLUSTER_K", 15, 1, 15, 1 },
    { "photon_light_cutoff", "PHOTON_LIGHT_CUTOFF", 0.01f, 0.0001f, 1, 0 },
    /* P5b (analysis/18): the static lights per cell the split keeps per pixel, the rest folded per draw (latched at map load; 0 = no split) */
    { "photon_cluster_ks", "PHOTON_CLUSTER_KS", 11, 0, 15, 1 },
    /* UI1 (user, 2026-11-08): the preset: 0 Optimized (v1 look, the default), 1 Better lighting (v2, balanced), 2 Best quality (v2, the
     * sharpest), 3 Custom (only the settings below). Each setting -1 follows the preset; any other value overrides it. */
    { "photon_preset", "PHOTON_PRESET", 0, 0, 3, 1 },
    { "photon_shadows", "PHOTON_SHADOWS_DRAW", -1, -1, 1, 1 },          /* Photon's sun shadows instead of the engine's blobs */
    { "photon_shadow_res", "PHOTON_SHADOW_RES", -1, -1, 4096, 1 },      /* shadow map size: 1024, 2048, 4096 */
    { "photon_shadow_dist", "PHOTON_SHADOW_DIST", -1, -1, 8192, 1 },    /* the shadowed area's half width around the camera, units */
    { "photon_shadow_soft", "PHOTON_SHADOW_SOFT", -1, -1, 1, 1 },       /* 0 hard edges (4 taps), 1 soft (16 taps) */
    { "photon_shadow_strength", "PHOTON_SHADOW_STRENGTH", -1, -1, 1, 0 }, /* darkness 0..1 */
    /* UI2: dynamic quality: hold this frame rate by stepping the v2 quality below the preset (the ceiling); 0 = off */
    { "photon_target_fps", "PHOTON_TARGET_FPS", 0, 0, 500, 1 },
    /* UI3 (user, 2026-11-08): who casts shadows; off = neither Photon's nor the engine's shadow, receiving is unaffected */
    { "photon_shadow_props", "PHOTON_SHADOW_PROPS", 1, 0, 1, 1 },        /* physics props */
    { "photon_shadow_characters", "PHOTON_SHADOW_CHARACTERS", 1, 0, 1, 1 }, /* players, NPCs, ragdolls */
    /* L1 (analysis/35): map lamps with a real-time shadow where dynamic casters are near them (0 off, 1..4), and their tile size */
    { "photon_light_shadows", "PHOTON_LIGHT_SHADOWS", -1, -1, 4, 1 },
    { "photon_light_shadow_res", "PHOTON_LIGHT_SHADOW_RES", -1, -1, 1024, 1 },
    /* L3 (analysis/37): the re-baked sharp lamp shadows on the world (0 off, 1 on) and the voxel size in units (8, 4) */
    { "photon_rebake", "PHOTON_REBAKE", -1, -1, 1, 1 },
    { "photon_rebake_res", "PHOTON_REBAKE_RES", -1, -1, 8, 1 },
    /* the map's static props cast Photon's sun shadows (off: their shadow is the map's own bake, the author's look) */
    { "photon_shadow_static", "PHOTON_SHADOWS_STATIC", 0, 0, 1, 1 },
    /* F3 (analysis/38): generated LODs for models without them, written to garrysmod/addons/!photon_lod; off until the user's OK */
    { "photon_auto_lod", "PHOTON_LOD", 0, 0, 1, 1 },
    /* L4 (analysis/39): the map's lightmaps path-traced by Photon (baked once per map, cached); an opt-in mode that may change the look */
    { "photon_pathtrace", "PHOTON_PATHTRACE", 0, 0, 2, 1 },   /* 1 baked (L4), 2 real time (PT-RT on the Vulkan bridge, from the next start) */
    /* L4 / PT-RT: the path tracer's own settings (user, 2026-11-09: "path tracing having light ray amount as a config"): paths per sample
     * per pass, and bounces per path */
    { "photon_pt_rays", "PHOTON_PT_RAYS", 8, 1, 64, 1 },
    { "photon_pt_bounces", "PHOTON_PT_BOUNCES", 3, 0, 8, 1 },
    /* AA (taa.c): temporal anti-aliasing of the 3D view, 0 off (the default: TAA softens), 1 on */
    { "photon_taa", "PHOTON_TAA", 0, 0, 1, 1 },
    /* AA: the sharpening of the TAA'd frame (CAS-style, shadows.c), 0 off .. 1 strong */
    { "photon_taa_sharpen", "PHOTON_TAA_SHARPEN", 0.8f, 0, 1, 0 },
    /* UPSCALE (analysis/45): how a scaled 3D view (DYN's render scale, mat_viewportscale) is brought to full size: 0 the engine's
     * stretch, 1 Photon's temporal upscale (TAAU: jittered while scaled, even with photon_taa 0), 2 Photon's bilinear (the hook's test) */
    { "photon_upscale", "PHOTON_UPSCALE", 1, 0, 3, 1 },   /* 3: AMD FSR 2 on the Vulkan bridge (fsr2.c; TAAU when it is not available) */
    /* UPSCALE: the user's 3D view scale (photon-64's Lua applies it as mat_viewportscale, and DYN's renderscale steps scale from it) */
    { "photon_render_scale", "PHOTON_RENDER_SCALE", 1, 0.5f, 1, 0 },
    /* NANITE (analysis/44): the map's static props drawn by Photon on the Vulkan bridge (from the next start; experimental) */
    { "photon_nanite", "PHOTON_NANITE", 0, 0, 1, 1 },
    /* the CPU module's (photon-a0) animation stepping, settable by shader packs: 0 off, 1..8 a pose every Nth frame, 12..30 a fixed pose
     * rate (fps, cel style); read by the module through the host's setting() (version 7) */
    { "photon_cpu_anim_step", "PHOTON_CPU_ANIM_STEP", 0, 0, 30, 1 },
    /* SP layer 3 (fog.c): volumetric fog, off by default; packs set these (densities per 1000 units, heights / distances in units) */
    { "photon_fog", "PHOTON_FOG", 0, 0, 1, 1 },
    { "photon_fog_density", "PHOTON_FOG_DENSITY", 1.5f, 0, 100, 0 },
    { "photon_fog_height", "PHOTON_FOG_HEIGHT", 0, -16000, 16000, 0 },
    { "photon_fog_falloff", "PHOTON_FOG_FALLOFF", 400, 1, 16000, 0 },
    { "photon_fog_dist", "PHOTON_FOG_DIST", 6000, 100, 60000, 0 },
    { "photon_fog_r", "PHOTON_FOG_R", 0.55f, 0, 4, 0 },
    { "photon_fog_g", "PHOTON_FOG_G", 0.6f, 0, 4, 0 },
    { "photon_fog_b", "PHOTON_FOG_B", 0.68f, 0, 4, 0 },
    { "photon_fog_sun", "PHOTON_FOG_SUN", 3, 0, 50, 0 },
    { "photon_fog_aniso", "PHOTON_FOG_ANISO", 0.6f, -0.95f, 0.95f, 0 },
    { "photon_fog_steps", "PHOTON_FOG_STEPS", 24, 4, 64, 1 },
};
static float g_cfg[NKEY];                   /* under g_lock */
static int g_route_test;                    /* PHOTON_TEST_ROUTE: 1 = ENGINE draws go black, 2 = log only (P3c) */
static int g_cov_on;                        /* PHOTON_TEST_COVERAGE=1 (P3c2) */
/* S3: the draw thread's copy of g_cfg and the c56 value derived from it, refreshed only when the generation moves, so a
 * draw takes no lock and no log2f (95% of our draws are unlit and read nothing else). */
static volatile LONG g_cfg_gen;             /* bumped under g_lock with every g_cfg write */
static LONG g_draw_gen = -1;
static float g_draw_cfg[NKEY], g_draw_occ[4];
static wchar_t g_cfg_path[MAX_PATH];
static wchar_t g_pack_path[MAX_PATH];   /* SP (analysis/47): data/photon/packs/features.txt, the enabled shader packs' feature choices (photon_packs.lua) */
static wchar_t g_ui_path[MAX_PATH];   /* UI1: data/photon/settings.txt, written by the Spawn menu page (lua/autorun/client/photon_menu.lua) */
static FILETIME g_cfg_mtime;
static int g_cfg_exists = -1;

static int parse_value(int k, const char *s, float *out) {
    char *end;
    double v = strtod(s, &end);
    if (end == s || !isfinite(v)) return 0;
    if (v < k_keys[k].lo) v = k_keys[k].lo;
    if (v > k_keys[k].hi) v = k_keys[k].hi;
    *out = k_keys[k].integer ? (float)lrint(v) : (float)v;
    return 1;
}

static void utf8(const wchar_t *w, char *out, int cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) snprintf(out, (size_t)cap, "?");
}

/* DYN: the dynamic-quality step order (photon_dyn_order, a comma list of step names; empty = the default order), the one text setting */
static SRWLOCK g_order_lock = SRWLOCK_INIT;
static char g_order[512], g_order_load[512];
int photon_vlg_dyn_order(char *out, int cap) {
    AcquireSRWLockShared(&g_order_lock);
    snprintf(out, (size_t)cap, "%s", g_order);
    ReleaseSRWLockShared(&g_order_lock);
    return out[0] != 0;
}
static void take_order(const char *p) {   /* the value after the key: quoted or bare, up to the end of the line */
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    int n = 0;
    while (*p && *p != '"' && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' && n < (int)sizeof g_order_load - 1) g_order_load[n++] = *p++;
    g_order_load[n] = 0;
}

/* defaults <- photon.cfg <- environment. src[k] = 'd', 'f' or 'e'. Returns 1 if the file was read. */
static int read_cfg_file(const wchar_t *path, char tag, const char *name, float v[NKEY], char src[NKEY]) {
    FILE *f = path[0] ? _wfopen(path, L"r") : NULL;
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '/' && p[1] == '/') continue;
        char key[64], val[64];
        int n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < 63) key[n++] = *p++;
        key[n] = 0;
        if (_stricmp(key, "photon_dyn_order") == 0) { take_order(p); continue; }
        while (*p == ' ' || *p == '\t' || *p == '"') p++;
        n = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '"' && *p != '\r' && *p != '\n' && n < 63) val[n++] = *p++;
        val[n] = 0;
        if (!key[0]) continue;
        for (int k = 0; k < NKEY; k++)
            if (_stricmp(key, k_keys[k].key) == 0) {
                if (parse_value(k, val, &v[k])) src[k] = tag;
                else photon_log("vlg: %s: bad value \"%s\" for %s (ignored)", name, val, key);
            }
    }
    fclose(f);
    return 1;
}
/* defaults <- photon.cfg <- data/photon/settings.txt (the in-game page) <- environment. src[k] = 'd', 'f', 'u' or 'e'. Returns 1 if
 * photon.cfg was read. */
static int load_config(float v[NKEY], char src[NKEY]) {
    for (int k = 0; k < NKEY; k++) { v[k] = k_keys[k].def; src[k] = 'd'; }
    g_order_load[0] = 0;
    int read = read_cfg_file(g_cfg_path, 'f', "photon.cfg", v, src);
    read_cfg_file(g_ui_path, 'u', "settings.txt", v, src);
    read_cfg_file(g_pack_path, 'p', "packs/features.txt", v, src);   /* the enabled packs' features over the user's own settings, below env */
    { char e[512]; DWORD n = GetEnvironmentVariableA("PHOTON_DYN_ORDER", e, sizeof e); if (n > 0 && n < sizeof e) take_order(e); }
    AcquireSRWLockExclusive(&g_order_lock);
    if (strcmp(g_order, g_order_load)) { memcpy(g_order, g_order_load, sizeof g_order); photon_log("vlg: photon_dyn_order \"%s\"", g_order); }
    ReleaseSRWLockExclusive(&g_order_lock);
    for (int k = 0; k < NKEY; k++) {
        char e[64];
        DWORD n = GetEnvironmentVariableA(k_keys[k].env, e, sizeof e);
        if (n > 0 && n < sizeof e) {
            if (parse_value(k, e, &v[k])) src[k] = 'e';
            else photon_log("vlg: bad value %s=\"%s\" (ignored)", k_keys[k].env, e);
        }
    }
    return read;
}

static void log_config(const char *why, const float v[NKEY], const char src[NKEY]) {
    char line[1024];
    int n = 0;
    for (int k = 0; k < NKEY && n < (int)sizeof line - 48; k++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%g(%c)", k_keys[k].key, v[k], src[k]);
    photon_log("vlg: config %s:%s  [d default, f photon.cfg, u settings.txt (in-game page), p a shader pack, e environment]", why, line);
}

static int cfg_stamp(FILETIME *t) {   /* a combined stamp of photon.cfg and settings.txt: 0 when neither exists */
    WIN32_FILE_ATTRIBUTE_DATA a;
    int any = 0;
    ULARGE_INTEGER sum = { .QuadPart = 0 };
    const wchar_t *paths[3] = { g_cfg_path, g_ui_path, g_pack_path };
    for (int i = 0; i < 3; i++)
        if (paths[i][0] && GetFileAttributesExW(paths[i], GetFileExInfoStandard, &a)) {
            any |= 1 << i;
            sum.QuadPart += ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) * (ULONGLONG)(i + 1) + a.nFileSizeLow;
        }
    t->dwLowDateTime = sum.LowPart; t->dwHighDateTime = sum.HighPart;
    return any;
}

/* P4b: both settings at load (cubemap.c sets up the hooks, the pick table and, for the blend, B's sampler in our snapshots, only then);
 * the per-draw behaviour follows the live values */
/* P6 skips the engine's BindLocalCubemap with the rest of its light setup, so with P6 the pick is on unless set to 0 explicitly */
static int cube_pick_of(const float *cfg) { return cfg[K_CUBE_PICK] > 0 || (cfg[K_CUBE_PICK] < 0 && (cfg[K_CUBE_BLEND] != 0 || photon_p6_wanted())); }
void photon_vlg_cube_wanted(int *blend, int *pick) {
    *blend = *pick = 0;
    if (!S.wanted || !S.active) return;
    AcquireSRWLockShared(&g_lock);
    *blend = g_cfg[K_CUBE_BLEND] != 0 && g_cfg[K_LIGHTING] != 0;
    *pick = cube_pick_of(g_cfg) && g_cfg[K_LIGHTING] != 0;
    ReleaseSRWLockShared(&g_lock);
}

/* UI1: the presets' values of the user-facing settings (index: preset 0..2; Custom uses Optimized's for anything not set) */
static const float k_preset_lamps[3] = { 0, 1, 4 }, k_preset_lamp_res[3] = { 512, 512, 1024 };
static const float k_preset_rebake[3] = { 0, 0, 1 }, k_preset_rebake_res[3] = { 8, 8, 8 };   /* Best quality only, at 8 units: the pruned bake kept the maps' look (analysis/37); 4 units stays a manual choice (8x the voxels, untested size) */
static const float k_preset_shadows[3] = { 0, 1, 1 }, k_preset_res[3] = { 2048, 2048, 4096 }, k_preset_dist[3] = { 1024, 1024, 2048 },
                   k_preset_soft[3] = { 0, 0, 1 }, k_preset_strength[3] = { 0.55f, 0.55f, 0.6f };
static float preset_val(const float *cfg, int k, const float *table) {
    int p = (int)cfg[K_PRESET];
    if (cfg[k] >= 0) return cfg[k];
    return table[p >= 0 && p <= 2 ? p : 0];
}
int photon_vlg_shadows_wanted(void) {   /* UI1 / T4.2: Photon's sun shadows, live (photon_shadows, else the preset) */
    if (!S.wanted || !S.active) return 0;
    AcquireSRWLockShared(&g_lock);
    float sh = preset_val(g_cfg, K_SHADOWS, k_preset_shadows), li = g_cfg[K_LIGHTING];
    ReleaseSRWLockShared(&g_lock);
    return li != 0 && sh > 0;
}
int photon_vlg_taa(void) {   /* AA: photon_taa (live); taa.c caches it per Present */
    AcquireSRWLockShared(&g_lock);
    int v = (int)g_cfg[K_TAA];
    ReleaseSRWLockShared(&g_lock);
    return v;
}
int photon_vlg_upscale(void) {   /* UPSCALE: photon_upscale (live) */
    AcquireSRWLockShared(&g_lock);
    int v = (int)g_cfg[K_UPSCALE];
    ReleaseSRWLockShared(&g_lock);
    return v;
}
float photon_vlg_taa_sharpen(void) {   /* AA: photon_taa_sharpen (live) */
    AcquireSRWLockShared(&g_lock);
    float v = g_cfg[K_TAA_SHARPEN];
    ReleaseSRWLockShared(&g_lock);
    return v;
}
int photon_vlg_target_fps(void) {   /* UI2: 0 = dynamic quality off */
    AcquireSRWLockShared(&g_lock);
    int f = (int)g_cfg[K_TARGET_FPS];
    ReleaseSRWLockShared(&g_lock);
    return f;
}
void photon_vlg_shadow_settings(int *res, float *dist, int *soft, float *strength) {   /* UI1: the live shadow settings, preset applied */
    AcquireSRWLockShared(&g_lock);
    float r = preset_val(g_cfg, K_SH_RES, k_preset_res), d = preset_val(g_cfg, K_SH_DIST, k_preset_dist);
    float so = preset_val(g_cfg, K_SH_SOFT, k_preset_soft), st = preset_val(g_cfg, K_SH_STRENGTH, k_preset_strength);
    ReleaseSRWLockShared(&g_lock);
    *res = r >= 3000 ? 4096 : r >= 1500 ? 2048 : 1024;
    *dist = d < 256 ? 256 : d;
    *soft = so > 0;
    *strength = st < 0 ? 0 : st > 1 ? 1 : st;
}
int photon_vlg_rebake(int *voxel) {   /* L3: the re-bake wanted (preset applied) and its voxel size, 8 or 4 */
    AcquireSRWLockShared(&g_lock);
    float on = preset_val(g_cfg, K_L3, k_preset_rebake), r = preset_val(g_cfg, K_L3_RES, k_preset_rebake_res);
    ReleaseSRWLockShared(&g_lock);
    if (voxel) *voxel = r <= 5 ? 4 : 8;
    return on > 0;
}
int photon_vlg_toon(void) {   /* TOON: removed (user, 2026-11-11: "remove toon the way it is and later make it from scratch"); the old paths stay
                               * compiled but unreachable until the new anime / toon work (TASKS ANIME) replaces them */
    return 0;
}
int photon_vlg_pathtrace(void) {   /* L4: photon_pathtrace 1 (no preset: an explicit choice); 2 is the real-time path tracer */
    AcquireSRWLockShared(&g_lock);
    int on = g_cfg[K_PATHTRACE] > 0.5f && g_cfg[K_PATHTRACE] < 1.5f;
    ReleaseSRWLockShared(&g_lock);
    return on;
}
int photon_vlg_pathtrace_rt(void) {   /* PT-RT: photon_pathtrace 2 (read once at the start: its passes register then) */
    AcquireSRWLockShared(&g_lock);
    int on = g_cfg[K_PATHTRACE] > 1.5f;
    ReleaseSRWLockShared(&g_lock);
    return on;
}
float photon_vlg_setting(const char *key, int *found) {   /* any setting by its name, as resolved now (defaults, photon.cfg, settings.txt,
                                                           * the shader packs' features.txt, environment); the CPU module's host.setting */
    for (int k = 0; k < NKEY; k++)
        if (!strcmp(k_keys[k].key, key)) {
            AcquireSRWLockShared(&g_lock);
            float v = g_cfg[k];
            ReleaseSRWLockShared(&g_lock);
            if (found) *found = 1;
            return v;
        }
    if (found) *found = 0;
    return 0;
}
int photon_vlg_nanite(void) {   /* NANITE: photon_nanite (read once at the start) */
    AcquireSRWLockShared(&g_lock);
    int on = g_cfg[K_NANITE] != 0;
    ReleaseSRWLockShared(&g_lock);
    return on;
}
void photon_vlg_pt_params(int *rays, int *bounces) {   /* L4 / PT-RT: photon_pt_rays, photon_pt_bounces */
    AcquireSRWLockShared(&g_lock);
    *rays = (int)g_cfg[K_PT_RAYS]; *bounces = (int)g_cfg[K_PT_BOUNCES];
    ReleaseSRWLockShared(&g_lock);
}
int photon_vlg_auto_lod(void) {   /* F3: photon_auto_lod (no preset: an explicit choice, it writes into the install) */
    AcquireSRWLockShared(&g_lock);
    int on = g_cfg[K_AUTO_LOD] != 0;
    ReleaseSRWLockShared(&g_lock);
    return on;
}
int photon_vlg_light_shadows(int *res) {   /* L1: lamps with real-time shadows (0..4, preset applied) and the tile size */
    AcquireSRWLockShared(&g_lock);
    float n = preset_val(g_cfg, K_L1_LAMPS, k_preset_lamps), r = preset_val(g_cfg, K_L1_RES, k_preset_lamp_res);
    ReleaseSRWLockShared(&g_lock);
    if (res) *res = r >= 768 ? 1024 : 512;
    return n < 0 ? 0 : n > 4 ? 4 : (int)n;
}
void photon_vlg_shadow_casters(int *props, int *chars) {   /* UI3: independent of photon_lighting (they also switch the engine's shadows) */
    AcquireSRWLockShared(&g_lock);
    *props = g_cfg[K_SH_PROPS] != 0; *chars = g_cfg[K_SH_CHARS] != 0;
    if (g_cfg[K_SH_STATIC] != 0) *props |= 2;   /* bit 1: the map's static props cast the sun too */
    ReleaseSRWLockShared(&g_lock);
}
static int ambient_of(const float *cfg) { return cfg[K_AMBIENT] > 0 || (cfg[K_AMBIENT] < 0 && cfg[K_LIGHTS] != 0); }
int photon_vlg_ambient_wanted(void) {
    if (!S.wanted || !S.active) return 0;
    AcquireSRWLockShared(&g_lock);
    int on = ambient_of(g_cfg) && g_cfg[K_LIGHTING] != 0;
    ReleaseSRWLockShared(&g_lock);
    return on;
}

/* P5: photon_lights now (photon_lighting on), the cluster k and the cutoff */
int photon_vlg_lights_wanted(int *k, float *cutoff) {
    if (!S.wanted || !S.active) return 0;
    AcquireSRWLockShared(&g_lock);
    int on = g_cfg[K_LIGHTS] != 0 && g_cfg[K_LIGHTING] != 0;
    if (k) *k = (int)g_cfg[K_CLUSTER_K];
    if (cutoff) *cutoff = g_cfg[K_LIGHT_CUTOFF];
    ReleaseSRWLockShared(&g_lock);
    return on;
}

int photon_vlg_lights_split(void) {
    AcquireSRWLockShared(&g_lock);
    int ks = (int)g_cfg[K_CLUSTER_KS];
    ReleaseSRWLockShared(&g_lock);
    return ks;
}

int photon_vlg_poll_config(int force) {
    if (!S.wanted) return 0;
    FILETIME t = { 0, 0 };
    int exists = cfg_stamp(&t);
    if (!force && exists == g_cfg_exists && (!exists || CompareFileTime(&t, &g_cfg_mtime) == 0)) return 0;
    g_cfg_exists = exists;
    g_cfg_mtime = t;
    float v[NKEY];
    char src[NKEY];
    load_config(v, src);
    AcquireSRWLockExclusive(&g_lock);
    int changed = memcmp(v, g_cfg, sizeof v) != 0;
    memcpy(g_cfg, v, sizeof v);
    InterlockedIncrement(&g_cfg_gen);
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) { INC(config_reloads); log_config("reloaded", v, src); }
    return changed;
}

/* --- files ---------------------------------------------------------------------------------------------------- */
static int sha1_file_w(const wchar_t *path, char out_hex[41]) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;
    static unsigned char buf[1 << 16];
    unsigned char dig[20];
    DWORD got;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        ok = 1;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) if (BCryptHashData(h, buf, got, 0) != 0) { ok = 0; break; }
        if (ok && BCryptFinishHash(h, dig, sizeof dig, 0) == 0)
            for (int i = 0; i < 20; i++) sprintf(out_hex + 2 * i, "%02x", dig[i]);
        else ok = 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* <name>.ids (tools/t34_build_vlg20.py): "# ..." comments, "vcs_sha1 <hex>", "stock_sha1 <hex>", "count <n>", then
 * one static id per line (< nstatic). */
static int read_ids(VcsFile *vf, const wchar_t *path, char sha1[41], char *err, size_t errcap) {
    FILE *f = _wfopen(path, L"r");
    if (!f) { snprintf(err, errcap, "%s.ids missing", vf->ours); return 0; }
    char line[256];
    long count = -1;
    size_t cap = 64, n = 0;
    uint32_t *ids = malloc(cap * sizeof *ids);
    int ok = ids != NULL;
    sha1[0] = 0;
    while (ok && fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n");
        if (!line[len] && !feof(f)) {               /* longer than the buffer: only a comment may be */
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {}
            if (line[0] != '#') { snprintf(err, errcap, "%s.ids: over-long line", vf->ours); ok = 0; break; }
            continue;
        }
        line[len] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (strncmp(line, "vcs_sha1 ", 9) == 0) {
            if (strlen(line + 9) != 40) { snprintf(err, errcap, "%s.ids: bad vcs_sha1", vf->ours); ok = 0; break; }
            memcpy(sha1, line + 9, 41);
        } else if (strncmp(line, "stock_sha1 ", 11) == 0) {
        } else if (strncmp(line, "count ", 6) == 0) {
            count = strtol(line + 6, NULL, 10);
        } else {
            char *end;
            unsigned long v = strtoul(line, &end, 10);
            if (end == line || *end || v >= vf->nstatic) {
                snprintf(err, errcap, "%s.ids: bad line \"%.40s\"", vf->ours, line); ok = 0; break;
            }
            if (n == cap) {
                uint32_t *m = realloc(ids, (cap *= 2) * sizeof *ids);
                if (!m) { ok = 0; break; }
                ids = m;
            }
            ids[n++] = (uint32_t)v;
        }
    }
    fclose(f);
    if (ok && (!sha1[0] || count < 0 || (size_t)count != n || n == 0)) {
        snprintf(err, errcap, "%s.ids: %s (count %ld, %zu ids)", vf->ours, !sha1[0] ? "no vcs_sha1" : "count mismatch",
                 count, n);
        ok = 0;
    }
    if (ok) {
        qsort(ids, n, sizeof *ids, cmp_u32);
        for (size_t i = 1; i < n; i++)
            if (ids[i] == ids[i - 1]) { snprintf(err, errcap, "%s.ids: duplicate %u", vf->ours, ids[i]); ok = 0; break; }
    }
    if (!ok) { free(ids); return 0; }
    vf->ids = ids; vf->nids = n;
    return 1;
}

/* VCS v6 header (tools/vcs_write.py): version, total combos, dynamic combos, flags, centroid, records, crc. */
static int check_vcs_header(const VcsFile *vf, const wchar_t *path, char *err, size_t errcap) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) { snprintf(err, errcap, "%s.vcs missing", vf->ours); return 0; }
    int32_t h[7] = { 0 };
    size_t got = fread(h, sizeof h[0], 7, f);
    fclose(f);
    uint32_t total = vf->nstatic * vf->ndyn;
    if (got != 7 || h[0] != 6 || (uint32_t)h[2] != vf->ndyn || (uint32_t)h[1] != total) {
        snprintf(err, errcap, "%s.vcs header: version %d, %d dynamic, %u total (want 6, %u, %u)", vf->ours, h[0], h[2],
                 (uint32_t)h[1], vf->ndyn, total);
        return 0;
    }
    return 1;
}

/* Header, .ids and the SHA1 of the .vcs against the .ids. On failure err says why and nothing is kept. */
static int load_file(VcsFile *vf, const wchar_t *dir, char *err, size_t errcap) {
    wchar_t vcs[MAX_PATH], ids[MAX_PATH];
    _snwprintf(vcs, MAX_PATH, L"%ls\\shaders\\fxc\\%hs.vcs", dir, vf->ours); vcs[MAX_PATH - 1] = 0;
    _snwprintf(ids, MAX_PATH, L"%ls\\shaders\\fxc\\%hs.ids", dir, vf->ours); ids[MAX_PATH - 1] = 0;
    char want[41], have[41] = "";
    if (!check_vcs_header(vf, vcs, err, errcap) || !read_ids(vf, ids, want, err, errcap)) return 0;
    if (!sha1_file_w(vcs, have) || strcmp(have, want) != 0) {
        snprintf(err, errcap, "%s.vcs sha1 %.12s != .ids vcs_sha1 %.12s", vf->ours, have[0] ? have : "?", want);
        free(vf->ids); vf->ids = NULL; vf->nids = 0;
        return 0;
    }
    return 1;
}
static int file_present(const wchar_t *dir, const char *name) {
    wchar_t p[MAX_PATH];
    int any = 0;
    static const char *const ext[] = { "vcs", "ids" };
    for (int i = 0; i < 2; i++) {
        _snwprintf(p, MAX_PATH, L"%ls\\shaders\\fxc\\%hs.%hs", dir, name, ext[i]); p[MAX_PATH - 1] = 0;
        any |= GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    }
    return any;
}

static int listed(const VcsFile *vf, uint32_t sid) {
    return vf->nids && bsearch(&sid, vf->ids, vf->nids, sizeof *vf->ids, cmp_u32) != NULL;
}

/* <proxy dir>\..\..\garrysmod, or PHOTON_GAME_DIR. */
static int mod_dir(wchar_t *out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"PHOTON_GAME_DIR", out, cap);
    if (n > 0 && n < cap) return 1;
    HMODULE self = NULL;
    wchar_t p[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)mod_dir, &self)) return 0;
    n = GetModuleFileNameW(self, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(p, L'\\');
    if (!slash) return 0;
    static const wchar_t tail[] = L"..\\..\\garrysmod";
    if ((size_t)(slash + 1 - p) + sizeof tail / sizeof tail[0] > MAX_PATH) return 0;
    wcscpy(slash + 1, tail);
    n = GetFullPathNameW(p, cap, out, NULL);
    return n > 0 && n < cap;
}
int photon_vlg_mod_dir(wchar_t *out, unsigned long cap) { return mod_dir(out, (DWORD)cap); }   /* L3: the cache directory's base */

static void deactivate(const char *why) {
    if (InterlockedExchange(&S.active, 0)) {
        snprintf(S.reason, sizeof S.reason, "off: %s", why);
        photon_log("vlg: lighting switched OFF: %s (frame %llu); no further renames, marked draws now get enable = 0"
                   " (stock look)", why, (unsigned long long)photon_frame_count());
    }
}

/* --- the r_pixelfog / dxlevel gate ------------------------------------------------------------------------------ */
static int in_stock(uintptr_t p) {
    if (!g_stock_base) return 0;
    const IMAGE_NT_HEADERS64 *nt =
        (const IMAGE_NT_HEADERS64 *)(g_stock_base + (uintptr_t)((const IMAGE_DOS_HEADER *)g_stock_base)->e_lfanew);
    return p >= g_stock_base && p < g_stock_base + nt->OptionalHeader.SizeOfImage;
}
/* Stock's r_pixelfog ConVar, if it is exactly what this build defines (else NULL: lighting stays off). */
static void *find_pixelfog(char *err, size_t errcap) {
    char *cv = (char *)(g_stock_base + RVA_CV_PIXELFOG);
    void **vt = *(void ***)cv;
    const char *name = *(const char **)(cv + CV_OFF_NAME);
    if ((uintptr_t)vt != g_stock_base + RVA_CV_VTBL || (uintptr_t)vt[CV_SLOT_GETINT] != g_stock_base + RVA_CV_GETINT ||
        (uintptr_t)name != g_stock_base + RVA_STR_PIXELFOG || strcmp(name, "r_pixelfog") != 0) {
        snprintf(err, errcap, "r_pixelfog ConVar at stock+0x%x not as expected (vtable stock+0x%llx, GetInt stock+0x%llx,"
                 " name stock+0x%llx)", RVA_CV_PIXELFOG, (unsigned long long)((uintptr_t)vt - g_stock_base),
                 in_stock((uintptr_t)vt) ? (unsigned long long)((uintptr_t)vt[CV_SLOT_GETINT] - g_stock_base) : 0ull,
                 (unsigned long long)((uintptr_t)name - g_stock_base));
        return NULL;
    }
    return cv;
}
static int pixelfog_value(void) {
    typedef int (*FnGetInt)(void *);
    return g_cv_pixelfog ? ((FnGetInt)(*(void ***)g_cv_pixelfog)[CV_SLOT_GETINT])(g_cv_pixelfog) : 0;
}
static int dxlevel(void) {
    const char *caps = *(const char *const *)(g_stock_base + RVA_PCAPS);
    return caps ? *(const volatile int *)(caps + CAPS_OFF_DXLEVEL) : 0;
}
/* Renames are allowed only while stock would use pixel fog for ps20b, i.e. never enable D3D fixed-function fog: the
 * ps_3_0 substitute has no FF fog (analysis/14c §1). Read live at every candidate SetVertexShader. */
static volatile LONG g_gate_state = -1, g_gate_logs;
static int fog_gate(void) {
    int pf = pixelfog_value(), dx = dxlevel(), open = pf != 0 && dx >= 90;
    if (InterlockedExchange(&g_gate_state, open) != open && InterlockedIncrement(&g_gate_logs) <= 8)
        photon_log("vlg: renames %s: r_pixelfog %d, dxlevel %d (frame %llu)", open ? "allowed" : "PAUSED (stock names kept)",
                   pf, dx, (unsigned long long)photon_frame_count());
    return open;
}

static void free_pairs(void) {
    for (int k = 0; k < NPAIR; k++) {
        VcsFile *f[2] = { &g_pair[k].vs, &g_pair[k].ps };
        for (int i = 0; i < 2; i++) { free(f[i]->ids); f[i]->ids = NULL; f[i]->nids = 0; }
        g_pair[k].on = 0;
    }
}
/* Each pair is used iff any of its files is installed (and its key, photon_bump / photon_skin, is != 0); an installed
 * pair must then load completely (both .vcs/.ids, sha1, header), else lighting stays off altogether. At least one pair
 * must be used. */
static int load_pairs(const wchar_t *dir, const float v[NKEY], char *err, size_t errcap) {
    int any = 0;
    for (int k = 0; k < NPAIR; k++) {
        Pair *P = &g_pair[k];
        P->on = 0;
        if (!file_present(dir, P->vs.ours) && !file_present(dir, P->ps.ours)) continue;
        if (P->key >= 0 && v[P->key] == 0) {
            photon_log("vlg: %s 0: %s/%s installed but not used", k_keys[P->key].key, P->vs.ours, P->ps.ours);
            continue;
        }
        if (!load_file(&P->vs, dir, err, errcap) || !load_file(&P->ps, dir, err, errcap)) { free_pairs(); return 0; }
        P->on = 1;
        any = 1;
    }
    size_t nv = 0, np = 0;
    for (int k = 0; k < NPAIR; k++)
        if (g_pair[k].on) { nv += g_pair[k].vs.nids; np += g_pair[k].ps.nids; }
    if (nv > NHANDLES || np > NHANDLES) {
        snprintf(err, errcap, ".ids list %zu VS + %zu PS statics, the handle table holds %d each", nv, np, (int)NHANDLES);
        free_pairs();
        return 0;
    }
    if (!any) {
        int n = snprintf(err, errcap, "not installed (no");
        for (int k = 0; k < NPAIR && n > 0 && (size_t)n < errcap; k++)
            n += snprintf(err + n, errcap - (size_t)n, "%s %s/%s", k ? " or" : "", g_pair[k].vs.ours, g_pair[k].ps.ours);
        if (n > 0 && (size_t)n < errcap) snprintf(err + n, errcap - (size_t)n, " .vcs/.ids in use)");
    }
    return any;
}

void photon_vlg_setup(HMODULE stock, int build_ok_and_wrapping) {
    S.wanted = 0;
    S.active = 0;
    g_stock_base = (uintptr_t)stock;
    char e[64];
    wchar_t dir[MAX_PATH];
    char dir8[MAX_PATH * 3] = "?", err[200] = "";
    int have_dir = mod_dir(dir, MAX_PATH);
    if (have_dir) {
        utf8(dir, dir8, sizeof dir8);
        _snwprintf(g_cfg_path, MAX_PATH, L"%ls\\cfg\\photon.cfg", dir);
        g_cfg_path[MAX_PATH - 1] = 0;
        _snwprintf(g_ui_path, MAX_PATH, L"%ls\\data\\photon\\settings.txt", dir);
        _snwprintf(g_pack_path, MAX_PATH, L"%ls\\data\\photon\\packs\\features.txt", dir);
        g_pack_path[MAX_PATH - 1] = 0;
        g_ui_path[MAX_PATH - 1] = 0;
    }
    float v[NKEY];
    char src[NKEY];
    int cfg_read = load_config(v, src);
    if (GetEnvironmentVariableA("PHOTON_TEST_COVERAGE", e, sizeof e) > 0 && e[0] == '1') g_cov_on = 1;
    if (GetEnvironmentVariableA("PHOTON_TEST_ROUTE", e, sizeof e) > 0) {
        g_route_test = atoi(e);
        photon_log("vlg: TEST PHOTON_TEST_ROUTE=%d (%s)", g_route_test, g_route_test == 1 ? "ENGINE draws black" : "log only");
    }
    memcpy(g_cfg, v, sizeof v);
    InterlockedIncrement(&g_cfg_gen);
    g_cfg_exists = cfg_stamp(&g_cfg_mtime);
    if (!stock || !build_ok_and_wrapping) snprintf(err, sizeof err, "pass-through or unknown build");
    else if (GetEnvironmentVariableA("PHOTON_TEST_C32", e, sizeof e) > 0) snprintf(err, sizeof err, "PHOTON_TEST_C32 set");
    else if (PHOTON_VLG_MUTANT && GetEnvironmentVariableA("PHOTON_TEST_VLG", e, sizeof e) == 0)
        snprintf(err, sizeof err, "mutant test build outside the mock host");
    else if (v[K_LIGHTING] == 0) snprintf(err, sizeof err, "photon_lighting 0 (%s)", src[K_LIGHTING] == 'e' ? "environment" : "photon.cfg");
    else if (!have_dir) snprintf(err, sizeof err, "game directory not found");
    else if (!load_pairs(dir, v, err, sizeof err)) {
    } else if (!(g_cv_pixelfog = find_pixelfog(err, sizeof err))) {
        free_pairs();
    } else if (memcmp((const void *)(g_stock_base + RVA_FN_SETVSIDX), k_setvsidx_code, sizeof k_setvsidx_code) != 0) {
        snprintf(err, sizeof err, "SetVertexShaderIndex at stock+0x%x is not the expected store to [stock+0x%x]+%d",
                 RVA_FN_SETVSIDX, RVA_PSHADERMGR, MGR_OFF_VSIDX);
        free_pairs();
    } else g_pmgr = (char *const volatile *)(g_stock_base + RVA_PSHADERMGR);
    if (err[0]) {
        snprintf(S.reason, sizeof S.reason, "off: %s", err);
        photon_log("vlg: lighting OFF: %s (mod dir %s, photon.cfg %s); no vlg hooks", err, dir8, cfg_read ? "read" : "absent");
        return;
    }
    S.nvs = (int)g_pair[PAIR_VLG].vs.nids;
    S.nps = (int)g_pair[PAIR_VLG].ps.nids;
    S.nbvs = (int)g_pair[PAIR_BUMP].vs.nids;
    S.nbps = (int)g_pair[PAIR_BUMP].ps.nids;
    S.nsvs = (int)g_pair[PAIR_SKIN].vs.nids;
    S.nsps = (int)g_pair[PAIR_SKIN].ps.nids;
    S.wanted = 1;
    S.active = 1;
    snprintf(S.reason, sizeof S.reason, "on");
    char pairs[600];
    int pn = 0;
    pairs[0] = 0;
    for (int k = 0; k < NPAIR; k++) {
        const Pair *P = &g_pair[k];
        if (P->on)
            pn += snprintf(pairs + pn, sizeof pairs - (size_t)pn, "%s %s.vcs %zu VS statics (%u .. %u) + %s.vcs %zu PS statics"
                           " (%u .. %u); ", P->tag, P->vs.ours, P->vs.nids, P->vs.ids[0], P->vs.ids[P->vs.nids - 1],
                           P->ps.ours, P->ps.nids, P->ps.ids[0], P->ps.ids[P->ps.nids - 1]);
        else pn += snprintf(pairs + pn, sizeof pairs - (size_t)pn, "%s not used; ", P->tag);
    }
    photon_log("vlg: lighting wanted: %s\\shaders\\fxc: %ssha1 = .ids; r_pixelfog %d, dxlevel %d now; photon.cfg %s%s",
               dir8, pairs, pixelfog_value(), dxlevel(), cfg_read ? "read" : "absent",
               PHOTON_VLG_MUTANT ? "; MUTANT TEST BUILD: pair reverts and the snapshot pairing check are DISABLED" : "");
    log_config("at load", v, src);
}

/* --- light packing ------------------------------------------------------------------------------------------------ */
/* The cone clamps of stock SetLight (FUN_18001e910 +0x183..0x1c5): phi <= pi, theta <= phi - 0.001. */
static void stock_cone_clamp(float theta, float phi, float *t, float *p) {
    if ((double)phi > 3.141592653589793) phi = 3.14159274f;
    float d = theta - phi;
    if ((double)d > -0.001) theta = (float)((double)phi - 0.001);
    *t = theta; *p = phi;
}

static int pack_light(float out[PHOTON_LB_LIGHT_REGS][4], const LightDesc *d, int use_range) {
    float type, ci = 0, co = 0;
    switch (d->type) {
    case 1: type = PHOTON_LB_TYPE_POINT; break;
    case 2: type = PHOTON_LB_TYPE_DIRECTIONAL; break;
    case 3: {
        type = PHOTON_LB_TYPE_SPOT;
        float t, p;
        stock_cone_clamp(d->theta, d->phi, &t, &p);
        ci = cosf(t * 0.5f);          /* stopdot  (CommitVertexShaderLighting: cos(Theta * 0.5)) */
        co = cosf(p * 0.5f);          /* stopdot2 */
        break;
    }
    default: return 0;
    }
    photon_lb_pack_light(out, type, d->color, d->pos, d->dir, d->att0, d->att1, d->att2, use_range ? d->range : 0.0f,
                         d->falloff, ci, co);
    return 1;
}

static void build_block(PhotonLightBlock *b, float origin[3], int snapflags, const float cfg[NKEY], const float occ[4]) {
    LightDesc L[4];
    float cube[6][4];
    light_read_check();
    memcpy(L, g_lights, sizeof L);
    memcpy(cube, g_cube, sizeof cube);
    memcpy(origin, g_origin, sizeof g_origin);
    if (photon_p6_suppressed()) {   /* P6: the engine set no lights and no lighting origin for this draw; those copies are stale */
        for (int i = 0; i < 4; i++) L[i].type = 0;
        photon_p6_origin(origin);
        /* the sun the engine's skipped setup would have traced and set (p6sun.c): slot 0 is always free here, since every engine light was
         * dropped and the grid's lights travel in the grid texture, not in this block; lights_apply keeps directional lights at any grid weight */
        photon_p6_sun(&L[0]);   /* counted in p6.c (sunlit) */
    }
    memset(b, 0, sizeof *b);
    int n = 0;
    for (int i = 0; i < 4; i++) n += pack_light(b->lights[n], &L[i], cfg[K_RANGE] > 0.5f);
    float hl = cfg[K_HALFLAMBERT] < 0 ? ((snapflags & PHOTON_VLG_SNAP_HALFLAMBERT) ? 1.0f : 0.0f) : cfg[K_HALFLAMBERT];
    b->flags[0] = 1.0f; b->flags[1] = (float)n; b->flags[2] = hl; b->flags[3] = 0.0f;
    memcpy(b->ambient, cube, sizeof cube);
    b->tuning[0] = cfg[K_LIGHT_SCALE]; b->tuning[1] = cfg[K_AMBIENT_SCALE];
    b->tuning[2] = cfg[K_AMBIENT_FLOOR]; b->tuning[3] = cfg[K_DEBUG];
    if (cfg[K_DEBUG] == 0 && photon_ptrt_models() && !photon_shadows_main_done() && photon_shadows_scene_target()) {   /* PT-RT: the surface colour only (no lamps; ambient max(a x 0, 1) = 1, the volume too); the composite lights it */
        b->flags[1] = b->flags[3] = 0.0f;
        b->tuning[0] = 0.0f; b->tuning[1] = 0.0f; b->tuning[2] = 1.0f; b->tuning[3] = 0.0f;
    }
    if (cfg[K_DEBUG] > 2.5f) {   /* P5 photon_debug 3: the diffuse view of the grid's count only (c64.x, vol_apply): no engine light, no ambient */
        b->flags[1] = b->flags[3] = 0.0f;
        b->tuning[0] = 1.0f; b->tuning[1] = 0.0f; b->tuning[2] = 0.0f; b->tuning[3] = 1.0f;
    }
    memcpy(b->envocc, occ, sizeof b->envocc);
}

/* c56 for the draws of ours: the occlusion from the config, off (c56.x = 0) while photon_lighting is 0 */
static void occ_value(float occ[4], const float cfg[NKEY]) {
    photon_lb_envocc(occ, cfg[K_LIGHTING] != 0 ? cfg[K_ENVOCC] : 0.0f, cfg[K_ENVOCC_MIN], cfg[K_ENVOCC_MAX],
                     cfg[K_ENVOCC_CAL]);
}
/* c56 only (draws of ours without the light block): written when the device may hold another value */
static void occ_set(const float v[4]) {
    if (g_occ_valid && memcmp(g_occ_dev, v, sizeof g_occ_dev) == 0) return;
    if (photon_d3d_set_ps_const(PHOTON_LB_ENVOCC, v, 1) == S_OK) {
        memcpy(g_occ_dev, v, sizeof g_occ_dev); g_occ_valid = 1; INC(occ_writes);
    } else INC(write_fail);
}
static void occ_off(void) {
    static const float off[4] = { 0, 0, 0, 1 };
    occ_set(off);
}

static void flag_off(void) {
    if (!g_dev_on) return;
    static const float z[4] = { 0, 0, 0, 0 };
    if (photon_d3d_set_ps_const(PHOTON_LB_FLAGS, z, 1) == S_OK) { g_dev_on = 0; INC(flag_off_writes); }
    else INC(write_fail);
}

/* --- P3c: call-order routing (analysis/15d section 3) -------------------------------------------------------------
 * Engine and Lua/panel cubes share one transport (slot 130), and their values prove nothing. Only the order differs:
 * the engine sends SetAmbientLight(0,0,0) (129) right after the cube, the static-prop array sends the origin (128) right
 * after the cube once its setup latched, and overrides do neither. An engine update that changes this falls back to
 * ROUTE_OVERRIDE, which is stock lighting. Observed only for now: nothing but PHOTON_TEST_ROUTE acts on it. State is
 * global, not thread-local, because a queue flush can replay on another thread. */
enum { ROUTE_OVERRIDE = 0, ROUTE_ENGINE = 1 };
static volatile int g_route, g_route_pending, g_route_latch;
static volatile int g_route_array;   /* the ENGINE route came from the static-prop array signature (cube, then origin): static props stay on the cube (analysis/15e) */
enum { SEQ_CHARS = 72, SEQ_TABLE = 192 };
typedef struct { char seq[SEQ_CHARS]; int route, kind, lit; long long n; } SeqRec;
static char g_seq[SEQ_CHARS];   /* events since the last RenderPass (test mode only) */
static int g_seq_len;
static SeqRec g_seqs[SEQ_TABLE];
static int g_nseqs;
static long long g_seq_dropped;

static void seq_add(const char *t) {
    if (!g_route_test) return;
    size_t n = strlen(t);
    if (t[0] == 'L' && g_seq_len >= 2 && g_seq[g_seq_len - 2] == 'L') return;   /* n lights count as one */
    if ((size_t)g_seq_len + n + 2 > sizeof g_seq) return;
    memcpy(g_seq + g_seq_len, t, n);
    g_seq_len += (int)n;
    g_seq[g_seq_len++] = ' ';
    g_seq[g_seq_len] = 0;
}
static void seq_note(int kind, int lit) {
    if (!g_route_test) return;
    for (int i = 0; i < g_nseqs; i++) {
        SeqRec *r = &g_seqs[i];
        if (r->route == g_route && r->kind == kind && r->lit == lit && strcmp(r->seq, g_seq) == 0) { r->n++; return; }
    }
    if (g_nseqs == SEQ_TABLE) { g_seq_dropped++; return; }
    SeqRec *r = &g_seqs[g_nseqs];
    memcpy(r->seq, g_seq, sizeof r->seq);
    r->route = g_route; r->kind = kind; r->lit = lit; r->n = 1;
    g_nseqs++;
}
/* The event after a cube that is neither 129 nor 128 ends the signature: an override */
static void route_settle(void) {
    if (g_route_pending) g_route_latch = 0;
    g_route_pending = 0;
}

/* P3h (analysis/15d sections 8.1, 8.2), photon_ambient 1 only. A lit draw on the engine route inside the valid box adds f * volume(n) in the shader (c64.y = f) and
 * keeps in the cube only what the volume does not hold, so that the block compared and sent below already carries it. Anything else: f = 0, today's path. */
/* P5 (analysis/18_p5_lights.md): with the grid on this draw (weight g = f), the cube keeps only (1 - f) C (the lights it folded are grid lights now), the
 * engine's point and spot lights are scaled by 1 - g (dropped at g = 1) and its directional lights, the sun the engine traced for this model, stay.
 * P5b: the cube gets g F, F the part of the grid's lights not drawn per pixel (the split's far part and what a full cell had no room for), folded at
 * the origin as the engine folds (lights.h photon_lg_fold). */
static void lights_apply(PhotonLightBlock *b, float f, float g, const float origin[3]) {
    float fold[6][3];
    photon_lg_fold(origin, fold);
    for (int i = 0; i < PHOTON_LB_AMBIENT_N; i++)
        for (int k = 0; k < 4; k++) b->ambient[i][k] = b->ambient[i][k] * (1.0f - f) + (k < 3 ? g * fold[i][k] : 0.0f);
    int n = (int)b->flags[1], m = 0;
    for (int i = 0; i < n; i++) {
        int dir = b->lights[i][1][3] > 1.5f;
        if (!dir && g >= 1.0f) continue;
        if (m != i) memcpy(b->lights[m], b->lights[i], sizeof b->lights[m]);
        if (!dir) for (int k = 0; k < 3; k++) b->lights[m][1][k] *= 1.0f - g;
        m++;
    }
    for (int i = m; i < n; i++) memset(b->lights[i], 0, sizeof b->lights[i]);
    b->flags[1] = (float)m;
}

static void vol_apply(PhotonLightBlock *b, PhotonWrap *w, const float origin[3], const float cfg[NKEY], int kind) {
    int why = PHOTON_AMB_OFF_CONFIG;
    float f = 0.0f, g = 0.0f;
    float trace_c[6][4];
    int trace = photon_amb_trace_on();
    if (trace) memcpy(trace_c, b->ambient, sizeof trace_c);
    if (!ambient_of(cfg) || g_route_test == 1) why = PHOTON_AMB_OFF_CONFIG;
    else if (g_route != ROUTE_ENGINE) why = PHOTON_AMB_OFF_ROUTE;
    else if (g_route_array) { why = PHOTON_AMB_OFF_ROUTE; INC(vol_off_array); }
    else f = photon_amb_weight(w->real, origin, &why);
    if (f > 0.0f) {
        float c[4][4];
        photon_amb_constants(c);
        if (!g_vol_c_valid || memcmp(c, g_vol_c_dev, sizeof c) != 0) {
            if (photon_d3d_set_ps_const(PHOTON_LB_VOL_A, &c[0][0], 4) == S_OK) {
                memcpy(g_vol_c_dev, c, sizeof c);
                g_vol_c_valid = 1;
                INC(vol_const_writes);
            } else { INC(write_fail); g_vol_c_valid = 0; f = 0.0f; why = PHOTON_AMB_OFF_REST; }
        }
    }
    if (f > 0.0f && photon_lg_on && cfg[K_LIGHTS] != 0) {
        g = photon_lg_weight(w->real, kind == PAIR_BUMP, f);
        if (g > 0.0f) {
            float c63[4];
            photon_lg_constants(c63);
            if (!g_lg_c_valid || memcmp(c63, g_lg_c_dev, sizeof c63) != 0) {
                if (photon_d3d_set_ps_const(PHOTON_LB_GRID, c63, 1) == S_OK) {
                    memcpy(g_lg_c_dev, c63, sizeof c63);
                    g_lg_c_valid = 1;
                    INC(lg_const_writes);
                } else { INC(write_fail); g_lg_c_valid = 0; g = 0.0f; }
            }
        }
    }
    float dbg = cfg[K_DEBUG] > 2.5f ? 1.0f : 0.0f;   /* photon_debug 3: the grid's count (build_block set the rest of the view) */
    const float v[4] = { dbg, f, g, 0.0f };          /* c64.w stays 0: photon_lightblock.h */
    if (!g_vol_f_valid || memcmp(v, g_c64_dev, sizeof v) != 0) {
        if (photon_d3d_set_ps_const(PHOTON_LB_DRAW, v, 1) == S_OK) {
            memcpy(g_c64_dev, v, sizeof v);
            g_vol_f_valid = 1;
            INC(vol_weight_writes);
        } else { INC(write_fail); g_vol_f_valid = 0; f = g = 0.0f; why = PHOTON_AMB_OFF_REST; }
    }
    if (f > 0.0f) {
        if (g > 0.0f) { lights_apply(b, f, g, origin); INC(lg_draws[kind]); }
        else photon_amb_fold(origin, f, b->ambient);
        g_vol_draw = 1;
        INC(vol_draws);
    } else INC(vol_off[why]);
    if (trace) photon_amb_trace(origin, f, f > 0.0f ? -1 : why, (const float (*)[4])trace_c, (const float (*)[4])b->ambient);
}

/* --- T4.5 stage 2b: relighting replayed copies (p6.c, PHOTON_BATCH=replay) ---------------------------------------
 * A replayed copy skips the engine and so RenderPass: its group's recorded c32..c64 hold the first copy's sun, ambient fold and weights.
 * The block of the last lit suppressed pass is kept here before vol_apply (in P6 its lights are only the sun, slot 0), and relight
 * redoes the origin-dependent part (the sun, the volume and grid weights and folds) at the copy's origin, as pre_draw and vol_apply do.
 * Draw thread only, like pre_draw. */
typedef struct { int ok, kind, vol, amb, lg, range; const void *real; float dbg; PhotonLightBlock base; } Relight;
static Relight g_rl;

static void relight_note(const PhotonLightBlock *b, PhotonWrap *w, int kind, const float cfg[NKEY]) {
    g_rl.ok = 1; g_rl.kind = kind; g_rl.real = w->real; g_rl.base = *b;
    g_rl.vol = photon_amb_on;   /* vol_apply runs and writes c64 */
    g_rl.amb = photon_amb_on && ambient_of(cfg) && g_route_test != 1;
    g_rl.lg = photon_lg_on && cfg[K_LIGHTS] != 0;
    g_rl.range = cfg[K_RANGE] > 0.5f;
    g_rl.dbg = cfg[K_DEBUG] > 2.5f ? 1.0f : 0.0f;
}
size_t photon_vlg_relight_size(void) { return sizeof(Relight); }
void photon_vlg_relight_take(void *ctx) { memcpy(ctx, &g_rl, sizeof g_rl); g_rl.ok = 0; }
int photon_vlg_relight(const void *ctx, const float origin[3], const void *sun, float psc[65][4]) {
    const Relight *r = ctx;
    if (!r->ok) return 0;
    PhotonLightBlock b = r->base;
    memset(b.lights, 0, sizeof b.lights);
    int n = 0;
    if (sun && r->dbg == 0.0f) { LightDesc d; memcpy(&d, sun, sizeof d); n = pack_light(b.lights[0], &d, r->range); }
    b.flags[1] = (float)n;   /* photon_debug 3 (build_block): no engine light */
    float f = 0.0f, g = 0.0f;
    int why;
    if (r->amb) f = photon_amb_weight(r->real, origin, &why);
    if (f > 0.0f) {
        photon_amb_constants((float (*)[4])psc[PHOTON_LB_VOL_A]);
        if (r->lg) g = photon_lg_weight(r->real, r->kind == PAIR_BUMP, f);
        if (g > 0.0f) { photon_lg_constants(psc[PHOTON_LB_GRID]); lights_apply(&b, f, g, origin); }
        else photon_amb_fold(origin, f, b.ambient);
    }
    if (r->vol) { psc[PHOTON_LB_DRAW][0] = r->dbg; psc[PHOTON_LB_DRAW][1] = f; psc[PHOTON_LB_DRAW][2] = g; psc[PHOTON_LB_DRAW][3] = 0.0f; }
    memcpy(psc[PHOTON_LB_FIRST], &b.flags[0], offsetof(PhotonLightBlock, envocc));   /* c32..c55; c56 (occlusion) is not per origin */
    INC(relit);
    if (n) INC(relit_sun);
    return 1;
}

static __thread int t_model_pass, t_model_skinned;   /* T4.2: this RenderPass draws a model with one of our pairs (VLG / bump / skin), skinned */
int photon_vlg_model_pass(int *skinned) { if (skinned) *skinned = t_model_skinned; return t_model_pass; }
static void pre_draw(PhotonWrap *w) {
    int f = g_snap[t_cur_snap & 0xffff];
    t_model_pass = (f & PHOTON_VLG_SNAP_OURS) != 0; t_model_skinned = 0;
    if (photon_p6_suppressed()) g_rl.ok = 0;
    g_vol_wants = (f & PHOTON_VLG_SNAP_VOLTEST) != 0;
    if (!(f & PHOTON_VLG_SNAP_OURS)) { flag_off(); photon_p6_pass(P6_PASS_STOCK, -1); return; }
    INC(rp_ours);
    int p6 = photon_p6_suppressed();
    if (p6) { g_route = ROUTE_ENGINE; g_route_array = 0; g_route_pending = 0; }   /* P6: the engine sent no lighting state for this draw */
    int kind = (f & PHOTON_VLG_SNAP_BUMP) ? PAIR_BUMP : (f & PHOTON_VLG_SNAP_SKIN) ? PAIR_SKIN : PAIR_VLG;
    if (kind == PAIR_BUMP) INC(rp_ours_bump);
    else if (kind == PAIR_SKIN) INC(rp_ours_skin);
    /* D3D fixed-function fog on for a draw of our pair: r_pixelfog was set to 0 after these materials were
     * snapshotted (the gate only acts at snapshot time). Our ps_3_0 draws without fog until the materials are
     * re-snapshotted (mat_reloadallmaterials); counted and logged, nothing else to do at draw time. */
    if (*(volatile const char *)((const char *)w->real + OFF_API_FFFOG)) {
        if (INC(ours_ff_fog) <= 4)
            photon_log("vlg: WARNING: a draw of our SM3 pair with D3D fixed-function fog on (r_pixelfog 0 after the"
                       " materials loaded?): it draws without fog; mat_reloadallmaterials restores the stock shaders");
    }
    if (g_draw_gen != g_cfg_gen) {
        AcquireSRWLockShared(&g_lock);
        g_draw_gen = g_cfg_gen;
        memcpy(g_draw_cfg, g_cfg, sizeof g_draw_cfg);
        ReleaseSRWLockShared(&g_lock);
        occ_value(g_draw_occ, g_draw_cfg);
    }
    const float *cfg = g_draw_cfg, *occ = g_draw_occ;
    int lit = 0;
    if (kind == PAIR_BUMP) {
        /* the bump PS lights per pixel in every stored dynamic from the engine's lights + cube (the bump VS has no
         * lighting-mode dynamics), so the Photon path replaces it whenever lighting is on */
        lit = 1;
    } else {
        /* the index stock binds now: slot 40 on this thread since the last RenderPass, else stock's shader manager
         * (the command-buffer path VLG vs20 uses; mgr+8 is the dynamic index of whichever VS is drawn) */
        int idx = t_dyn_idx, fresh = t_vs_fresh;
        const char *mgr = (!fresh && g_pmgr) ? *g_pmgr : NULL;
        if (mgr) { idx = *(const volatile int *)(mgr + MGR_OFF_VSIDX); fresh = 1; INC(vsidx_from_mgr); }
        t_p6_idx = idx;
        int unlit = kind == PAIR_SKIN
            /* skin: the PS lights per pixel in every dynamic (as bump); only Hammer's LIGHTING_PREVIEW stays stock */
            ? idx < 0 || idx >= SVS_NDYN || (idx & SVSD_LIGHTING_PREVIEW)
            : idx < 0 || idx >= VS_NDYN || !(idx & VSD_DYNAMIC_LIGHT) ||
              (idx & (VSD_STATIC_LIGHT | VSD_STATIC_LIGHT_LIGHTMAP | VSD_LIGHTING_PREVIEW));
        /* skinned (SKINNING: vlg vs20 x32, skin_vs20 x4): the bone palette is per copy, so P6 replay must not take this group */
        if (fresh && idx >= 0 && (kind == PAIR_SKIN ? (idx & 4) : (idx & 32))) { photon_p6_skinned(); t_model_skinned = 1; }
        if (!fresh) INC(ours_no_vsidx);
        else if (unlit) { INC(ours_unlit); if (kind == PAIR_SKIN) INC(ours_skin_unlit); }
        else lit = 1;
    }
    seq_note(kind, lit && cfg[K_LIGHTING] != 0);
    /* P6: a pass counts as Photon-lit when nothing of the engine's light setup is needed: our pair, lit, engine route, and both the ambient
     * volume and the light grid on (the volume replaces the cube, the grid the local lights; the sun is not supplied yet) */
    photon_p6_pass(cfg[K_LIGHTING] == 0 || !photon_amb_on || !photon_lg_on ? P6_PASS_OFF : !lit ? P6_PASS_UNLIT :
                   g_route != ROUTE_ENGINE || g_route_array ? P6_PASS_ROUTE : P6_PASS_LIT, t_p6_idx);
    if (!lit || cfg[K_LIGHTING] == 0) {
        /* stock lighting (c32.x = 0), but the P1 occlusion still applies (C1: static props, SLL, VHV) */
        flag_off();
        occ_set(occ);
        return;
    }
    PhotonLightBlock b;
    float origin[3];
    build_block(&b, origin, f, cfg, occ);
    if (p6) INC(p6_draws);
    if (g_route == ROUTE_ENGINE) { INC(route_engine[kind]); g_vol_wants = 1; } else INC(route_override[kind]);
    if (g_route_test == 1 && g_route == ROUTE_ENGINE) {
        memset(b.ambient, 0, sizeof b.ambient);
        b.tuning[0] = 0; b.tuning[2] = 0;   /* light scale, ambient floor */
    }
    if (p6) relight_note(&b, w, kind, cfg);
    if (photon_amb_on) vol_apply(&b, w, origin, cfg, kind);
    if (g_blk_valid && memcmp(&b, &g_blk_dev, offsetof(PhotonLightBlock, envocc)) == 0) {
        INC(upload_skips);
        if (!g_dev_on) {
            if (photon_d3d_set_ps_const(PHOTON_LB_FLAGS, b.flags, 1) == S_OK) { g_dev_on = 1; INC(flag_on_writes); }
            else { INC(write_fail); g_blk_valid = 0; }
        }
        occ_set(occ);
        return;
    }
    if (photon_d3d_set_ps_const(PHOTON_LB_FIRST, &b.flags[0], PHOTON_LB_COUNT) == S_OK) {
        g_dev_on = 1; INC(uploads);
        memcpy(g_occ_dev, occ, sizeof g_occ_dev); g_occ_valid = 1;
        g_blk_dev = b; g_blk_valid = 1;
    } else { INC(write_fail); g_occ_valid = 0; g_blk_valid = 0; }
}

/* --- LightDesc_t layout check against stock's own D3DLIGHT9 conversion -------------------------------------------- */
static int same(float a, float b) { uint32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4); return x == y; }

static void check_layout(const void *real, int i, const LightDesc *d) {
    const D3DLIGHT9 *r = (const D3DLIGHT9 *)((const char *)real + OFF_API_D3DLIGHT + i * D3DLIGHT_STRIDE);
    D3DLIGHTTYPE t = d->type == 1 ? D3DLIGHT_POINT : d->type == 2 ? D3DLIGHT_DIRECTIONAL : D3DLIGHT_SPOT;
    float th, ph;
    stock_cone_clamp(d->theta, d->phi, &th, &ph);
    int ok = r->Type == t && same(r->Diffuse.r, d->color[0]) && same(r->Diffuse.g, d->color[1]) &&
             same(r->Diffuse.b, d->color[2]) && same(r->Position.x, d->pos[0]) && same(r->Position.y, d->pos[1]) &&
             same(r->Position.z, d->pos[2]) && same(r->Direction.x, d->dir[0]) && same(r->Direction.y, d->dir[1]) &&
             same(r->Direction.z, d->dir[2]) && (t == D3DLIGHT_DIRECTIONAL || same(r->Range, d->range)) &&
             same(r->Falloff, d->falloff) && same(r->Attenuation0, d->att0) && same(r->Attenuation1, d->att1) &&
             same(r->Attenuation2, d->att2) && same(r->Theta, th) && same(r->Phi, ph);
    if (ok) { INC(layout_ok); return; }
    LONG64 n = INC(layout_mismatch);
    if (n <= 4)
        photon_log("vlg: LightDesc_t layout MISMATCH light %d: ours type %d col %g %g %g pos %g %g %g falloff %g att %g %g %g"
                   " theta %g phi %g | stock D3DLIGHT9 type %d col %g %g %g pos %g %g %g falloff %g att %g %g %g theta %g"
                   " phi %g", i, d->type, d->color[0], d->color[1], d->color[2], d->pos[0], d->pos[1], d->pos[2],
                   d->falloff, d->att0, d->att1, d->att2, th, ph, (int)r->Type, r->Diffuse.r, r->Diffuse.g, r->Diffuse.b,
                   r->Position.x, r->Position.y, r->Position.z, r->Falloff, r->Attenuation0, r->Attenuation1,
                   r->Attenuation2, r->Theta, r->Phi);
    deactivate("LightDesc_t layout differs from stock's D3DLIGHT9 conversion");
}

/* --- hooks ---------------------------------------------------------------------------------------------------- */
typedef void (*Fn3)(void *, uintptr_t, uintptr_t, uintptr_t);
typedef void (*FnNameIdx)(void *, const char *, int);
typedef uintptr_t (*FnRet)(void *);
typedef void (*FnInt)(void *, int);
typedef void (*FnLight)(void *, int, const LightDesc *);
typedef void (*FnPtr)(void *, const void *);
typedef int (*FnGetInt)(void *);

static void send_vs(PhotonWrap *w, const char *name, int idx) { ((FnNameIdx)g_sh_prev[SH_SETVS])(w, name, idx); }
static void send_ps(PhotonWrap *w, const char *name, int idx) { ((FnNameIdx)g_sh_prev[SH_SETPS])(w, name, idx); }
static uint64_t sh_handle(const PhotonWrap *w, int off) { return *(volatile const uint64_t *)((const char *)w->real + off); }
static int bad_handle(uint64_t h) { return h == 0xffffffffull || h == ~0ull; }

static void sh_setdefault(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    /* stock SetDefaultState sets the shadow's VS and PS to (NULL, 0) through its own vtable (FUN_18002a2e0 +0xb8/+0xc0) */
    t_vs_ours = t_ps_ours = t_vs_hl = t_ps_voltest = t_ps_water = t_vs_pending = 0;
    t_vs_name = t_ps_name = NULL;
    ((Fn3)g_sh_prev[SH_SETDEFAULT])(w, a, b, c);
}

/* Distinct shader names seen at ShaderShadow slots 23/24 while lighting is wanted (snapshot path only, bounded), for
 * the summary: which VLG variants (vs20/vs30, ps20b/ps30) the game actually selects. */
enum { NNAMES = 96 };
static struct { char name[56]; int ps; LONG64 n; } g_names[NNAMES];
static int g_nnames;
static SRWLOCK g_names_lock = SRWLOCK_INIT;
static void note_name(const char *name, int ps) {
    if (!name) return;
    AcquireSRWLockExclusive(&g_names_lock);
    int i;
    for (i = 0; i < g_nnames; i++) if (g_names[i].ps == ps && strncmp(g_names[i].name, name, sizeof g_names[i].name - 1) == 0) break;
    if (i == g_nnames && g_nnames < NNAMES) {
        snprintf(g_names[i].name, sizeof g_names[i].name, "%s", name);
        g_names[i].ps = ps;
        g_nnames++;
    }
    if (i < g_nnames) g_names[i].n++;
    ReleaseSRWLockExclusive(&g_names_lock);
}
static void dump_names(void) {
    char line[1024];
    int n = 0;
    AcquireSRWLockShared(&g_names_lock);
    for (int i = 0; i < g_nnames && n < (int)sizeof line - 80; i++)
        if (strstr(g_names[i].name, "vertexlit") || strstr(g_names[i].name, "VertexLit") || strstr(g_names[i].name, "photon_") ||
            strstr(g_names[i].name, "decalmodulate") || strncmp(g_names[i].name, "skin_", 5) == 0 || strstr(g_names[i].name, "lightmapped") ||
            strstr(g_names[i].name, "worldvertex"))
            n += snprintf(line + n, sizeof line - (size_t)n, " %s:%s=%lld", g_names[i].ps ? "PS" : "VS", g_names[i].name,
                          (long long)g_names[i].n);
    int total = g_nnames;
    ReleaseSRWLockShared(&g_names_lock);
    photon_log("vlg: shader names at ShaderShadow 23/24 (%d distinct%s), vertexlit/decal/skin/world ones:%s", total,
               total >= NNAMES ? ", table full" : "", n ? line : " none");
}

/* PAIRING RULE (analysis/14 "Erratum 2 decision" A2): our vs_3_0 must never be snapshotted with a stock ps_2_x, and our
 * ps_3_0 never with a stock vs_2_0. The VS arrives first (slot 23), so it is renamed speculatively; whatever is not
 * renamed afterwards sends the other one back to its stock name (the caller's own pointer and index). */
static volatile LONG g_revert_logs;
static void revert_vs(PhotonWrap *w, const char *why) {
    if (!t_vs_ours) return;
    int nd = (int)g_pair[t_vs_ours - 1].vs.ndyn;
    t_vs_ours = 0;
    INC(pair_reverts);
    if (InterlockedIncrement(&g_revert_logs) <= 8)
        photon_log("vlg: pair revert: VS back to %s static %d (%s)", t_vs_name ? t_vs_name : "(null)", t_vs_sidx / nd, why);
    if (!PHOTON_VLG_MUTANT) send_vs(w, t_vs_name, t_vs_sidx);
}
static void revert_ps(PhotonWrap *w, const char *why) {
    if (!t_ps_ours) return;
    int nd = (int)g_pair[t_ps_ours - 1].ps.ndyn;
    t_ps_ours = 0;
    INC(pair_reverts);
    if (InterlockedIncrement(&g_revert_logs) <= 8)
        photon_log("vlg: pair revert: PS back to %s static %d (%s)", t_ps_name ? t_ps_name : "(null)", t_ps_sidx / nd, why);
    if (!PHOTON_VLG_MUTANT) send_ps(w, t_ps_name, t_ps_sidx);
}

static volatile LONG g_vs_logs, g_rename_logs;
/* PHOTON_TEST_COVERAGE=1 (P3c2): every static the game asks of our pairs at snapshot time, built or not, so that which combos
 * are worth compiling comes from real content (tools/p3_coverage.py reads the exit log). Snapshot path only. */
enum { COV_SLOTS = 16384 };
typedef struct { int used, pair, ps, st, listed; long long n; } CovRec;
static CovRec g_cov[COV_SLOTS];
static SRWLOCK g_cov_lock = SRWLOCK_INIT;
static long long g_cov_full;

static void cov_note(int kind, int ps, int idx, int nd, const VcsFile *vf) {
    if (!g_cov_on || idx < 0 || idx % nd) return;
    int st = idx / nd;
    AcquireSRWLockExclusive(&g_cov_lock);
    unsigned h = ((unsigned)st * 2654435761u + (unsigned)kind * 31u + (unsigned)ps) % COV_SLOTS;
    for (int probe = 0;; probe++, h = (h + 1) % COV_SLOTS) {
        CovRec *r = &g_cov[h];
        if (probe == COV_SLOTS) { g_cov_full++; break; }
        if (!r->used) { r->used = 1; r->pair = kind - 1; r->ps = ps; r->st = st; r->listed = vf && listed(vf, (uint32_t)st); r->n = 1; break; }
        if (r->pair == kind - 1 && r->ps == ps && r->st == st) { r->n++; break; }
    }
    ReleaseSRWLockExclusive(&g_cov_lock);
}

/* L2 (analysis/36): the world shaders' statics too, before their pair exists (kind COV_LMG; LightmappedGeneric 256 x 8 / 9437184 x 96) */
enum { COV_LMG = 4 };
static void cov_world(const char *name, int ps, int idx) {
    if (!g_cov_on || !name) return;
    if (!ps && strcmp(name, "lightmappedgeneric_vs20") == 0) cov_note(COV_LMG, 0, idx, 8, NULL);
    else if (ps && strcmp(name, "lightmappedgeneric_ps20b") == 0) cov_note(COV_LMG, 1, idx, 96, NULL);
}
static void sh_setvs(void *self, const char *name, int idx) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SH_SETVS, -1)) { ((FnNameIdx)w->cls->real_vtbl[SH_SETVS])(w->real, name, idx); return; }   /* T4.6: stock's own SetDefaultState */
    g_sh_obj = w;
    note_name(name, 0);
    cov_world(name, 0, idx);
    int ours = 0, hl = 0, pend = 0, kind = S.active && g_api_ready && name ? vs_kind(name) : 0;
    if (kind) {
        const VcsFile *vf = &g_pair[kind - 1].vs;
        int nd = (int)vf->ndyn;
        INC(vs_requests);
        cov_note(kind, 0, idx, nd, vf);
        int ok = idx >= 0 && idx % nd == 0 && listed(vf, (uint32_t)idx / (uint32_t)nd);
        if (ok && !fog_gate()) { INC(fog_blocks); ok = 0; }
        hl = ok && kind == PAIR_VLG + 1 && ((idx / VS_NDYN) & VS_ST_HALFLAMBERT) != 0;
        if (InterlockedIncrement(&g_vs_logs) <= 6)
            photon_log("vlg: SetVertexShader(%s, %d): static %d%s -> %s", name, idx, idx / nd,
                       idx % nd ? " (NOT a multiple of the dynamic count)" : "", ok ? vf->ours : "stock");
        if (ok && t_ps_ours != kind && !PHOTON_VLG_MUTANT_EAGER) {
            /* deferred: the stock VS now, ours only when the PS pairs (sh_setps), so a VS whose PS does not pair is never created
             * (each was a load and, under DXVK, a pipeline compile at map load: analysis/S1c) */
            pend = kind;
        } else if (ok) {
            send_vs(w, vf->ours, idx);
            if (bad_handle(sh_handle(w, OFF_SH_VSHANDLE))) {
                char why[96];
                INC(load_fallbacks);
                photon_log("vlg: shader manager returned the invalid handle for %s static %d: re-sent as %s", vf->ours,
                           idx / nd, name);
                snprintf(why, sizeof why, "%s failed to load", vf->ours);
                deactivate(why);
            } else {
                handle_add(g_hv, &g_nhv, sh_handle(w, OFF_SH_VSHANDLE), idx, kind);
                INC(vs_renames);
                if (kind == PAIR_BUMP + 1) INC(bump_vs_renames);
                else if (kind == PAIR_SKIN + 1) INC(skin_vs_renames);
                ours = kind;
            }
        }
    }
    if (!ours) send_vs(w, name, idx);
    t_vs_ours = ours; t_vs_pending = pend; t_vs_hl = hl; t_vs_name = name; t_vs_sidx = idx;
    if (t_ps_ours && t_ps_ours != ours) revert_ps(w, ours ? "the VS was replaced by another pair's" : "the VS was replaced by a stock one");
}

static void sh_setps(void *self, const char *name, int idx) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SH_SETPS, -1)) { ((FnNameIdx)w->cls->real_vtbl[SH_SETPS])(w->real, name, idx); return; }
    g_sh_obj = w;
    note_name(name, 1);
    cov_world(name, 1, idx);
    t_ps_voltest = photon_vol_on && name && strcmp(name, "photon_debugvol_ps30") == 0;
    t_ps_water = name && strncmp(name, "water", 5) == 0;
    t_ps_ours = 0;                                   /* the shadow's PS is replaced either way */
    int pilot = 0, kind = S.active && g_api_ready && name ? ps_kind(name) : 0;
    const Pair *P = kind ? &g_pair[kind - 1] : NULL;
    if (P) {
        int nd = (int)P->ps.ndyn;
        INC(ps_requests);
        cov_note(kind, 1, idx, nd, &P->ps);
        pilot = idx >= 0 && idx % nd == 0 && listed(&P->ps, (uint32_t)idx / (uint32_t)nd);
        if (pilot && t_vs_ours != kind && t_vs_pending != kind) { INC(vs_rejects); pilot = 0; }   /* the pass VS is not this pair's VS (ours or pending) */
    }
    if (pilot && t_vs_pending == kind) {   /* the deferred VS rename, now that its PS pairs */
        const VcsFile *vf = &P->vs;
        int vnd = (int)vf->ndyn;
        t_vs_pending = 0;
        send_vs(w, vf->ours, t_vs_sidx);
        if (bad_handle(sh_handle(w, OFF_SH_VSHANDLE))) {
            char why[96];
            INC(load_fallbacks);
            photon_log("vlg: shader manager returned the invalid handle for %s static %d: re-sent as %s", vf->ours, t_vs_sidx / vnd, t_vs_name);
            snprintf(why, sizeof why, "%s failed to load", vf->ours);
            deactivate(why);
            send_vs(w, t_vs_name, t_vs_sidx);
            pilot = 0;
        } else {
            handle_add(g_hv, &g_nhv, sh_handle(w, OFF_SH_VSHANDLE), t_vs_sidx, kind);
            INC(vs_renames);
            if (kind == PAIR_BUMP + 1) INC(bump_vs_renames);
            else if (kind == PAIR_SKIN + 1) INC(skin_vs_renames);
            t_vs_ours = kind;
        }
    }
    t_vs_pending = 0;
    if (pilot) {
        int nd = (int)P->ps.ndyn;
        send_ps(w, P->ps.ours, idx);
        if (bad_handle(sh_handle(w, OFF_SH_PSHANDLE))) {
            char why[96];
            INC(load_fallbacks);
            photon_log("vlg: shader manager returned the invalid handle for %s static %d: re-sent as %s", P->ps.ours,
                       idx / nd, name);
            snprintf(why, sizeof why, "%s failed to load", P->ps.ours);
            deactivate(why);
            send_ps(w, name, idx);
            revert_vs(w, "our PS failed to load");
            return;
        }
        handle_add(g_hp, &g_nhp, sh_handle(w, OFF_SH_PSHANDLE), idx, kind);
        INC(ps_renames);
        if (kind == PAIR_BUMP + 1) INC(bump_ps_renames);
        else if (kind == PAIR_SKIN + 1) INC(skin_ps_renames);
        t_ps_ours = kind; t_ps_name = name; t_ps_sidx = idx;
        if (InterlockedIncrement(&g_rename_logs) <= 10)
            photon_log("vlg: pair renamed: %s static %d -> %s, %s static %d -> %s%s", P->vs.stock,
                       t_vs_sidx / (int)P->vs.ndyn, P->vs.ours, name, idx / nd, P->ps.ours, t_vs_hl ? " (half-Lambert)" : "");
        return;
    }
    revert_vs(w, name ? name : "(null PS)");          /* before the PS call, as the rule says */
    send_ps(w, name, idx);
}

/* P4b, photon_cubemap_blend 1 at load: B's sampler (s13 for vlg and bump, s5 for skin) is enabled in every snapshot of ours, so stock's own
 * RenderPass binds whatever BindTexture last gave it (FUN_18001fd30 only records a handle for a disabled sampler), and it reads sRGB exactly as
 * the envmap sampler does (s1, s8: the shadow's byte +0x250 + sampler, bit 1, written by EnableSRGBRead FUN_1800299a0). Skin's s5 is enabled
 * by stock already (the normalisation cube); its sRGB read then follows s8's, which only matters in LDR (HDR reads the cubemap linear). */
enum { SH_ENABLESRGBREAD = 28, SH_ENABLETEXTURE = 31, OFF_SH_SRGBREAD = 0x250 };
static volatile LONG64 g_cube_snapshots;
static void snapshot_cube_sampler(PhotonWrap *sh, int pair) {
    int es = pair == PAIR_SKIN ? 8 : 1, bs = pair == PAIR_SKIN ? 5 : 13;
    int srgb = (*((volatile const unsigned char *)sh->real + OFF_SH_SRGBREAD + es) & 2) != 0;
    ((void (*)(PhotonWrap *, uintptr_t, uintptr_t))sh->cls->thunk_vtbl[SH_ENABLETEXTURE])(sh, (uintptr_t)bs, 1);
    ((void (*)(PhotonWrap *, uintptr_t, uintptr_t))sh->cls->thunk_vtbl[SH_ENABLESRGBREAD])(sh, (uintptr_t)bs, (uintptr_t)srgb);
    InterlockedIncrement64(&g_cube_snapshots);
}

static uintptr_t api_takesnapshot(void *self) {
    PhotonWrap *w = photon_wrap_of(self);
    PhotonWrap *sh = g_sh_obj;
    int mark = 0;
    if (sh) {
        /* 1. the rule: VS renamed but no (renamed) PS of the same pair since */
        if (t_vs_ours && t_ps_ours != t_vs_ours) revert_vs(sh, "TakeSnapshot without our PS");
        if (t_ps_ours && t_vs_ours != t_ps_ours) revert_ps(sh, "TakeSnapshot without our VS");
        /* 2. the net: the shadow's real handles (stock may set shaders without our wrapper, e.g. SetDefaultState) */
        if (!PHOTON_VLG_MUTANT) {
            int vi = *(volatile const int *)((const char *)sh->real + OFF_SH_VSIDX);
            int pi = *(volatile const int *)((const char *)sh->real + OFF_SH_PSIDX);
            int vkind = 0, pkind = 0;
            int kv = handle_find(g_hv, g_nhv, sh_handle(sh, OFF_SH_VSHANDLE), vi, &vkind);
            int kp = handle_find(g_hp, g_nhp, sh_handle(sh, OFF_SH_PSHANDLE), pi, &pkind);
            /* A known handle counts as ours only together with an index recorded for it: the shader manager reuses
             * the handles of released shaders, e.g. after a level change (found in game, T3.4f A5, r_pixelfog 0 run:
             * our handles from before the map load matched the skybox's PS, index 1, and re-sending the VLG name there
             * broke the sky). A handle match with another index is a reused handle: counted and left alone. */
            if (kv == 2 || kp == 2) {
                if (INC(stale_handles) <= 8)
                    photon_log("vlg: TakeSnapshot: the shadow's %s handle matches one of ours but with index %d, never"
                               " recorded for it (a reused handle): left alone, snapshot not marked", kv == 2 ? "VS" : "PS",
                               kv == 2 ? vi : pi);
            }
            int rv = kv == 1, rp = kp == 1;
            if (rv != rp || (rv && vkind != pkind)) {
                /* ours can only have come from a request for that pair's stock name with the index the shadow still
                 * holds; a VS and PS of ours from different pairs both go back */
                if (INC(pair_violations) <= 8)
                    photon_log("vlg: PAIRING VIOLATION at TakeSnapshot: the shadow holds VS %s (pair %d) with PS %s (pair %d)"
                               " (tracked: VS %d PS %d): re-sent %s%s%s", rv ? "ours" : "stock", vkind, rp ? "ours" : "stock",
                               pkind, t_vs_ours, t_ps_ours, rv ? g_pair[vkind - 1].vs.stock : "", rv && rp ? " + " : "",
                               rp ? g_pair[pkind - 1].ps.stock : "");
                if (rv) send_vs(sh, g_pair[vkind - 1].vs.stock, vi);
                if (rp) send_ps(sh, g_pair[pkind - 1].ps.stock, pi);
                t_vs_ours = t_ps_ours = 0;
            }
            mark = rv && rp && vkind == pkind && t_vs_ours == vkind && t_ps_ours == vkind;
        } else mark = t_vs_ours && t_ps_ours == t_vs_ours;
    }
    if (mark && photon_cube_blend_wanted) snapshot_cube_sampler(sh, t_vs_ours - 1);
    uintptr_t r = ((FnRet)g_api_prev[API_TAKESNAPSHOT])(w);
    uint16_t id = (uint16_t)r;
    if (id != 0xffff) {
        if (mark) {
            g_snap[id] = (uint8_t)(PHOTON_VLG_SNAP_OURS | (t_vs_hl ? PHOTON_VLG_SNAP_HALFLAMBERT : 0) |
                                   g_pair[t_vs_ours - 1].snapbit);
            INC(snapshots_marked);
        }
        if (t_ps_voltest) g_snap[id] |= PHOTON_VLG_SNAP_VOLTEST;
        if (t_ps_water) g_snap[id] |= PHOTON_VLG_SNAP_WATER;
        else g_snap[id] &= (uint8_t)~PHOTON_VLG_SNAP_WATER;
    }
    return r;
}

/* A device Reset zeroes the shader constants, and our caches of c32..c56 would then skip the writes that restore them
 * (lit draws black). Called from IShaderUtil::RestoreShaderObjects (proxy.c), the one reset notification that reaches us:
 * stock calls slot 260's body internally, and it has never been seen through the thunk in game (analysis/15d). */
void photon_vlg_device_state_lost(void) {
    photon_cube_device_lost();
    photon_cull_device_lost();
    photon_waterq_device_lost();
    g_maxl = -1;   /* the light cap is read again from the new device */
    InterlockedExchange(&g_occ_valid, 0);
    InterlockedExchange(&g_blk_valid, 0);
    InterlockedExchange(&g_vol_f_valid, 0);
    InterlockedExchange(&g_vol_c_valid, 0);
    InterlockedExchange(&g_lg_c_valid, 0);
    INC(device_resets);
}

static void clear_marks(const char *why) {
    memset((void *)g_snap, 0, sizeof g_snap);
    /* the snapshots are gone, and with them possibly the last users of our shaders: forget their handles (the next
     * renames add them again), so a released handle reused for another shader is not taken for ours */
    AcquireSRWLockExclusive(&g_hlock);
    InterlockedExchange(&g_nhv, 0); InterlockedExchange(&g_nhp, 0);
    ReleaseSRWLockExclusive(&g_hlock);
    InterlockedExchange(&g_occ_valid, 0);
    InterlockedExchange(&g_blk_valid, 0);
    InterlockedExchange(&g_vol_f_valid, 0);
    InterlockedExchange(&g_vol_c_valid, 0);
    InterlockedExchange(&g_lg_c_valid, 0);
    photon_cube_device_lost();
    if (InterlockedIncrement64(&S.snapshot_clears) <= 8)
        photon_log("vlg: snapshot marks cleared (%s, frame %llu)", why, (unsigned long long)photon_frame_count());
}
static void api_clearsnapshots(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    clear_marks("ClearSnapshots, slot 170");
    ((Fn3)g_api_prev[API_CLEARSNAPSHOTS])(w, a, b, c);
}
static void api_reset260(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, API_RESET260, -1)) { ((Fn3)w->cls->real_vtbl[API_RESET260])(w->real, a, b, c); return; }   /* T4.6: stock's own calls never cleared marks */
    clear_marks("slot 260");
    ((Fn3)g_api_prev[API_RESET260])(w, a, b, c);
}

static void api_beginpass(void *self, uintptr_t snap, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    t_cur_snap = (int)(snap & 0xffff);
    ((Fn3)g_api_prev[API_BEGINPASS])(w, snap, b, c);
}

static void api_setvsidx(void *self, int idx) {
    PhotonWrap *w = photon_wrap_of(self);
    t_dyn_idx = idx;
    t_vs_fresh = 1;
    ((FnInt)g_api_prev[API_SETVSIDX])(w, idx);
}

static void *volatile g_api_real;     /* the real ShaderApi030 object and its stock table, for photon_vlg_api_matrix (T4.2) */
static void **volatile g_api_vt;
enum { API_SLOT_GETMATRIX = 12 };
int photon_vlg_api_matrix(int mode, float out[16]) {   /* stock GetMatrix(mode, dst) on the calling thread: 0 VIEW, 1 PROJECTION */
    void *r = g_api_real;
    void **vt = g_api_vt;
    if (!r || !vt) return 0;
    ((void (*)(void *, int, float *))vt[API_SLOT_GETMATRIX])(r, mode, out);
    return 1;
}
/* AA (taa.c): the projection's sub-pixel jitter, on a copy of the matrix the material system loads */
/* the previous layer (thunk) takes the wrapper, as the other hooks chain: in direct mode (the laptop: call counts off) self is the real
 * object and the thunk's `mov rcx, [rcx + 8]` would read past it (the dyn60f/g startup crash) */
static void api_matrixmode(void *self, int mode) {
    PhotonWrap *w = photon_wrap_of(self);
    photon_taa_matrix_mode(mode);
    ((void (*)(void *, int))g_api_prev[API_MATRIXMODE])(w, mode);
}
static void api_loadmatrix(void *self, float *m) {
    PhotonWrap *w = photon_wrap_of(self);
    if (m && photon_taa_on()) {
        float c[16];
        memcpy(c, m, sizeof c);
        photon_taa_load_matrix(c);
        ((void (*)(void *, float *))g_api_prev[API_LOADMATRIX])(w, c);
        return;
    }
    ((void (*)(void *, float *))g_api_prev[API_LOADMATRIX])(w, m);
}
static void api_renderpass(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    if (!g_api_real) { g_api_vt = w->cls->real_vtbl; g_api_real = w->real; }
    INC(rp_total);
    route_settle();
    /* P0 meter mq_photon_us (frames.h): QPC around our part and around the whole hook, only while the CSV is open */
    int perf = photon_perf_on();
    LARGE_INTEGER t0, t1, t2;
    if (perf) QueryPerformanceCounter(&t0);
    int ours = perf && S.active && (g_snap[t_cur_snap & 0xffff] & PHOTON_VLG_SNAP_OURS);
    if (photon_amb_on) photon_amb_first_draw();
    if (photon_lg_on) photon_lg_first_draw();
    g_vol_draw = 0;
    if (S.active) pre_draw(w);
    else { flag_off(); occ_off(); g_vol_wants = 0; }   /* switched off: every draw of ours gets the stock result (c32.x = c56.x = 0) */
    if (photon_vol_on && !photon_vol_predraw(w->real, g_vol_wants)) g_vol_wants = 0;
    if (photon_cube_on) {   /* P4a probe (PHOTON_TEST_CUBEPROBE) observes; P4b (photon_cubemap_blend/_pick) binds A and B */
        float o[3];
        if (photon_p6_suppressed()) photon_p6_origin(o); else memcpy(o, g_origin, sizeof o);   /* P6: the engine set no origin */
        photon_cube_draw(w, S.active ? g_snap[t_cur_snap & 0xffff] : 0, g_route, g_route_array, o, S.active && g_dev_on,
                         S.active && g_draw_cfg[K_CUBE_BLEND] != 0, S.active && cube_pick_of(g_draw_cfg));
    }
    if (perf) QueryPerformanceCounter(&t1);
    if (S.active && (g_snap[t_cur_snap & 0xffff] & PHOTON_VLG_SNAP_OURS)) photon_shadows_model_draw();   /* L3 model mask */
    int water = (g_snap[t_cur_snap & 0xffff] & PHOTON_VLG_SNAP_WATER) != 0;
    if (water) photon_waterq_begin();
    ((Fn3)g_api_prev[API_RENDERPASS])(w, a, b, c);
    if (water) photon_waterq_end();
    t_model_pass = 0;
    if (photon_vol_on) photon_vol_postdraw(g_vol_wants);
    else if (g_vol_draw) photon_vol_postdraw(1);
    if (perf) {
        QueryPerformanceCounter(&t2);
        photon_mq_photon_add(t1.QuadPart - t0.QuadPart, t2.QuadPart - t0.QuadPart, ours);
    }
    t_cur_snap = 0xffff;     /* stock RenderPass resets its current snapshot (+0x35e8) to -1 the same way */
    t_vs_fresh = 0;
    g_seq_len = 0; g_seq[0] = 0;
}

static void api_setlight(void *self, int i, const LightDesc *d) {
    PhotonWrap *w = photon_wrap_of(self);
    ((FnLight)g_api_prev[API_SETLIGHT])(w, i, d);
    if (!S.active || !d) return;
    route_settle();
    seq_add("L");
    if (g_maxl < 0) g_maxl = ((FnGetInt)w->cls->real_vtbl[API_GETMAXLIGHTS])(w->real);   /* stock ignores i >= caps MaxNumLights; a device
                                                                                            * cap, so read once per device (reset clears it) */
    int maxl = g_maxl;
    if (i < 0 || i >= maxl || i >= 4) return;
    g_lights[i] = *d;
    g_light_tid = GetCurrentThreadId();
    INC(lights_set);
    if (d->type >= 1 && d->type <= 3 && S.layout_ok + S.layout_mismatch < 4096) check_layout(w->real, i, d);
    if (photon_lg_on) photon_lg_cal(d);   /* PHOTON_TEST_LIGHTCAL only */
}

static void api_setorigin(void *self, const void *v) {
    PhotonWrap *w = photon_wrap_of(self);
    ((FnPtr)g_api_prev[API_SETORIGIN])(w, v);
    if (!S.active || !v) return;
    if (g_route_pending && g_route_latch) { g_route = ROUTE_ENGINE; g_route_array = 1; }
    g_route_pending = 0;
    seq_add("O");
    memcpy(g_origin, v, sizeof g_origin);
    g_light_tid = GetCurrentThreadId();
}

static void api_setambient(void *self, float r, float g, float b) {
    PhotonWrap *w = photon_wrap_of(self);
    ((void (*)(void *, float, float, float))g_api_prev[API_SETAMBIENT])(w, r, g, b);
    if (!S.active) return;
    int zero = r == 0 && g == 0 && b == 0;
    if (g_route_pending && zero) { g_route = ROUTE_ENGINE; g_route_latch = 1; }
    g_route_pending = 0;
    seq_add(zero ? "A0" : (r == 1 && g == 1 && b == 1) ? "A1" : "Ax");
}

static void api_setcube(void *self, const void *cube) {
    PhotonWrap *w = photon_wrap_of(self);
    ((FnPtr)g_api_prev[API_SETCUBE])(w, cube);
    if (!S.active || !cube) return;
    g_route = ROUTE_OVERRIDE;
    g_route_array = 0;
    g_route_pending = 1;
    if (g_route_test) {   /* the sequence log tells white and black cubes apart; the route never uses values */
        const float *c = cube;
        int white = 1, black = 1;
        for (int i = 0; i < 6; i++) for (int k = 0; k < 3; k++) {
            white &= c[i * 4 + k] == 1;
            black &= c[i * 4 + k] == 0;
        }
        seq_add(white ? "Cw" : black ? "Ck" : "Cx");
    }
    memcpy(g_cube, cube, sizeof g_cube);
    g_light_tid = GetCurrentThreadId();
}

static void api_disablelights(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    ((Fn3)g_api_prev[API_DISABLELIGHTS])(w, a, b, c);
    if (!S.active) return;
    route_settle();
    seq_add("D");
    for (int i = 0; i < 4; i++) g_lights[i].type = 0;
    g_light_tid = GetCurrentThreadId();
}

/* --- install ---------------------------------------------------------------------------------------------------- */
static int check_rvas(PhotonWrapClass *cls, const SlotRva *t, size_t n) {
    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        uintptr_t f = (size_t)t[i].slot < cls->nslots ? (uintptr_t)cls->real_vtbl[t[i].slot] : 0;
        if (f != g_stock_base + t[i].rva) {
            photon_log("vlg: %s slot %d = stock+0x%llx, expected +0x%x", cls->name, t[i].slot,
                       (unsigned long long)(f - g_stock_base), t[i].rva);
            ok = 0;
        }
    }
    return ok;
}

static void hook(PhotonWrapClass *cls, void **prev, int slot, void *fn) {
    prev[slot] = cls->thunk_vtbl[slot];
    photon_wrap_set_hook(cls, (size_t)slot, fn);
}

static void install_fastvtx(PhotonWrapClass *cls);
void photon_vlg_install(PhotonWrapClass *cls) {
    if (strcmp(cls->name, "MaterialSystemHardwareConfig012") == 0) { install_fastvtx(cls); return; }
    if (!S.wanted) return;
    if (strcmp(cls->name, "ShaderShadow010") == 0) {
        if (!check_rvas(cls, k_sh_rva, sizeof k_sh_rva / sizeof k_sh_rva[0])) {
            deactivate("ShaderShadow010 vtable differs from the validated build");
            return;
        }
        hook(cls, g_sh_prev, SH_SETDEFAULT, (void *)sh_setdefault);
        hook(cls, g_sh_prev, SH_SETVS, (void *)sh_setvs);
        hook(cls, g_sh_prev, SH_SETPS, (void *)sh_setps);
        S.shadow_hooked = 1;
        photon_log("vlg: ShaderShadow010 slots 0/23/24 hooked (vtable as expected)");
    } else if (strcmp(cls->name, "ShaderApi030") == 0) {
        if (!check_rvas(cls, k_api_rva, sizeof k_api_rva / sizeof k_api_rva[0])) {
            deactivate("ShaderApi030 vtable differs from the validated build");
            return;
        }
        if (!photon_d3d_api_ready()) { deactivate("d3dconst API not available"); return; }
        hook(cls, g_api_prev, API_SETVSIDX, (void *)api_setvsidx);
        hook(cls, g_api_prev, API_MATRIXMODE, (void *)api_matrixmode);   /* AA: pass-through while TAA is off */
        hook(cls, g_api_prev, API_LOADMATRIX, (void *)api_loadmatrix);
        hook(cls, g_api_prev, API_TAKESNAPSHOT, (void *)api_takesnapshot);
        hook(cls, g_api_prev, API_BEGINPASS, (void *)api_beginpass);
        hook(cls, g_api_prev, API_RENDERPASS, (void *)api_renderpass);
        hook(cls, g_api_prev, API_SETLIGHT, (void *)api_setlight);
        hook(cls, g_api_prev, API_SETORIGIN, (void *)api_setorigin);
        hook(cls, g_api_prev, API_SETAMBIENT, (void *)api_setambient);
        hook(cls, g_api_prev, API_SETCUBE, (void *)api_setcube);
        hook(cls, g_api_prev, API_CLEARSNAPSHOTS, (void *)api_clearsnapshots);
        hook(cls, g_api_prev, API_DISABLELIGHTS, (void *)api_disablelights);
        hook(cls, g_api_prev, API_RESET260, (void *)api_reset260);
        S.api_hooked = 1;
        g_api_ready = 1;
        photon_log("vlg: ShaderApi030 slots 40/109/124/125/127/128/129/130/170/207/260 hooked (vtable as expected); light"
                   " block c%d..c%d (c%d envmap occlusion) via d3dconst", PHOTON_LB_FIRST,
                   PHOTON_LB_FIRST + PHOTON_LB_COUNT - 1, PHOTON_LB_ENVOCC);
    }
}

/* --- PHOTON_TEST_FASTVTX=1 (test only, vlg.h) ------------------------------------------------------------------ */
enum { HW_HASFASTVERTEXTEXTURES = 65, RVA_HW_FASTVTX = 0x4030 };   /* CHardwareConfig 0x1800788c0 slot 65: xor al,al; ret */
static void *g_hw_prev[80];
static uintptr_t g_ss_lo, g_ss_hi;
static volatile LONG g_ss_tried;
static volatile LONG64 g_fv_true, g_fv_other;
static int g_fv_vlg_only;   /* PHOTON_TEST_FASTVTX=vlg: only inside the VLG helper FUN_1800639c0 (stdshader_dx9 build
                             * sha1 3eab6c6e..., checked by the call bytes), except its decal-morph texcoord site */
enum { SS_VLG_LO = 0x639c0, SS_VLG_HI = 0x6b210, SS_DECAL_RA = 0x6472b };
typedef unsigned char (*FnBool)(void *);
static unsigned char hw_fastvtx(void *self) {
    PhotonWrap *w = photon_wrap_of(self);
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    if (!g_ss_hi && !InterlockedExchange(&g_ss_tried, 1)) {
        HMODULE m = GetModuleHandleA("stdshader_dx9.dll");
        if (m) {
            IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
            g_ss_lo = (uintptr_t)m;
            g_ss_hi = g_ss_lo + nt->OptionalHeader.SizeOfImage;
            if (g_fv_vlg_only) {
                static const uint32_t sites[] = { 0x64725, 0x6489d, 0x65b22 };   /* call qword [rax+0x208] */
                static const uint8_t call208[] = { 0xff, 0x90, 0x08, 0x02, 0x00, 0x00 };
                int ok = nt->OptionalHeader.SizeOfImage > SS_VLG_HI;
                for (size_t i = 0; ok && i < sizeof sites / sizeof sites[0]; i++)
                    ok = memcmp((char *)m + sites[i], call208, sizeof call208) == 0;
                photon_log("vlg: TEST PHOTON_TEST_FASTVTX=vlg: stdshader_dx9 VLG call sites %s", ok ? "as expected" : "DIFFER: ignored");
                if (ok) { g_ss_hi = g_ss_lo + SS_VLG_HI; g_ss_lo += SS_VLG_LO; } else g_ss_hi = g_ss_lo = 0;
            }
        } else g_ss_tried = 0;   /* not loaded yet: try again on the next call */
    }
    if (ra >= g_ss_lo && ra < g_ss_hi && !(g_fv_vlg_only && ra == g_ss_lo - SS_VLG_LO + SS_DECAL_RA)) {
        InterlockedIncrement64(&g_fv_true);
        return 1;
    }
    InterlockedIncrement64(&g_fv_other);
    return ((FnBool)g_hw_prev[HW_HASFASTVERTEXTEXTURES])(w);
}
static void install_fastvtx(PhotonWrapClass *cls) {
    char e[8];
    if (GetEnvironmentVariableA("PHOTON_TEST_FASTVTX", e, sizeof e) <= 0 || (strcmp(e, "1") != 0 && strcmp(e, "vlg") != 0)) return;
    g_fv_vlg_only = strcmp(e, "vlg") == 0;
    if (!g_stock_base || cls->nslots <= HW_HASFASTVERTEXTEXTURES ||
        (uintptr_t)cls->real_vtbl[HW_HASFASTVERTEXTEXTURES] != g_stock_base + RVA_HW_FASTVTX) {
        photon_log("vlg: TEST PHOTON_TEST_FASTVTX=1 ignored: unknown build or HWConfig slot 65 is not stock+0x%x", RVA_HW_FASTVTX);
        return;
    }
    hook(cls, g_hw_prev, HW_HASFASTVERTEXTEXTURES, (void *)hw_fastvtx);
    photon_log("vlg: TEST PHOTON_TEST_FASTVTX=%s: HWConfig slot 65 HasFastVertexTextures returns true for callers in"
               " stdshader_dx9.dll%s (stock: always false), so VertexLitGeneric selects vs30/ps30", e,
               g_fv_vlg_only ? "'s VLG helper FUN_1800639c0 (not its decal-morph site)" : "");
}

int photon_vlg_snapshot_flags(int id) { return id >= 0 && id < 65536 ? g_snap[id] : 0; }

void photon_vlg_dump(const char *why) {
    if (g_fv_true || g_fv_other)
        photon_log("vlg: TEST PHOTON_TEST_FASTVTX: HasFastVertexTextures -> true %lld (stdshader_dx9 callers), stock %lld",
                   (long long)g_fv_true, (long long)g_fv_other);
    if (!S.wanted) return;
    if (g_cube_snapshots) photon_log("vlg: P4b: B's sampler enabled in %lld snapshots of ours", (long long)g_cube_snapshots);
    photon_log("vlg summary (%s, frame %llu): %s; VS requests %lld, VS renames %lld, PS requests %lld, PS renames %lld,"
               " PS pilot without our VS %lld, pair reverts %lld, PAIR VIOLATIONS %lld, stale handles %lld, fog-gate blocks %lld, load fallbacks"
               " %lld; snapshots marked %lld, clears %lld; RenderPass %lld, ours %lld, uploads %lld, flag-off writes %lld,"
               " ours without VS index %lld, VS index from the shader manager %lld, ours unlit (SL/LP/no DL) %lld, ours with FF fog %lld, write failures %lld;"
               " lights set %lld, layout ok %lld mismatch %lld; config reloads %lld; r_pixelfog %d dxlevel %d",
               why, (unsigned long long)photon_frame_count(), S.reason, (long long)S.vs_requests, (long long)S.vs_renames,
               (long long)S.ps_requests, (long long)S.ps_renames, (long long)S.vs_rejects, (long long)S.pair_reverts,
               (long long)S.pair_violations, (long long)S.stale_handles, (long long)S.fog_blocks, (long long)S.load_fallbacks,
               (long long)S.snapshots_marked, (long long)S.snapshot_clears, (long long)S.rp_total, (long long)S.rp_ours,
               (long long)S.uploads, (long long)S.flag_off_writes, (long long)S.ours_no_vsidx, (long long)S.vsidx_from_mgr,
               (long long)S.ours_unlit, (long long)S.ours_ff_fog, (long long)S.write_fail, (long long)S.lights_set, (long long)S.layout_ok,
               (long long)S.layout_mismatch, (long long)S.config_reloads, pixelfog_value(), dxlevel());
    photon_log("vlg summary (%s): bump pair %s: VS renames %lld, PS renames %lld, RenderPass ours %lld; skin pair %s: VS"
               " renames %lld, PS renames %lld, RenderPass ours %lld (stock look: LIGHTING_PREVIEW/index %lld); c56"
               " occlusion-only writes %lld; light block uploads skipped (cached) %lld, c32-only restores %lld, device resets %lld", why, g_pair[PAIR_BUMP].on ? "on" : "not used", (long long)S.bump_vs_renames,
               (long long)S.bump_ps_renames, (long long)S.rp_ours_bump, g_pair[PAIR_SKIN].on ? "on" : "not used",
               (long long)S.skin_vs_renames, (long long)S.skin_ps_renames, (long long)S.rp_ours_skin,
               (long long)S.ours_skin_unlit, (long long)S.occ_writes, (long long)S.upload_skips,
               (long long)S.flag_on_writes, (long long)S.device_resets);
    photon_log("vlg route (%s): lit draws engine/override: vlg %lld/%lld, bump %lld/%lld, skin %lld/%lld", why,
               (long long)S.route_engine[PAIR_VLG], (long long)S.route_override[PAIR_VLG],
               (long long)S.route_engine[PAIR_BUMP], (long long)S.route_override[PAIR_BUMP],
               (long long)S.route_engine[PAIR_SKIN], (long long)S.route_override[PAIR_SKIN]);
    if (S.vol_draws || S.vol_off[0] || S.vol_off[1] || S.vol_off[2] || S.vol_off[3] || S.vol_off[4])
        photon_log("vlg volume (%s): draws with weight %lld; weight 0: setting %lld, route %lld (of which static-prop array %lld), r_shader_srgb or mat_fullbright %lld, out of range or no data %lld,"
                   " s15 not at rest %lld; writes c59..c62 %lld, c64 %lld", why, (long long)S.vol_draws, (long long)S.vol_off[PHOTON_AMB_OFF_CONFIG], (long long)S.vol_off[PHOTON_AMB_OFF_ROUTE],
                   (long long)S.vol_off_array,
                   (long long)S.vol_off[PHOTON_AMB_OFF_GATE], (long long)S.vol_off[PHOTON_AMB_OFF_RANGE], (long long)S.vol_off[PHOTON_AMB_OFF_REST], (long long)S.vol_const_writes,
                   (long long)S.vol_weight_writes);
    if (photon_lg_on)
        photon_log("vlg lights (%s): draws with the light grid vlg %lld, bump %lld, skin %lld; c63 writes %lld", why, (long long)S.lg_draws[PAIR_VLG],
                   (long long)S.lg_draws[PAIR_BUMP], (long long)S.lg_draws[PAIR_SKIN], (long long)S.lg_const_writes);
    if (g_cov_on) {   /* cumulative: every dump, since a sweep may not reach a clean exit; tools/p3_coverage.py keeps the largest n */
        static const char *const kn[] = { "vlg", "bump", "skin", "lmg" };
        for (int i = 0; i < COV_SLOTS; i++)
            if (g_cov[i].used)
                photon_log("coverage %s %s static %d %s n=%lld", kn[g_cov[i].pair], g_cov[i].ps ? "ps" : "vs", g_cov[i].st,
                           g_cov[i].listed ? "built" : "NOT-BUILT", g_cov[i].n);
        photon_log("coverage end (table overflow %lld)", g_cov_full);
    }
    if (g_route_test && strcmp(why, "exit") == 0) {
        static const char *const kn[] = { "vlg", "bump", "skin" };
        photon_log("route sequences: %d distinct, %lld not recorded", g_nseqs, g_seq_dropped);
        for (int i = 0; i < g_nseqs; i++)
            photon_log("route seq n=%lld %s %s %s [ %s]", g_seqs[i].n, kn[g_seqs[i].kind], g_seqs[i].lit ? "lit" : "unlit",
                       g_seqs[i].route == ROUTE_ENGINE ? "ENGINE" : "OVERRIDE", g_seqs[i].seq);
    }
    dump_names();
}

void photon_vlg_frame(void) {
    if (!S.wanted) return;
    unsigned long long f = photon_frame_count();
    if (f % 120 == 0) photon_vlg_poll_config(0);
    if (f % 600 == 0) photon_vlg_dump("periodic");
}

void photon_vlg_csv(FILE *f, int header) {
    if (!S.wanted) return;
    enum { N = 12 };
    static LONG64 prev[N];
    static int amb_columns, lg_columns;   /* fixed with the header, as every column: the volume can switch off later */
    if (header) {
        fprintf(f, ",vlg_rp_ours,vlg_uploads,vlg_flag_off,vlg_ours_no_vsidx,vlg_vs_renames,vlg_ps_renames,vlg_pair_reverts,"
                   "vlg_pair_violations,vlg_rp_ours_bump,vlg_occ_writes,vlg_rp_ours_skin,vlg_upload_skips");
        amb_columns = photon_amb_on;
        if (amb_columns) { fprintf(f, ",vlg_vol_draws"); photon_amb_csv(f, 1); }
        lg_columns = photon_lg_on;
        if (lg_columns) { fprintf(f, ",vlg_lg_draws"); photon_lg_csv(f, 1); }
        return;
    }
    LONG64 cur[N] = { S.rp_ours, S.uploads, S.flag_off_writes, S.ours_no_vsidx, S.vs_renames, S.ps_renames, S.pair_reverts,
                      S.pair_violations, S.rp_ours_bump, S.occ_writes, S.rp_ours_skin, S.upload_skips };
    for (int i = 0; i < N; i++) { fprintf(f, ",%lld", (long long)(cur[i] - prev[i])); prev[i] = cur[i]; }
    if (amb_columns) {
        static LONG64 prev_draws;
        LONG64 d = S.vol_draws;
        fprintf(f, ",%lld", (long long)(d - prev_draws));
        prev_draws = d;
        photon_amb_csv(f, 0);
    }
    if (lg_columns) {
        static LONG64 prev_lg;
        LONG64 d = S.lg_draws[0] + S.lg_draws[1] + S.lg_draws[2];
        fprintf(f, ",%lld", (long long)(d - prev_lg));
        prev_lg = d;
        photon_lg_csv(f, 0);
    }
}
