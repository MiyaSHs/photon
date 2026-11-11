/* abi.h: every game-ABI declaration used by photon_shaders.dll (analysis/11_shaderdll_design.md §3).
 *
 * Our own two interfaces (ShaderDLL004, IShader) are typed vtable structs with static size checks. The interfaces we
 * call into (IShaderShadow, IShaderDynamicAPI, IShaderSystem, IMaterialVar, HWConfig) are slot numbers plus typed
 * inline call wrappers; only the slots we use are listed.
 *
 * MSVC x64 ABI rules (src/README.md, analysis/11 §0/§3):
 *   - ms_abi is the default on this target; `self` is always the first argument.
 *   - A bool RETURN is `msbool` (1 byte in al; MSVC callers test only al).
 *   - A bool ARGUMENT we receive is `int` and is tested with `& 0xff`: stock MSVC code passes e.g. rdx=0x6ffffe9d2201
 *     for `true` (analysis/11 §0, runtime probe) [confirmed].
 *   - A bool argument we pass is `int` 0 or 1 (the callee reads the low byte).
 *   - Enums are `int`; floats go by position (xmm1-3), the 5th+ argument on the stack (handled by the C compiler).
 *   - None of the methods used here returns a UDT, so no hidden return pointer is needed [confirmed, mangled names].
 * Slot evidence: analysis/scratch/m3_contract.md §1-4 (LIB vtables, MS call sites) and the analysis/11 §0 probe of
 * stock stdshader_dbg Fillrate; the [likely] ones are by MINX64 declaration order.
 */
#ifndef PHOTON_SHADERDLL_ABI_H
#define PHOTON_SHADERDLL_ABI_H

#include <stddef.h>

typedef unsigned char msbool;
typedef void *(*CreateInterfaceFn)(const char *name, int *rc);

typedef struct IMaterialVar { void *const *vt; } IMaterialVar;   /* fields read at fixed offsets, see mv_* below */
typedef struct IShaderShadow { void *const *vt; } IShaderShadow;
typedef struct IShaderDynamicAPI { void *const *vt; } IShaderDynamicAPI;
typedef struct IShaderInit { void *const *vt; } IShaderInit;
typedef struct IShaderSystem { void *const *vt; } IShaderSystem;
typedef struct IHWConfig { void *const *vt; } IHWConfig;          /* MaterialSystemHardwareConfig012 */
typedef struct CBasePerMaterialContextData CBasePerMaterialContextData;

/* ---- ShaderDLL004 (IShaderDLLInternal), 6 slots [confirmed C §1, analysis/11 §0] ---- */
typedef struct ShaderDLL ShaderDLL;
typedef struct IShader IShader;
typedef struct ShaderDLLVtbl {
    msbool   (*Connect)(ShaderDLL *self, CreateInterfaceFn f, int bIsMaterialSystem);   /* 0 +0x00 */
    void     (*Disconnect)(ShaderDLL *self, int bIsMaterialSystem);                     /* 1 +0x08 */
    int      (*ShaderCount)(ShaderDLL *self);                                           /* 2 +0x10 */
    IShader *(*GetShader)(ShaderDLL *self, int i);                                      /* 3 +0x18 */
    msbool   (*Connect1)(ShaderDLL *self, CreateInterfaceFn f);                         /* 4 +0x20 = Connect(f,0) */
    void     (*Disconnect0)(ShaderDLL *self);                                           /* 5 +0x28 = Disconnect(0) */
} ShaderDLLVtbl;
_Static_assert(sizeof(ShaderDLLVtbl) == 6 * 8, "ShaderDLL004 has 6 slots");
_Static_assert(offsetof(ShaderDLLVtbl, GetShader) == 0x18, "GetShader at +0x18");
struct ShaderDLL { const ShaderDLLVtbl *vt; };

/* ---- IShader 0-15 + CBaseShader 16-19, 20 slots [confirmed C §2] ---- */
typedef struct IShaderVtbl {
    const char *(*GetName)(const IShader *self);                                                         /* 0  */
    const char *(*GetFallbackShader)(const IShader *self, IMaterialVar **params);                        /* 1  */
    int         (*GetNumParams)(const IShader *self);                                                    /* 2  */
    void        (*InitShaderParams)(IShader *self, IMaterialVar **params, const char *matName);          /* 3  */
    void        (*InitShaderInstance)(IShader *self, IMaterialVar **params, IShaderInit *init,
                                      const char *matName, const char *texGroup);                        /* 4  */
    void        (*DrawElements)(IShader *self, IMaterialVar **params, int modFlags, IShaderShadow *shadow,
                                IShaderDynamicAPI *api, int vertexCompression,
                                CBasePerMaterialContextData **ctx);                                      /* 5  */
    const char *(*GetParamName)(const IShader *self, int i);                                             /* 6  */
    const char *(*GetParamHelp)(const IShader *self, int i);                                             /* 7  */
    int         (*GetParamType)(const IShader *self, int i);                                             /* 8  */
    const char *(*GetParamDefault)(const IShader *self, int i);                                          /* 9  */
    int         (*ComputeModulationFlags)(IShader *self, IMaterialVar **params, IShaderDynamicAPI *api); /* 10 */
    msbool      (*NeedsPowerOfTwoFrameBufferTexture)(const IShader *self, IMaterialVar **p, int bCheck); /* 11 */
    msbool      (*NeedsFullFrameBufferTexture)(const IShader *self, IMaterialVar **p, int bCheck);       /* 12 */
    msbool      (*IsTranslucent)(const IShader *self, IMaterialVar **params);                            /* 13 */
    int         (*GetParamFlags)(const IShader *self, int i);                                            /* 14 */
    int         (*GetFlags)(const IShader *self);                                                        /* 15 */
    void       *(*GetSoftwareVertexShader)(const IShader *self);                    /* 16 CBaseShader-only */
    void        (*OnInitShaderParams)(IShader *self, IMaterialVar **p, const char *mat);                 /* 17 stub */
    void        (*OnInitShaderInstance)(IShader *self, IMaterialVar **p, IShaderInit *i, const char *mat); /* 18 stub */
    void        (*OnDrawElements)(IShader *self, IMaterialVar **p, IShaderShadow *s, IShaderDynamicAPI *a,
                                  int vc, CBasePerMaterialContextData **ctx);                            /* 19 stub */
} IShaderVtbl;
_Static_assert(sizeof(IShaderVtbl) == 20 * 8, "CBaseShader vtable has 20 slots");
_Static_assert(offsetof(IShaderVtbl, DrawElements) == 0x28, "DrawElements at +0x28");
_Static_assert(offsetof(IShaderVtbl, GetFlags) == 0x78, "GetFlags at +0x78");

/* ShaderParamInfo_t: {name +0, help +8, type +0x10, default +0x18, flags +0x20}, 40 bytes [confirmed C §3] */
typedef struct ShaderParamInfo { const char *name, *help; int type; const char *def; int flags; } ShaderParamInfo;
_Static_assert(sizeof(ShaderParamInfo) == 40, "ShaderParamInfo_t is 40 bytes");

enum { SHADER_PARAM_TYPE_TEXTURE = 0, SHADER_PARAM_TYPE_INTEGER = 1, SHADER_PARAM_TYPE_COLOR = 2,
       SHADER_PARAM_TYPE_FLOAT = 7, SHADER_PARAM_TYPE_MATRIX = 10 };
enum { SHADER_PARAM_NOT_EDITABLE = 1 };
/* standard param indices (s_StandardParams, C §3) */
enum { P_FLAGS, P_FLAGS_DEFINED, P_FLAGS2, P_FLAGS_DEFINED2, P_COLOR, P_ALPHA, P_BASETEXTURE, P_FRAME,
       P_BASETEXTURETRANSFORM, P_FLASHLIGHTTEXTURE, P_FLASHLIGHTTEXTUREFRAME, P_COLOR2, P_SRGBTINT, P_NUM_STANDARD };

/* MATERIAL_VAR_* (FLAGS) and MATERIAL_VAR2_* (FLAGS2), MINX64 materialsystem/imaterial.h */
enum { MATERIAL_VAR_NO_DEBUG_OVERRIDE = 1 << 1, MATERIAL_VAR_ZNEARER = 1 << 10, MATERIAL_VAR_NOCULL = 1 << 13,
       MATERIAL_VAR_IGNOREZ = 1 << 15, MATERIAL_VAR_DECAL = 1 << 16, MATERIAL_VAR_TRANSLUCENT = 1 << 21,
       MATERIAL_VAR_WIREFRAME = 1 << 28, MATERIAL_VAR_ALLOWALPHATOCOVERAGE = 1 << 29 };
enum { MATERIAL_VAR2_SUPPORTS_HW_SKINNING = 0x40000 };
enum { VERTEX_POSITION = 0x1 };
enum { MATERIAL_FOG_LINEAR_BELOW_FOG_Z = 2 };
enum { SHADER_POLYMODEFACE_FRONT_AND_BACK = 2, SHADER_POLYMODE_LINE = 1, SHADER_DEPTHFUNC_NEARER = 1 };

/* ---- IShaderShadow (52 slots) ---- */
enum {
    SH_SetDefaultState = 0, SH_DepthFunc = 1, SH_EnableDepthWrites = 2, SH_EnableDepthTest = 3,
    SH_EnablePolyOffset = 4, SH_EnableBlending = 15, SH_BlendFunc = 16, SH_PolyMode = 19, SH_EnableCulling = 20,
    SH_VertexShaderVertexFormat = 22, SH_SetVertexShader = 23, SH_SetPixelShader = 24,
    SH_EnableAlphaToCoverage = 48, SH_NUM_SLOTS = 52
};
/* ---- IShaderDynamicAPI (100 slots; 99 = GMOD_SamplerBorderClamp) ---- */
enum {
    API_GetSceneFogMode = 4, API_SetVertexShaderConstant = 30, API_SetPixelShaderConstant = 31,
    API_GetCurrentNumBones = 34, API_SetVertexShaderIndex = 40, API_SetPixelShaderIndex = 41, API_NUM_SLOTS = 100
};
/* ---- IShaderSystem (ShaderSystem002, 7 slots) ---- */
enum { SYS_TakeSnapshot = 3, SYS_DrawSnapshot = 4, SYS_NUM_SLOTS = 7 };
/* ---- IMaterialVar (31 slots) ---- */
enum { MV_SetFloatValue = 3, MV_SetIntValue = 4, MV_SetVecValue4 = 9, MV_SetVecValue3 = 10, MV_SetVecValue2 = 11,
       MV_IsDefined = 17, MV_NUM_SLOTS = 31 };
/* ---- MaterialSystemHardwareConfig012 (78 slots) ---- */
enum { HW_GetDXSupportLevel = 32, HW_GetMaxDXSupportLevel = 44, HW_SupportsPixelShaders_2_b = 56,
       HW_NeedsShaderSRGBConversion = 62, HW_SupportsShaderModel_3_0 = 64, HW_NUM_SLOTS = 78 };
/* VMaterialSystemConfig002: plain struct; CBaseShader::Draw reads byte +0x42 [confirmed offset, LIB] */
enum { CFG_DRAW_OVERRIDE_BYTE = 0x42 };

/* ---- typed call wrappers ---- */
#define PH_FN(obj, slot, type) ((type)(obj)->vt[slot])

static inline void sh_call0(IShaderShadow *s, int slot) { PH_FN(s, slot, void (*)(IShaderShadow *))(s); }
static inline void sh_call1(IShaderShadow *s, int slot, int a) { PH_FN(s, slot, void (*)(IShaderShadow *, int))(s, a); }
static inline void sh_call2(IShaderShadow *s, int slot, int a, int b) {
    PH_FN(s, slot, void (*)(IShaderShadow *, int, int))(s, a, b);
}
static inline void sh_VertexShaderVertexFormat(IShaderShadow *s, unsigned flags, int ntex, int *dims, int user) {
    PH_FN(s, SH_VertexShaderVertexFormat, void (*)(IShaderShadow *, unsigned, int, int *, int))(s, flags, ntex, dims, user);
}
static inline void sh_SetShader(IShaderShadow *s, int slot, const char *name, int staticIdx) {
    PH_FN(s, slot, void (*)(IShaderShadow *, const char *, int))(s, name, staticIdx);
}
static inline int api_int0(IShaderDynamicAPI *a, int slot) { return PH_FN(a, slot, int (*)(IShaderDynamicAPI *))(a); }
static inline void api_call1(IShaderDynamicAPI *a, int slot, int v) {
    PH_FN(a, slot, void (*)(IShaderDynamicAPI *, int))(a, v);
}
static inline void api_SetConstant(IShaderDynamicAPI *a, int slot, int reg, const float *v, int n, int force) {
    PH_FN(a, slot, void (*)(IShaderDynamicAPI *, int, const float *, int, int))(a, reg, v, n, force);
}
static inline void sys_TakeSnapshot(IShaderSystem *s) { PH_FN(s, SYS_TakeSnapshot, void (*)(IShaderSystem *))(s); }
static inline void sys_DrawSnapshot(IShaderSystem *s, int make) {
    PH_FN(s, SYS_DrawSnapshot, void (*)(IShaderSystem *, int))(s, make);
}
static inline int hw_int(IHWConfig *h, int slot) { return PH_FN(h, slot, int (*)(IHWConfig *))(h); }
static inline int hw_bool(IHWConfig *h, int slot) { return (PH_FN(h, slot, msbool (*)(IHWConfig *))(h) & 0xff) != 0; }

/* IMaterialVar: writes through the vtable (setters keep the other representations and the change id) [likely];
 * reads straight from the fields as stock FAST_MATERIALVAR_ACCESS code does: m_intVal +0x10, m_VecVal float[4] +0x14
 * [confirmed C §3]. One-line fix here if the layout ever changes. */
static inline int mv_int(const IMaterialVar *v) { return *(const int *)((const char *)v + 0x10); }
static inline const float *mv_vec(const IMaterialVar *v) { return (const float *)((const char *)v + 0x14); }
static inline int mv_IsDefined(IMaterialVar *v) { return (PH_FN(v, MV_IsDefined, msbool (*)(IMaterialVar *))(v) & 0xff) != 0; }
static inline void mv_SetIntValue(IMaterialVar *v, int x) { PH_FN(v, MV_SetIntValue, void (*)(IMaterialVar *, int))(v, x); }
static inline void mv_SetFloatValue(IMaterialVar *v, float x) {
    PH_FN(v, MV_SetFloatValue, void (*)(IMaterialVar *, float))(v, x);
}
static inline void mv_SetVecValue3(IMaterialVar *v, float x, float y, float z) {
    PH_FN(v, MV_SetVecValue3, void (*)(IMaterialVar *, float, float, float))(v, x, y, z);
}

#endif
