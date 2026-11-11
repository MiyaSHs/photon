/* baseshader.h: the C replacement for Valve's CBaseShader (analysis/11 §4-5). One shared 20-slot IShader vtable
 * dispatches through a per-shader const descriptor. Threading rule (analysis/11 §2): no mutable globals on the draw
 * path; params/shadow/api are passed explicitly. The only shared state is the interface set (written once in
 * Connect) and the per-shader atomic counters. */
#ifndef PHOTON_BASESHADER_H
#define PHOTON_BASESHADER_H

#include "abi.h"

/* Interfaces fetched by ShaderDLL004::Connect; read-only while shaders draw. */
typedef struct PhotonEnv {
    IHWConfig *hw;                    /* MaterialSystemHardwareConfig012 (the proxy's wrapper in game) */
    const unsigned char *cfg;         /* VMaterialSystemConfig002 (plain struct) */
    IShaderSystem *sys;               /* ShaderSystem002 */
    void *cvar;                       /* VEngineCvar007, optional, unused until ConVars exist */
} PhotonEnv;
extern PhotonEnv g_photon_env;

/* atomic counters; dyn_tid = thread of the latest dynamic draw, tid_changes = how often it changed (diagnostic: shows
 * the MatQueue render thread taking over; logged for the first 16 changes only) */
typedef struct PhotonShaderStats { volatile long snapshot, dynamic, dyn_tid, tid_changes; } PhotonShaderStats;

typedef struct PhotonShaderDesc {
    const char *name;                                  /* GetName */
    int nparams; const ShaderParamInfo *params;        /* shader-specific params, indices 13.. */
    int flags;                                         /* GetFlags */
    int stock_name;                                    /* documentation only: 1 = meant to shadow a stock name. The
                                                          filter decides by name (no "Photon_" prefix = shadow:
                                                          base mode + PHOTON_SHADOW opt-in only), shaderdll.c */
    const char *(*fallback)(IMaterialVar **p);         /* NULL = none */
    void (*init_params)(IMaterialVar **p, const char *mat);
    void (*init_instance)(IMaterialVar **p, IShaderInit *init, const char *mat, const char *texGroup);
    void (*draw)(IMaterialVar **p, int mod, IShaderShadow *sh, IShaderDynamicAPI *api, int vc,
                 CBasePerMaterialContextData **ctx);
    int  (*modulation)(IMaterialVar **p, IShaderDynamicAPI *api);   /* NULL = 0 */
    PhotonShaderStats *stats;                          /* mutable counters (atomic) */
} PhotonShaderDesc;

struct IShader { const IShaderVtbl *vt; const PhotonShaderDesc *desc; };  /* +8 = our descriptor */

extern const IShaderVtbl g_photon_shader_vt;
extern const ShaderParamInfo g_photon_standard_params[P_NUM_STANDARD];

/* CBaseShader::Draw(): one pass. Snapshot: config override then TakeSnapshot; dynamic: DrawSnapshot(make). Both
 * phases of a shader must call it the same number of times (<= 4) [confirmed C §4.1]. */
void ps_draw(IMaterialVar **p, IShaderShadow *sh, int make_draw);

#endif
