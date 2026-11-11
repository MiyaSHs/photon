/* PBR step 2 (analysis/41): a Source material's roughness and metalness as per-material constants, from what its .vmt carries (the rules
 * of tools/pbr_derive.py without the textures): roughness from $phongexponent (Blinn-Phong -> GGX), else the material type's prior,
 * pulled toward glossy by $envmap (and its tint); metalness 1 for a metal type that reflects ($envmap), 0.25 for a metal type that does
 * not (painted), else 0. The type from $surfaceprop, else keywords in the material path. Pure C; files through the caller's reader. */
#ifndef PHOTON_MTL_PBR_H
#define PHOTON_MTL_PBR_H
#include "mtl_tex.h"
typedef struct MtlPbr { float roughness, metalness; int type; int skip; } MtlPbr;   /* skip: not a lit surface (tools, sky, decals, unlit) */
/* material: e.g. "brick/brickwall001a" (no "materials/", no ".vmt"). Unknown or unreadable: the default (roughness 0.5, metal 0). */
void mtl_pbr_get(const char *material, MtlReadFn read, void *ctx, MtlPbr *out);
const char *mtl_pbr_type_name(int type);
#endif
