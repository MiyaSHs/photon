/* F3 auto LOD (analysis/38): generated LOD levels for a Source model with one LOD, from its .mdl / .vvd / .dx90.vtx in memory. The C twin
 * of tools/f3_vtx_lod.py --quadric (same layout, byte for byte): each level is a new set of strip groups over a subset of LOD 0's vertices
 * (lod_simplify), the .vvd header lists the levels, and the .mdl gets BONE_USED_BY_VERTEX_LOD<n> on the bones each level uses (and their
 * ancestors). Switch points: 200 x the level's error (the engine's metric is 100 / pixels per unit: under a pixel; x2 for an RMS error).
 * Pure C, thread-safe; the outputs are malloc'd and freed with lod_model_free. */
#ifndef PHOTON_LOD_MODEL_H
#define PHOTON_LOD_MODEL_H
#include <stddef.h>
#include <stdint.h>

enum { LOD_OK = 0, LOD_ERR_FORMAT = 1, LOD_ERR_HAS_LODS = 2, LOD_ERR_FLEX = 3, LOD_ERR_EMPTY = 4, LOD_ERR_STRIPS = 5, LOD_ERR_FIXUPS = 6,
       LOD_ERR_MEMORY = 7 };

typedef struct LodModelOut {
    uint8_t *mdl, *vvd, *vtx;
    size_t mdl_n, vvd_n, vtx_n;
    int levels;
    uint32_t tris[8];       /* LOD 0 and each level, summed over the models */
    float switch_point[8];  /* of the first body part model */
} LodModelOut;

int lod_model_build(const uint8_t *mdl, size_t mdl_n, const uint8_t *vvd, size_t vvd_n, const uint8_t *vtx, size_t vtx_n, const double *ratios,
                    int nratios, LodModelOut *out);
void lod_model_free(LodModelOut *out);
#endif
