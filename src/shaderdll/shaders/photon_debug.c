/* Photon_Debug (T3.2): a solid-colour test shader that reuses the stock fillrate_vs20 / fillrate_ps20(b) VCS, so no
 * shader compilation is needed (analysis/11 §6.1, contract recipe analysis/scratch/m3_contract.md §5).
 * Output = $photoncolor * $color, alpha = $alpha. fillrate_ps20b outputs 0.5 * c0 (VCS disassembly, C §4.2), so the
 * constant is doubled; a product above 1.0 saturates at 1.0 in the render target.
 * One pass, modulation combo 0. Snapshot and dynamic phases each call ps_draw() exactly once. */
#include "photon_debug.h"

static const ShaderParamInfo k_params[] = {
    { "$photoncolor", "Photon_Debug output colour (multiplied by $color)", SHADER_PARAM_TYPE_COLOR, "[1 0 1]", 0 },
};

static void dbg_init_params(IMaterialVar **p, const char *mat) {
    (void)mat;
    /* magenta default, so an unconfigured material is obviously ours; MS fills only type defaults afterwards */
    if (!mv_IsDefined(p[P_PHOTONCOLOR])) mv_SetVecValue3(p[P_PHOTONCOLOR], 1.0f, 0.0f, 1.0f);
    /* fillrate_vs20 has a SKINNING combo: advertise HW skinning as stock Fillrate does (analysis/11 §0) */
    mv_SetIntValue(p[P_FLAGS2], mv_int(p[P_FLAGS2]) | MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
}

/* The fillrate_vs20 / fillrate_ps20(b) pass, shared with Photon_Debug30's no-SM3 fallback (photon_debug.h). */
void photon_fillrate_draw(IMaterialVar **p, IShaderShadow *sh, IShaderDynamicAPI *api) {
    photon_fillrate_draw_rgb(p, sh, api, mv_vec(p[P_PHOTONCOLOR]));
}

void photon_fillrate_draw_rgb(IMaterialVar **p, IShaderShadow *sh, IShaderDynamicAPI *api, const float *pc) {
    if (sh) {
        IHWConfig *hw = g_photon_env.hw;
        sh_VertexShaderVertexFormat(sh, VERTEX_POSITION, 1, NULL, 0);
        sh_SetShader(sh, SH_SetVertexShader, "fillrate_vs20", 0);
        if (hw_bool(hw, HW_SupportsPixelShaders_2_b))
            sh_SetShader(sh, SH_SetPixelShader, "fillrate_ps20b", hw_bool(hw, HW_NeedsShaderSRGBConversion));
        else
            sh_SetShader(sh, SH_SetPixelShader, "fillrate_ps20", 0);
        ps_draw(p, sh, 1);
    } else {
        const float *col = mv_vec(p[P_COLOR]);
        float alpha = mv_vec(p[P_ALPHA])[0];
        float c[4] = { 2.0f * pc[0] * col[0], 2.0f * pc[1] * col[1], 2.0f * pc[2] * col[2], 2.0f * alpha };
        api_SetConstant(api, API_SetPixelShaderConstant, 0, c, 1, 0);
        int fog = api_int0(api, API_GetSceneFogMode);
        int bones = api_int0(api, API_GetCurrentNumBones);
        /* fillrate_vs20 dynamic combos: DOWATERFOG + 2 * SKINNING (stock order, analysis/11 §0) */
        api_call1(api, API_SetVertexShaderIndex, (fog == MATERIAL_FOG_LINEAR_BELOW_FOG_Z) + 2 * (bones > 0));
        api_call1(api, API_SetPixelShaderIndex, 0);
        ps_draw(p, NULL, 1);
    }
}

static void dbg_draw(IMaterialVar **p, int mod, IShaderShadow *sh, IShaderDynamicAPI *api, int vc,
                     CBasePerMaterialContextData **ctx) {
    (void)mod; (void)vc; (void)ctx;   /* *ctx stays NULL: no per-material context */
    photon_fillrate_draw(p, sh, api);
}

static PhotonShaderStats s_stats_debug;

const PhotonShaderDesc photon_debug_desc = {
    .name = "Photon_Debug", .nparams = 1, .params = k_params, .flags = 1 /* SHADER_NOT_EDITABLE, like stock debug */,
    .init_params = dbg_init_params, .draw = dbg_draw, .stats = &s_stats_debug,
};

#ifdef PHOTON_TEST_CLONES
/* Test-only clones, compiled only into the photon_shaders_test variant (never installed) and registered only when
 * PHOTON_TEST_FILTER=1 (tests/shaderhost item 4). They exercise the registration filter: a stock name with the
 * stock_name flag (dropped unless base mode + PHOTON_SHADOW opt-in), a stock name WITHOUT the flag (the name alone
 * must decide, same result) and a GMod name (always dropped). */
static PhotonShaderStats s_stats_test_vlg, s_stats_test_lmg, s_stats_test_bloom;
const PhotonShaderDesc photon_test_vlg_desc = {
    .name = "VertexLitGeneric", .nparams = 1, .params = k_params, .stock_name = 1,
    .init_params = dbg_init_params, .draw = dbg_draw, .stats = &s_stats_test_vlg,
};
const PhotonShaderDesc photon_test_lmg_desc = {
    .name = "LightmappedGeneric", .nparams = 1, .params = k_params, .stock_name = 0 /* deliberately "forgotten" */,
    .init_params = dbg_init_params, .draw = dbg_draw, .stats = &s_stats_test_lmg,
};
const PhotonShaderDesc photon_test_bloom_desc = {
    .name = "g_bloom", .nparams = 1, .params = k_params,
    .init_params = dbg_init_params, .draw = dbg_draw, .stats = &s_stats_test_bloom,
};
#endif
