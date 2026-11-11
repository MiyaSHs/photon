/* P4a: the cubemap pick table (lighting plan v2 P4, analysis/15a section 1.2, analysis/17). Pure C11 like amb_bake.h: no threads, no Windows, no
 * globals; the proxy runs it on workers (cubework.c) and tests/ambient/picktest.c drives it on the host.
 *
 * WHAT THE ENGINE DOES [confirmed decomp, engine.dll FUN_1801d9d30]: the env_cubemap of a draw is the lump-42 sample whose origin is nearest
 * (squared Euclidean distance in float, first minimum in lump order) to the lighting point, with no visibility test. The lighting point is the
 * light-cache cell's (8 x 8 x 16 units) centre when r_lightcachecenter is on and the centre is reachable, else the draw's lighting origin, and the
 * result is cached per cell and leaf (FUN_1801dafa0, FUN_1801d7400). amb_cube_nearest is that rule for one point.
 *
 * WHAT THE TABLE HOLDS: for every voxel of the ambient volume's world-aligned grid (amb_bake.h: voxel a has its centre at (a + 0.5) * size, the
 * cascade-0 size is 32), the two nearest samples that the voxel can see, and flags. "Can see": the segment from the voxel's probe point to the
 * sample's target point stays in open leaves (amb_segment_solid: CONTENTS_SOLID brush leaves only; displacements, static props and entities do
 * not occlude, which is the choice analysis/17 documents). The probe point is the voxel centre, or when that is in solid, the first open point of
 * centre +- 12 units along x, then y, then z; with none open the voxel is SOLID and takes the two nearest by distance. A sample's target point is its
 * origin, or the same nudge of it (by 8 units) when the mapper left it in solid. Samples are tried in order of distance from the probe point, at most
 * max_tests of them: without two visible ones among those, the missing picks are the nearest remaining by distance and the FALLBACK flags say so
 * (a closed room without its own env_cubemap).
 *
 * Entry layout (uint32_t): bits 0..10 pick A, 11..21 pick B (sample indices; AMB_PICK_NONE when the map has fewer samples), 22..31 flags. */
#ifndef PHOTON_AMB_PICK_H
#define PHOTON_AMB_PICK_H
#include <stddef.h>
#include <stdint.h>
#include "amb_bake.h"

enum { AMB_CUBES_MAX = 1024,       /* MAX_MAP_CUBEMAPSAMPLES (SDK bspfile.h); more and the map is refused */
       AMB_PICK_NONE = 0x7ff,
       AMB_PICK_SOLID = 1 << 22,       /* no open probe point: A, B by distance alone */
       AMB_PICK_MOVED = 1 << 23,       /* the centre was solid; a nudged probe point was used */
       AMB_PICK_A_FALLBACK = 1 << 24,  /* no visible sample among the tested ones: A is the nearest by distance */
       AMB_PICK_B_FALLBACK = 1 << 25,  /* fewer than two visible: B is the nearest remaining by distance */
       AMB_PICK_NEAR_HIDDEN = 1 << 26, /* the nearest sample by distance (the engine's choice at this point) is not visible */
       AMB_PICK_EMPTY = 1 << 27 };     /* the map has no samples (A = B = NONE) */
#define AMB_PICK_A(e) ((int)((e) & 0x7ff))
#define AMB_PICK_B(e) ((int)(((e) >> 11) & 0x7ff))

typedef struct AmbCubes {
    int n;
    int origin[AMB_CUBES_MAX][3];   /* dcubemapsample_t origin (integers) */
    int size[AMB_CUBES_MAX];        /* its size field: 0 = default, else 2^(size - 1) px */
} AmbCubes;

/* Lump 42 (LUMP_CUBEMAPS, 16-byte dcubemapsample_t) of a BSP: from memory, or through a reader that fetches the header and the lump only (both
 * L4D2-ordered and normal headers, as amb_map_load). AMB_ERR_FORMAT for a lump size that is not a multiple of 16 (the engine's "funny lump size")
 * or more than AMB_CUBES_MAX samples, AMB_ERR_LZMA for a compressed lump. A map with no samples loads with n = 0. */
int amb_cubes_load(const void *bsp, size_t len, AmbCubes *out);
int amb_cubes_load_reader(const AmbReader *r, AmbCubes *out, AmbReadStats *stats);

/* The engine's pick at p: index of the nearest sample (float math in the engine's order, first minimum wins), -1 for a map without samples */
int amb_cube_nearest(const AmbCubes *c, const float p[3]);

typedef struct AmbPickParams {
    double voxel;               /* world units per voxel, all three axes (cascade 0: 32) */
    int max_tests;              /* samples tried per voxel, nearest first (default 16: on gm_construct 8 left a place whose only visible sample within 2x was the 9th) */
    AmbParallelFor pfor;        /* may be NULL; tasks are rows of voxels */
    void *pool;
    const volatile int *cancel; /* may be NULL; non-zero stops within one row on each thread (AMB_ERR_CANCELLED) */
} AmbPickParams;
void amb_pick_params_default(AmbPickParams *p);   /* 32, 16, inline, no cancel */

/* The samples' target points for a map (the nudge above): a few point walks, once per map. Immutable afterwards, shareable by any threads. */
typedef struct AmbPickTargets { int n; double p[AMB_CUBES_MAX][3]; uint8_t solid[AMB_CUBES_MAX]; } AmbPickTargets;
void amb_pick_targets(const AmbMap *m, const AmbCubes *c, AmbPickTargets *t);

/* One voxel (absolute index a) */
uint32_t amb_pick_voxel(const AmbMap *m, const AmbCubes *c, const AmbPickTargets *t, const AmbPickParams *p, const int a[3]);
/* Every voxel of box, out[((k * dim[1] + j) * dim[0] + i)] (x fastest, as the bake). Bit-identical for any thread count. */
int amb_pick_box(const AmbMap *m, const AmbCubes *c, const AmbPickTargets *t, const AmbPickParams *p, const AmbBox *box, uint32_t *out);

/* Toroidal write of a box's entries into a clipmap table of clip_dim voxels: voxel a goes to entry (a mod clip_dim), x fastest. AMB_ERR_ARG if the
 * box is larger than the table. */
int amb_pick_write_toroidal(const uint32_t *src, const AmbBox *box, const int clip_dim[3], uint32_t *table);
#endif
