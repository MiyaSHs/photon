/* F3 auto LOD (analysis/38): quadric-error simplification by half-edge collapse. Every collapse merges a vertex into one of its neighbours,
 * so a simplified level only uses a subset of the input's vertices (the model's .vvd stays as it is; a level is a new index list).
 *  - Collapses only join vertices of the same class (the caller's key: bones and weights), so skinning never changes.
 *  - Open edges (one triangle: UV seams and hard edges, which Source stores as split vertices, and the edges where strip groups meet) get a
 *    heavy perpendicular quadric, so the outline, seams and group borders hold.
 *  - A collapse that would flip or degenerate a remaining triangle is refused.
 * Pure C, no allocation outside the call's own scratch; thread-safe (no globals). */
#ifndef PHOTON_LOD_SIMPLIFY_H
#define PHOTON_LOD_SIMPLIFY_H
#include <stdint.h>

typedef struct LodParams {
    uint32_t target_tris;    /* stop at or below this many triangles (0: no triangle target) */
    double max_error;        /* stop before a collapse whose error exceeds this (world units: the RMS distance of the merged vertices' planes,
                              * border planes weighted in); <= 0: none */
    double border_weight;    /* weight of the open-edge quadrics against the surface ones (e.g. 100) */
} LodParams;

typedef struct LodResult {
    uint32_t tris;           /* triangles written to out_idx */
    double error;            /* the largest collapse error accepted (world units) */
    uint32_t collapses, refused_flip, refused_class;
} LodResult;

/* pos: nv x 3 floats; cls: nv class keys; idx: ntri x 3 indices (a triangle list). out_idx: room for ntri x 3. Returns 0 on success. */
int lod_simplify(const float *pos, const uint32_t *cls, uint32_t nv, const uint32_t *idx, uint32_t ntri, const LodParams *p, uint32_t *out_idx,
                 LodResult *r);
#endif
