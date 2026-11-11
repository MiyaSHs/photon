/* photon_lightblock.h: the Photon lighting v0 constant block read by photon_vlg_ps30 (T3.4b).
 *
 * Shader side: src/shaderdll/hlsl/photon_vlg_ps2x.fxc (the header comment there is the same layout). The block is
 * written with photon_d3d_set_ps_const() (d3dconst.h) straight to the D3D9 device on the draw thread at RenderPass,
 * never through IShaderDynamicAPI (its PS constant shadow holds 32 registers; analysis/14 erratum).
 * tools/t34_build_vlg.py parses the PHOTON_LB_* register defines below and checks them against the constant table
 * of every compiled photon_vlg_ps30 combo, so the two sides cannot drift silently.
 *
 *   c32       flags   x enable (> 0.5 per-pixel path, else the exact stock result), y light count 0..4,
 *                     z half-Lambert 0/1 (the VS HALFLAMBERT static), w vertex-colour weight (1 adds the VS colour,
 *                     e.g. VHV static light when the VS ran with DYNAMIC_LIGHT=0; 0 replaces it)
 *   c33..c38  ambient cube +X -X +Y -Y +Z -Z (linear; the stock cube format of SetAmbientLightCube; .w unused)
 *   c39..c54  4 lights x 4 registers, light k at c39 + 4k:
 *             +0  position.xyz, w = 1/range^2 (0 = no range window, as stock)
 *             +1  colour.rgb (linear, as the stock VS cLightInfo.color), w = type 0 point, 1 spot, 2 directional
 *             +2  direction.xyz (unit; spot: premultiplied by 1/(cos inner - cos outer)), w = spot exponent
 *             +3  (att0, att1, att2) of 1/(att0 + att1 d + att2 d^2), w = spot: cos outer / (cos inner - cos outer)
 *   c55       tuning  x light scale, y ambient scale, z ambient floor (all channels), w debug view
 *                     (0 off, 1 diffuse lighting only, 2 world normal)
 *   c56       envmap occlusion (lighting plan v2 P1; shader side src/shaderdll/hlsl/photon_envocc_ps.h), folded from
 *             strength s, factor range [kMin, kMax] and knee k by photon_lb_envocc(); x > 0 enables it. It is read by every CUBEMAP combo with diffuse
 *             lighting of the substituted VLG, bump and skin ps_3_0, whether or not c32.x is set (also by the VLG
 *             STATIC_LIGHT_LIGHTMAP combos, which have no c32..c55), so it must be valid for EVERY draw of ours.
 *
 * The per-light math is the stock VS's (common_vs_fxc.h VertexAttenInternal/CosineTermInternal) evaluated per pixel:
 * spot = saturate(pow(max(1e-4, dot(dir', -L) - w3), exponent)) equals stock's
 * saturate(pow(max(1e-4, (cos theta - cosOuter) * 1/(cosInner - cosOuter)), exponent)).
 * The whole block must be written for every draw that uses the Photon path; a zero tuning register is black.
 */
#ifndef PHOTON_LIGHTBLOCK_H
#define PHOTON_LIGHTBLOCK_H

#include <math.h>

#define PHOTON_LB_FLAGS        32   /* g_PhotonFlags */
#define PHOTON_LB_AMBIENT      33   /* g_PhotonAmbientCube[6] */
#define PHOTON_LB_AMBIENT_N    6
#define PHOTON_LB_LIGHTS       39   /* g_PhotonLights[16] */
#define PHOTON_LB_MAX_LIGHTS   4
#define PHOTON_LB_LIGHT_REGS   4
#define PHOTON_LB_TUNING       55   /* g_PhotonTuning */
#define PHOTON_LB_ENVOCC       56   /* g_PhotonEnvOcc (P1) */
#define PHOTON_LB_FIRST        PHOTON_LB_FLAGS
#define PHOTON_LB_COUNT        25   /* c32..c56, one photon_d3d_set_ps_const() call */
/* The two-cubemap blend (P4b, src/shaderdll/hlsl/photon_cubeblend_ps.h): per draw, in every PHOTON_CUBEBLEND combo (the occlusion's) */
#define PHOTON_LB_CUBE_PLANE   57   /* g_PhotonCubePlane: B's weight = saturate(dot(worldPos, xyz) + w) */
#define PHOTON_LB_CUBE         58   /* g_PhotonCube: x = 1 B is bound, 0 off */
/* The ambient volume (P3f, src/shaderdll/hlsl/photon_ambvol_ps.h): per frame c59..c62, per draw c64 */
#define PHOTON_LB_VOL_A        59   /* g_PhotonVolA */
#define PHOTON_LB_VOL_B        60   /* g_PhotonVolB */
#define PHOTON_LB_VOL_MIN      61   /* g_PhotonVolMin */
#define PHOTON_LB_VOL_MAX      62   /* g_PhotonVolMax */
#define PHOTON_LB_DRAW         64   /* g_PhotonDraw: x = photon_debug 3 light count view, y = volume weight, z = light grid weight (P5), w = 0 always
                                     * (skin's select on it keeps FXC from sinking the stock lighting, photon_skin_light_ps.h) */
/* The light grid (P5, src/shaderdll/hlsl/photon_lights_ps.h; layout src/ambient/lg_grid.h): per frame c63 = (the window's first cell, 1 / cell size);
 * the texture on s9 (VLG, skin) / s10 (bump) */
#define PHOTON_LB_GRID         63   /* g_PhotonGrid */
#define PHOTON_LB_GRID_SAMPLER      9
#define PHOTON_LB_GRID_SAMPLER_BUMP 10

#define PHOTON_LB_TYPE_POINT        0.0f
#define PHOTON_LB_TYPE_SPOT         1.0f
#define PHOTON_LB_TYPE_DIRECTIONAL  2.0f

typedef struct PhotonLightBlock {
    float flags[4];
    float ambient[PHOTON_LB_AMBIENT_N][4];
    float lights[PHOTON_LB_MAX_LIGHTS][PHOTON_LB_LIGHT_REGS][4];
    float tuning[4];
    float envocc[4];
} PhotonLightBlock;

_Static_assert(sizeof(PhotonLightBlock) == PHOTON_LB_COUNT * 16, "light block is c32..c56");
_Static_assert(PHOTON_LB_ENVOCC == PHOTON_LB_TUNING + 1, "layout");
_Static_assert(PHOTON_LB_AMBIENT == PHOTON_LB_FLAGS + 1, "layout");
_Static_assert(PHOTON_LB_LIGHTS == PHOTON_LB_AMBIENT + PHOTON_LB_AMBIENT_N, "layout");
_Static_assert(PHOTON_LB_TUNING == PHOTON_LB_LIGHTS + PHOTON_LB_MAX_LIGHTS * PHOTON_LB_LIGHT_REGS, "layout");

/* One light in the shader's format. type: PHOTON_LB_TYPE_*. dir: unit vector the light points along (spot and
 * directional). att0..2: 1/(att0 + att1 d + att2 d^2). range: 0 = no window. falloff: spot exponent. cos_inner,
 * cos_outer: cos of the inner/outer HALF angles. Stock (CommitVertexShaderLighting FUN_180012e70) uses
 * cos(m_Theta * 0.5) / cos(m_Phi * 0.5) of LightDesc_t (full cone angles, after SetLight's clamps), not m_ThetaDot /
 * m_PhiDot; the proxy (vlg.c) passes those. */
static inline void photon_lb_pack_light(float out[PHOTON_LB_LIGHT_REGS][4], float type, const float color[3],
                                        const float pos[3], const float dir[3], float att0, float att1, float att2,
                                        float range, float falloff, float cos_inner, float cos_outer)
{
    float oodot = 0.0f;
    if (type == PHOTON_LB_TYPE_SPOT) {
        float d = cos_inner - cos_outer;
        oodot = d > 1e-6f ? 1.0f / d : 1e6f;
    }
    float dscale = type == PHOTON_LB_TYPE_SPOT ? oodot : 1.0f;
    out[0][0] = pos[0]; out[0][1] = pos[1]; out[0][2] = pos[2];
    out[0][3] = range > 0.0f ? 1.0f / (range * range) : 0.0f;
    out[1][0] = color[0]; out[1][1] = color[1]; out[1][2] = color[2]; out[1][3] = type;
    out[2][0] = dir[0] * dscale; out[2][1] = dir[1] * dscale; out[2][2] = dir[2] * dscale;
    out[2][3] = type == PHOTON_LB_TYPE_SPOT ? falloff : 0.0f;
    out[3][0] = att0; out[3][1] = att1; out[3][2] = att2;
    out[3][3] = type == PHOTON_LB_TYPE_SPOT ? cos_outer * oodot : 0.0f;
}

/* c56 from the occlusion settings: strength s (0 = off: c56.x = 0), the factor range [kmin, kmax] and the knee k. The
 * shader (photon_envocc_ps.h) computes t = saturate(log2(x) * c56.x + c56.y), f = t^2 (3 - 2t),
 * factor = f * c56.z + c56.w = lerp(1, kmin + (kmax - kmin) * f, s), where f is a smoothstep on log2(x) from
 * x = k / 2^PHOTON_LB_ENVOCC_OCTAVES (f = 0) to x = k (f = 1); x = Y(diffuse) / Y(cubemap lowest mip).
 * kmax is raised to kmin when below it. */
#define PHOTON_LB_ENVOCC_OCTAVES 2.0f
static inline void photon_lb_envocc(float out[4], float s, float kmin, float kmax, float k)
{
    if (!(s > 0.0f) || !(k > 0.0f)) { out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f; return; }
    if (s > 1.0f) s = 1.0f;
    if (!(kmin >= 0.0f)) kmin = 0.0f;
    if (!(kmax >= kmin)) kmax = kmin;
    float lo = 1.0f + s * (kmin - 1.0f), hi = 1.0f + s * (kmax - 1.0f), w = PHOTON_LB_ENVOCC_OCTAVES;
    out[0] = 1.0f / w; out[1] = (w - log2f(k)) / w; out[2] = hi - lo; out[3] = lo;
}

#endif
