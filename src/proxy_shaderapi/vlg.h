/* T3.4c/T3.4f: Photon lighting v0 in the proxy, Option A (analysis/14_t34_route_decision.md "Erratum 2 decision", A2;
 * evidence analysis/14c_optionA_sm3_pair.md), extended by lighting plan v2 (analysis/15) P1 + P2 (bump, skin). GMod draws
 * VertexLitGeneric with SM2 pairs; for their pilot statics both shaders of a pair are swapped for our SM3 builds at the
 * identical combo layouts:
 *   vlg   vertexlit_and_unlit_generic_vs20 + _ps20b           -> photon_vlg_vs20 + photon_vlg_ps20b (t34_build_vlg20.py)
 *   bump  vertexlit_and_unlit_generic_bump_vs20 + _bump_ps20b -> photon_bump_vs20 + photon_bump_ps20b
 *                                                                (t34_build_bump20.py)
 *   skin  skin_vs20 + skin_ps20b (VertexLitGeneric $phong)       -> photon_skin_vs20 + photon_skin_ps20b
 *                                                                (t34_build_skin20.py). stdshader_dx9 names skin_vs20 and
 *                                                                skin_ps20b once each, in one function, VS first (0x1800548e7,
 *                                                                0x180054c44), and has no skin_ps20, so skin_vs20 has no other
 *                                                                user [confirmed binary, t34_build_skin20.py header]
 * The light block c32..c55 and the envmap occlusion c56 (photon_lightblock.h) are written to the D3D9 device
 * (d3dconst.h) at RenderPass for the draws that use them. Stock C++ still believes it is on the SM2 path (HW slot 65 is
 * never touched).
 *
 * ENABLE (decided once at load, photon_vlg_setup; any failed condition = no hook installed at all, so the proxy
 * behaves exactly as without this module):
 *   - the T1.5 build guard matches and wrapping is on (every RVA/offset below is for build 26229091);
 *   - PHOTON_TEST_C32 is not set (that test owns RenderPass);
 *   - photon_lighting != 0 (env PHOTON_LIGHTING, else garrysmod/cfg/photon.cfg; default 1);
 *   - per pair: a pair is used iff any of its four files is in <mod>/shaders/fxc (scripts/install_proxy.sh installs
 *     them; bump also needs photon_bump != 0, skin photon_skin != 0); a used pair must have BOTH <name>.{vcs,ids}
 *     files, each .ids parses, its vcs_sha1 equals the SHA1 of its .vcs, and each .vcs header is v6 with the stock
 *     layout (vlg vs20 1536 x 384, ps20b 31457280 x 12; bump vs20 8 x 24, ps20b 43008 x 20; skin vs20 2 x 48, ps20b
 *     86016 x 80), else lighting is off altogether; at least one pair
 *     must be used. <mod> = <proxy dir>/../../garrysmod (PHOTON_GAME_DIR overrides it; tests only);
 *   - stock's r_pixelfog ConVar (stock+0xa2a90) has this build's vtable, GetInt and name.
 *   Then, per class (at creation): every hooked slot of the live vtable must be the expected stock function (RVA
 *   check), and the d3dconst API must be ready; otherwise that class gets no hooks and lighting stays off.
 *
 * HOOKS (each forwards through the previous thunk of its slot, so call counters and other hooks, e.g. PHOTON_DIAG's
 * slot 128, are kept):
 *   ShaderShadow010  0 SetDefaultState      resets the per-thread shadow tracking (stock sets VS/PS to NULL)
 *                   23 SetVertexShader      a used pair's stock VS name, idx = static*ndyn (vlg 384, bump 24, skin 48), static
 *                                           in its VS .ids, and the FOG GATE open -> forwarded as our VS name
 *                                           (speculatively: see PAIRING). Otherwise unchanged.
 *                   24 SetPixelShader       a used pair's stock PS name, idx = static*ndyn (vlg 12, bump 20, skin 80), static in
 *                                           its PS .ids, and the VS of this pass renamed to THE SAME pair's VS ->
 *                                           forwarded as our PS name. Otherwise unchanged.
 *   ShaderApi030   109 TakeSnapshot         pairing check (below); marks the returned id (uint16) as "ours"
 *                                           (+ half-Lambert bit from vlg VS static bit 2, + the bump or skin bit) iff
 *                                           both are ours and of the same pair
 *                  170 ClearSnapshots, 260 (device-reset path): both reach the snapshot reset FUN_18002f100; the
 *                                           marks are cleared
 *                  124 BeginPass            current snapshot (per thread; reset after RenderPass, like stock +0x35e8)
 *                   40 SetVertexShaderIndex vs20 dynamic index (0..383): COMPRESSED_VERTS x1, DYNAMIC_LIGHT x2,
 *                                           STATIC_LIGHT_VERTEX x4, STATIC_LIGHT_LIGHTMAP x8, DOWATERFOG x16,
 *                                           SKINNING x32, LIGHTING_PREVIEW x64, NUM_LIGHTS x128 (analysis/14c §2)
 *                  127 SetLight, 128 SetLightingOrigin, 130 SetAmbientLightCube, 207 DisableAllLocalLights: copies
 *                  125 RenderPass           entry: a vlg snapshot of ours + a VS index set since the last RenderPass
 *                                           with DYNAMIC_LIGHT = 1 and STATIC_LIGHT_VERTEX = STATIC_LIGHT_LIGHTMAP =
 *                                           LIGHTING_PREVIEW = 0, or a bump snapshot of ours (any VS index: the bump
 *                                           PS lights per pixel in every dynamic), or a skin snapshot of ours + a VS
 *                                           index < 48 with LIGHTING_PREVIEW (x8) = 0 (skin_vs20 dynamics:
 *                                           COMPRESSED_VERTS x1, DOWATERFOG x2, SKINNING x4, LIGHTING_PREVIEW x8,
 *                                           NUM_LIGHTS x16 (0..2), SDK 2015 skin_vs20.fxc; the skin PS lights per
 *                                           pixel from the engine's lights + cube in every dynamic, as bump; LIGHTING_
 *                                           PREVIEW is Hammer's fixed-lighting mode, kept stock), + photon_lighting -> write
 *                                           c32..c56 (enable = 1) in one call. Another draw of ours -> c32 = 0 (stock
 *                                           lighting; written only when the device may still hold enable = 1) and
 *                                           c56 (P1 envmap occlusion, which applies to every draw of ours with a
 *                                           cubemap, C1: VHV static props, SLL; written only when the device's c56,
 *                                           cached, differs). Not ours -> c32 = 0 as before (c56 is read only by our
 *                                           shaders). Static props (STATIC_LIGHT, baked vertex lighting) and unlit
 *                                           draws therefore keep stock diffuse lighting. A draw of ours while D3D FF
 *                                           fog is on (+0x132d) is counted and logged (see FOG GATE). Lighting
 *                                           switched off (a failed guard or load): c32 = c56.x = 0 for every draw.
 * PAIRING RULE (a vs_3_0 must never be snapshotted with a stock ps_2_x, nor a ps_3_0 with a stock vs_2_0, nor our VS
 * of one pair with our PS of the other; DecalModulate also uses vertexlit_and_unlit_generic_vs20 with
 * decalmodulate_ps20b, and UnlitGeneric uses the same VS with non-pilot DIFFUSELIGHTING=0 statics): the VS name
 * arrives first, so slot 23 renames it speculatively and remembers "VS ours, pair k" (per thread, until
 * SetDefaultState). If the following slot 24 is not renamed (another name, the other pair's PS, a non-pilot static, a
 * load failure, lighting switched off) the stock VS name and index are re-sent through slot 23 BEFORE the PS is
 * forwarded. A VS set after our PS that is not the same pair's VS of ours sends the PS back to stock. TakeSnapshot
 * reverts a VS renamed without a PS of its pair since. Then, as a net, it compares the shadow's real VS/PS (handle,
 * index) pairs (+0x270/+0x280, +0x278/+0x284) with the exact (handle, index, pair) records the shader manager returned
 * for our names (cleared with the snapshot marks): one ours and the other not, or ours of different pairs, is a PAIR
 * VIOLATION (counted, must be 0): the stock name(s) are re-sent with the shadow's own index and the snapshot is not
 * marked. A handle of ours with an index never recorded for it is a reused handle (stale_handles): left alone, not
 * marked.
 * FOG GATE: renames happen only while stock would use pixel fog for ps20b: r_pixelfog != 0 (stock's own ConVar, via its
 * GetInt) and dxlevel >= 90 (caps +0x4f8), read at every candidate SetVertexShader. With r_pixelfog 0 stock enables
 * D3D fixed-function fog for ps20b, which a ps_3_0 cannot have (analysis/14c §1). The gate acts at snapshot time; a
 * change of r_pixelfog takes effect for materials (re-)snapshotted afterwards (mat_reloadallmaterials).
 * LOAD FAILURE: if the shader manager returns the invalid handle for our VS or PS name (shadow +0x270/+0x278 =
 * 0xffffffff), the stock names are re-sent (both, for a PS failure) and lighting is switched off for the session
 * (both pairs).
 * LightDesc_t is read per the public mathlib/lightdesc.h (88 bytes). The layout is checked at runtime against stock's
 * own conversion: after the real SetLight, the D3DLIGHT9 record stock built at ShaderApi030+0xf84+i*0x68 (read by
 * CommitVertexShaderLighting FUN_180012e70 for the VS constants) must match our parse field for field; a mismatch
 * switches lighting off. Spot cones follow that function: stopdot = cos(theta/2), stopdot2 = cos(phi/2), with
 * SetLight's clamps (phi <= pi, theta <= phi - 0.001); the light range is not used by the stock VS (photon_range 1
 * enables our range window).
 *
 * THREADS: snapshot tracking is per thread (TLS); the draw path (BeginPass, VS index, RenderPass) is per thread too;
 * the light copies and the config are shared under one SRW lock; the snapshot marks are a byte array; our shader
 * handles are an append-only table. In queued mode materials are snapshotted on the main thread while the MatQueue
 * thread draws.
 *
 * CONFIG keys (photon.cfg lines "key value", // comments; the environment variable overrides the file):
 *   photon_lighting       PHOTON_LIGHTING       1   0 at load: no hooks; 0 later (live): enable = 0 (stock look)
 *   photon_light_scale    PHOTON_LIGHT_SCALE    1   c55.x, 0..16
 *   photon_ambient_scale  PHOTON_AMBIENT_SCALE  1   c55.y, 0..16
 *   photon_ambient_floor  PHOTON_AMBIENT_FLOOR  0   c55.z, 0..1
 *   photon_halflambert    PHOTON_HALFLAMBERT   -1   -1 = the material's (VS static), 0/1 = forced
 *   photon_range          PHOTON_RANGE          0   1 = window lights by LightDesc_t m_Range (not stock)
 *   photon_debug          PHOTON_DEBUG          0   c55.w: 1 diffuse lighting only, 2 world normal
 *   photon_envmap_occlusion      PHOTON_ENVMAP_OCCLUSION      1   P1 strength s 0..1 (0: stock envmap, c56.x = 0)
 *   photon_envmap_occlusion_min  PHOTON_ENVMAP_OCCLUSION_MIN  0   factor where the lighting/cubemap ratio x <= k/4, 0..1
 *   photon_envmap_occlusion_max  PHOTON_ENVMAP_OCCLUSION_MAX  1   factor where x >= k, 0..4 (1: never brighter than stock)
 *   photon_envmap_calibration    PHOTON_ENVMAP_CALIBRATION  0.5   k: the knee, the ratio from which the envmap is kept
 *                                                                 (0.01..100)
 *   photon_bump           PHOTON_BUMP           1   0 at load: the bump pair is not used even if installed (latched)
 *   photon_skin           PHOTON_SKIN           1   0 at load: the skin pair is not used even if installed (latched)
 *   photon_ambient        PHOTON_AMBIENT        0   1 at load: the ambient volume (ambvol.h, analysis/15d section 8): hooks, workers and device writes exist only
 *                                                   then. 0 later (live): weight 0 everywhere and the workers idle. 1 later, if it was 0 at load: nothing.
 * c56 = photon_lb_envocc(s, min, max, k): factor = lerp(1, min + (max - min) * smoothstep(log2(k/4), log2(k), log2(x)), s),
 * x = Y(diffuse) / Y(cubemap lowest mip)
 * on the stock envmap term (src/shaderdll/hlsl/photon_envocc_ps.h). $envmaplightscale is not reachable here (the proxy
 * sees shader names, indices and constants, and no VLG/bump/skin PS constant carries it, analysis/15a §2), so the keys
 * apply to every material. The file is re-read when its modification time changes (checked every 120 frames from
 * Present).
 *
 * FINDING (in game, 2026-11-03): stock GMod never selects the vs30/ps30 VertexLitGeneric combos. stdshader_dx9's VLG
 * helper (FUN_1800639c0) takes the vs30/ps30 branch only if IMaterialSystemHardwareConfig::HasFastVertexTextures()
 * (slot 65, call at 0x18006489d) is true, and stock shaderapidx9's CHardwareConfig slot 65 (0x180004030) is
 * "xor al,al; ret". Hence Option A (the SM2 pair). PHOTON_TEST_FASTVTX=1 (TEST ONLY, default off, independent of
 * photon_lighting): HWConfig slot 65 returns true for callers inside stdshader_dx9.dll (other callers get stock's
 * false), which makes VLG select vs30/ps30 (crashes GMod; kept for diagnosis). */
#ifndef PHOTON_VLG_H
#define PHOTON_VLG_H
#include <windows.h>
#include <stdio.h>
#include "wrap.h"

enum { PHOTON_VLG_SNAP_OURS = 1, PHOTON_VLG_SNAP_HALFLAMBERT = 2, PHOTON_VLG_SNAP_BUMP = 4, PHOTON_VLG_SNAP_SKIN = 8, PHOTON_VLG_SNAP_VOLTEST = 16 /* P3d: Photon_DebugVol, samples s15 */,
       PHOTON_VLG_SNAP_WATER = 32 /* W1: a water shader (PS name "water*"), its draws are occlusion-queried (waterq.h) */ };

typedef struct PhotonVlgStats {
    int version;                     /* 7 (7: + P3h volume counters; 6: + P3c route counters; 5: + S3 upload cache counters; 2: Option A vs20/ps20b pair; 3: + bump pair, c56 occlusion; 4: + skin pair) */
    int wanted;                      /* the load-time conditions passed */
    volatile LONG active;            /* hooks are doing work (cleared by a failed guard or a load failure) */
    int shadow_hooked, api_hooked;   /* the per-class RVA checks passed and the hooks are in */
    int nvs, nps;                    /* static ids in photon_vlg_vs20.ids / photon_vlg_ps20b.ids (0: vlg pair not used) */
    char reason[160];                /* why lighting is off, or "on" */
    /* counters (cumulative) */
    volatile LONG64 vs_requests;     /* SetVertexShader("vertexlit_and_unlit_generic_vs20", ...) seen while active */
    volatile LONG64 vs_renames;      /* forwarded as photon_vlg_vs20 */
    volatile LONG64 ps_requests;     /* SetPixelShader("vertexlit_and_unlit_generic_ps20b", ...) seen while active */
    volatile LONG64 ps_renames;      /* forwarded as photon_vlg_ps20b */
    volatile LONG64 vs_rejects;      /* pilot PS static, but the pass VS was not renamed: PS kept stock */
    volatile LONG64 pair_reverts;    /* a renamed VS (or PS) sent back to its stock name by the pairing rule */
    volatile LONG64 pair_violations; /* TakeSnapshot found the shadow holding one of ours with a stock other: must be 0 */
    volatile LONG64 stale_handles;   /* TakeSnapshot: a handle of ours with an index our file lacks (reused): ignored */
    volatile LONG64 fog_blocks;      /* listed VS static not renamed because r_pixelfog = 0 or dxlevel < 90 */
    volatile LONG64 load_fallbacks;  /* our name returned the invalid handle: re-sent with the stock name */
    volatile LONG64 snapshots_marked, snapshot_clears;
    volatile LONG64 rp_total, rp_ours;   /* RenderPass calls, those on a marked snapshot */
    volatile LONG64 uploads;             /* full light blocks written (enable = 1) */
    volatile LONG64 flag_off_writes;     /* c32 = 0 written */
    volatile LONG64 ours_no_vsidx;       /* marked snapshot, but no VS index known (no slot 40, no manager) (stock look) */
    volatile LONG64 vsidx_from_mgr;      /* marked draws whose VS index came from stock's shader manager (cmd buffer) */
    volatile LONG64 ours_unlit;          /* marked snapshot, VS index without DL or with SL/SLL/LP (stock look) */
    volatile LONG64 ours_ff_fog;         /* marked snapshot drawn with D3D fixed-function fog on (r_pixelfog changed) */
    volatile LONG64 write_fail;          /* photon_d3d_set_ps_const failed (no device, ...) */
    volatile LONG64 lights_set, layout_ok, layout_mismatch;
    volatile LONG64 config_reloads;
    volatile LONG64 handle_table_full;   /* more distinct shader handles of ours than the table holds (net weakened) */
    /* version 3 */
    int nbvs, nbps;                      /* static ids in photon_bump_vs20.ids / photon_bump_ps20b.ids (0: not used) */
    volatile LONG64 bump_vs_renames, bump_ps_renames;   /* the bump share of vs_renames / ps_renames */
    volatile LONG64 rp_ours_bump;        /* RenderPass on a snapshot marked as a bump pair of ours */
    volatile LONG64 occ_writes;          /* c56 written alone (draws of ours without the light block) */
    /* version 4 */
    int nsvs, nsps;                      /* static ids in photon_skin_vs20.ids / photon_skin_ps20b.ids (0: not used) */
    volatile LONG64 skin_vs_renames, skin_ps_renames;   /* the skin share of vs_renames / ps_renames */
    volatile LONG64 rp_ours_skin;        /* RenderPass on a snapshot marked as a skin pair of ours */
    volatile LONG64 ours_skin_unlit;     /* the skin share of ours_unlit (LIGHTING_PREVIEW or VS index >= 48) */
    /* version 5 (S3) */
    volatile LONG64 upload_skips;        /* lit draws whose light block the device already held (no 25-register write) */
    volatile LONG64 flag_on_writes;      /* of those, c32 restored alone (flag_off had cleared it) */
    volatile LONG64 device_resets;       /* RestoreShaderObjects seen: the constant caches were dropped */
    /* version 6 (P3c, analysis/15d section 3): lit draws of ours by the call-order route, per pair (vlg, bump, skin) */
    volatile LONG64 route_engine[3], route_override[3];
    /* version 7 (P3h): lit draws of ours while photon_ambient is 1 at load */
    volatile LONG64 vol_draws;           /* with weight > 0: the volume is in their ambient */
    volatile LONG64 vol_off[5];          /* weight 0 by PHOTON_AMB_OFF_*: setting, route, gate (r_shader_srgb, mat_fullbright), range, s15 not at rest */
    volatile LONG64 vol_const_writes, vol_weight_writes;   /* c59..c62 written, c64 written */
    /* version 8 (P3h, analysis/15e): engine-route draws of the static-prop array signature (130 then 128), kept on the cube; part of vol_off[route] */
    volatile LONG64 vol_off_array;
    /* version 9 (P5, analysis/18): lit draws of ours with the light grid (weight > 0) by pair, c63 writes */
    volatile LONG64 lg_draws[3], lg_const_writes;
    /* version 10 (P6 stage 1): lit draws of ours inside a suppressed DrawModelExecute */
    volatile LONG64 p6_draws;
    /* version 11 (T4.5 stage 2b): replayed copies relit at their own origin, and those of them sunlit */
    volatile LONG64 relit, relit_sun;
} PhotonVlgStats;

extern PhotonVlgStats g_photon_vlg;

/* From load_stock(): decides whether lighting is wanted (build, env/cfg, installed files). */
void photon_vlg_setup(HMODULE stock, int build_ok_and_wrapping);
/* From install_hooks(), after photon_d3d_install(): RVA checks and hooks for ShaderShadow010 / ShaderApi030. */
void photon_vlg_install(PhotonWrapClass *cls);
/* Snapshot marks (PHOTON_VLG_SNAP_*) of a snapshot id; for tests. */
int photon_vlg_snapshot_flags(int id);
/* The device's shader constants may be gone (device Reset): drop the caches of c32..c56 and c59..c64. */
void photon_vlg_device_state_lost(void);
/* photon_ambient is 1 and lighting is wanted and on: the bake has a reason to run */
int photon_vlg_ambient_wanted(void);
int photon_vlg_lights_wanted(int *k, float *cutoff);   /* P5: photon_lights now (photon_lighting on); photon_cluster_k, photon_light_cutoff */
int photon_vlg_lights_split(void);                      /* P5b: photon_cluster_ks now */
void photon_vlg_cube_wanted(int *blend, int *pick);   /* P4b: photon_cubemap_blend / _pick at load (photon_lighting on) */
/* Re-reads photon.cfg if its time stamp changed (force: always). Returns 1 if the config changed. */
int photon_vlg_poll_config(int force);
/* Per frame (Present): config poll every 120 frames, summary every 600. */
void photon_vlg_frame(void);
void photon_vlg_dump(const char *why);
/* photon_frames.csv: extra columns while lighting is wanted (header = 1: names; 0: per-frame deltas). */
void photon_vlg_csv(FILE *f, int header);
/* T4.5 stage 2b (p6.c, PHOTON_BATCH=replay): the last lit suppressed pass's light block before its origin-dependent parts. take copies it
 * into ctx (photon_vlg_relight_size() bytes) and clears it; relight writes c32..c55, c59..c63 and c64 of a replayed copy at origin into
 * psc (c0..c64 of the recorded state), sun = its directional LightDesc_t or NULL. relight returns 0 when ctx holds nothing (an unlit pass). */
size_t photon_vlg_relight_size(void);
void photon_vlg_relight_take(void *ctx);
int photon_vlg_relight(const void *ctx, const float origin[3], const void *sun, float psc[65][4]);
int photon_vlg_api_matrix(int mode, float out[16]);   /* T4.2: stock ShaderAPI GetMatrix (0 VIEW, 1 PROJECTION), row-vector D3D layout */
int photon_vlg_model_pass(int *skinned);   /* T4.2: inside a RenderPass of one of our model pairs (device thread) */
int photon_vlg_shadows_wanted(void);   /* UI1: sun shadows wanted now (photon_shadows, else photon_preset >= 1) */
void photon_vlg_shadow_settings(int *res, float *dist, int *soft, float *strength);   /* UI1: live shadow settings with the preset applied */
void photon_vlg_shadow_casters(int *props, int *chars);
int photon_vlg_light_shadows(int *res);
int photon_vlg_toon(void);               /* TOON: photon_toon (env PHOTON_TOON) */
void photon_vlg_pt_params(int *rays, int *bounces);   /* L4 / PT-RT: paths per sample per pass, bounces */
int photon_vlg_pathtrace(void);          /* L4: photon_pathtrace 1 (env PHOTON_PATHTRACE) */
int photon_vlg_pathtrace_rt(void);       /* PT-RT: photon_pathtrace 2 */
int photon_vlg_nanite(void);
float photon_vlg_setting(const char *key, int *found);   /* a setting by name, resolved (incl. shader packs) */             /* NANITE: photon_nanite (env PHOTON_NANITE) */
int photon_vlg_auto_lod(void);           /* F3: photon_auto_lod (env PHOTON_LOD) */
int photon_vlg_rebake(int *voxel);       /* L3: photon_rebake (preset applied); *voxel 8 or 4 */
int photon_vlg_mod_dir(wchar_t *out, unsigned long cap);   /* the garrysmod directory (PHOTON_GAME_DIR, else beside the DLL) */   /* L1: photon_light_shadows (0..4) with the preset applied; *res the tile size */   /* UI3: photon_shadow_props / photon_shadow_characters */
int photon_vlg_taa(void);
float photon_vlg_taa_sharpen(void);
int photon_vlg_upscale(void);   /* UPSCALE: 0 the engine's, 1 Photon TAAU, 2 Photon bilinear (test) */   /* AA: photon_taa_sharpen, 0..1 */          /* AA: photon_taa (0 off, 1 on), live */
int photon_vlg_target_fps(void);   /* UI2: the dynamic-quality target, 0 off */
int photon_vlg_dyn_order(char *out, int cap);   /* DYN: photon_dyn_order (comma list of step names); 0 if empty (the default order) */
#endif
