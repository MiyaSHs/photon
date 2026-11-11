/* P6 sun visibility (analysis/22_p6_sun.md): "is the sun visible from this point?" from the map's BSP, the way the engine answers it for a
 * model's light setup. Pure C11 like amb_bake.h and lg_map.h: no threads, no Windows, no globals. A SunMap is immutable after the load, so
 * any number of threads may trace it at once, each with its own SunScratch (or none).
 *
 * WHAT THE ENGINE DOES FOR A MODEL [confirmed decomp, engine.dll]:
 *   1. LightcacheGet (FUN_1801dafa0) keys a cache entry by ((int)x + 0x8000) >> 3, the same for y, ((int)z + 0x8000) >> 4 and the origin's
 *      leaf. A new entry's point (FUN_1801d7400, r_lightcachecenter, default "1") is the centre of the origin's 8 x 8 x 16 cell (cell from
 *      the truncated |coordinate|, complemented below 0) when a MASK_OPAQUE world-only trace from the origin reaches it (a centre in another
 *      leaf is first moved 0.5 past each plane onto the origin's side, FUN_18017d680, or skipped when that leaf is solid); else the
 *      centre's x and y at the origin's height, moved the same way, when that is reachable; else the origin. A trace that starts in solid
 *      keeps the origin. The point stays with the entry.
 *   2. Every draw, the sun term (FUN_1801d6c70) is skipped unless the point's leaf has LEAF_FLAGS_SKY or LEAF_FLAGS_SKY2D (its PVS sees the
 *      sky), and only the first sunlit emit_skylight counts. There is no other PVS test for the sun.
 *   3. The trace (FUN_1801da890 / FUN_1801da170, through FUN_1801daca0): a ray, not a box, from the point to point - normal * 57016.32 (the
 *      float at 0x1803c1a7c), mask 0x4081 = MASK_OPAQUE (CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_OPAQUE), CTraceFilterWorldOnly (the
 *      WorldAndProps filter only with flag 4, which this path never sets). Lit iff the hit surface has SURF_SKY (0x4; vbsp gives 2D-sky
 *      materials SURF_SKY | SURF_SKY2D, so one bit covers both); no hit or any other surface is shadow.
 *   sun_model_visible is 1-3, sun_visible is 3 alone (what util.TraceLine with MASK_OPAQUE shows), SunCache is 1's cache.
 *
 * WHAT IS TRACED HERE, the world model's collision as the engine's world trace has it:
 *   - brushes, through the node tree of model 0: a node whose plane the ray crosses visits both sides with DIST_EPSILON (1/32) of overlap,
 *     near side first; in every leaf visited, whatever the leaf's own contents (vbsp lists func_detail brushes in the empty leaves they
 *     cross), every brush of the leaf's leafbrushes whose contents meet the mask is clipped against its non-bevel sides
 *     (enter fraction (d1 - 1/32) / (d1 - d2), the side that sets it is the hit side). func_detail is part of model 0, so it blocks; water,
 *     glass (CONTENTS_WINDOW) and grates do not meet MASK_OPAQUE, so they do not.
 *   - a ray that starts inside brushes is not stopped by them: it reports the first surface after the solid run it starts in, brushes that
 *     touch or overlap counting as one run, and one that never ends stops it at once with no surface [confirmed in game, analysis/22 §3].
 *   - displacements (their contents meet MASK_OPAQUE, no SURF_NORAY_COLL flag in minTess' high-bit flags): the triangles of the public
 *     SDK's CCoreDispInfo (corner nearest startPosition first, the alternating diagonals of GenerateCollisionSurface), one-sided as
 *     CDispCollTree::AABBTree_Ray tests them (IntersectRayWithTriangle with vertices 0, 2, 1: a ray going out of the back is not hit),
 *     a quad's two triangles only when the quad's box grown by 1/32 meets the ray, and a t down to -1e-3 of the ray (57 units for the sun's
 *     ray) clamped to 0, as that function's ray tolerance has it. A displacement hit has no SURF_SKY: shadow.
 *   - not traced: static props, brush entities (doors, func_brush) and every other entity, as the world-only filter skips them.
 * The first hit along the ray decides; ties between a brush and a displacement go to the brush.
 *
 * Read (through an AmbReader as amb_map_load_reader, so maps inside a GMA work the same way; every range checked against the file and every
 * count against a fixed cap before anything is read): planes 1, texinfo 6, vertexes 3, nodes 5, faces 7 (58 if 7 is empty), leaves 10
 * (v0 or v1), edges 12, surfedges 13, model 0 (14), leafbrushes 17, brushes 18, brushsides 19, dispinfo 26, dispverts 33, and the
 * worldlights 54 if prefer_hdr and it is non-empty, else 15 (lg_map.c's rule). An LZMA lump that the trace needs: AMB_ERR_LZMA; an LZMA light
 * lump: no sun (info.lights_lzma). */
#ifndef PHOTON_SUN_VIS_H
#define PHOTON_SUN_VIS_H
#include <stddef.h>
#include <stdint.h>
#include "amb_bake.h"

enum { SUN_MASK_OPAQUE = 0x4081, SUN_SURF_SKY = 0x4, SUN_SURF_SKY2D = 0x2 };
#define SUN_TRACE_LENGTH 57016.3203125   /* engine.dll 0x1803c1a7c */
#define SUN_DIST_EPSILON 0.03125

typedef struct SunMap SunMap;
typedef struct SunInfo {
    int bsp_version, leaf_version;
    int nplanes, nnodes, nleaves, nbrushes, nsides, ntexinfo, ndisps, ndisp_tris, nsky_texinfo;
    int light_lump, light_lump_version, lights_lzma, nlights;
    int has_sun;          /* 1: the map has an emit_skylight worldlight; 0: no sun (sun_visible returns SUN_NONE) */
    int nskylights;       /* emit_skylight records (the engine lights with each; the first is reported below) */
    float sun_normal[3];  /* the worldlight's normal: the direction the light travels (from the sky down) */
    float sun_dir[3];     /* -normal: from a point toward the sun, the direction traced */
    float sun_intensity[3]; /* the worldlight's intensity (linear, from the chosen lump, before its light style) */
    int sun_style;
    float world_min[3], world_max[3];
} SunInfo;

int sun_map_load_reader(const AmbReader *r, int prefer_hdr, SunMap **out, AmbReadStats *stats);
int sun_map_load(const void *bsp, size_t len, int prefer_hdr, SunMap **out);
void sun_map_free(SunMap *m);
/* displacements (PT-RT's voxel world): the count, and one's vertex grid: w x w vertices, row-major, triangles as the collision tree
 * builds them (see clip_disp), and its base face's texinfo */
int sun_map_disp_count(const SunMap *m);
int sun_map_disp_grid(const SunMap *m, int i, const float (**v)[3], int *w, int *texinfo);
void sun_map_info(const SunMap *m, SunInfo *info);

/* Per-thread scratch: marks so a brush or displacement listed in several leaves is clipped once per trace. Optional (NULL works, a little
 * slower). One scratch per thread; it belongs to the map it was made for. */
typedef struct SunScratch SunScratch;
SunScratch *sun_scratch_new(const SunMap *m);
void sun_scratch_free(SunScratch *s);

enum { SUN_HIT_NONE = 0, SUN_HIT_BRUSH = 1, SUN_HIT_DISP = 2 };
typedef struct SunTrace {
    double fraction;     /* of the segment, 1 if nothing was hit */
    int hit;             /* SUN_HIT_* */
    int brush, side;     /* brush index and brushside index of a brush hit, else -1 */
    int disp;            /* displacement index of a displacement hit, else -1 */
    int texinfo, surf_flags; /* the hit side's texinfo and its flags (0 for no hit, a displacement, or a side without texinfo) */
    int startsolid, allsolid;
    int leaves;          /* leaves visited (statistics) */
    float disp_n[3];     /* a displacement hit: the hit triangle's unit normal (its front side), else 0 */
    int disp_texinfo;    /* a displacement hit: its base face's texinfo, else -1 */
} SunTrace;

/* The engine's world-only ray trace of model 0 from a to b with a contents mask (SUN_MASK_OPAQUE for the sun). */
void sun_trace(const SunMap *m, SunScratch *s, const double a[3], const double b[3], int mask, SunTrace *out);

enum { SUN_NONE = -1, SUN_SHADOW = 0, SUN_LIT = 1 };
/* The trace alone at p (step 3): the ray to p + sun_dir * SUN_TRACE_LENGTH, MASK_OPAQUE, lit iff the hit surface has SURF_SKY. */
int sun_visible(const SunMap *m, SunScratch *s, const double p[3]);

/* The leaf of p (d >= 0 front, as the engine's point walk) and that leaf's contents and flags (dleaf_t's 7 flag bits) */
int sun_point_leaf(const SunMap *m, const double p[3]);
int sun_leaf_contents(const SunMap *m, int leaf);
int sun_leaf_flags(const SunMap *m, int leaf);

/* Step 1: the point the engine traces the sun from for a light cache entry made at origin (float, as the engine). Returns how it was
 * chosen. */
enum { SUN_POINT_ORIGIN = 0, SUN_POINT_CENTRE = 1, SUN_POINT_CENTRE_XY = 2 };
int sun_lightcache_point(const SunMap *m, SunScratch *s, const double origin[3], double out[3]);
/* Steps 1-3 for a model lit at origin, without a cache */
int sun_model_visible(const SunMap *m, SunScratch *s, const double origin[3]);

/* LightcacheGet's key of an origin (cells and leaf, 54 bits); 0 for NaN or |coordinate| >= 32767 (not cached) */
int sun_cache_key(const SunMap *m, const double origin[3], uint64_t *key);
/* A cache of sun_model_visible with the engine's own key, so an entry is made from the first origin of its cell and leaf, as the engine's
 * is. Direct-mapped, 2^log2_entries entries of 8 bytes, a full key compare (no false hits); a collision replaces the entry (the engine
 * also recycles its entries). Owned by one thread; it holds its own scratch. Clear it when the map changes (or make a new one). */
typedef struct SunCache SunCache;
typedef struct SunCacheStats { long long lookups, hits, traces; } SunCacheStats;
SunCache *sun_cache_new(const SunMap *m, int log2_entries);
void sun_cache_free(SunCache *c);
int sun_cache_visible(SunCache *c, const double origin[3]);
void sun_cache_clear(SunCache *c);
void sun_cache_stats(const SunCache *c, SunCacheStats *st);
#endif
