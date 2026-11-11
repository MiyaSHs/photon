/* P3 (a): per-pixel ambient volume bake, a C11 port of tools/lpv2_ambient_volume.py --mode leaflocal (AMB §2a,
 * analysis/15b_ambient_volume.md; plan analysis/15_lighting_plan_v2.md P3).
 *
 * Pure library: no Windows/D3D dependencies, no globals, no hidden state. An AmbMap is immutable after
 * amb_map_load(), so any number of threads may read it at once; every bake call only writes its own outputs and its
 * own heap scratch, so bakes are reentrant. The proxy runs them on worker threads and uploads finished slabs on the
 * render thread (AMB §4 Threading).
 *
 * Bake (identical semantics to the Python; numbers in tests/ambient):
 *   1. eval: at every voxel centre the engine's leaf-local rule (amb_eval_point) with weight 1/(d^2 + soft^2);
 *      solid voxels (centre in a CONTENTS_SOLID leaf) are 0.
 *   2. faces: the face between voxels i and i+1 along an axis is passable if both are open and the 3 points at
 *      t = 0.25, 0.5, 0.75 of the centre-to-centre segment are not in solid leaves.
 *   3. blur_passes x: every open voxel becomes the mean of itself and its neighbours across passable faces.
 *   4. dilate_passes x: every not-yet-filled voxel with >= 1 filled 6-neighbour becomes their mean (fills solid
 *      space so trilinear filtering next to walls does not pull in black).
 * Precision: BSP walks and per-voxel sums are double (as numpy); the volume between stages is float, which costs
 * < 1e-6 relative against the all-double Python. Results are bit-identical for any thread count and any
 * AmbParallelFor, because every voxel of every stage is computed by the same code from the previous stage only.
 * Compile with -std=c11 (or -ffp-contract=off): GCC's GNU modes contract a*b+c into FMA on FMA targets, which would
 * change the last bits between builds (not between threads).
 *
 * Voxel grid: world-aligned. Voxel a = (ax, ay, az) (integer index) has its centre at (a + 0.5) * size, size =
 * (voxel_xy, voxel_xy, voxel_z). For integer sizes this is bit-identical to the Python's lo + (i + 0.5) * st with
 * lo = floor(bmin / st) * st (both are exact). A box is {org, dim} in voxel indices.
 *
 * Volume layout (all float outputs): vol[((k * dim[1] + j) * dim[0] + i) * 18 + face * 3 + rgb], x fastest, faces
 * +X -X +Y -Y +Z -Z (vrad g_BoxDirections / the engine cube order), linear rgb in engine units. */
#ifndef PHOTON_AMB_BAKE_H
#define PHOTON_AMB_BAKE_H
#include <stddef.h>
#include <stdint.h>

enum {
    AMB_OK = 0,
    AMB_ERR_ARG = -1,     /* bad parameter (NULL, empty box, non-positive voxel size, ...) */
    AMB_ERR_FORMAT = -2,  /* not a VBSP, lump out of range, inconsistent sizes, unknown leaf lump version */
    AMB_ERR_LZMA = -3,    /* a lump this library needs is LZMA-compressed (not supported, see amb_bake.c) */
    AMB_ERR_NOMEM = -4,
    AMB_ERR_CANCELLED = -5, /* AmbBakeParams.cancel (or AmbReader.cancel) was set: the output is unusable */
    AMB_ERR_READ = -6,    /* AmbReader.read returned fewer bytes than asked for */
};
enum { AMB_FACES = 6, AMB_VOXEL_FLOATS = 18 };

typedef struct AmbMap AmbMap;

typedef struct AmbMapInfo {
    int bsp_version;          /* VBSP header version (20 for GMod maps) */
    int leaf_lump_version;    /* LUMP_LEAFS version: 0 (56-byte leaves, inline cube) or 1 (32-byte) */
    int hdr;                  /* 1: ambient from lumps 51/55 (HDR), 0: 52/56 (LDR) or the leaf lump (v0) */
    int ambient_source;       /* 0 indexed lumps, 1 one cube per leaf (legacy lump/v0 leaves), 2 none (grey 0.5) */
    int nplanes, nnodes, nleaves, nsamples;
    float world_min[3], world_max[3];   /* model 0 bounds */
} AmbMapInfo;

/* Parses a BSP held in memory. Copies everything it needs, so `bsp` may be freed afterwards. prefer_hdr = 1 uses
 * the HDR ambient lumps (51/55) when lump 55 is non-empty, else the LDR ones (52/56), like the Python's default.
 * Returns AMB_OK and *out, or an AMB_ERR_* code (*out = NULL). */
int amb_map_load(const void *bsp, size_t len, int prefer_hdr, AmbMap **out);

/* The same parse, reading only what it uses: the 1032-byte header, 4 bytes at the start of each lump it looks at (the LZMA tag), and the
 * planes, nodes, leaves, the first 48 bytes of the models, the ambient index and the samples the index names (lumps 1, 5, 10, 14, 51/52,
 * 55/56). Every range is checked against size before it is read, and each lump's size against the limits the parse enforces before any
 * of it is read, so a hostile header costs at most AMB_READ_MAX bytes of reads and as much memory. Results are bit for bit those of amb_map_load on the same
 * bytes, error codes included (tests/ambient/readtest.c), plus AMB_ERR_READ and AMB_ERR_CANCELLED.
 *   read: fill buf with bytes [ofs, ofs + n) of the file; n is at most AMB_READ_CHUNK. Returns the bytes read; anything but n fails the load.
 *   cancel: may be NULL; checked before every read call, so a set flag stops the load within one chunk.
 * stats (may be NULL): what the load read and held. */
enum { AMB_READ_CHUNK = 1 << 20 };
/* header + 8 LZMA tags (lump 55 is looked at twice) + 65536 planes (20 B), nodes (32 B), v0 leaves (56 B), one model, 65536 index entries (4 B), 2^20 samples (28 B) */
#define AMB_READ_MAX (1032ll + 8 * 4 + 65536ll * (20 + 32 + 56 + 4) + 48 + 28ll * 1048576)
typedef struct AmbReader {
    void *ctx;
    long long size;
    int (*read)(void *ctx, long long ofs, void *buf, int n);
    const volatile int *cancel;
} AmbReader;
typedef struct AmbReadStats {
    long long bytes_read, peak_bytes;   /* peak_bytes: lump buffers held at once, the map itself not included */
    int reads;
} AmbReadStats;
int amb_map_load_reader(const AmbReader *r, int prefer_hdr, AmbMap **out, AmbReadStats *stats);
/* Tests: 1 if two maps hold the same planes, nodes, leaves, samples and info, bit for bit */
int amb_map_same(const AmbMap *a, const AmbMap *b);
void amb_map_free(AmbMap *m);
void amb_map_info(const AmbMap *m, AmbMapInfo *info);

/* Leaf index containing p (model 0 tree, plane test d >= 0 -> front child) and whether that leaf is CONTENTS_SOLID. */
int amb_point_leaf(const AmbMap *m, const double p[3]);
int amb_point_solid(const AmbMap *m, const double p[3]);
/* P4a line of sight: 1 if the closed segment a-b meets a CONTENTS_SOLID leaf of the model 0 tree, 0 if it stays in open leaves. The same plane
 * rule as amb_point_leaf (d >= 0 is the front side): a node whose plane the segment crosses visits both parts, the near one first, and the
 * crossing point counts on both sides, so a segment that only touches a solid leaf at one point is blocked. Brushes only: displacements,
 * static props and entities are not in the tree. Iterative, no allocation, reentrant. */
int amb_segment_solid(const AmbMap *m, const double a[3], const double b[3]);

/* Leaf ambient at p: the samples of the leaf containing p, weighted 1/(d^2 + soft^2) (soft = 1 is the engine's 1/(d^2 + 1)). Writes 18 floats
 * (6 faces rgb) and returns the number of samples used (0: out is all zero). The two differ only for a leaf with no samples, such as a solid one:
 *   amb_eval_engine: the engine's rule (FUN_180115950): zero, because its loader leaves `first` 0 for those leaves and the sampler follows only a
 *     non-zero `first` [confirmed decomp]. Use it wherever the result stands for the engine's own cube.
 *   amb_eval_point: vrad's rule (Mod_LeafAmbientColorAtPos, NearestNeighborWithLight), as the Python does: the leaf its stored firstAmbientSample names.
 *     Tool compatibility, and the bake, which never evaluates a solid leaf. */
int amb_eval_engine(const AmbMap *m, const double p[3], double soft, float out[AMB_VOXEL_FLOATS]);
int amb_eval_point(const AmbMap *m, const double p[3], double soft, float out[AMB_VOXEL_FLOATS]);

/* Parallel-for hook: run fn(ctx, i) for every i in [0, n) on any threads, in any order, and return only when all
 * calls have finished (a barrier). The bake calls it once per stage with independent tasks (rows of voxels).
 * NULL runs the tasks inline on the calling thread. */
typedef void (*AmbTaskFn)(void *ctx, int i);
typedef void (*AmbParallelFor)(void *pool, int n, AmbTaskFn fn, void *ctx);

typedef struct AmbBakeParams {
    double voxel_xy, voxel_z;   /* voxel size in world units (integers keep the grid exact) */
    double soft;                /* weight 1/(d^2 + soft^2); the Python's --soft (32) */
    int blur_passes;            /* the Python's --blur (its default 2; the repro and P3 use 4) */
    int dilate_passes;          /* the Python's dilate n (3) */
    int wrap_edges;             /* 1: dilation wraps around the box like np.roll (Python-compatible, for tests);
                                   0: a box edge has no neighbours outside (amb_bake_region uses this) */
    AmbParallelFor pfor;        /* may be NULL */
    void *pool;                 /* passed to pfor */
    const volatile int *cancel; /* may be NULL; non-zero makes a bake stop within one row of voxels on each thread */
} AmbBakeParams;

typedef struct AmbBox { int org[3]; int dim[3]; } AmbBox;

/* voxel 32/32, soft 32, blur 4, dilate 3, wrap 0, no pool */
void amb_params_default(AmbBakeParams *p);
/* The Python's box for --bounds: org = floor(bmin / size), dim = ceil(bmax / size) - org. AMB_ERR_ARG for NaN or out-of-range bounds. */
int amb_box_from_bounds(const double bmin[3], const double bmax[3], double voxel_xy, double voxel_z, AmbBox *box);
/* Voxels a region bake adds on every side: blur_passes + dilate_passes (each stage reads 1 voxel further). */
int amb_region_margin(const AmbBakeParams *p);

/* Bakes exactly `box` (edges per p->wrap_edges). vol: box voxels * 18 floats. open_out (optional, 1 byte per voxel):
 * 1 if the voxel centre is not in a solid leaf. With wrap_edges = 1 the result is the Python's bake_leaflocal. */
int amb_bake_box(const AmbMap *m, const AmbBakeParams *p, const AmbBox *box, float *vol, uint8_t *open_out);

/* Bakes `box` as a part of the unbounded world grid: internally bakes box grown by amb_region_margin() with
 * wrap_edges = 0 and returns the interior, so every voxel equals the same voxel of any larger region bake (and of a
 * box bake whose edges are >= margin away), bit for bit. This is the clipmap slab bake. p->wrap_edges is ignored. */
int amb_bake_region(const AmbMap *m, const AmbBakeParams *p, const AmbBox *box, float *vol, uint8_t *open_out);

/* --- GPU packing: D3DFMT_A16B16G16R16F (memory order R, G, B, A as IEEE half), AMB §2a atlas. -----------------
 * A volume of dim (X, Y, Z) is an atlas texture of (6 X) x Y x Z texels: face f occupies u in [f X, f X + X).
 * Texel = (r, g, b, 1). Pitches are in bytes (D3DLOCKED_BOX RowPitch / SlicePitch, >= 6 X * 8 and >= Y * RowPitch). */
uint16_t amb_f32_to_f16(float f);   /* round to nearest even; overflow -> inf; NaN stays NaN (quiet) */
float amb_f16_to_f32(uint16_t h);
void amb_pack_atlas(const float *vol, const int dim[3], void *dst, size_t row_pitch, size_t slice_pitch);
/* Toroidal clipmap write: voxel a of `region` goes to texel (a mod clip_dim) of each face band of the atlas. Bands are
 * AMB_BAND_TEXELS(X) = X + 1 wide, so the atlas is 6 * (X + 1) x Y x Z: texel X of a band is a copy of its texel 0 (the
 * gutter). AMB §2a's per-face U clamp alone would leave a seam where X wraps inside the view: filtering across a band
 * edge cannot reach the band's other end. The shader reads, with p in voxel units (world / size):
 *     band_u = frac((p.x - 0.5) / X) * X + 0.5              in [0.5, X + 0.5): never leaves texels 0..X of the band
 *     u = (face * (X + 1) + band_u) / (6 * (X + 1)),  v = p.y / Y,  w = p.z / Z     (V and W sampler addressing WRAP)
 * Use it for the full cascade and for the slabs entering on a camera move. Returns AMB_ERR_ARG if region->dim exceeds
 * clip_dim. Pitches as above, with RowPitch >= 6 * (X + 1) * 8. */
#define AMB_BAND_TEXELS(x) ((x) + 1)
int amb_write_toroidal(const float *vol, const AmbBox *region, const int clip_dim[3], void *atlas, size_t row_pitch,
                       size_t slice_pitch);
#endif
