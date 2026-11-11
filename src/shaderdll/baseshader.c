/* baseshader.c: shared IShader vtable (CBaseShader replacement) for every Photon shader. analysis/11 §5.
 * Behaviour mirrored from the public shaderlib.lib (LIB 03_BaseShader.obj disassembly, analysis/11 §0):
 *   - DrawElements: snapshot -> SetInitialShadowState, then the shader body; dynamic -> the shader body. Stock keeps
 *     params/api/shadow in process-global statics; we pass them explicitly (no mutable globals on the draw path).
 *   - SetInitialShadowState: SetDefaultState, then FLAGS bits 15/16/13/10/28/29.
 *   - Draw(): snapshot: if config byte +0x42 && !(FLAGS & NO_DEBUG_OVERRIDE): EnableDepthWrites(1),
 *     EnableBlending(0); then TakeSnapshot. Dynamic: DrawSnapshot(make).
 *   - IsTranslucent = (FLAGS >> 21) & 1. */
#include <windows.h>
#include "baseshader.h"
#include "log.h"

/* Standard 13 params: names, types, defaults and flags as stock (C §3 table, checked field by field against stock
 * stdshader_dbg Fillrate by tests/shaderhost). Help texts are our own. */
const ShaderParamInfo g_photon_standard_params[P_NUM_STANDARD] = {
    { "$flags",                  "material flags",                 SHADER_PARAM_TYPE_INTEGER, "0", SHADER_PARAM_NOT_EDITABLE },
    { "$flags_defined",          "which material flags are set",   SHADER_PARAM_TYPE_INTEGER, "0", SHADER_PARAM_NOT_EDITABLE },
    { "$flags2",                 "material flags 2",               SHADER_PARAM_TYPE_INTEGER, "0", SHADER_PARAM_NOT_EDITABLE },
    { "$flags_defined2",         "which material flags 2 are set", SHADER_PARAM_TYPE_INTEGER, "0", SHADER_PARAM_NOT_EDITABLE },
    { "$color",                  "colour modulation",              SHADER_PARAM_TYPE_COLOR,   "[1 1 1]", 0 },
    { "$alpha",                  "alpha modulation",               SHADER_PARAM_TYPE_FLOAT,   "1.0", 0 },
    { "$basetexture",            "base texture",                   SHADER_PARAM_TYPE_TEXTURE, "shadertest/BaseTexture", 0 },
    { "$frame",                  "base texture frame",             SHADER_PARAM_TYPE_INTEGER, "0", 0 },
    { "$basetexturetransform",   "base texture transform",         SHADER_PARAM_TYPE_MATRIX,
      "center .5 .5 scale 1 1 rotate 0 translate 0 0", 0 },
    { "$flashlighttexture",      "flashlight spot texture",        SHADER_PARAM_TYPE_TEXTURE, "effects/flashlight001", SHADER_PARAM_NOT_EDITABLE },
    { "$flashlighttextureframe", "flashlight texture frame",       SHADER_PARAM_TYPE_INTEGER, "0", SHADER_PARAM_NOT_EDITABLE },
    { "$color2",                 "colour modulation 2",            SHADER_PARAM_TYPE_COLOR,   "[1 1 1]", 0 },
    { "$srgbtint",               "tint in sRGB space",             SHADER_PARAM_TYPE_COLOR,   "[1 1 1]", 0 },
};

static const ShaderParamInfo *param_info(const IShader *self, int i) {
    if (i < 0) return NULL;
    if (i < P_NUM_STANDARD) return &g_photon_standard_params[i];
    i -= P_NUM_STANDARD;
    return i < self->desc->nparams ? &self->desc->params[i] : NULL;
}

static const char *bs_GetName(const IShader *self) { return self->desc->name; }
static const char *bs_GetFallbackShader(const IShader *self, IMaterialVar **p) {
    return self->desc->fallback ? self->desc->fallback(p) : NULL;
}
static int bs_GetNumParams(const IShader *self) { return P_NUM_STANDARD + self->desc->nparams; }
static void bs_InitShaderParams(IShader *self, IMaterialVar **p, const char *mat) {
    if (self->desc->init_params) self->desc->init_params(p, mat);
}
static void bs_InitShaderInstance(IShader *self, IMaterialVar **p, IShaderInit *init, const char *mat, const char *tg) {
    if (self->desc->init_instance) self->desc->init_instance(p, init, mat, tg);
}

static void initial_shadow_state(IMaterialVar **p, IShaderShadow *sh) {
    int f = mv_int(p[P_FLAGS]);
    sh_call0(sh, SH_SetDefaultState);
    if (f & MATERIAL_VAR_IGNOREZ) { sh_call1(sh, SH_EnableDepthTest, 0); sh_call1(sh, SH_EnableDepthWrites, 0); }
    if (f & MATERIAL_VAR_DECAL) { sh_call1(sh, SH_EnablePolyOffset, 1); sh_call1(sh, SH_EnableDepthWrites, 0); }
    if (f & MATERIAL_VAR_NOCULL) sh_call1(sh, SH_EnableCulling, 0);
    if (f & MATERIAL_VAR_ZNEARER) sh_call1(sh, SH_DepthFunc, SHADER_DEPTHFUNC_NEARER);
    if (f & MATERIAL_VAR_WIREFRAME) sh_call2(sh, SH_PolyMode, SHADER_POLYMODEFACE_FRONT_AND_BACK, SHADER_POLYMODE_LINE);
    if (f & MATERIAL_VAR_ALLOWALPHATOCOVERAGE) sh_call1(sh, SH_EnableAlphaToCoverage, 1);
}

void ps_draw(IMaterialVar **p, IShaderShadow *sh, int make_draw) {
    if (sh) {
        const unsigned char *cfg = g_photon_env.cfg;
        if (cfg && cfg[CFG_DRAW_OVERRIDE_BYTE] && !(mv_int(p[P_FLAGS]) & MATERIAL_VAR_NO_DEBUG_OVERRIDE)) {
            sh_call1(sh, SH_EnableDepthWrites, 1);
            sh_call1(sh, SH_EnableBlending, 0);
        }
        sys_TakeSnapshot(g_photon_env.sys);
    } else {
        sys_DrawSnapshot(g_photon_env.sys, make_draw ? 1 : 0);
    }
}

static void bs_DrawElements(IShader *self, IMaterialVar **p, int mod, IShaderShadow *sh, IShaderDynamicAPI *api,
                            int vc, CBasePerMaterialContextData **ctx) {
    const PhotonShaderDesc *d = self->desc;
    if (sh) {
        long n = InterlockedIncrement(&d->stats->snapshot);
        if (n == 1) photon_log("%s: first snapshot (mod %d)", d->name, mod);
        initial_shadow_state(p, sh);
        d->draw(p, mod, sh, NULL, vc, ctx);
    } else if (api) {
        long n = InterlockedIncrement(&d->stats->dynamic);
        long tid = (long)GetCurrentThreadId(), prev = InterlockedExchange(&d->stats->dyn_tid, tid);
        if (prev != tid && InterlockedIncrement(&d->stats->tid_changes) <= 16)
            photon_log("%s: dynamic draws on thread %ld from #%ld", d->name, tid, n);
        else if (n % 600 == 0) photon_log("%s: dynamic draw #%ld", d->name, n);
        d->draw(p, mod, NULL, api, vc, ctx);
    }
}

static const char *bs_GetParamName(const IShader *self, int i) {
    const ShaderParamInfo *pi = param_info(self, i); return pi ? pi->name : NULL;
}
static const char *bs_GetParamHelp(const IShader *self, int i) {
    const ShaderParamInfo *pi = param_info(self, i); return pi ? pi->help : "";
}
static int bs_GetParamType(const IShader *self, int i) {
    const ShaderParamInfo *pi = param_info(self, i); return pi ? pi->type : 0;
}
static const char *bs_GetParamDefault(const IShader *self, int i) {
    const ShaderParamInfo *pi = param_info(self, i); return pi ? pi->def : "";
}
static int bs_GetParamFlags(const IShader *self, int i) {
    const ShaderParamInfo *pi = param_info(self, i); return pi ? pi->flags : 0;
}
static int bs_ComputeModulationFlags(IShader *self, IMaterialVar **p, IShaderDynamicAPI *api) {
    return self->desc->modulation ? self->desc->modulation(p, api) : 0;
}
static msbool bs_NeedsPow2FB(const IShader *self, IMaterialVar **p, int b) { (void)self; (void)p; (void)b; return 0; }
static msbool bs_NeedsFullFB(const IShader *self, IMaterialVar **p, int b) { (void)self; (void)p; (void)b; return 0; }
static msbool bs_IsTranslucent(const IShader *self, IMaterialVar **p) {
    (void)self; return (mv_int(p[P_FLAGS]) & MATERIAL_VAR_TRANSLUCENT) ? 1 : 0;
}
static int bs_GetFlags(const IShader *self) { return self->desc->flags; }
static void *bs_GetSoftwareVertexShader(const IShader *self) { (void)self; return NULL; }
static void bs_OnInitShaderParams(IShader *self, IMaterialVar **p, const char *m) { (void)self; (void)p; (void)m; }
static void bs_OnInitShaderInstance(IShader *self, IMaterialVar **p, IShaderInit *i, const char *m) {
    (void)self; (void)p; (void)i; (void)m;
}
static void bs_OnDrawElements(IShader *self, IMaterialVar **p, IShaderShadow *s, IShaderDynamicAPI *a, int vc,
                              CBasePerMaterialContextData **ctx) {
    (void)self; (void)p; (void)s; (void)a; (void)vc; (void)ctx;
}

const IShaderVtbl g_photon_shader_vt = {
    bs_GetName, bs_GetFallbackShader, bs_GetNumParams, bs_InitShaderParams, bs_InitShaderInstance, bs_DrawElements,
    bs_GetParamName, bs_GetParamHelp, bs_GetParamType, bs_GetParamDefault, bs_ComputeModulationFlags,
    bs_NeedsPow2FB, bs_NeedsFullFB, bs_IsTranslucent, bs_GetParamFlags, bs_GetFlags,
    bs_GetSoftwareVertexShader, bs_OnInitShaderParams, bs_OnInitShaderInstance, bs_OnDrawElements,
};
