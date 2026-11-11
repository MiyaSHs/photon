/* L3b (analysis/37): the re-bake of the map's lamp visibility as signed distances in sparse world-space bricks. Pure library: no globals,
 * no D3D, no engine; any number of bakes may run at once (each with its own SunScratch). A bake reads only the BSP in memory.
 *
 * WHAT IS BAKED
 *   The shell: every voxel (size voxel units) whose centre is in air within 1.5 voxels in front of a lit flat face (not sky, not nolight;
 *   displacements are not in v1). Voxels group into bricks of L3_B^3; a brick keeps up to L3_LAMPS lamps, its strongest point / spot
 *   worldlights at its centre (any style: the runtime scales each by its style). Per voxel and lamp: the visibility of the lamp from the
 *   voxel centre (sun_vis.c's world-only ray, MASK_OPAQUE), turned into a signed distance to the visibility edge within the brick (in
 *   voxels, lit positive, clamped to +-L3_DMAX), stored as a byte: 128 + d * 127 / L3_DMAX. Voxels outside the shell take their nearest
 *   shell voxel's value (so trilinear filtering at the shell's edge stays consistent); a brick lamp no voxel of which is lit or shadowed
 *   differently stays constant (all 255 or all 0).
 *
 * WHAT IT IS NOT: lighting. The runtime pass (L3c) evaluates each lamp's light per pixel and uses these only for its visibility. */
#ifndef PHOTON_L3_BAKE_H
#define PHOTON_L3_BAKE_H
#include <stddef.h>
#include <stdint.h>

enum { L3_B = 8, L3_LAMPS = 4, L3_DMAX = 4 };
enum { L3_BAKE_VERSION = 4 };   /* part of the cache key: bump whenever a bake of the same map would come out different (2: visibility on the surface; 3: min_share; 4: no double flip of side-1 faces) */

typedef struct L3Brick {
    int32_t x, y, z;              /* brick coordinates: world = (x * L3_B + i) * voxel */
    int16_t lamp[L3_LAMPS];       /* worldlight indices, -1 unused */
    uint8_t sdf[L3_B * L3_B * L3_B][L3_LAMPS];   /* voxel i + L3_B * (j + L3_B * k) */
} L3Brick;

typedef struct L3Params {
    double voxel;                 /* units per voxel: 8, 4, 2 */
    double min_light;             /* a brick keeps a lamp only if its unoccluded light at the brick centre reaches this (linear units) */
    const volatile int *cancel;   /* may be NULL; checked per brick */
    double min_share;             /* drop a brick's lamp whose unoccluded light at the brick centre is under this share of all lamps' (0: keep all) */
} L3Params;

typedef struct L3Stats {
    long faces, shell_voxels, voxels_solid, bricks, brick_lamps, rays, edge_bricks;   /* bricks: kept (edge bricks and their neighbours) */
    long slots_dropped, bricks_dropped, slots_weak;   /* lamp slots without an edge in a kept brick, and kept bricks left with none */
    double ms_shell, ms_rays, ms_sdf;
} L3Stats;

typedef struct L3Bake {
    double voxel;
    long nbricks;
    L3Brick *bricks;
} L3Bake;

/* L3c: the bake packed for the GPU (all little-endian, ready for D3D9 LockBox / LockRect):
 *   l1   L16 3D, l1_dim: one texel per super-brick (L3_SB^3 bricks) over the kept bricks' bounds from sb_min; value: page + 1 (0 empty);
 *   l2   L16 3D, (L3_PX * L3_SB) x (L3_PY * L3_SB) x (pz * L3_SB): page g at (g % L3_PX, g / L3_PX % L3_PY, g / (L3_PX * L3_PY)), its
 *        texel (bx, by, bz) local brick = brick + 1 (0 empty);
 *   atlas A8R8G8B8 3D, (L3_AX * L3_B) x (L3_AY * L3_B) x (az * L3_B): brick i at (i % L3_AX, i / L3_AX % L3_AY, i / (L3_AX * L3_AY));
 *        bytes B, G, R, A = lamp slots 2, 1, 0, 3 (so .r = slot 0, .g = 1, .b = 2, .a = 3 in the shader);
 *   lamps float x 4 per brick: the bricks' worldlight indices (-1 unused), as an A32B32G32R32F row texture of width L3_LW.
 * l3_pack_lookup: the shader's lookup on these arrays (point texels for l1/l2, the atlas texel at the voxel), for tests. */
enum { L3_SB = 8, L3_PX = 8, L3_PY = 8, L3_AX = 16, L3_AY = 16, L3_LW = 256 };
typedef struct L3Pack {
    double voxel;
    int sb_min[3], l1_dim[3], npages, pz, nbricks, az, lamp_rows;
    uint16_t *l1, *l2;
    uint8_t *atlas;
    float *lamps;
} L3Pack;
int l3_pack(const L3Bake *b, L3Pack **out);
void l3_pack_free(L3Pack *p);
/* the 4 slots' bytes at voxel (x, y, z) and the brick's lamps; 0 if the voxel's brick is not in the pack */
int l3_pack_lookup(const L3Pack *p, int x, int y, int z, uint8_t sdf[L3_LAMPS], int lamp[L3_LAMPS]);

/* a pack as one buffer (the cache file) and back; the buffer is the caller's to free; load checks every size against the buffer */
int l3_pack_save(const L3Pack *p, uint8_t **buf, size_t *len);
int l3_pack_load(const uint8_t *buf, size_t len, L3Pack **out);

/* 0 = AMB_OK; the bake is the caller's to free */
int l3_bake(const void *bsp, size_t len, const L3Params *p, L3Bake **out, L3Stats *st);
void l3_bake_free(L3Bake *b);
#endif
