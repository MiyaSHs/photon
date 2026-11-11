/* P5: see lg_grid.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lg_grid.h"

/* Test only (scripts/run_ambient_test.sh): each removes one rule, and tests/ambient/lgtest.c must then fail.
 * 1 PVS ignored, 2 style-off lights kept, 3 a moved window keeps the old cell's texels, 4 last frame's dynamic cells not reassembled, 5 ranked weakest
 * first, 6 a static record freed while a cell still uses it, 7 the sphere tested against the cell centre, 8 negative cells wrap one cell off,
 * 9 only the centre's cluster; P5b (the split and the fold): 10 no split (rho = r), 11 kept split statics left out of the fold list, 12 a dropped dynamic
 * light left out of the fold list, 13 the per-pixel test d <= rho (one more static than k_static where the split cut), 14 the fold of a kept static is
 * window(r) (double counted), 15 the split ranks weakest first */
#ifndef LG_GRID_MUTANT
#define LG_GRID_MUTANT 0
#endif

enum { NSAMPLE = 9, DYN_NONE = 0xffff, MAX_BUCKET_ENTRIES = 4 << 20 };

typedef struct { LgDesc desc; double r, rho, lum; float we; int style, cluster; } Src;   /* we: W's gain (lg_grid.h) */
typedef struct { float imp; uint16_t id; uint16_t pp; } Cand;   /* id: static index, or 0x8000 | dynamic slot; pp: a static whose rho reaches the cell */
typedef struct {
    int key[3];
    uint8_t state;              /* 0 unassigned, 1 pending, 2 ready */
    uint8_t nclus, nstat, ndyn, nasm, overflow, dirty, touched, queued;
    uint8_t nfold, lost_stat, ndyn_all, lost;   /* fold entries; static candidates beyond the cap; dynamic candidates (all); lost now */
    int16_t clus[NSAMPLE];
    Cand stat[LG_STATIC_MAX];
    Cand dyn[LG_DYN_MAX];
    uint16_t asm_rec[LG_KMAX];
    uint16_t asm_id[LG_KMAX];
} Cell;
typedef struct { int slot, cluster; LgDesc desc; double r, lum; float scale; } Dyn;

struct LgGrid {
    const LgMap *map;
    LgParams p;
    int nsrc;
    Src *src;
    double bsize;
    int borg[3], bdim[3];
    int *bstart, *blist;
    Cell *cells;
    int lo[3], have_lo;
    int16_t *src_slot;          /* per static light: its record, -1 */
    int refcount[LG_NSTATIC_REC];
    int freestack[LG_NSTATIC_REC], nfree;
    float *image;
    uint8_t dirty[LG_TEX_H];
    uint64_t *pq;               /* cells waiting for their static candidates: distance key << 16 | cell, sorted near-first */
    int npq, pqhead;
    int npending;
    Dyn dyn[2][LG_NDYN];
    int ndyn[2], cur;
    int *touched[2], ntouched[2];
    int *dirtylist, ndirty;
    uint8_t style_on[128];
    int styles_known, k_now;
    int overflow_cells, overflow_lights;
    int record_overflow;
    /* P5b */
    uint16_t *fold;             /* the fold image */
    LgFoldLight *ftab;          /* nsrc + LG_NDYN */
    float *fscale;              /* per static light: the scale its fold entry was made with (-1: none yet) */
    long fseq;
    int fold_written, fold_cells, fold_entries, fold_lost, split;
};

void lg_params_default(LgParams *p) {
    memset(p, 0, sizeof *p);
    p->cell = 128;
    p->k = LG_KMAX;
    p->k_static = LG_KSTATIC;
    p->cutoff = 0.01;
    p->budget = 32;
    lg_exptab_default(p->exptab);
    p->minlight_floor = 1.0f;
}

static int imod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }
static int tor(const int c[3]) {
    if (LG_GRID_MUTANT == 8 && c[0] < 0) return imod(c[0] + 1, LG_GX) + LG_GX * imod(c[1], LG_GY) + LG_GX * LG_GY * imod(c[2], LG_GZ);
    return imod(c[0], LG_GX) + LG_GX * imod(c[1], LG_GY) + LG_GX * LG_GY * imod(c[2], LG_GZ);
}
static float *texel(LgGrid *g, int row, int col) { return g->image + ((size_t)row * LG_TEX_W + (size_t)col) * 4; }

/* the distance from p to the cell box, or -1 when it is certainly beyond r (the exact test, sqrt(d2) > r, stays the callers') */
static double box_dist(const int key[3], double s, const float p[3], double r) {
    double d2 = 0;
    for (int k = 0; k < 3; k++) {
        double lo = key[k] * s, hi = lo + s, v = p[k], d = 0;
        if (LG_GRID_MUTANT == 7) d = fabs(v - (lo + hi) * 0.5);
        else if (v < lo) d = lo - v;
        else if (v > hi) d = v - hi;
        d2 += d * d;
    }
    if (d2 > r * r * 1.000001) return -1.0;
    return sqrt(d2);
}

static int sees_list(const LgGrid *g, const int16_t *clus, int nclus, int cluster) {
    if (cluster < 0) return 0;
    if (LG_GRID_MUTANT == 1) return 1;
    for (int i = 0; i < nclus; i++) if (lg_cluster_sees(g->map, clus[i], cluster)) return 1;
    return 0;
}
static int sees(const LgGrid *g, const Cell *c, int cluster) { return sees_list(g, c->clus, c->nclus, cluster); }

/* the distinct clusters of a cell's 9 sample points (its centre and the 8 points half way to its corners) */
static int cell_clusters(const LgGrid *g, const int key[3], int16_t clus[NSAMPLE]) {
    double s = g->p.cell;
    int n = 0;
    for (int q = 0; q < NSAMPLE; q++) {
        if (LG_GRID_MUTANT == 9 && q) break;
        double pt[3];
        for (int k = 0; k < 3; k++) {
            double off = q == 0 ? 0.0 : ((((q - 1) >> k) & 1) ? 0.25 : -0.25);
            pt[k] = (key[k] + 0.5 + off) * s;
        }
        int cl = lg_point_cluster(g->map, pt);
        if (cl < 0) continue;
        int dup = 0;
        for (int j = 0; j < n; j++) dup |= clus[j] == cl;
        if (!dup) clus[n++] = (int16_t)cl;
    }
    return n;
}

/* a before b in a cell's list */
static int better(const Cand *a, const Cand *b) {
    if (LG_GRID_MUTANT == 5) return a->imp < b->imp;
    if (a->imp != b->imp) return a->imp > b->imp;
    return a->id < b->id;   /* static ids (< 0x8000) before dynamic ones */
}
/* inserts into a sorted list of at most cap; returns 0 if it fell off the end */
static int insert(Cand *list, uint8_t *n, int cap, Cand c) {
    int i = *n;
    if (i == cap) { if (!better(&c, &list[cap - 1])) return 0; i = cap - 1; }
    else (*n)++;
    while (i > 0 && better(&c, &list[i - 1])) { list[i] = list[i - 1]; i--; }
    list[i] = c;
    return 1;
}

/* --- setup ------------------------------------------------------------------------------------------------------- */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int build_buckets(LgGrid *g) {
    if (!g->nsrc) return AMB_OK;
    double mn[3] = { 1e300, 1e300, 1e300 }, mx[3] = { -1e300, -1e300, -1e300 };
    for (int i = 0; i < g->nsrc; i++)
        for (int k = 0; k < 3; k++) {
            double a = g->src[i].desc.pos[k] - g->src[i].r, b = g->src[i].desc.pos[k] + g->src[i].r;
            if (a < mn[k]) mn[k] = a;
            if (b > mx[k]) mx[k] = b;
        }
    for (double bs = g->p.cell * 4; bs <= 65536; bs *= 2) {
        long long cells = 1, entries = 0;
        int org[3], dim[3];
        for (int k = 0; k < 3; k++) {
            org[k] = (int)floor(mn[k] / bs);
            dim[k] = (int)floor(mx[k] / bs) - org[k] + 1;
            cells *= dim[k];
        }
        if (cells > MAX_BUCKET_ENTRIES) continue;
        for (int i = 0; i < g->nsrc; i++) {
            long long n = 1;
            for (int k = 0; k < 3; k++)
                n *= (long long)floor((g->src[i].desc.pos[k] + g->src[i].r) / bs) - (long long)floor((g->src[i].desc.pos[k] - g->src[i].r) / bs) + 1;
            entries += n;
        }
        if (entries > MAX_BUCKET_ENTRIES) continue;
        g->bsize = bs;
        memcpy(g->borg, org, sizeof org);
        memcpy(g->bdim, dim, sizeof dim);
        g->bstart = calloc((size_t)cells + 1, sizeof *g->bstart);
        g->blist = malloc((size_t)(entries ? entries : 1) * sizeof *g->blist);
        if (!g->bstart || !g->blist) return AMB_ERR_NOMEM;
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < g->nsrc; i++) {
                int a[3], b[3];
                for (int k = 0; k < 3; k++) {
                    a[k] = (int)floor((g->src[i].desc.pos[k] - g->src[i].r) / bs) - org[k];
                    b[k] = (int)floor((g->src[i].desc.pos[k] + g->src[i].r) / bs) - org[k];
                }
                for (int z = a[2]; z <= b[2]; z++)
                    for (int y = a[1]; y <= b[1]; y++)
                        for (int x = a[0]; x <= b[0]; x++) {
                            size_t bi = ((size_t)z * dim[1] + (size_t)y) * dim[0] + (size_t)x;
                            if (pass == 0) g->bstart[bi + 1]++;
                            else g->blist[g->bstart[bi]++] = i;
                        }
            }
            if (pass == 0) for (long long c = 0; c < cells; c++) g->bstart[c + 1] += g->bstart[c];
            else for (long long c = cells; c > 0; c--) g->bstart[c] = g->bstart[c - 1];
        }
        g->bstart[0] = 0;
        return AMB_OK;
    }
    return AMB_ERR_NOMEM;
}

/* THE SPLIT (lg_grid.h): rho of every static light, over every cell of the map that more than k_static lights' spheres can reach (a bucket's list
 * is a superset of each of its cells' candidates, so only buckets listing more than k_static lights are looked at). A cell's candidates and their
 * ranking are compute_static's (every style on, no cap); the ones ranked beyond k_static get rho <= their distance to the cell, so the per-pixel
 * test d < rho fails there. rho only ever shrinks, so the order of the cells does not matter. */
/* The same intended race as AmbBakeParams.cancel: one int, a late read only costs one more bucket (tests/ambient/worktest.supp matches the name) */
static __attribute__((noinline)) int split_cancelled(const LgParams *p) { return p->cancel && *p->cancel; }
static int cmp_cand(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    return better(x, y) ? -1 : better(y, x) ? 1 : 0;
}
static int compute_split(LgGrid *g) {
    for (int i = 0; i < g->nsrc; i++) g->src[i].rho = g->src[i].r;
    int ks = g->p.k_static;
    g->split = 0;
    if (!ks || !g->bsize || LG_GRID_MUTANT == 10) return AMB_OK;
    int per = (int)(g->bsize / g->p.cell + 0.5), maxl = 0;
    long long nb = (long long)g->bdim[0] * g->bdim[1] * g->bdim[2];
    for (long long b = 0; b < nb; b++) if (g->bstart[b + 1] - g->bstart[b] > maxl) maxl = g->bstart[b + 1] - g->bstart[b];
    Cand *cand = malloc((size_t)(maxl ? maxl : 1) * sizeof *cand);
    double *dist = malloc((size_t)g->nsrc * sizeof *dist);
    if (!cand || !dist) { free(cand); free(dist); return AMB_ERR_NOMEM; }
    double s = g->p.cell;
    for (int bz = 0; bz < g->bdim[2]; bz++)
        for (int by = 0; by < g->bdim[1]; by++)
            for (int bx = 0; bx < g->bdim[0]; bx++) {
                size_t bi = ((size_t)bz * g->bdim[1] + (size_t)by) * g->bdim[0] + (size_t)bx;
                int e0 = g->bstart[bi], e1 = g->bstart[bi + 1];
                if (e1 - e0 <= ks) continue;
                if (split_cancelled(&g->p)) { free(cand); free(dist); return AMB_ERR_CANCELLED; }
                int base[3] = { (g->borg[0] + bx) * per, (g->borg[1] + by) * per, (g->borg[2] + bz) * per };
                for (int cz = 0; cz < per; cz++)
                    for (int cy = 0; cy < per; cy++)
                        for (int cx = 0; cx < per; cx++) {
                            int key[3] = { base[0] + cx, base[1] + cy, base[2] + cz };
                            int16_t clus[NSAMPLE];
                            int nc = cell_clusters(g, key, clus), n = 0;
                            if (!nc) continue;
                            for (int e = e0; e < e1; e++) {
                                int si = g->blist[e];
                                const Src *src = &g->src[si];
                                double d = box_dist(key, s, src->desc.pos, src->r);
                                if (d < 0 || d > src->r || !sees_list(g, clus, nc, src->cluster)) continue;
                                Cand cd = { (float)lg_importance(&src->desc, src->r, d), (uint16_t)si, 0 };
                                if (!(cd.imp > 0)) continue;
                                dist[si] = d;
                                cand[n++] = cd;
                            }
                            if (n <= ks) continue;
                            qsort(cand, (size_t)n, sizeof *cand, cmp_cand);
                            for (int j = LG_GRID_MUTANT == 15 ? 0 : ks; j < (LG_GRID_MUTANT == 15 ? n - ks : n); j++) {
                                Src *src = &g->src[cand[j].id];
                                if (dist[cand[j].id] < src->rho) src->rho = dist[cand[j].id];
                            }
                        }
            }
    free(cand);
    free(dist);
    for (int i = 0; i < g->nsrc; i++) g->split += g->src[i].rho < g->src[i].r;
    return AMB_OK;
}

void lg_grid_free(LgGrid *g) {
    if (!g) return;
    free(g->src);
    free(g->bstart);
    free(g->blist);
    free(g->cells);
    free(g->src_slot);
    free(g->image);
    free(g->touched[0]);
    free(g->touched[1]);
    free(g->dirtylist);
    free(g->pq);
    free(g->fold);
    free(g->ftab);
    free(g->fscale);
    free(g);
}

int lg_grid_new(const LgMap *map, const LgParams *p, LgGrid **out) {
    if (!out) return AMB_ERR_ARG;
    *out = NULL;
    if (!map || !p || !(p->cell >= 1 && p->cell <= 4096) || p->k < 1 || p->k > LG_KMAX || p->k_static < 0 || p->k_static > LG_KMAX || !(p->cutoff > 0) ||
        p->budget < 0) return AMB_ERR_ARG;
    LgGrid *g = calloc(1, sizeof *g);
    if (!g) return AMB_ERR_NOMEM;
    g->map = map;
    g->p = *p;
    int nw;
    const LgWorldLight *w = lg_map_lights(map, &nw);
    LgMapInfo mi;
    lg_map_info(map, &mi);
    int hdr = mi.light_lump == 54;   /* FUN_1800d98d0's constant follows the lump the engine loaded [likely: its flag is Mod_LoadWorldlights' HDR one] */
    g->src = calloc(nw ? (size_t)nw : 1, sizeof *g->src);
    g->src_slot = malloc((nw ? (size_t)nw : 1) * sizeof *g->src_slot);
    g->cells = calloc(LG_NCELL, sizeof *g->cells);
    g->image = calloc((size_t)LG_TEX_W * LG_TEX_H * 4, sizeof *g->image);
    g->touched[0] = malloc(LG_NCELL * sizeof(int));
    g->touched[1] = malloc(LG_NCELL * sizeof(int));
    g->dirtylist = malloc(LG_NCELL * sizeof(int));
    g->pq = malloc(2 * LG_NCELL * sizeof *g->pq);   /* consumed entries stay until the next compaction: at most one live entry per cell besides */
    g->fold = calloc((size_t)LG_FOLD_ROWS * LG_FOLD_ROW, sizeof *g->fold);
    if (!g->src || !g->src_slot || !g->cells || !g->image || !g->touched[0] || !g->touched[1] || !g->dirtylist || !g->pq || !g->fold) { lg_grid_free(g); return AMB_ERR_NOMEM; }
    for (int i = 0; i < nw && g->nsrc < LG_NSTATIC_MAX; i++) {
        Src s;
        memset(&s, 0, sizeof s);
        if (!lg_desc_from_worldlight(&w[i], &s.desc) || s.desc.type == 2) continue;
        s.r = lg_window_radius(&w[i], p->cutoff);
        if (!(s.r > 0)) continue;
        double re = lg_window_radius(&w[i], hdr ? 1.0 / 66.66667175292969 : 1.0 / 33.333335876464844), xe = re / s.r;   /* the engine's radius */
        if (!(xe < 0.9)) xe = 0.9;
        s.we = (float)(1.0 / (1.0 - xe * xe * xe * xe));
        s.style = w[i].style;
        s.cluster = w[i].cluster;
        if (s.cluster < 0) { double o[3] = { w[i].origin[0], w[i].origin[1], w[i].origin[2] }; s.cluster = lg_point_cluster(map, o); }
        g->src_slot[g->nsrc] = -1;
        g->src[g->nsrc++] = s;
    }
    int rc = build_buckets(g);
    if (!rc) rc = compute_split(g);
    if (!rc) {
        g->ftab = calloc((size_t)g->nsrc + LG_NDYN, sizeof *g->ftab);
        g->fscale = malloc((g->nsrc ? (size_t)g->nsrc : 1) * sizeof *g->fscale);
        if (!g->ftab || !g->fscale) rc = AMB_ERR_NOMEM;
        else for (int i = 0; i < g->nsrc; i++) g->fscale[i] = -1.0f;
    }
    if (rc) { lg_grid_free(g); return rc; }
    g->p.cancel = NULL;
    for (int i = 0; i < LG_NSTATIC_REC; i++) g->freestack[i] = LG_NSTATIC_REC - 1 - i;
    g->nfree = LG_NSTATIC_REC;
    g->k_now = p->k;
    memset(g->dirty, 1, sizeof g->dirty);   /* the first take uploads everything */
    *out = g;
    return AMB_OK;
}

/* --- records ------------------------------------------------------------------------------------------------------ */
static int acquire(LgGrid *g, int s) {
    if (g->src_slot[s] >= 0) { g->refcount[g->src_slot[s]]++; return g->src_slot[s]; }
    if (!g->nfree) return -1;
    int slot = g->freestack[--g->nfree];
    g->src_slot[s] = (int16_t)slot;
    g->refcount[slot] = 1;
    return slot;
}
static void release(LgGrid *g, int s) {
    int slot = g->src_slot[s];
    if (slot < 0) return;
    if (LG_GRID_MUTANT == 6 || --g->refcount[slot] == 0) {
        g->refcount[slot] = 0;
        g->src_slot[s] = -1;
        g->freestack[g->nfree++] = slot;
    }
}

/* --- cells -------------------------------------------------------------------------------------------------------- */
static void mark(LgGrid *g, int i) {
    if (!g->cells[i].dirty) { g->cells[i].dirty = 1; g->dirtylist[g->ndirty++] = i; }
}

static void write_cell(LgGrid *g, int i) {
    Cell *c = &g->cells[i];
    int row = i / LG_GX, col = 4 * (i % LG_GX);
    float *t = texel(g, row, col);
    memset(t, 0, 16 * sizeof(float));
    t[0] = (float)c->nasm;
    for (int k = 0; k < c->nasm; k++) t[k + 1] = (float)c->asm_rec[k];
    g->dirty[row] = 1;
}

static uint16_t *fold_of(LgGrid *g, int i) { return g->fold + (size_t)(i / LG_GX) * LG_FOLD_ROW + (size_t)(i % LG_GX) * LG_FOLD_STRIDE; }
static void write_fold(LgGrid *g, int i, const uint16_t *list, int n) {
    uint16_t *t = fold_of(g, i);
    memset(t, 0, LG_FOLD_STRIDE * sizeof *t);
    t[0] = (uint16_t)n;
    if (n) memcpy(t + 2, list, (size_t)n * sizeof *list);
    g->dirty[i / LG_GX] = 1;
    g->fold_written = 1;
}

static void retire(LgGrid *g, int i, const int key[3]) {
    Cell *c = &g->cells[i];
    for (int k = 0; k < c->nasm; k++) if (!(c->asm_id[k] & 0x8000)) release(g, c->asm_id[k]);
    if (c->overflow) { g->overflow_cells--; g->overflow_lights -= c->overflow; }
    if (c->nfold) { g->fold_cells--; g->fold_entries -= c->nfold; }
    g->fold_lost -= c->lost;
    if (LG_GRID_MUTANT != 3) write_fold(g, i, NULL, 0);
    uint8_t dirty = c->dirty, queued = c->queued;
    memset(c, 0, sizeof *c);
    c->dirty = dirty;
    c->queued = queued;
    memcpy(c->key, key, sizeof c->key);
    c->state = 1;
    g->npending++;
    if (LG_GRID_MUTANT != 3) write_cell(g, i);
}

static void compute_static(LgGrid *g, int i) {
    Cell *c = &g->cells[i];
    double s = g->p.cell;
    c->nclus = (uint8_t)cell_clusters(g, c->key, c->clus);
    c->nstat = 0;
    c->lost_stat = 0;
    if (!c->nclus || !g->bsize) return;
    int b[3];
    for (int k = 0; k < 3; k++) {
        b[k] = (int)floor((c->key[k] + 0.5) * s / g->bsize) - g->borg[k];
        if (b[k] < 0 || b[k] >= g->bdim[k]) return;
    }
    size_t bi = ((size_t)b[2] * g->bdim[1] + (size_t)b[1]) * g->bdim[0] + (size_t)b[0];
    int tot = 0;
    for (int e = g->bstart[bi]; e < g->bstart[bi + 1]; e++) {
        int si = g->blist[e];
        const Src *src = &g->src[si];
        double d = box_dist(c->key, s, src->desc.pos, src->r);
        if (d < 0 || d > src->r || !sees(g, c, src->cluster)) continue;
        Cand cd = { (float)lg_importance(&src->desc, src->r, d), (uint16_t)si, (uint16_t)(LG_GRID_MUTANT == 13 ? d <= src->rho : d < src->rho) };
        if (!(cd.imp > 0)) continue;
        tot++;
        insert(c->stat, &c->nstat, LG_STATIC_MAX, cd);
    }
    c->lost_stat = (uint8_t)(tot - c->nstat > 255 ? 255 : tot - c->nstat);
}

static int style_on(const LgGrid *g, int style) { return (unsigned)style >= 128u || g->style_on[style] || LG_GRID_MUTANT == 2; }

static void assemble(LgGrid *g, int i) {
    Cell *c = &g->cells[i];
    c->dirty = 0;
    if (c->state != 2) return;
    int k = g->k_now;
    /* the per-pixel statics (style on) while they fit, then the dynamic candidates by importance while the list is below k, merged in importance order */
    Cand ks[LG_KMAX], out[LG_KMAX];
    uint8_t on[LG_STATIC_MAX], keptq[LG_STATIC_MAX];   /* per stat entry: style on; in ks */
    int nks = 0, npp = 0;
    for (int a = 0; a < c->nstat; a++) {
        on[a] = (uint8_t)style_on(g, g->src[c->stat[a].id].style);
        keptq[a] = 0;
        if (!on[a] || !c->stat[a].pp) continue;
        npp++;
        if (nks < k) { ks[nks++] = c->stat[a]; keptq[a] = 1; }
    }
    int nkd = c->ndyn < k - nks ? c->ndyn : k - nks, n = 0, a = 0, b = 0;
    while (a < nks || b < nkd) {
        const Cand *x = a < nks ? &ks[a] : NULL, *y = b < nkd ? &c->dyn[b] : NULL;
        const Cand *pick = !y || (x && better(x, y)) ? x : y;
        if (pick == x) a++; else b++;
        out[n++] = *pick;
    }
    int total = npp + c->ndyn;
    int old_n = c->nasm, old_over = c->overflow, same = n == old_n;
    for (int j = 0; j < n && same; j++) same = out[j].id == c->asm_id[j];
    if (!same) {
        uint16_t old_id[LG_KMAX];
        memcpy(old_id, c->asm_id, sizeof old_id);
        int m = 0;
        for (int j = 0; j < n; j++) {
            int rec;
            if (out[j].id & 0x8000) rec = LG_NSTATIC_REC + (out[j].id & 0x7fff);
            else if ((rec = acquire(g, out[j].id)) < 0) { g->record_overflow++; continue; }
            c->asm_id[m] = out[j].id;
            c->asm_rec[m] = (uint16_t)rec;
            m++;
        }
        c->nasm = (uint8_t)m;
        for (int j = 0; j < old_n; j++) if (!(old_id[j] & 0x8000)) release(g, old_id[j]);
        write_cell(g, i);
    }
    c->overflow = (uint8_t)(total > n ? total - n : 0);
    g->overflow_cells += (c->overflow > 0) - (old_over > 0);
    g->overflow_lights += c->overflow - old_over;
    /* the fold list: what is not per pixel in full, from the lights the cell really holds (a record overflow folds its light). When every wanted
     * light got its record the cell holds exactly ks and the first nkd dynamic candidates; otherwise its list is searched. */
    uint16_t fold[LG_FOLD_MAX];
    int nf = 0, full = c->nasm == n;
    for (int q = 0; q < c->nstat; q++) {
        if (!on[q]) continue;
        int id = c->stat[q].id, kept = keptq[q];
        if (!full) { kept = 0; for (int j = 0; j < c->nasm && !kept; j++) kept = c->asm_id[j] == id; }
        if (kept && LG_GRID_MUTANT == 11 && g->src[id].rho < g->src[id].r) continue;
        fold[nf++] = (uint16_t)(id | (kept ? LG_FOLD_KEPT : 0));
    }
    for (int q = nkd; q < c->ndyn; q++) {   /* dynamic records never run out: the first nkd are held */
        if (LG_GRID_MUTANT == 12 && q == c->ndyn - 1) continue;
        fold[nf++] = (uint16_t)(LG_FOLD_DYN | (c->dyn[q].id & 0x7fff));
    }
    const uint16_t *t = fold_of(g, i);
    if (t[0] != nf || (nf && memcmp(t + 2, fold, (size_t)nf * sizeof *fold) != 0)) write_fold(g, i, fold, nf);
    g->fold_cells += (nf > 0) - (c->nfold > 0);
    g->fold_entries += nf - c->nfold;
    c->nfold = (uint8_t)nf;
    int lost = c->lost_stat + (c->ndyn_all - c->ndyn);
    if (lost > 255) lost = 255;
    g->fold_lost += lost - c->lost;
    c->lost = (uint8_t)lost;
}

/* --- dynamic lights ------------------------------------------------------------------------------------------------------ */
static int convert_dyn(LgGrid *g, const LgSnapshot *s, Dyn *out) {
    int n = 0;
    for (int which = 0; which < 2; which++) {
        int cnt = which ? LG_NEL : LG_NDL;
        for (int i = 0; i < cnt; i++) {
            const LgDlight *dl = which ? &s->el[i] : &s->dl[i];
            if (which ? !(s->el_on && dl->radius > 0.0f) : !(s->dl_mask >> i & 1)) continue;
            if (dl->flags & LG_DLIGHT_FLAGS_REJECT) continue;
            LgWorldLight w;
            lg_worldlight_from_dlight(dl, g->p.exptab, s->minlight_floor > 0.0f ? s->minlight_floor : g->p.minlight_floor, &w);
            Dyn *d = &out[n];
            memset(d, 0, sizeof *d);
            if (!lg_desc_from_worldlight(&w, &d->desc)) continue;
            double o[3] = { w.origin[0], w.origin[1], w.origin[2] };
            d->cluster = lg_point_cluster(g->map, o);
            if (d->cluster < 0) continue;     /* the engine's rule */
            d->slot = which ? LG_NDL + i : i;
            d->r = w.radius;
            d->scale = lg_style_scale(s->style, w.style);
            if (!(d->scale > 0)) continue;
            n++;
        }
    }
    return n;
}

static void bin_dyn(LgGrid *g, const Dyn *d) {
    double s = g->p.cell;
    int a[3], b[3];
    for (int k = 0; k < 3; k++) {
        a[k] = (int)floor((d->desc.pos[k] - d->r) / s);
        b[k] = (int)floor((d->desc.pos[k] + d->r) / s);
        int lo = g->lo[k], hi = g->lo[k] + (k == 0 ? LG_GX : k == 1 ? LG_GY : LG_GZ) - 1;
        if (a[k] < lo) a[k] = lo;
        if (b[k] > hi) b[k] = hi;
        if (a[k] > b[k]) return;
    }
    LgDesc scaled = d->desc;
    for (int k = 0; k < 3; k++) scaled.color[k] = d->scale * d->desc.color[k];
    int cur = g->cur;
    /* per axis: the squared distance to the cell's slab and its toroidal part, so a cell costs three adds before its memory is touched; the sum is
     * box_dist's own, in its order */
    double ax[3][LG_GX];
    int at[3][LG_GX];
    for (int k = 0; k < 3; k++)
        for (int v = a[k]; v <= b[k]; v++) {
            double lo = v * s, hi = lo + s, p = d->desc.pos[k], e = 0;
            if (LG_GRID_MUTANT == 7) e = fabs(p - (lo + hi) * 0.5);
            else if (p < lo) e = lo - p;
            else if (p > hi) e = p - hi;
            ax[k][v - a[k]] = e * e;
            int key[3] = { 0, 0, 0 };
            key[k] = v;
            at[k][v - a[k]] = tor(key);
        }
    double rr = d->r * d->r * 1.000001;
    for (int z = a[2]; z <= b[2]; z++)
        for (int y = a[1]; y <= b[1]; y++)
            for (int x = a[0]; x <= b[0]; x++) {
                double d2 = ax[0][x - a[0]] + ax[1][y - a[1]] + ax[2][z - a[2]];
                if (d2 > rr) continue;
                double dist = sqrt(d2);
                if (dist > d->r) continue;
                int i = at[0][x - a[0]] + at[1][y - a[1]] + at[2][z - a[2]];
                Cell *c = &g->cells[i];
                if (c->state != 2 || !sees(g, c, d->cluster)) continue;
                Cand cd = { (float)lg_importance(&scaled, d->r, dist), (uint16_t)(0x8000 | d->slot), 0 };
                if (!(cd.imp > 0)) continue;
                if (!(c->touched & (1 << cur))) { c->touched |= (uint8_t)(1 << cur); g->touched[cur][g->ntouched[cur]++] = i; }
                if (c->ndyn_all < 255) c->ndyn_all++;
                insert(c->dyn, &c->ndyn, LG_DYN_MAX, cd);
                mark(g, i);
            }
}

static int same_dyn(const Dyn *a, int na, const Dyn *b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++)
        if (a[i].slot != b[i].slot || a[i].cluster != b[i].cluster || a[i].scale != b[i].scale || a[i].r != b[i].r ||
            memcmp(&a[i].desc, &b[i].desc, sizeof a[i].desc) != 0) return 0;
    return 1;
}

/* --- the frame ----------------------------------------------------------------------------------------------------------- */
int lg_grid_frame(LgGrid *g, const LgSnapshot *s, LgStats *st) {
    LgStats z;
    memset(&z, 0, sizeof z);
    g->record_overflow = 0;
    int lo[3];
    static const int half[3] = { LG_GX / 2, LG_GY / 2, LG_GZ / 2 };
    for (int k = 0; k < 3; k++) {
        double v = s->cam[k] / g->p.cell;
        if (!(fabs(v) < 1e7)) return AMB_ERR_ARG;
        lo[k] = (int)floor(v) - half[k];
    }
    /* 1. the window: only the cells that were not in the last one are touched */
    if (!g->have_lo || memcmp(lo, g->lo, sizeof lo) != 0) {
        z.window_moved = 1;
        int had = g->have_lo, olo[3];
        memcpy(olo, g->lo, sizeof olo);
        memcpy(g->lo, lo, sizeof lo);
        g->have_lo = 1;
        int pushed = 0;
        for (int zz = 0; zz < LG_GZ; zz++)
            for (int y = 0; y < LG_GY; y++)
                for (int x = 0; x < LG_GX; x++) {
                    int key[3] = { lo[0] + x, lo[1] + y, lo[2] + zz };
                    if (had && key[0] >= olo[0] && key[0] < olo[0] + LG_GX && key[1] >= olo[1] && key[1] < olo[1] + LG_GY && key[2] >= olo[2] &&
                        key[2] < olo[2] + LG_GZ) continue;
                    int i = tor(key);
                    if (g->cells[i].state == 1) g->npending--;
                    retire(g, i, key);
                    if (!g->cells[i].queued) { g->cells[i].queued = 1; g->pq[g->npq++] = (uint64_t)i; pushed = 1; }
                }
        if (pushed) {   /* near the camera first: compact the queue, then sort it by the distance to the window's centre */
            int w = 0;
            for (int j = g->pqhead; j < g->npq; j++) {
                int i = (int)(g->pq[j] & 0xffff);
                uint64_t d2 = 0;
                static const int dim[3] = { LG_GX, LG_GY, LG_GZ };
                for (int k = 0; k < 3; k++) { int64_t d = 2 * (int64_t)(g->cells[i].key[k] - lo[k]) - dim[k] + 1; d2 += (uint64_t)(d * d); }
                g->pq[w++] = d2 << 16 | (uint64_t)i;
            }
            g->npq = w;
            g->pqhead = 0;
            qsort(g->pq, (size_t)g->npq, sizeof g->pq[0], cmp_u64);
        }
    }
    /* 2. static candidates, near-first */
    int budget = g->p.budget ? g->p.budget : LG_NCELL;
    while (g->pqhead < g->npq && budget > 0) {
        int i = (int)(g->pq[g->pqhead++] & 0xffff);
        Cell *c = &g->cells[i];
        c->queued = 0;
        if (c->state != 1) continue;
        compute_static(g, i);
        c->state = 2;
        g->npending--;
        budget--;
        z.computed++;
        mark(g, i);
    }
    if (g->pqhead == g->npq) g->pqhead = g->npq = 0;
    /* 3. dynamic lights: rebinned when anything about them changed, and then every cell they touched last frame or touch now is reassembled */
    int nxt = g->cur ^ 1;
    int nd = convert_dyn(g, s, g->dyn[nxt]);
    z.dyn_lights = nd;
    if (!same_dyn(g->dyn[g->cur], g->ndyn[g->cur], g->dyn[nxt], nd) || z.window_moved || z.computed) {
        int prev = g->cur;
        g->cur = nxt;
        g->ndyn[nxt] = nd;
        for (int j = 0; j < g->ntouched[prev]; j++) {
            Cell *c = &g->cells[g->touched[prev][j]];
            c->touched &= (uint8_t)~(1 << prev);
            c->ndyn = 0;
            c->ndyn_all = 0;
            if (LG_GRID_MUTANT != 4) mark(g, g->touched[prev][j]);
        }
        g->ntouched[prev] = 0;
        g->ntouched[nxt] = 0;
        for (int j = 0; j < nd; j++) bin_dyn(g, &g->dyn[nxt][j]);
    }
    /* 4. style on/off changes and k */
    uint8_t on[128];
    int changed = !g->styles_known;
    for (int k = 0; k < 128; k++) { on[k] = s->style[k] != 0; changed |= on[k] != g->style_on[k]; }
    int k_new = s->k > 0 ? (s->k < LG_KMAX ? s->k : LG_KMAX) : g->p.k;
    if (changed || k_new != g->k_now) {
        uint8_t was[128];
        memcpy(was, g->style_on, sizeof was);
        memcpy(g->style_on, on, sizeof on);
        int all = !g->styles_known || k_new != g->k_now;
        g->styles_known = 1;
        g->k_now = k_new;
        for (int i = 0; i < LG_NCELL; i++) {
            Cell *c = &g->cells[i];
            if (c->state != 2) continue;
            int hit = all;
            for (int j = 0; j < c->nstat && !hit; j++) { int sy = g->src[c->stat[j].id].style; hit = (unsigned)sy < 128u && was[sy] != on[sy]; }
            if (hit) mark(g, i);
        }
    }
    /* 5. reassemble */
    z.dirty_cells = g->ndirty;
    for (int j = 0; j < g->ndirty; j++) {
        assemble(g, g->dirtylist[j]);
        if (g->cells[g->dirtylist[j]].nasm > z.max_n) z.max_n = g->cells[g->dirtylist[j]].nasm;
    }
    g->ndirty = 0;
    /* 6. records */
    float rec[4][4];
    for (int si = 0; si < g->nsrc; si++) {
        int slot = g->src_slot[si];
        if (slot < 0) continue;
        z.records_static++;
        lg_record(&g->src[si].desc, lg_style_scale(s->style, g->src[si].style), (float)g->src[si].rho, rec);
        float *t = texel(g, LG_REC_ROW + slot / 32, 4 * (slot % 32));
        if (memcmp(t, rec, sizeof rec)) { memcpy(t, rec, sizeof rec); g->dirty[LG_REC_ROW + slot / 32] = 1; }
    }
    /* 7. the fold table (P5b): a static entry when its scale changed, a dynamic one when its record did */
    int fchanged = g->fold_written;
    g->fold_written = 0;
    for (int si = 0; si < g->nsrc; si++) {
        float sc = lg_style_scale(s->style, g->src[si].style);
        if (sc == g->fscale[si]) continue;
        g->fscale[si] = sc;
        LgFoldLight *f = &g->ftab[si];
        lg_record(&g->src[si].desc, sc, 0.0f, f->rec);
        f->r = (float)g->src[si].r;
        f->rho = (float)g->src[si].rho;
        f->we = g->src[si].we;
        fchanged = 1;
    }
    const Dyn *dd = g->dyn[g->cur];
    for (int j = 0; j < g->ndyn[g->cur]; j++) {
        int slot = LG_NSTATIC_REC + dd[j].slot;
        lg_record(&dd[j].desc, dd[j].scale, (float)dd[j].r, rec);
        float *t = texel(g, LG_REC_ROW + slot / 32, 4 * (slot % 32));
        if (memcmp(t, rec, sizeof rec)) { memcpy(t, rec, sizeof rec); g->dirty[LG_REC_ROW + slot / 32] = 1; }
        LgFoldLight fl;
        lg_record(&dd[j].desc, dd[j].scale, 0.0f, fl.rec);
        fl.r = fl.rho = (float)dd[j].r;
        fl.we = 0.0f;
        LgFoldLight *f = &g->ftab[g->nsrc + dd[j].slot];
        if (memcmp(f, &fl, sizeof fl)) { *f = fl; fchanged = 1; }
    }
    g->fseq += fchanged;
    z.pending = g->npending;
    z.static_lights = g->nsrc;
    z.overflow_cells = g->overflow_cells;
    z.overflow_lights = g->overflow_lights;
    z.record_overflow = g->record_overflow;
    z.fold_cells = g->fold_cells;
    z.fold_entries = g->fold_entries;
    z.fold_lost = g->fold_lost;
    z.split_lights = g->split;
    z.fold_seq = g->fseq;
    for (int r = 0; r < LG_TEX_H; r++) z.dirty_rows += g->dirty[r];
    if (st) *st = z;
    return AMB_OK;
}

const float *lg_grid_image(const LgGrid *g) { return g->image; }

int lg_grid_take_dirty(LgGrid *g, uint8_t rows[LG_TEX_H]) {
    int n = 0;
    for (int r = 0; r < LG_TEX_H; r++) { rows[r] = g->dirty[r]; n += g->dirty[r]; g->dirty[r] = 0; }
    return n;
}

void lg_grid_constants(const LgGrid *g, float c63[4]) {
    for (int k = 0; k < 3; k++) c63[k] = (float)g->lo[k];
    c63[3] = (float)(1.0 / g->p.cell);
}

int lg_grid_static_count(const LgGrid *g) { return g->nsrc; }
double lg_grid_rho(const LgGrid *g, int i) { return i >= 0 && i < g->nsrc ? g->src[i].rho : -1.0; }
const uint16_t *lg_grid_fold_image(const LgGrid *g) { return g->fold; }
const LgFoldLight *lg_grid_fold_table(const LgGrid *g, int *n, long *seq) {
    if (n) *n = g->nsrc + LG_NDYN;
    if (seq) *seq = g->fseq;
    return g->ftab;
}

/* --- the fold (P5b) ------------------------------------------------------------------------------------------------------- */
float lg_window(float d2, float radius) {
    if (!(radius > 0.0f)) return 0.0f;
    float q = d2 * (1.0f / (radius * radius));
    float w = 1.0f - q * q;
    w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
    return w * w;
}

float lg_fold_whole(const LgFoldLight *l, float d2) {
    if (!(l->we > 0.0f)) return lg_window(d2, l->r);
    if (!(l->r > 0.0f)) return 0.0f;
    float q = d2 * (1.0f / (l->r * l->r));
    float w = (1.0f - q * q) * l->we;
    return w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
}

void lg_fold_eval(const LgFoldLight *tab, int ntab, int nstatic, const uint16_t *cell, const float p[3], float out[6][3]) {
    memset(out, 0, 18 * sizeof(float));
    int n = cell[0];
    if (n > LG_FOLD_MAX) n = LG_FOLD_MAX;
    for (int j = 0; j < n; j++) {
        unsigned e = cell[2 + j];
        int dyn = (e & LG_FOLD_DYN) != 0, kept = !dyn && (e & LG_FOLD_KEPT);
        int idx = dyn ? nstatic + (int)(e & 0x7fff) : (int)(e & 0x3fff);
        if (idx < 0 || idx >= ntab) continue;
        const LgFoldLight *L = &tab[idx];
        const float (*r)[4] = L->rec;
        if (r[1][3] > 1.5f) continue;   /* directional: never a grid light */
        float to[3] = { r[0][0] - p[0], r[0][1] - p[1], r[0][2] - p[2] };
        float d2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
        if (d2 < 1.0e-6f) d2 = 1.0e-6f;
        float ood = 1.0f / sqrtf(d2), l[3] = { to[0] * ood, to[1] * ood, to[2] * ood };
        float w = lg_fold_whole(L, d2);
        if (kept) w -= LG_GRID_MUTANT == 14 ? 0.0f : lg_window(d2, L->rho);
        if (!(w > 0.0f)) continue;
        float att = r[3][0] + r[3][1] * (d2 * ood) + r[3][2] * d2;
        float a = 1.0f / (att > 1.0e-6f ? att : 1.0e-6f);
        if (r[1][3] > 0.5f) {
            float cd = r[2][0] * -l[0] + r[2][1] * -l[1] + r[2][2] * -l[2] - r[3][3];
            float sp = powf(cd > 0.0001f ? cd : 0.0001f, r[2][3]);
            a *= sp < 0.0f ? 0.0f : sp > 1.0f ? 1.0f : sp;
        }
        a *= w;
        for (int f = 0; f < 6; f++) {
            float dt = (f & 1) ? -l[f >> 1] : l[f >> 1];
            if (!(dt > 0.0f)) continue;
            float v = dt * a;
            for (int c = 0; c < 3; c++) out[f][c] += v * r[1][c];
        }
    }
}

const LgDesc *lg_grid_static(const LgGrid *g, int i, double *r, int *cluster, int *style) {
    if (i < 0 || i >= g->nsrc) return NULL;
    if (r) *r = g->src[i].r;
    if (cluster) *cluster = g->src[i].cluster;
    if (style) *style = g->src[i].style;
    return &g->src[i].desc;
}

void lg_grid_window(const LgGrid *g, int lo[3]) { memcpy(lo, g->lo, 3 * sizeof(int)); }
int lg_grid_cell_state(const LgGrid *g, const int key[3]) {
    const Cell *c = &g->cells[tor(key)];
    return memcmp(c->key, key, sizeof c->key) == 0 ? c->state : 0;
}
int lg_grid_refcount(const LgGrid *g, int slot) { return slot >= 0 && slot < LG_NSTATIC_REC ? g->refcount[slot] : -1; }
