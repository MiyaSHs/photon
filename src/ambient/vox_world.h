/* PT-RT (analysis/43): the voxel world for secondary rays. 8-unit voxels in sparse bricks of 8^3 (64 units), built from a map's brush
 * faces and displacements; per voxel the surface colour (averaged over the samples that fell in it), the surface normal and a coverage
 * count. Traced with a two-level DDA (bricks, then voxels). Pure C; the GPU version reads the same bricks. */
#ifndef PHOTON_VOX_WORLD_H
#define PHOTON_VOX_WORLD_H
#include <stddef.h>
#include <stdint.h>
#include "sun_vis.h"

enum { VOX_SIZE = 8, VOX_BRICK = 8 };   /* units per voxel, voxels per brick side */
typedef struct VoxWorld VoxWorld;
typedef struct VoxInfo { long bricks, voxels, samples, faces, disps; size_t bytes; } VoxInfo;

/* the map's drawn surfaces as triangles (brush faces as fans, displacements per quad), each with its texture colour and the face plane's
 * normal (the side it is seen from); the 3D skybox, sky, nodraw, tool and translucent faces left out. 0 on success. */
typedef void (*VoxTriFn)(void *ctx, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]);
int vox_triangles(const uint8_t *bsp, size_t size, const SunMap *sm, const float (*colour)[3], int ncolour, VoxTriFn fn, void *ctx,
                  long *nfaces, long *ndisps);
/* during a vox_triangles callback: the triangle's texinfo index in the map (its texture's axes and texdata; -1 outside a walk) */
extern __thread int vox_tri_texinfo;

/* the 3D skybox's bounds (its area's leaves), 1 if the map has one: anything inside belongs to the miniature sky scene */
/* displacement quads: which take the other diagonal ((row + column) & 1 == p; -1 none), to match the engine's checkerboard split */
void vox_set_disp_parity(int p);
int vox_skybox_bounds(const uint8_t *bsp, size_t size, const SunMap *sm, float box[6]);

/* colour: per texdata entry, linear RGB (NULL: VRAD's reflectivity from the map); the map in memory, its SunMap (displacements) */
int vox_build(const uint8_t *bsp, size_t size, const SunMap *sm, const float (*colour)[3], int ncolour, VoxWorld **out);
void vox_info(const VoxWorld *w, VoxInfo *info);
void vox_free(VoxWorld *w);

typedef struct VoxHit { float t, p[3], n[3], albedo[3]; } VoxHit;
/* the first occupied voxel along o + t d (unit d), t in (tmin, tmax); 1 if hit. The normal is the voxel's surface normal. */
int vox_trace(const VoxWorld *w, const double o[3], const double d[3], double tmin, double tmax, VoxHit *hit);

/* camera-centred cascades (vox_cascade.c): VOXC_LEVELS levels of VOXC_WIN^3 bricks, voxels of 8 << level units; fixed memory */
enum { VOXC_LEVELS = 5, VOXC_WIN = 16 };
typedef struct VoxCascades VoxCascades;
typedef struct VoxCascadeInfo { long triangles, faces, disps, rebuilds, bricks[VOXC_LEVELS], voxels[VOXC_LEVELS]; size_t bytes; } VoxCascadeInfo;
int vox_cascades_new(const uint8_t *bsp, size_t size, const SunMap *sm, const float (*colour)[3], int ncolour, VoxCascades **out);
void vox_cascades_update(VoxCascades *vc, const double cam[3]);   /* rebuilds the levels the camera moved */
int vox_cascades_trace(const VoxCascades *vc, const double o[3], const double d[3], double tmin, double tmax, VoxHit *hit);
void vox_cascades_info(const VoxCascades *vc, VoxCascadeInfo *info);
void vox_cascades_free(VoxCascades *vc);
/* one more world-space triangle (e.g. a static prop's), before the first update; ref: the side it is seen from (0 0 0: two-sided) */
void vox_cascades_add(VoxCascades *vc, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]);
/* placed models (props): a model's triangles once (model space, with colour and the side seen from), then any number of placements
 * (world = X . local); a level expands only the placements touching its window. add_model returns the model's id (-1: no memory). */
int vox_cascades_add_model(VoxCascades *vc, const float (*v)[3][3], const float (*col)[3], const float (*ref)[3], long n);
int vox_cascades_add_instance(VoxCascades *vc, int model, const double X[3][4]);

/* the map's static props into the cascades (vox_props.c); read: a file by its game path ("models/x.mdl"), malloc'd, NULL if missing */
typedef uint8_t *(*VoxReadFn)(void *ctx, const char *path, size_t *n);
typedef struct VoxPropsInfo { long props, placed, skybox, missing, models, models_loaded, triangles; } VoxPropsInfo;
int vox_cascades_add_props(VoxCascades *vc, const uint8_t *bsp, size_t size, const SunMap *sm, VoxReadFn read, void *ctx, VoxPropsInfo *info);

/* the GPU form for shaders/vk/ptrt_trace.comp (layout in vox_cascade.c); the words used, 0 if cap (words) is too small */
/* lights: nlights records of VOX_LIGHT_WORDS floats: origin xyz, type (1 point, 2 spot), intensity rgb, radius (0 none), constant,
 * linear, quadratic attenuation, exponent, spot axis xyz (the way it shines), stopdot, stopdot2, 0 x 3 */
enum { VOX_LIGHT_WORDS = 20 };
size_t vox_cascades_pack(const VoxCascades *vc, uint32_t *out, size_t cap, const float sun[3], const float sunc[3], const float sky[3],
                         const float *lights, int nlights);
#endif
