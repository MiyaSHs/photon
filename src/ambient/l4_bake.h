/* L4 (analysis/39): Photon's path-traced lightmaps for a BSP, in the map's own lightmap layout. Per face, per light style the face lists,
 * per luxel: RGB from the map's worldlights of that style (vrad's direct terms with shadow rays, L3a-validated) plus paths of up to
 * `bounces` surfaces (texdata reflectivity as albedo; a path that escapes adds that style's sky ambient). Accumulated over passes so a
 * bake can stop early with a complete, noisier result. Encoded as the BSP lighting lump (ColorRGBExp32, faces at their own lightofs).
 * v0: bump-mapped faces get the flat value in all four of their lightmaps (no directional detail yet). Pure C; passes can run on several
 * threads over disjoint face ranges. */
#ifndef PHOTON_L4_BAKE_H
#define PHOTON_L4_BAKE_H
#include <stddef.h>
#include <stdint.h>

typedef struct L4Bake L4Bake;

/* the map in memory (kept by the caller for the bake's life); 0 on success */
int l4_bake_new(const uint8_t *bsp, size_t size, int bounces, L4Bake **out);
int l4_bake_faces(const L4Bake *b);
long l4_bake_luxels(const L4Bake *b);   /* luxels x styles to compute */
/* one pass of `samples` paths per luxel over faces [f0, f1); thread-safe across disjoint ranges (each caller needs its own seed) */
void l4_bake_pass(L4Bake *b, int f0, int f1, int samples, uint64_t seed);
/* the lighting lump as the map stores it (HDR if the map is HDR), from the passes so far: a copy of the map's lump with every baked face's
 * luxels replaced; *n its size. malloc'd. */
uint8_t *l4_bake_lump(const L4Bake *b, size_t *n, int *lump_index);
void l4_bake_free(L4Bake *b);
#endif
