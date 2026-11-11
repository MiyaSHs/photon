/* Photon_Debug30 (T3.3): Photon_Debug on our own SM3 bytecode (analysis/11 §6.2). The VCS files come from
 * src/shaderdll/hlsl via scripts/build_shaders.sh (d3dcompiler_47 + tools/vcs_write.py) and are installed loose into
 * garrysmod/shaders/fxc/ by scripts/install_proxy.sh:
 *   photon_debug30_vs30: o.pos = mul(float4(pos.xyz, 1), cModelViewProj @ c4..c7), no other outputs;
 *   photon_debug30_ps30: oC0 = c0.
 * Both have one static and one dynamic combo (index 0). Output = $photoncolor * $color, alpha = $alpha, written to c0
 * as is (no doubling: the PS is "mov oC0, c0", unlike fillrate_ps20b's 0.5 * c0).
 * Without SupportsShaderModel_3_0 (HW slot 64) it takes Photon_Debug's fillrate path. The decision is made in
 * InitShaderParams and both DrawElements phases from the same constant hardware query, so it never differs between
 * the snapshot and the dynamic phase. The SM3 VS has no skinning, so HW skinning is advertised only on the fallback.
 * One pass; *ctx stays NULL. */
#include "photon_debug.h"

static const ShaderParamInfo k_params[] = {
    { "$photoncolor", "Photon_Debug30 output colour (multiplied by $color)", SHADER_PARAM_TYPE_COLOR, "[1 0 1]", 0 },
};

static int use_sm3(void) { return hw_bool(g_photon_env.hw, HW_SupportsShaderModel_3_0); }

static void dbg30_init_params(IMaterialVar **p, const char *mat) {
    (void)mat;
    if (!mv_IsDefined(p[P_PHOTONCOLOR])) mv_SetVecValue3(p[P_PHOTONCOLOR], 1.0f, 0.0f, 1.0f);
    if (!use_sm3()) mv_SetIntValue(p[P_FLAGS2], mv_int(p[P_FLAGS2]) | MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
}

static void dbg30_draw(IMaterialVar **p, int mod, IShaderShadow *sh, IShaderDynamicAPI *api, int vc,
                       CBasePerMaterialContextData **ctx) {
    (void)mod; (void)vc; (void)ctx;
    if (!use_sm3()) { photon_fillrate_draw(p, sh, api); return; }
    if (sh) {
        sh_VertexShaderVertexFormat(sh, VERTEX_POSITION, 1, NULL, 0);
        sh_SetShader(sh, SH_SetVertexShader, "photon_debug30_vs30", 0);
        sh_SetShader(sh, SH_SetPixelShader, "photon_debug30_ps30", 0);
        ps_draw(p, sh, 1);
    } else {
        const float *pc = mv_vec(p[P_PHOTONCOLOR]), *col = mv_vec(p[P_COLOR]);
        float c[4] = { pc[0] * col[0], pc[1] * col[1], pc[2] * col[2], mv_vec(p[P_ALPHA])[0] };
        api_SetConstant(api, API_SetPixelShaderConstant, 0, c, 1, 0);
        api_call1(api, API_SetVertexShaderIndex, 0);
        api_call1(api, API_SetPixelShaderIndex, 0);
        ps_draw(p, NULL, 1);
    }
}

static PhotonShaderStats s_stats_debug30;

const PhotonShaderDesc photon_debug30_desc = {
    .name = "Photon_Debug30", .nparams = 1, .params = k_params, .flags = 1 /* SHADER_NOT_EDITABLE */,
    .init_params = dbg30_init_params, .draw = dbg30_draw, .stats = &s_stats_debug30,
};

/* Photon_Debug32 (T3.4a2): the test shader for direct PS constants c32+. Photon_Debug30's vertex shader with
 * photon_debug32_ps30, whose output is c32 exactly ("mov oC0, c32"). The dynamic phase writes no PS constant: c32 is
 * not part of shaderapi's constant state (32 registers, analysis/13 R15), only the proxy writes it, straight to the
 * D3D9 device (src/proxy_shaderapi/d3dconst.c; test mode PHOTON_TEST_C32="r g b a" at every RenderPass). Without
 * SM3 it draws fillrate magenta, so the c32 test can never pass on the fallback. $photoncolor is accepted (the
 * harness sets it) and ignored. */
static void dbg32_init_params(IMaterialVar **p, const char *mat) {
    (void)mat;
    if (!mv_IsDefined(p[P_PHOTONCOLOR])) mv_SetVecValue3(p[P_PHOTONCOLOR], 1.0f, 0.0f, 1.0f);
    if (!use_sm3()) mv_SetIntValue(p[P_FLAGS2], mv_int(p[P_FLAGS2]) | MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
}

static void dbg32_draw(IMaterialVar **p, int mod, IShaderShadow *sh, IShaderDynamicAPI *api, int vc,
                       CBasePerMaterialContextData **ctx) {
    (void)mod; (void)vc; (void)ctx;
    if (!use_sm3()) { static const float magenta[3] = { 1.0f, 0.0f, 1.0f }; photon_fillrate_draw_rgb(p, sh, api, magenta); return; }
    if (sh) {
        sh_VertexShaderVertexFormat(sh, VERTEX_POSITION, 1, NULL, 0);
        sh_SetShader(sh, SH_SetVertexShader, "photon_debug30_vs30", 0);
        sh_SetShader(sh, SH_SetPixelShader, "photon_debug32_ps30", 0);
        ps_draw(p, sh, 1);
    } else {
        api_call1(api, API_SetVertexShaderIndex, 0);
        api_call1(api, API_SetPixelShaderIndex, 0);
        ps_draw(p, NULL, 1);
    }
}

static PhotonShaderStats s_stats_debug32;

const PhotonShaderDesc photon_debug32_desc = {
    .name = "Photon_Debug32", .nparams = 1, .params = k_params, .flags = 1 /* SHADER_NOT_EDITABLE */,
    .init_params = dbg32_init_params, .draw = dbg32_draw, .stats = &s_stats_debug32,
};

/* Photon_DebugVol (P3d): photon_debugvol_ps30 samples the ambient volume on s15, which only the proxy binds
 * (src/proxy_shaderapi/ambvol.c). No texture is declared to shaderapi on purpose: that is the route under test. */
static void dbgvol_draw(IMaterialVar **p, int mod, IShaderShadow *sh, IShaderDynamicAPI *api, int vc,
                        CBasePerMaterialContextData **ctx) {
    (void)mod; (void)vc; (void)ctx;
    if (!use_sm3()) { static const float magenta[3] = { 1.0f, 0.0f, 1.0f }; photon_fillrate_draw_rgb(p, sh, api, magenta); return; }
    if (sh) {
        sh_VertexShaderVertexFormat(sh, VERTEX_POSITION, 1, NULL, 0);
        sh_SetShader(sh, SH_SetVertexShader, "photon_debug30_vs30", 0);
        sh_SetShader(sh, SH_SetPixelShader, "photon_debugvol_ps30", 0);
        ps_draw(p, sh, 1);
    } else {
        api_call1(api, API_SetVertexShaderIndex, 0);
        api_call1(api, API_SetPixelShaderIndex, 0);
        ps_draw(p, NULL, 1);
    }
}

static PhotonShaderStats s_stats_debugvol;

const PhotonShaderDesc photon_debugvol_desc = {
    .name = "Photon_DebugVol", .nparams = 1, .params = k_params, .flags = 1 /* SHADER_NOT_EDITABLE */,
    .init_params = dbg32_init_params, .draw = dbgvol_draw, .stats = &s_stats_debugvol,
};
