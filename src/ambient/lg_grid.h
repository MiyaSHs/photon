/* P5 (analysis/18_p5_lights.md): the light grid, built per frame on one worker. Pure library: no Windows, no globals; an LgGrid is used by one
 * thread at a time (the worker), reads an immutable LgMap, and writes a CPU copy of the GPU texture plus the rows that changed.
 *
 * WINDOW: world-aligned cells of p.cell units; the window is the LG_GX x LG_GY x LG_GZ cells whose first cell is floor(cam / cell) - (GX, GY, GZ) / 2.
 * World cell (x, y, z) lives at toroidal place i = (x mod GX) + GX (y mod GY) + GX GY (z mod GZ): texel row i / GX, columns 4 (i mod GX) .. + 3:
 *   (n, r0, r1, r2), (r3 .. r6), (r7 .. r10), (r11 .. r14): n <= p.k <= LG_KMAX record indices. Rows LG_REC_ROW .. LG_TEX_H - 1: 512 records,
 *   record p at row LG_REC_ROW + p / 32, columns 4 (p mod 32) .. + 3, the c39.. layout (lg_record). Records 0 .. LG_NSTATIC_REC - 1 are static
 *   lights, held while a cell uses them (a refcount); dynamic light s (dlight i: s = i, elight j: s = 32 + j) is record LG_NSTATIC_REC + s.
 *   photon_lights_ps.h reads exactly this layout (its PHOTON_LG_* are these numbers).
 * A CELL'S LIGHTS: the static grid lights (point, spot and surface worldlights: lg_desc_from_worldlight; the sun and the sky ambient are not
 *   grid lights) and this frame's dynamic lights whose window sphere (lg_window_radius, a dlight's radius) meets the cell box, whose cluster is
 *   >= 0 and in the PVS of one of the cell's clusters (those of its centre and of the 8 points half way to its corners), and whose style value
 *   is not 0; ranked by lg_importance at the box's nearest point (a static light by its colour at style scale 1, a dynamic one at its style
 *   scale), the first p.k kept. Ties: static before dynamic, then the lower index. A candidate beyond p.k is dropped and counted (rule C13).
 *   A cell with no open cluster (all 9 points solid) has no lights. Of each cell's static candidates the first LG_STATIC_MAX are kept.
 * THE SPLIT (P5b, analysis/18 "P5b"): every static light has a per-pixel radius rho <= r, fixed for the map by lg_grid_new: the light is per pixel
 *   with the window saturate(1 - d^4/rho^4)^2 (its record's window is rho), and the rest of it, window(r) - window(rho), is folded per draw into
 *   the draw's cube (lg_fold_eval). rho is chosen once over every cell of the map: in each cell, of its static candidates (above, every style on),
 *   those ranked beyond p.k_static get rho <= their distance to the cell, so no cell ever has more than k_static statics whose rho reaches it
 *   (they are its "per-pixel statics"). As rho is a light's own and fixed, both parts are continuous in space and nothing changes with the camera.
 *   p.k_static = 0: rho = r for every light (P5's grid).
 *   A cell's per-pixel list: its per-pixel statics (style on; at most k), then its dynamic candidates by importance while the list is below k, kept
 *   in the merged importance order (P5's order when nothing is dropped). The cell's FOLD LIST holds every static candidate (style on), flagged if
 *   kept, and every dynamic candidate not kept. A static light's whole is W(d) = saturate((1 - d^4/r^4) / (1 - xe^4)), xe = min(re / r, 0.9), re
 *   the engine's own radius (FUN_1800d98d0: |I| / att = 1/66.67 with HDR lights, 1/33.33 without, or the light's radius): 1 out to where the
 *   engine cuts the light, then down to 0 at r, and never below window(r). So its fold is W - window(rho) where it is kept and W elsewhere: the
 *   per-pixel window loses no light against stock's hard cutoff. A dynamic light's whole stays window(r) (P5's: its fold is window(r) where
 *   dropped). Candidates beyond the LG_STATIC_MAX / LG_DYN_MAX caps are the only light that is lost (counted, fold_lost).
 *   FOLD IMAGE: cell i's list at row i / GX, LG_FOLD_STRIDE uint16 from (i mod GX) LG_FOLD_STRIDE: n, 0, then n entries: a static light s
 *   (s < LG_NSTATIC_MAX) as s | LG_FOLD_KEPT if kept, a dynamic slot d as LG_FOLD_DYN | d. Its rows change with the texture's (the same dirty
 *   bits). FOLD TABLE: entry s < nstatic the static light s, entry nstatic + d dynamic slot d: lg_record with no window at this frame's
 *   scale, (r, rho) and we = 1 / (1 - xe^4) (0 for a dynamic light: its whole is window(r)); its sequence number changes whenever an entry does.
 * INCREMENTAL: a cell's static candidates are computed once while it stays in the window, near the camera first, at most p.budget cells a frame
 *   (until then the cell holds no lights, and is counted pending); a cell is reassembled only when its candidates or the style on/off states or
 *   p.k changed. The result is the same as a fresh build of the same frame once nothing is pending (tests/ambient/lgtest.c checks that). */
#ifndef PHOTON_LG_GRID_H
#define PHOTON_LG_GRID_H
#include <stdint.h>
#include "lg_light.h"

enum { LG_GX = 32, LG_GY = 32, LG_GZ = 16, LG_NCELL = LG_GX * LG_GY * LG_GZ, LG_TEX_W = 4 * LG_GX, LG_REC_ROW = LG_GY * LG_GZ,
       LG_NREC = 512, LG_TEX_H = LG_REC_ROW + LG_NREC / LG_GX, LG_NSTATIC_REC = 416, LG_NDYN = 96, LG_KMAX = 15, LG_STATIC_MAX = 63,
       LG_DYN_MAX = 31, LG_NDL = 32, LG_NEL = 64, LG_KSTATIC = 11, LG_NSTATIC_MAX = 0x4000,
       LG_FOLD_MAX = LG_STATIC_MAX + LG_DYN_MAX, LG_FOLD_STRIDE = 2 + LG_FOLD_MAX, LG_FOLD_ROW = LG_GX * LG_FOLD_STRIDE, LG_FOLD_ROWS = LG_REC_ROW,
       LG_FOLD_KEPT = 0x4000, LG_FOLD_DYN = 0x8000 };
_Static_assert(LG_NSTATIC_REC + LG_NDYN == LG_NREC, "record split");

typedef struct LgParams {
    double cell;                /* cell size in world units (128) */
    int k;                      /* lights per cell, 1..LG_KMAX (15); also the live value LgSnapshot.k overrides it each frame */
    int k_static;               /* the split's static lights per cell, 0..LG_KMAX (LG_KSTATIC = 11: k - 4, so 4 dynamic lights always fit); 0 = no split */
    double cutoff;              /* lg_window_radius's cutoff for static lights (0.01) */
    int budget;                 /* static cell computations per frame, 0 = no limit (32: a 512-cell slab in 16 frames, at the far edge) */
    float exptab[256];          /* the engine's TexLightToLinear table */
    float minlight_floor;       /* [0x1804f13e0] */
    const volatile int *cancel; /* may be NULL: checked by lg_grid_new's split pass (AMB_ERR_CANCELLED), not kept */
} LgParams;
void lg_params_default(LgParams *p);

/* The engine state the grid needs, as the render thread copied it this frame */
typedef struct LgSnapshot {
    double cam[3];
    uint32_t dl_mask;           /* active dlights (0 if the engine's dlight in-use flag is 0) */
    int el_on;                  /* the elight in-use flag */
    LgDlight dl[LG_NDL], el[LG_NEL];
    int style[128];             /* d_lightstylevalue[0..127] */
    int k;                      /* photon_cluster_k now; 0 = p.k */
    float minlight_floor;       /* [0x1804f13e0] now (the engine changes it after load: 1.0 in the file, 1/256 in game); 0 = p.minlight_floor */
} LgSnapshot;

typedef struct LgStats {
    double ms;                  /* this frame's lg_grid_frame: set by the caller, which owns the clock */
    int window_moved, pending, computed, dirty_cells, dyn_lights, static_lights;
    int overflow_cells, overflow_lights;   /* now, over the window: cells with candidates beyond k, candidates dropped */
    int records_static, record_overflow;   /* static records in use; static candidates dropped for want of a record (this frame) */
    int max_n, dirty_rows;      /* max_n: the most lights of a cell assembled this frame */
    int fold_cells, fold_entries;  /* now, over the window: cells with a fold list, and its entries */
    int fold_lost;              /* now, over the window: candidates beyond LG_STATIC_MAX / LG_DYN_MAX, neither per pixel nor folded */
    int split_lights;           /* static lights with rho < r (the map's) */
    long fold_seq;              /* the fold table's sequence number */
} LgStats;

/* One light of the fold table: rec = lg_record(desc, scale, 0) (no window), the two window radii and W's gain (0: the whole is window(r)) */
typedef struct LgFoldLight { float rec[4][4]; float r, rho, we; } LgFoldLight;

typedef struct LgGrid LgGrid;
/* AMB_ERR_ARG / AMB_ERR_NOMEM / AMB_ERR_CANCELLED, or AMB_OK and *out */
int lg_grid_new(const LgMap *map, const LgParams *p, LgGrid **out);
void lg_grid_free(LgGrid *g);
int lg_grid_frame(LgGrid *g, const LgSnapshot *s, LgStats *st);
/* the CPU image (LG_TEX_W x LG_TEX_H x 4 floats, rows of LG_TEX_W * 4 floats) and the rows changed since the last take (cleared by it) */
const float *lg_grid_image(const LgGrid *g);
int lg_grid_take_dirty(LgGrid *g, uint8_t rows[LG_TEX_H]);
void lg_grid_constants(const LgGrid *g, float c63[4]);   /* (first window cell, 1 / cell) */
int lg_grid_static_count(const LgGrid *g);
/* The fold image (LG_FOLD_ROWS rows of LG_FOLD_ROW uint16; the texture's dirty rows below LG_REC_ROW cover it too) and the fold table
 * (lg_grid_static_count + LG_NDYN entries; *seq changes whenever an entry does) */
const uint16_t *lg_grid_fold_image(const LgGrid *g);
const LgFoldLight *lg_grid_fold_table(const LgGrid *g, int *n, long *seq);
/* The cube a draw at p gets for the folded part of a cell's lights: cell = that cell's LG_FOLD_STRIDE entries in the fold image, tab the fold
 * table, nstatic its static count. Each light adds max(0, axis . L) * colour * att(d) * spot * w to the 6 faces (+X -X +Y -Y +Z -Z), as the
 * engine's fold (FUN_1801d6970) with the attenuation of photon_lights_ps.h; w = whole(d) - window(rho) for a kept static, whole(d) for the
 * rest (lg_fold_whole). Entries naming no table entry are skipped. Float arithmetic in the shader's order. */
void lg_fold_eval(const LgFoldLight *tab, int ntab, int nstatic, const uint16_t *cell, const float p[3], float out[6][3]);
/* the window saturate(1 - d2^2 / R^4)^2 for squared distance d2 and radius R (0 for R <= 0), as photon_lights_ps.h */
float lg_window(float d2, float radius);
/* a fold table light's whole at squared distance d2: W = saturate((1 - d2^2 / r^4) we) when we > 0, else lg_window(d2, r) */
float lg_fold_whole(const LgFoldLight *l, float d2);
/* Tests: static grid light i's desc, window radius and cluster; the window's first cell; a record slot's refcount */
const LgDesc *lg_grid_static(const LgGrid *g, int i, double *r, int *cluster, int *style);
double lg_grid_rho(const LgGrid *g, int i);   /* static light i's per-pixel radius (-1 for no such light) */
void lg_grid_window(const LgGrid *g, int lo[3]);
int lg_grid_cell_state(const LgGrid *g, const int key[3]);   /* 0 not in the window's cache, 1 pending, 2 ready */
int lg_grid_refcount(const LgGrid *g, int slot);
#endif
