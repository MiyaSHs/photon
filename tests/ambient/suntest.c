/* P6 sun visibility, host tests of src/ambient/sun_vis.c (analysis/22_p6_sun.md):
 *   synth    a hand-built BSP (tree, brushes with sky, glass, water and opaque contents, a brush overhang, two displacements facing up and
 *            down, a point inside a brush): the facts the engine's rule implies, and every point of a grid against the reference below;
 *   ref      an independent reference written from the BSP format alone: every brush of model 0's tree (no tree walk, no epsilon), every
 *            displacement rebuilt in double, exact one-sided triangles. On gm_construct and gm_flatgrass, random points anywhere in the
 *            world box: the core's lit/shadow equals the reference's except at a near tie (two surfaces within 0.1 unit along the ray, or a
 *            ray grazing a brush or a triangle edge), and those are counted;
 *   read     the reader route equals the memory route, inside a GMA too (amb_gma_find + amb_sub_reader), peak bytes held, cancel and short
 *            reads, truncated and byte-mutated files (no crash, under ASan), the sun equals lg_map.c's emit_skylight on both lumps;
 *   cache    against the exact answer at the cell centre, with a tiny table (collisions), far and NaN points, a map without a sun;
 *   bench    single-thread host speed: the sun trace with and without scratch, the whole model rule, a cache hit, a P6-like workload.
 * Modes: suntest [--bench N] <construct.bsp> <flatgrass.bsp>;  suntest --compare <map.bsp> <harness lines>  (analysis/22's numbers);
 *        suntest --info <map.bsp>. Prints SUNTEST OK. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/ambient/sun_vis.h"
#include "../../src/ambient/lg_map.h"
#include "../../src/ambient/amb_gma.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } fails++; } } while (0)

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static double rndu(void) { return (double)(rnd64() >> 11) / 9007199254740992.0; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

typedef struct { const uint8_t *b; size_t n; int short_at; volatile int cancel; long long calls; } Mem;
static int mem_read(void *ctx, long long ofs, void *buf, int n) {
    Mem *m = ctx;
    m->calls++;
    if (ofs < 0 || (size_t)ofs > m->n || (size_t)n > m->n - (size_t)ofs) return 0;
    if (m->short_at && m->calls == m->short_at) return n - 1;
    memcpy(buf, m->b + ofs, (size_t)n);
    return n;
}
static AmbReader mem_reader(Mem *m) { AmbReader r = { m, (long long)m->n, mem_read, &m->cancel }; return r; }

/* =================================================================================================================================== */
/* The reference: written from the BSP format (public SDK bspfile.h) and the engine's rule as sun_vis.h states it, sharing no code      */

static int32_t r32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static int16_t r16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static uint16_t ru16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static float rf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static const uint8_t *lump(const uint8_t *b, int i, size_t *len, int *ver) {
    const uint8_t *h = b + 8 + 16 * i;
    *len = (size_t)r32(h + 4);
    if (ver) *ver = r32(h + 8);
    return b + r32(h);
}

typedef struct { double n[3], d; } RPlane;
typedef struct { int nsides; int contents, orig; RPlane *pl; int *flags; } RBrush;
typedef struct { int w, contents, flags; double (*v)[3]; } RDisp;
typedef struct {
    int nb, nd;
    RBrush *b;
    RDisp *d;
    /* the point-leaf walk, for choosing points */
    int head;
    int *node_plane, (*node_child)[2], *leaf_contents;
    RPlane *planes;
} Ref;

static void ref_free(Ref *r) {
    for (int i = 0; i < r->nb; i++) { free(r->b[i].pl); free(r->b[i].flags); }
    for (int i = 0; i < r->nd; i++) free(r->d[i].v);
    free(r->b); free(r->d); free(r->node_plane); free(r->node_child); free(r->leaf_contents); free(r->planes);
    memset(r, 0, sizeof *r);
}

static void ref_load(Ref *r, const uint8_t *b) {
    memset(r, 0, sizeof *r);
    size_t n;
    int ver;
    const uint8_t *P = lump(b, 1, &n, NULL);
    int np = (int)(n / 20);
    r->planes = calloc((size_t)np, sizeof *r->planes);
    for (int i = 0; i < np; i++) { for (int k = 0; k < 3; k++) r->planes[i].n[k] = rf(P + 20 * i + 4 * k); r->planes[i].d = rf(P + 20 * i + 12); }
    const uint8_t *T = lump(b, 6, &n, NULL);
    int nt = (int)(n / 72);
    const uint8_t *N = lump(b, 5, &n, NULL);
    int nn = (int)(n / 32);
    r->node_plane = calloc((size_t)nn + 1, sizeof *r->node_plane);
    r->node_child = calloc((size_t)nn + 1, sizeof *r->node_child);
    for (int i = 0; i < nn; i++) { r->node_plane[i] = r32(N + 32 * i); r->node_child[i][0] = r32(N + 32 * i + 4); r->node_child[i][1] = r32(N + 32 * i + 8); }
    const uint8_t *L = lump(b, 10, &n, &ver);
    size_t ls = ver == 0 ? 56 : 32;
    int nl = (int)(n / ls);
    r->leaf_contents = calloc((size_t)nl, sizeof *r->leaf_contents);
    for (int i = 0; i < nl; i++) r->leaf_contents[i] = r32(L + ls * i);
    const uint8_t *LB = lump(b, 17, &n, NULL);
    const uint8_t *B = lump(b, 18, &n, NULL);
    int nb = (int)(n / 12);
    const uint8_t *S = lump(b, 19, &n, NULL);
    const uint8_t *M = lump(b, 14, &n, NULL);
    r->head = r32(M + 36);
    /* model 0's brushes: those its tree's leaves list */
    uint8_t *in0 = calloc((size_t)nb + 1, 1);
    int *stack = malloc(sizeof(int) * (size_t)(nn + nl + 8)), sp = 0;
    stack[sp++] = r->head;
    while (sp) {
        int x = stack[--sp];
        if (x >= 0) { stack[sp++] = r->node_child[x][0]; stack[sp++] = r->node_child[x][1]; continue; }
        const uint8_t *lf = L + ls * (size_t)(-1 - x);
        int first = ru16(lf + 24), cnt = ru16(lf + 26);
        for (int i = 0; i < cnt; i++) in0[ru16(LB + 2 * (size_t)(first + i))] = 1;
    }
    free(stack);
    r->b = calloc((size_t)nb + 1, sizeof *r->b);
    for (int i = 0; i < nb; i++) {
        if (!in0[i]) continue;
        RBrush *br = &r->b[r->nb++];
        int fs = r32(B + 12 * i), ns = r32(B + 12 * i + 4);
        br->contents = r32(B + 12 * i + 8);
        br->orig = i;
        br->pl = calloc((size_t)ns + 1, sizeof *br->pl);
        br->flags = calloc((size_t)ns + 1, sizeof *br->flags);
        for (int s = 0; s < ns; s++) {
            const uint8_t *sd = S + 8 * (size_t)(fs + s);
            if (sd[6]) continue;   /* bevel: not part of a ray's brush */
            br->pl[br->nsides] = r->planes[ru16(sd)];
            int ti = r16(sd + 2);
            br->flags[br->nsides] = ti >= 0 && ti < nt ? r32(T + 72 * ti + 64) : 0;
            br->nsides++;
        }
    }
    free(in0);
    /* displacements, in double */
    const uint8_t *D = lump(b, 26, &n, NULL);
    int nd = (int)(n / 176);
    const uint8_t *F = lump(b, 7, &n, NULL);
    if (!n) F = lump(b, 58, &n, NULL);
    const uint8_t *E = lump(b, 12, &n, NULL), *SE = lump(b, 13, &n, NULL), *V = lump(b, 3, &n, NULL), *DV = lump(b, 33, &n, NULL);
    r->d = calloc((size_t)nd + 1, sizeof *r->d);
    for (int i = 0; i < nd; i++) {
        const uint8_t *di = D + 176 * i;
        RDisp *d = &r->d[r->nd++];
        int pw = r32(di + 20), mt = r32(di + 24);
        d->w = (1 << pw) + 1;
        d->contents = r32(di + 32);
        d->flags = mt < 0 ? (mt & 0x7fffffff) : 0;
        const uint8_t *f = F + 56 * ru16(di + 36);
        int fe = r32(f + 4);
        double c[4][3], st[3];
        for (int k = 0; k < 3; k++) st[k] = rf(di + 4 * k);
        for (int j = 0; j < 4; j++) {
            int se = r32(SE + 4 * (size_t)(fe + j));
            int vi = se >= 0 ? ru16(E + 4 * (size_t)se) : ru16(E + 4 * (size_t)(-se) + 2);
            for (int k = 0; k < 3; k++) c[j][k] = rf(V + 12 * (size_t)vi + 4 * (size_t)k);
        }
        int s0 = 0;
        double bd = 1e300;
        for (int j = 0; j < 4; j++) {
            double q = 0;
            for (int k = 0; k < 3; k++) q += (st[k] - c[j][k]) * (st[k] - c[j][k]);
            if (q < bd) { bd = q; s0 = j; }
        }
        double p0[3], p1[3], p2[3], p3[3];
        for (int k = 0; k < 3; k++) { p0[k] = c[s0][k]; p1[k] = c[(s0 + 1) % 4][k]; p2[k] = c[(s0 + 2) % 4][k]; p3[k] = c[(s0 + 3) % 4][k]; }
        d->v = calloc((size_t)(d->w * d->w), sizeof *d->v);
        const uint8_t *dv = DV + 20 * (size_t)r32(di + 12);
        for (int a = 0; a < d->w; a++)
            for (int bb = 0; bb < d->w; bb++) {
                double s = (double)a / (d->w - 1), t = (double)bb / (d->w - 1);
                const uint8_t *q = dv + 20 * (size_t)(a * d->w + bb);
                for (int k = 0; k < 3; k++) {
                    double e0 = p0[k] + (p1[k] - p0[k]) * s, e1 = p3[k] + (p2[k] - p3[k]) * s;
                    d->v[a * d->w + bb][k] = e0 + (e1 - e0) * t + rf(q + 4 * k) * rf(q + 12);
                }
            }
    }
}

/* the leaf of p, as the engine's point walk (d >= 0 front) */
static int ref_contents(const Ref *r, const double p[3]) {
    int x = r->head;
    while (x >= 0) {
        const RPlane *pl = &r->planes[r->node_plane[x]];
        x = r->node_child[x][pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d >= 0 ? 0 : 1];
    }
    return r->leaf_contents[-1 - x];
}

typedef struct { double t, t2; int sky, kind; int graze; int startsolid; int id; } RHit;

typedef struct { double enter, leave; int sky, id, graze; } RCand;
static int rcand_cmp(const void *x, const void *y) {
    const RCand *a = x, *b = y;
    return a->enter < b->enter ? -1 : a->enter > b->enter ? 1 : a->id - b->id;
}

/* The engine's world trace as sun_vis.h states it, from every brush and triangle: the first entry along the ray; a ray that starts inside
 * brushes reports the first entry after the solid run it starts in (brushes entered within 0.1 unit of where the run is left join it) */
static void ref_trace(const Ref *r, const double a[3], const double b[3], int mask, RHit *h) {
    static RCand cand[1 << 16];
    double D[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, len = sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
    h->t = h->t2 = 2;
    h->sky = 0; h->kind = 0; h->graze = 0; h->startsolid = 0; h->id = -1;
    int graze_best = 0, nc = 0;
    double L = 0;   /* where the start's solid run is left */
    for (int i = 0; i < r->nb; i++) {
        const RBrush *br = &r->b[i];
        if (!(br->contents & mask) || !br->nsides) continue;
        double enter = -1e300, leave = 1e300;
        int lead = -1, inside = 1, miss = 0;
        for (int s = 0; s < br->nsides && !miss; s++) {
            const RPlane *pl = &br->pl[s];
            double d1 = pl->n[0] * a[0] + pl->n[1] * a[1] + pl->n[2] * a[2] - pl->d;
            double d2 = pl->n[0] * b[0] + pl->n[1] * b[1] + pl->n[2] * b[2] - pl->d;
            if (d1 > 0) inside = 0;
            if (d1 > 0 && d2 > 0) { miss = 1; break; }
            if (d1 <= 0 && d2 <= 0) continue;
            double t = d1 / (d1 - d2);
            if (d1 > d2) { if (t > enter) { enter = t; lead = s; } }
            else if (t < leave) leave = t;
        }
        if (miss) continue;
        if (inside) {
            h->startsolid = 1;
            if (leave >= 1) { h->t2 = h->t; h->t = 0; h->sky = 0; h->kind = 1; h->id = br->orig; return; }   /* never leaves: stops at the start */
            if (leave > L) L = leave;
            continue;
        }
        if (!(enter <= leave) || enter < 0 || enter > 1) continue;
        if (nc < (int)(sizeof cand / sizeof cand[0])) {
            RCand c = { enter, leave, (br->flags[lead] & 4) != 0, br->orig, (leave - enter) * len < 0.1 };
            cand[nc++] = c;
        }
    }
    qsort(cand, (size_t)nc, sizeof cand[0], rcand_cmp);
    int i0 = 0;
    if (h->startsolid) {
        for (; i0 < nc; i0++) {
            if ((cand[i0].enter - L) * len > 0.1) break;
            if (cand[i0].leave > L) L = cand[i0].leave;
        }
        if (L >= 1) { h->t = 0; h->kind = 1; return; }
    }
    if (i0 < nc) {
        h->t = cand[i0].enter; h->sky = cand[i0].sky; h->kind = 1; h->id = cand[i0].id; graze_best = cand[i0].graze;
        if (h->startsolid && (h->t - L) * len < 0.2) graze_best = 1;
        if (i0 + 1 < nc) h->t2 = cand[i0 + 1].enter;
    }
    for (int i = 0; i < r->nd; i++) {
        const RDisp *d = &r->d[i];
        if (!(d->contents & mask) || !(d->contents & 0x4081) || (d->flags & 8)) continue;
        int w = d->w;
        for (int y = 0; y < w - 1; y++)
            for (int x = 0; x < w - 1; x++) {
                int q = y * w + x, tri[2][3];
                if (q & 1) { int t0[2][3] = { { q, q + w, q + 1 }, { q + 1, q + w, q + w + 1 } }; memcpy(tri, t0, sizeof tri); }
                else { int t0[2][3] = { { q, q + w, q + w + 1 }, { q, q + w + 1, q + 1 } }; memcpy(tri, t0, sizeof tri); }
                /* the quad's box grown by 1/32 must meet the segment */
                double lo[3], hi[3], t0 = 0, t1 = 1;
                for (int k = 0; k < 3; k++) {
                    lo[k] = fmin(fmin(d->v[q][k], d->v[q + 1][k]), fmin(d->v[q + w][k], d->v[q + w + 1][k])) - 0.03125;
                    hi[k] = fmax(fmax(d->v[q][k], d->v[q + 1][k]), fmax(d->v[q + w][k], d->v[q + w + 1][k])) + 0.03125;
                    if (D[k] == 0) { if (a[k] < lo[k] || a[k] > hi[k]) t0 = 2; continue; }
                    double ta = (lo[k] - a[k]) / D[k], tb = (hi[k] - a[k]) / D[k];
                    t0 = fmax(t0, fmin(ta, tb));
                    t1 = fmin(t1, fmax(ta, tb));
                }
                if (t0 > t1) continue;
                for (int k = 0; k < 2; k++) {
                    const double *A = d->v[tri[k][0]], *B2 = d->v[tri[k][2]], *C = d->v[tri[k][1]];   /* the order 0, 2, 1 */
                    double e1[3] = { B2[0] - A[0], B2[1] - A[1], B2[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
                    double nr[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                    if (nr[0] * D[0] + nr[1] * D[1] + nr[2] * D[2] >= 0) continue;   /* one-sided */
                    /* Moller-Trumbore */
                    double pv[3] = { D[1] * e2[2] - D[2] * e2[1], D[2] * e2[0] - D[0] * e2[2], D[0] * e2[1] - D[1] * e2[0] };
                    double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                    if (det == 0) continue;
                    double tv[3] = { a[0] - A[0], a[1] - A[1], a[2] - A[2] };
                    double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
                    double qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
                    double v = (D[0] * qv[0] + D[1] * qv[1] + D[2] * qv[2]) / det;
                    double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
                    double edge = fmin(fmin(u, v), 1 - u - v);
                    if (edge < -1e-6 || t < -1e-3 || t > 1 + 1e-3) continue;   /* IntersectRayWithTriangle's ray tolerance, then clamped */
                    int gz = edge < 1e-5 || fabs(t + 1e-3) < 1e-7;
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    if (h->startsolid && t <= L) continue;   /* inside the start's solid run */
                    if (t < h->t) { h->t2 = h->t; h->t = t; h->sky = 0; h->kind = 2; h->id = i; graze_best = gz; }
                    else if (t < h->t2) h->t2 = t;
                }
            }
    }
    h->graze = graze_best || (h->t2 - h->t) * len < 0.1;
}

static int ref_sun(const Ref *r, const SunInfo *si, const double p[3], RHit *h) {
    double b[3];
    for (int k = 0; k < 3; k++) b[k] = p[k] + (double)si->sun_dir[k] * SUN_TRACE_LENGTH;
    ref_trace(r, p, b, SUN_MASK_OPAQUE, h);
    return h->sky;
}

/* =================================================================================================================================== */
/* The synthetic map                                                                                                                     */

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(Buf *b, int32_t v) { put(b, &v, 4); }
static void put16(Buf *b, int v) { int16_t s = (int16_t)v; put(b, &s, 2); }
static void putf(Buf *b, float f) { put(b, &f, 4); }

enum { TX_WALL = 0, TX_SKY = 1, TX_NODRAW = 2, TX_SKY2D = 3 };
typedef struct { float mn[3], mx[3]; int contents; int tex[6]; } SBox;   /* sides +z, -z, +x, -x, +y, -y */

/* Shell: floor z [-64, 0], sky z [1024, 1088] (only its bottom side is sky; its first side, the top, is nodraw), walls at |x|, |y| >= 1024.
 * Inside: an overhang [0, 512] x [-1024, 1024] x [512, 576]; glass [-512, -256] x ... x [256, 272]; water x [-1024, -700], z [0, 100];
 * an opaque-only brush (CONTENTS_OPAQUE) [-200, -100] x [-1024, 1024] x [600, 610]; displacement 0 at z 400 over x [-1000, -800] wound
 * one way, displacement 1 at z 400 over x [800, 1000] wound the other way, both bumped. variant bit 0: no emit_skylight (a map without
 * a sun); bit 1: the sky side is a 2D sky. */
static uint8_t *synth_sun_bsp(int variant, size_t *len) {
    static const SBox boxes0[] = {
        { { -1088, -1088, -64 }, { 1088, 1088, 0 }, 1, { 0, 0, 0, 0, 0, 0 } },
        { { -1088, -1088, 1024 }, { 1088, 1088, 1088 }, 1, { TX_NODRAW, TX_SKY, TX_NODRAW, TX_NODRAW, TX_NODRAW, TX_NODRAW } },
        { { 1024, -1088, -64 }, { 1088, 1088, 1088 }, 1, { 0, 0, 0, 0, 0, 0 } },
        { { -1088, -1088, -64 }, { -1024, 1088, 1088 }, 1, { 0, 0, 0, 0, 0, 0 } },
        { { -1088, 1024, -64 }, { 1088, 1088, 1088 }, 1, { 0, 0, 0, 0, 0, 0 } },
        { { -1088, -1088, -64 }, { 1088, -1024, 1088 }, 1, { 0, 0, 0, 0, 0, 0 } },
        { { 0, -1024, 512 }, { 512, 1024, 576 }, 1, { 0, 0, 0, 0, 0, 0 } },                 /* 6 overhang (east leaf) */
        { { -512, -1024, 256 }, { -256, 1024, 272 }, 2, { 0, 0, 0, 0, 0, 0 } },             /* 7 glass (west leaf) */
        { { -1024, -1024, 0 }, { -700, 1024, 100 }, 0x20, { 0, 0, 0, 0, 0, 0 } },           /* 8 water (west) */
        { { -200, -1024, 600 }, { -100, 1024, 610 }, 0x80, { 0, 0, 0, 0, 0, 0 } },          /* 9 opaque only (west) */
    };
    enum { NB = 10, NSHELL = 6 };
    SBox boxes[NB];
    memcpy(boxes, boxes0, sizeof boxes);
    if (variant & 2) boxes[1].tex[1] = TX_SKY2D;
    Buf L[64];
    memset(L, 0, sizeof L);
    /* texinfo: 72 bytes, flags at 64 */
    static const int texflags[4] = { 0, 0x404, 0x280, 0x406 };
    for (int t = 0; t < 4; t++) { uint8_t z[64] = { 0 }; put(&L[6], z, 64); put32(&L[6], texflags[t]); put32(&L[6], 0); }
    /* planes 0..: node planes first, then 6 per brush */
    static const float np[9][4] = { { 0, 0, 1, 1024 }, { 0, 0, 1, 0 }, { 1, 0, 0, 1024 }, { -1, 0, 0, 1024 }, { 0, 1, 0, 1024 }, { 0, -1, 0, 1024 }, { 1, 0, 0, 0 },
                                    { 0, 0, 1, 997 }, { 1, 0, 0, 600 } };
    for (int i = 0; i < 9; i++) { for (int k = 0; k < 4; k++) putf(&L[1], np[i][k]); put32(&L[1], i < 2 || i == 7 ? 2 : i == 4 || i == 5 ? 1 : 0); }
    int nplanes = 9;
    for (int i = 0; i < NB; i++) {
        const SBox *x = &boxes[i];
        float pl[6][4] = { { 0, 0, 1, x->mx[2] }, { 0, 0, -1, -x->mn[2] }, { 1, 0, 0, x->mx[0] }, { -1, 0, 0, -x->mn[0] }, { 0, 1, 0, x->mx[1] }, { 0, -1, 0, -x->mn[1] } };
        for (int s = 0; s < 6; s++) {
            for (int k = 0; k < 4; k++) putf(&L[1], pl[s][k]);
            put32(&L[1], 0);
            put16(&L[19], nplanes++); put16(&L[19], x->tex[s]); put16(&L[19], -1); put16(&L[19], 0);
        }
        put32(&L[18], 6 * i); put32(&L[18], 6); put32(&L[18], x->contents);
    }
    /* a bevel side on the overhang that would cut it if it were traced: x + z <= 600 (the overhang reaches x + z = 1088) */
    {
        float bp[4] = { 0.70710678f, 0, 0.70710678f, 600 * 0.70710678f };
        for (int k = 0; k < 4; k++) putf(&L[1], bp[k]);
        put32(&L[1], 5);
        put16(&L[19], nplanes++); put16(&L[19], TX_WALL); put16(&L[19], -1); put16(&L[19], 1);
        /* rewrite brush 6 to 7 sides: its sides are 36..41, the bevel is side 60 (after all 60), so give brush 6 its own copy */
    }
    /* brush 10: the overhang again (same box), with the bevel as a 7th side, its own sides 61..66 (copies of 36..41) */
    {
        const SBox *x = &boxes[6];
        float pl[6][4] = { { 0, 0, 1, x->mx[2] }, { 0, 0, -1, -x->mn[2] }, { 1, 0, 0, x->mx[0] }, { -1, 0, 0, -x->mn[0] }, { 0, 1, 0, x->mx[1] }, { 0, -1, 0, -x->mn[1] } };
        for (int s = 0; s < 6; s++) {
            for (int k = 0; k < 4; k++) putf(&L[1], pl[s][k]);
            put32(&L[1], 0);
            put16(&L[19], nplanes++); put16(&L[19], 0); put16(&L[19], -1); put16(&L[19], 0);
        }
        put32(&L[18], 60); put32(&L[18], 7); put32(&L[18], 1);   /* sides 60 (bevel), 61..66 */
    }
    /* brush 11: a box on the floor, touching it (x [650, 750], z [0, 16]); brush 12: a box on the overhang (brush 10, not a box), touching
     * it (x [200, 300], z [576, 590]). Sides 67..72 and 73..78. */
    {
        static const float bx[2][6] = { { 650, -1024, 0, 750, 1024, 16 }, { 200, -1024, 576, 300, 1024, 590 } };
        for (int j = 0; j < 2; j++) {
            const float *x = bx[j];
            float pl[6][4] = { { 0, 0, 1, x[5] }, { 0, 0, -1, -x[2] }, { 1, 0, 0, x[3] }, { -1, 0, 0, -x[0] }, { 0, 1, 0, x[4] }, { 0, -1, 0, -x[1] } };
            for (int k2 = 0; k2 < 6; k2++) {
                for (int k = 0; k < 4; k++) putf(&L[1], pl[k2][k]);
                put32(&L[1], 0);
                put16(&L[19], nplanes++); put16(&L[19], 0); put16(&L[19], -1); put16(&L[19], 0);
            }
            put32(&L[18], 67 + 6 * j); put32(&L[18], 6); put32(&L[18], 1);
        }
    }
    /* leaves: 0 sky, 1 floor, 2 x+, 3 x-, 4 y+, 5 y-, 6 middle east (x >= 0, z < 997), 7 middle west, 8 east x >= 600 z >= 997 (solid, no
     * brushes, like the void), 9 east x < 600 z >= 997 (open, no brushes, and its flags say it sees no sky) */
    int leafbrush_first[10] = { 0 }, leafbrush_n[10] = { 0 }, nlb = 0;
    for (int l = 0; l < 6; l++) { leafbrush_first[l] = nlb; for (int i = 0; i < NSHELL; i++) { put16(&L[17], i); nlb++; } leafbrush_n[l] = NSHELL; }
    leafbrush_first[6] = nlb; for (int i = 10; i < 13; i++) { put16(&L[17], i); nlb++; } leafbrush_n[6] = 3;   /* brush 6 itself is in no leaf */
    leafbrush_first[7] = nlb; for (int i = 7; i < 10; i++) { put16(&L[17], i); nlb++; } leafbrush_n[7] = 3;
    static const int lc[10] = { 1, 1, 1, 1, 1, 1, 1, 0x2 | 0x20 | 0x80, 1, 0 };
    for (int l = 0; l < 10; l++) {
        put32(&L[10], lc[l]); put16(&L[10], l == 6 || l == 7 || l == 9 ? 0 : -1); put16(&L[10], l == 9 ? 0 : 1 << 9);   /* flags: LEAF_FLAGS_SKY */
        for (int k = 0; k < 6; k++) put16(&L[10], 0);
        put16(&L[10], 0); put16(&L[10], 0); put16(&L[10], leafbrush_first[l]); put16(&L[10], leafbrush_n[l]); put16(&L[10], -1); put16(&L[10], 0);
    }
    /* nodes: plane, children, then 20 bytes (mins, maxs, firstface, numfaces, area, pad) */
    static const int nodes[9][3] = { { 0, -1, 1 }, { 1, 2, -2 }, { 2, -3, 3 }, { 3, -4, 4 }, { 4, -5, 5 }, { 5, -6, 6 }, { 6, 7, -8 }, { 7, 8, -7 }, { 8, -9, -10 } };
    for (int i = 0; i < 9; i++) { put32(&L[5], nodes[i][0]); put32(&L[5], nodes[i][1]); put32(&L[5], nodes[i][2]); uint8_t z[20] = { 0 }; put(&L[5], z, 20); }
    /* model 0 */
    for (int k = 0; k < 3; k++) putf(&L[14], -1088);
    for (int k = 0; k < 3; k++) putf(&L[14], 1088);
    for (int k = 0; k < 3; k++) putf(&L[14], 0);
    put32(&L[14], 0); put32(&L[14], 0); put32(&L[14], 0);
    /* displacements: power 2 (5 x 5), faces 0 and 1, corners wound opposite ways; startPosition at a corner that is not the first */
    for (int d = 0; d < 2; d++) {
        float x0 = d ? 800.0f : -700.0f, x1 = d ? 1000.0f : -500.0f;
        float c[4][3] = { { x0, 600, 400 }, { x1, 600, 400 }, { x1, 900, 400 }, { x0, 900, 400 } };
        if (d) { float t[3]; memcpy(t, c[1], 12); memcpy(c[1], c[3], 12); memcpy(c[3], t, 12); }
        int v0 = d * 4;
        for (int j = 0; j < 4; j++) { for (int k = 0; k < 3; k++) putf(&L[3], c[j][k]); }
        for (int j = 0; j < 4; j++) { put16(&L[12], v0 + j); put16(&L[12], v0 + (j + 1) % 4); }
        for (int j = 0; j < 4; j++) put32(&L[13], d * 4 + j);   /* surfedge +e: v[0] of edge e */
        /* face: planenum, side, onNode, firstedge, numedges, texinfo, dispinfo, ... 56 bytes */
        put16(&L[7], 1); put(&L[7], "\0\0", 2); put32(&L[7], d * 4); put16(&L[7], 4); put16(&L[7], 0); put16(&L[7], d);
        { uint8_t z[42] = { 0 }; put(&L[7], z, 42); }
        /* dispinfo: startPosition = corner 2 */
        for (int k = 0; k < 3; k++) putf(&L[26], c[2][k]);
        put32(&L[26], d * 25); put32(&L[26], d * 32); put32(&L[26], 2); put32(&L[26], 0); putf(&L[26], 0); put32(&L[26], 1);
        put16(&L[26], d); put16(&L[26], 0);
        { uint8_t z[176 - 40] = { 0 }; put(&L[26], z, sizeof z); }
        for (int j = 0; j < 25; j++) {
            putf(&L[33], 0); putf(&L[33], 0); putf(&L[33], 1);
            int a = j / 5, b = j % 5;
            putf(&L[33], (float)(10 * a + 7 * b * (a & 1) - 30));   /* uneven bumps, so the corner and the diagonals matter */
            putf(&L[33], 0);
        }
    }
    /* worldlights v0, 88 bytes: a point light and (unless variant 1) the sun; lump 15 LDR intensity, 54 HDR intensity */
    for (int h = 0; h < 2; h++) {
        Buf *w = &L[h ? 54 : 15];
        for (int i = 0; i < 2; i++) {
            if (i == 1 && (variant & 1)) break;
            float o[3] = { 0, 0, 500 }, inten[3] = { 1, 1, 1 }, nrm[3] = { 0, 0, 0 };
            int type = 1;
            if (i == 1) {
                type = 3;
                double n[3] = { 0.3, -0.2, -1 }, l = sqrt(0.09 + 0.04 + 1);
                for (int k = 0; k < 3; k++) { nrm[k] = (float)(n[k] / l); inten[k] = h ? 1.5f + 0.1f * (float)k : 2.0f; }
            }
            for (int k = 0; k < 3; k++) putf(w, o[k]);
            for (int k = 0; k < 3; k++) putf(w, inten[k]);
            for (int k = 0; k < 3; k++) putf(w, nrm[k]);
            put32(w, 0); put32(w, type); put32(w, i ? 33 : 0);
            for (int k = 0; k < 7; k++) putf(w, 0);
            put32(w, 0); put32(w, -1); put32(w, 0);
        }
    }
    Buf out = { 0 };
    uint8_t hdr[8 + 64 * 16 + 4] = { 0 };
    memcpy(hdr, "VBSP", 4);
    hdr[4] = 20;
    put(&out, hdr, sizeof hdr);
    for (int i = 0; i < 64; i++) {
        uint32_t f[3] = { (uint32_t)out.n, (uint32_t)L[i].n, i == 10 ? 1u : 0u };
        memcpy(out.p + 8 + 16 * i, f, 12);
        if (L[i].n) put(&out, L[i].p, L[i].n);
        free(L[i].p);
    }
    *len = out.n;
    return out.p;
}

static SunMap *load_mem(const uint8_t *b, size_t n, int hdr, const char *what) {
    SunMap *m = NULL;
    int rc = sun_map_load(b, n, hdr, &m);
    CHECK(rc == AMB_OK, "%s: sun_map_load %d", what, rc);
    return m;
}

static int lit(const SunMap *m, SunScratch *s, double x, double y, double z) { double p[3] = { x, y, z }; return sun_visible(m, s, p); }

static void synth_tests(void) {
    size_t n;
    uint8_t *b = synth_sun_bsp(0, &n);
    SunMap *m = load_mem(b, n, 1, "synth");
    if (!m) { free(b); return; }
    SunInfo si;
    sun_map_info(m, &si);
    CHECK(si.has_sun && si.nskylights == 1 && si.light_lump == 54 && fabsf(si.sun_intensity[2] - 1.7f) < 1e-6f && si.sun_style == 33,
          "synth: the sun from lump 54 (has %d, lump %d, intensity %g)", si.has_sun, si.light_lump, si.sun_intensity[2]);
    CHECK(si.ndisps == 2 && si.ndisp_tris == 64 && si.nbrushes == 13 && si.nsky_texinfo == 2, "synth: counts %d %d %d %d", si.ndisps, si.ndisp_tris, si.nbrushes, si.nsky_texinfo);
    SunMap *ml = load_mem(b, n, 0, "synth ldr");
    if (ml) {
        SunInfo sl;
        sun_map_info(ml, &sl);
        CHECK(sl.light_lump == 15 && sl.sun_intensity[0] == 2.0f, "synth: LDR lump 15 (%d, %g)", sl.light_lump, sl.sun_intensity[0]);
        sun_map_free(ml);
    }
    SunScratch *s = sun_scratch_new(m);
    /* sun dir = (-0.3, 0.2, 1) / |..|: a point at height z reaches the ceiling 1024 after dx = -0.3 (1024 - z), dy = 0.2 (1024 - z) */
    CHECK(lit(m, s, 900, 0, 100) == SUN_LIT, "synth: open floor east of the overhang lit");
    CHECK(lit(m, s, 300, 0, 100) == SUN_SHADOW, "synth: under the overhang shadowed");
    CHECK(lit(m, s, 300, 0, 800) == SUN_LIT, "synth: above the overhang lit");
    CHECK(lit(m, s, -380, 0, 100) == SUN_LIT, "synth: under the glass lit (CONTENTS_WINDOW is not in MASK_OPAQUE)");
    CHECK(lit(m, s, -720, 0, 50) == SUN_LIT, "synth: in the water lit (CONTENTS_WATER is not in MASK_OPAQUE)");
    CHECK(lit(m, s, -100, 0, 300) == SUN_SHADOW, "synth: under the opaque-only brush shadowed (CONTENTS_OPAQUE is)");
    CHECK(lit(m, s, -1000, 900, 900) == SUN_SHADOW, "synth: the ray reaches a wall before the sky");
    CHECK(lit(m, s, 100, 0, 540) == SUN_LIT, "synth: inside the overhang brush: the start brush is not a hit, the ray leaves it and reaches the sky");
    /* a start in solid: a brush touching a start box is part of the start's solid (a box is left DIST_EPSILON late); one touching a start
     * brush that is not a box is a hit (left DIST_EPSILON early, entered DIST_EPSILON early: not before) */
    CHECK(lit(m, s, 700, 0, -10) == SUN_LIT, "synth: in the floor box under a box touching it: one solid, then the sky");
    CHECK(lit(m, s, 250, 0, 540) == SUN_SHADOW, "synth: in the overhang (not a box) under a box touching it: the box is hit");
    /* the light cache's point (FUN_1801d7400) */
    {
        static const struct { double o[3], want[3]; int how; const char *what; } lc[] = {
            { { 300.5, 0.5, 100 }, { 300, 4, 104 }, SUN_POINT_CENTRE, "the cell centre" },
            { { -8, 0.5, 100 }, { -12, 4, 104 }, SUN_POINT_CENTRE, "x = -8: the truncated |x| cell, complemented" },
            { { 100.5, 0.5, 995 }, { 100, 4, 996.5 }, SUN_POINT_CENTRE, "a centre in another open leaf, moved 0.5 back across the plane" },
            { { 700.5, 0.5, 995 }, { 700, 4, 995 }, SUN_POINT_CENTRE_XY, "a centre in a solid leaf: its x, y at the origin's height" },
            { { 100.5, 0.5, 540 }, { 100.5, 0.5, 540 }, SUN_POINT_ORIGIN, "an origin in a brush: the origin" },
            { { -150.5, 0.5, 598 }, { -148, 4, 598 }, SUN_POINT_CENTRE_XY, "a brush between origin and centre: x, y at the origin's height" },
        };
        for (size_t i = 0; i < sizeof lc / sizeof lc[0]; i++) {
            double got[3];
            int how = sun_lightcache_point(m, s, lc[i].o, got);
            CHECK(how == lc[i].how && fabs(got[0] - lc[i].want[0]) < 1e-4 && fabs(got[1] - lc[i].want[1]) < 1e-4 && fabs(got[2] - lc[i].want[2]) < 1e-4,
                  "synth: light cache point, %s: %d (%g %g %g), wanted %d (%g %g %g)", lc[i].what, how, got[0], got[1], got[2], lc[i].how, lc[i].want[0],
                  lc[i].want[1], lc[i].want[2]);
        }
        /* a point whose leaf does not see the sky: no sun for the model, though the ray reaches the sky */
        double o[3] = { 100.5, 0.5, 1000.2 }, pt[3];
        sun_lightcache_point(m, s, o, pt);
        CHECK(sun_visible(m, s, pt) == SUN_LIT && sun_model_visible(m, s, o) == SUN_SHADOW, "synth: the sky-flag gate");
        double o2[3] = { 300.5, 0.5, 800 };
        CHECK(sun_model_visible(m, s, o2) == SUN_LIT, "synth: a model in the open, lit");
    }
    /* the displacements: exactly one of the two faces down, so exactly one shadows the point under its middle */
    int d0 = lit(m, s, -600, 700, 300), d1 = lit(m, s, 900, 700, 300);
    CHECK(d0 != d1, "synth: the two oppositely wound displacements both %s", d0 ? "pass" : "block");
    CHECK(lit(m, s, -600, 700, 480) == SUN_LIT && lit(m, s, 900, 700, 480) == SUN_LIT, "synth: above the displacements lit");
    /* the reference on a grid over the box, three heights and the inside of brushes */
    Ref r;
    ref_load(&r, b);
    int agree = 0, total = 0, ties = 0, insolid = 0;
    for (double x = -1080; x <= 1080; x += 40)
        for (double y = -1080; y <= 1080; y += 120)
            for (double z = -40; z <= 1060; z += 55) {
                double p[3] = { x + 0.37, y + 0.21, z + 0.13 };
                RHit h;
                int want = ref_sun(&r, &si, p, &h), got = sun_visible(m, s, p);
                if (h.startsolid) { insolid++; continue; }   /* the order-dependent rule: not the reference's to decide */
                total++;
                if (want == got) agree++;
                else if (h.graze) ties++;
                else CHECK(0, "synth: at (%g, %g, %g) the core says %d, the reference %d (t %g kind %d)", p[0], p[1], p[2], got, want, h.t, h.kind);
            }
    printf("  synth: %d of %d grid points equal the reference, %d near ties (%d more start inside brushes)\n", agree, total, ties, insolid);
    CHECK(ties * 100 <= total, "synth: too many ties %d", ties);
    /* a trace's details */
    SunTrace tr;
    double a[3] = { 300, 0, 100 }, e[3] = { 300, 0, 2000 };
    sun_trace(m, s, a, e, SUN_MASK_OPAQUE, &tr);
    CHECK(tr.hit == SUN_HIT_BRUSH && tr.brush == 10 && fabs(tr.fraction * 1900 - 412) < 0.05 && tr.surf_flags == 0, "synth: straight up under the overhang: hit %d brush %d at %g",
          tr.hit, tr.brush, tr.fraction * 1900);
    double a2[3] = { 600, 0, 100 };
    double e2[3] = { 600, 0, 2000 };
    sun_trace(m, s, a2, e2, SUN_MASK_OPAQUE, &tr);
    CHECK(tr.hit == SUN_HIT_BRUSH && tr.brush == 1 && tr.side == 7 && (tr.surf_flags & SUN_SURF_SKY), "synth: straight up east: the sky brush's bottom side (brush %d side %d flags %x)",
          tr.brush, tr.side, tr.surf_flags);
    sun_trace(m, NULL, a2, e2, SUN_MASK_OPAQUE | 0x2, &tr);
    CHECK(tr.hit == SUN_HIT_BRUSH && tr.brush == 1, "synth: no scratch gives the same");
    sun_scratch_free(s);
    ref_free(&r);
    sun_map_free(m);
    free(b);
    /* a 2D sky counts (SURF_SKY | SURF_SKY2D) */
    b = synth_sun_bsp(2, &n);
    m = load_mem(b, n, 1, "synth 2d");
    if (m) { CHECK(lit(m, NULL, 900, 0, 100) == SUN_LIT, "synth: a 2D sky side is sky"); sun_map_free(m); }
    free(b);
    /* no sun */
    b = synth_sun_bsp(1, &n);
    m = load_mem(b, n, 1, "synth no sun");
    if (m) {
        sun_map_info(m, &si);
        CHECK(!si.has_sun && si.nlights == 1 && lit(m, NULL, 900, 0, 100) == SUN_NONE, "synth: a map without emit_skylight says so");
        SunCache *c = sun_cache_new(m, 8);
        double p[3] = { 1, 2, 3 };
        CHECK(c && sun_cache_visible(c, p) == SUN_NONE, "synth: the cache of a map without a sun");
        sun_cache_free(c);
        sun_map_free(m);
    }
    free(b);
}

/* =================================================================================================================================== */
/* Real maps: the core against the reference                                                                                            */

/* A point where models are: a random point of the world box dropped onto the first floor below it (a ray, mask CONTENTS_SOLID, brushes and
 * displacements), then 4..260 units up; 0 if there is no open point there. Half the points of a run are these, half plain random points. */
static int ground_point(const SunMap *m, const Ref *r, const SunInfo *si, double p[3]) {
    for (int k = 0; k < 3; k++) p[k] = si->world_min[k] + rndu() * (si->world_max[k] - si->world_min[k]);
    if (ref_contents(r, p) & 1) return 0;
    double down[3] = { p[0], p[1], p[2] - 16384 };
    SunTrace t;
    sun_trace(m, NULL, p, down, 1, &t);
    if (t.hit == SUN_HIT_NONE || t.startsolid) return 0;
    double z = p[2] - 16384 * t.fraction + 4 + 256 * rndu();
    if (z > p[2]) return 0;
    p[2] = z;
    return !(ref_contents(r, p) & 1);
}

static void ref_compare(const char *name, const uint8_t *b, size_t n, int npts) {
    SunMap *m = load_mem(b, n, 1, name);
    if (!m) return;
    SunInfo si;
    sun_map_info(m, &si);
    Ref r;
    ref_load(&r, b);
    SunScratch *s = sun_scratch_new(m);
    int open = 0, agree = 0, ties = 0, solid = 0, solid_agree = 0, lits = 0, disp_hits = 0, ground = 0, ground_lit = 0;
    for (int i = 0; i < npts; i++) {
        double p[3];
        int g = 0;
        if (i & 1) { int tries = 0; while (!(g = ground_point(m, &r, &si, p)) && tries++ < 200) {} }
        if (!g) for (int k = 0; k < 3; k++) p[k] = si.world_min[k] + rndu() * (si.world_max[k] - si.world_min[k]);
        int sol = ref_contents(&r, p) & 1;
        RHit h;
        int want = ref_sun(&r, &si, p, &h), got = sun_visible(m, s, p);
        if (h.kind == 2) disp_hits++;
        /* a start in a solid leaf is only reported: the core, like the engine, tests only the brushes the leaves on the way list, and the
         * outside of the world is solid leaves that list none of the brushes they overlap */
        if (sol || h.startsolid) {   /* a start in a solid leaf, or inside a brush (the order-dependent rule): counted only */
         solid++; solid_agree += want == got; continue; }
        open++;
        lits += want;
        ground += g;
        ground_lit += g && want;
        if (want == got) agree++;
        else if (h.graze) ties++;
        else CHECK(0, "%s: (%.3f %.3f %.3f) core %d, reference %d (t %.6f t2 %.6f kind %d)", name, p[0], p[1], p[2], got, want, h.t, h.t2, h.kind);
    }
    printf("  %s: %d open points (%d near a floor, %d of those lit), %d agree with the reference, %d near ties, %d lit; %d in solid leaves or inside brushes (%d agree); "
           "%d first hits on displacements\n", name, open, ground, ground_lit, agree, ties, lits, solid, solid_agree, disp_hits);
    CHECK(ties * 500 <= open + 1, "%s: too many near ties: %d", name, ties);
    /* segments of 4096 units in random directions from points near floors: what is hit first, displacements included */
    int seg = 0, seg_agree = 0, seg_ties = 0, seg_disp = 0, seg_far = 0;
    for (int i = 0; i < npts / 2; i++) {
        double p[3], e[3], d[3], l2 = 0;
        int tries = 0;
        while (!ground_point(m, &r, &si, p) && tries++ < 200) {}
        do { for (int k = 0; k < 3; k++) d[k] = rndu() * 2 - 1; l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]; } while (l2 > 1 || l2 < 1e-4);
        for (int k = 0; k < 3; k++) e[k] = p[k] + d[k] / sqrt(l2) * 4096;
        SunTrace t;
        RHit h;
        sun_trace(m, s, p, e, SUN_MASK_OPAQUE, &t);
        ref_trace(&r, p, e, SUN_MASK_OPAQUE, &h);
        seg++;
        seg_disp += h.kind == 2;
        int core_sky = (t.surf_flags & SUN_SURF_SKY) != 0;
        int same = t.hit == h.kind && core_sky == h.sky;
        if (same && t.hit && fabs(t.fraction - h.t) * 4096 > 1) seg_far++;
        if (same) seg_agree++;
        else if (h.graze) seg_ties++;
        else CHECK(0, "%s: segment from (%.3f %.3f %.3f) to (%.3f %.3f %.3f): core hit %d sky %d at %.4f, reference %d sky %d at %.4f", name, p[0], p[1], p[2], e[0], e[1],
                   e[2], t.hit, core_sky, t.fraction * 4096, h.kind, h.sky, h.t * 4096);
    }
    printf("  %s: %d random segments from points near floors, %d hit the same thing first (%d of them more than 1 unit apart), %d near ties; "
           "%d first hits on displacements\n", name, seg, seg_agree, seg_far, seg_ties, seg_disp);
    CHECK(seg_ties * 200 <= seg + 1 && seg_far * 100 <= seg + 1, "%s: segments: %d ties, %d far", name, seg_ties, seg_far);
    CHECK(lits > open / 20, "%s: an implausible lit share %d of %d", name, lits, open);
    sun_scratch_free(s);
    ref_free(&r);
    sun_map_free(m);
}

/* =================================================================================================================================== */
/* Reading                                                                                                                              */

static void put_le(Buf *b, unsigned long long v, int n) { uint8_t t[8]; for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * i)); put(b, t, (size_t)n); }
static void put_s(Buf *b, const char *s) { put(b, s, strlen(s) + 1); }

static int same_info(const SunInfo *a, const SunInfo *b) { return memcmp(a, b, sizeof *a) == 0; }

static void same_answers(const char *what, const SunMap *a, const SunMap *b, int npts) {
    SunInfo si;
    sun_map_info(a, &si);
    int diff = 0;
    for (int i = 0; i < npts; i++) {
        double p[3];
        for (int k = 0; k < 3; k++) p[k] = si.world_min[k] + rndu() * (si.world_max[k] - si.world_min[k]);
        SunTrace t1, t2;
        double e[3] = { p[0] + si.sun_dir[0] * 9000, p[1] + si.sun_dir[1] * 9000, p[2] + si.sun_dir[2] * 9000 };
        sun_trace(a, NULL, p, e, SUN_MASK_OPAQUE, &t1);
        sun_trace(b, NULL, p, e, SUN_MASK_OPAQUE, &t2);
        diff += memcmp(&t1, &t2, sizeof t1) != 0;
    }
    CHECK(diff == 0, "%s: %d traces differ between the two loads", what, diff);
}

static void read_tests(const char *name, const uint8_t *b, size_t n, int hostile) {
    for (int hdr = 0; hdr < 2; hdr++) {
        SunMap *m0 = load_mem(b, n, hdr, name), *m1 = NULL, *m2 = NULL;
        if (!m0) continue;
        SunInfo i0, i1, i2;
        sun_map_info(m0, &i0);
        Mem mm = { b, n, 0, 0, 0 };
        AmbReader ar = mem_reader(&mm);
        AmbReadStats st;
        int rc = sun_map_load_reader(&ar, hdr, &m1, &st);
        CHECK(rc == AMB_OK, "%s: reader load %d", name, rc);
        if (m1) {
            sun_map_info(m1, &i1);
            CHECK(same_info(&i0, &i1), "%s hdr %d: the reader's info differs", name, hdr);
            same_answers(name, m0, m1, 300);
            if (hostile) printf("  %s hdr %d: read %lld of %zu bytes (%.1f%%) in %d reads, peak held %lld bytes\n", name, hdr, st.bytes_read, n,
                                100.0 * (double)st.bytes_read / (double)n, st.reads, st.peak_bytes);
            CHECK(st.bytes_read < (long long)n && st.peak_bytes < (long long)n, "%s: read %lld held %lld of %zu", name, st.bytes_read, st.peak_bytes, n);
        }
        /* inside a GMA, after a decoy */
        static uint8_t pad1[777], pad2[4096];
        memset(pad1, 'a', sizeof pad1);
        memset(pad2, 'b', sizeof pad2);
        memcpy(pad2, "VBSP", 4);
        Buf g = { 0 };
        put(&g, "GMAD", 4); put_le(&g, 3, 1); put_le(&g, 76561197960265728ull, 8); put_le(&g, 1462129767ull, 8); put_s(&g, ""); put_s(&g, "t"); put_s(&g, "{}"); put_s(&g, "a"); put_le(&g, 1, 4);
        const char *names[3] = { "materials/a.vmt", "maps/decoy.bsp", "maps/x.bsp" };
        long long sizes[3] = { sizeof pad1, sizeof pad2, (long long)n };
        for (int i = 0; i < 3; i++) { put_le(&g, (unsigned)(i + 1), 4); put_s(&g, names[i]); put_le(&g, (unsigned long long)sizes[i], 8); put_le(&g, 0, 4); }
        put_le(&g, 0, 4);
        put(&g, pad1, sizeof pad1); put(&g, pad2, sizeof pad2); put(&g, b, n); put_le(&g, 0, 4);
        Mem gm = { g.p, g.n, 0, 0, 0 };
        AmbReader ga = mem_reader(&gm);
        long long o = -1, sz = -1;
        int rg = amb_gma_find(&ga, "MAPS\\X.BSP", -1, &o, &sz, NULL);
        CHECK(rg == AMB_OK && sz == (long long)n, "%s: the GMA index %d", name, rg);
        AmbSubReader sub;
        AmbReader sa;
        if (rg == AMB_OK && amb_sub_reader(&sa, &sub, &ga, o, sz) == AMB_OK) {
            rc = sun_map_load_reader(&sa, hdr, &m2, NULL);
            CHECK(rc == AMB_OK, "%s: GMA load %d", name, rc);
            if (m2) { sun_map_info(m2, &i2); CHECK(same_info(&i0, &i2), "%s: the GMA load's info differs", name); same_answers(name, m0, m2, 200); }
        }
        free(g.p);
        /* the sun equals lg_map.c's emit_skylight */
        LgMap *lm = NULL;
        if (lg_map_load(b, n, hdr, &lm) == AMB_OK) {
            int nl;
            const LgWorldLight *wl = lg_map_lights(lm, &nl);
            int k = 0;
            while (k < nl && wl[k].type != LG_EMIT_SKYLIGHT) k++;
            if (k < nl) CHECK(i0.has_sun && !memcmp(wl[k].normal, i0.sun_normal, 12) && !memcmp(wl[k].intensity, i0.sun_intensity, 12) && wl[k].style == i0.sun_style,
                              "%s hdr %d: the sun differs from lg_map's skylight", name, hdr);
            else CHECK(!i0.has_sun, "%s: a sun lg_map does not see", name);
            lg_map_free(lm);
        }
        /* cancel, short read */
        Mem cm = { b, n, 0, 1, 0 };
        AmbReader ca = mem_reader(&cm);
        SunMap *mx = NULL;
        CHECK(sun_map_load_reader(&ca, hdr, &mx, NULL) == AMB_ERR_CANCELLED && !mx, "%s: cancel", name);
        for (int at = 2; at < 12; at += 3) {
            Mem sm = { b, n, at, 0, 0 };
            AmbReader sa2 = mem_reader(&sm);
            int r2 = sun_map_load_reader(&sa2, hdr, &mx, NULL);
            CHECK((r2 == AMB_ERR_READ && !mx) || (r2 == AMB_OK && sm.calls < at), "%s: a short read at call %d gave %d", name, at, r2);
            sun_map_free(mx);
            mx = NULL;
        }
        sun_map_free(m0); sun_map_free(m1); sun_map_free(m2);
    }
    if (!hostile) return;
    /* truncated and mutated files: any result but a crash; a map that loads can be traced */
    uint8_t *c = malloc(n);
    int loaded = 0, tries = 0;
    for (int it = 0; it < hostile; it++) {
        memcpy(c, b, n);
        size_t len = n;
        if (it % 3 == 0) len = (size_t)(rndu() * (double)n);
        else {
            int flips = 1 + (int)(rnd64() % 8);
            for (int f = 0; f < flips; f++) {
                size_t at = (it % 3 == 1) ? 8 + (size_t)(rnd64() % (64 * 16)) : (size_t)(rnd64() % n);
                c[at] = (uint8_t)rnd64();
            }
        }
        SunMap *m = NULL;
        tries++;
        if (sun_map_load(c, len, (int)(it & 1), &m) == AMB_OK) {
            loaded++;
            SunInfo si;
            sun_map_info(m, &si);
            for (int q = 0; q < 20; q++) {
                double p[3] = { (rndu() - 0.5) * 40000, (rndu() - 0.5) * 40000, (rndu() - 0.5) * 40000 };
                sun_visible(m, NULL, p);
            }
            sun_map_free(m);
        }
    }
    printf("  %s: %d truncated or mutated files, %d loaded and were traced\n", name, tries, loaded);
    free(c);
}

/* =================================================================================================================================== */
/* The cache                                                                                                                            */

/* The cache answers as sun_model_visible from the origin that made the entry (the first of its key, LightcacheGet's cells and leaf, since the
 * entry was last made); a collision remakes it. Points jittered around a few bases, a large and a 16-entry table (collisions everywhere). */
static void cache_tests(const char *name, const uint8_t *b, size_t n) {
    SunMap *m = load_mem(b, n, 1, name);
    if (!m) return;
    SunInfo si;
    sun_map_info(m, &si);
    CHECK(sun_cache_new(m, 0) == NULL && sun_cache_new(m, 27) == NULL && sun_cache_new(NULL, 8) == NULL, "%s: bad cache arguments accepted", name);
    enum { N = 3000 };
    static uint64_t keys[N];
    static double first[N][3];
    for (int pass = 0; pass < 2; pass++) {
        SunCache *c = sun_cache_new(m, pass ? 4 : 16);
        int bad = 0, nk = 0;
        double base[48][3];
        Ref r;
        ref_load(&r, b);
        for (int i = 0; i < 48; i++) {
            int tries = 0;
            while (!ground_point(m, &r, &si, base[i]) && tries++ < 400) {}
        }
        ref_free(&r);
        for (int i = 0; i < N; i++) {
            double p[3];
            const double *bp = base[rnd64() % 48];
            for (int k = 0; k < 3; k++) p[k] = bp[k] + (rndu() - 0.5) * 24;
            SunCacheStats b0;
            sun_cache_stats(c, &b0);
            int got = sun_cache_visible(c, p);
            SunCacheStats b1;
            sun_cache_stats(c, &b1);
            uint64_t key;
            int j = -1;
            if (sun_cache_key(m, p, &key)) {
                for (j = 0; j < nk && keys[j] != key; j++) {}
                if (j == nk) { keys[nk] = key; nk++; }
                if (b1.traces != b0.traces) memcpy(first[j], p, sizeof p);   /* a miss: this origin makes the entry */
            }
            /* the answer is the entry maker's; a hit must be that of the origin that made the entry, and never another key's */
            if (got != (j >= 0 ? sun_model_visible(m, NULL, first[j]) : sun_model_visible(m, NULL, p))) bad++;
        }
        double far[3] = { 1e12, 0, 0 }, nanp[3] = { NAN, 0, 0 };
        int fv = sun_cache_visible(c, far);
        CHECK(fv == SUN_SHADOW || fv == SUN_LIT, "%s: a far point", name);
        int nv = sun_cache_visible(c, nanp);
        CHECK(nv == SUN_SHADOW || nv == SUN_LIT, "%s: a NaN point", name);
        SunCacheStats cs;
        sun_cache_stats(c, &cs);
        CHECK(bad == 0, "%s: %d cache answers are not those of the origin that made the entry (table 2^%d)", name, bad, pass ? 4 : 16);
        CHECK(cs.lookups == N + 2 && cs.hits + cs.traces == cs.lookups && (pass || cs.hits > N / 3), "%s: stats %lld %lld %lld (%d keys)", name, cs.lookups,
              cs.hits, cs.traces, nk);
        sun_cache_clear(c);
        sun_cache_free(c);
    }
    sun_map_free(m);
}

/* =================================================================================================================================== */
/* Speed                                                                                                                                */

static void bench(const char *name, const uint8_t *b, size_t n, int nq) {
    SunMap *m = load_mem(b, n, 1, name);
    if (!m) return;
    SunInfo si;
    sun_map_info(m, &si);
    Ref r;
    ref_load(&r, b);
    /* points near floors (where models are) */
    double (*pt)[3] = malloc(sizeof *pt * (size_t)nq);
    int got = 0, guard = 0;
    while (got < nq && guard++ < nq * 400) {
        double p[3];
        if (ground_point(m, &r, &si, p)) memcpy(pt[got++], p, sizeof p);
    }
    nq = got;
    SunScratch *s = sun_scratch_new(m);
    long long leaves = 0;
    int nlit0 = 0;
    volatile int nlit = 0;
    for (int i = 0; i < nq; i++) nlit0 += sun_visible(m, s, pt[i]) == SUN_LIT;   /* warm */
    double best_s = 1e30, best_n = 1e30;
    for (int rep = 0; rep < 3; rep++) {
        double t0 = now_us();
        for (int i = 0; i < nq; i++) nlit += sun_visible(m, s, pt[i]);
        double t1 = now_us();
        for (int i = 0; i < nq; i++) nlit += sun_visible(m, NULL, pt[i]);
        double t2 = now_us();
        if (t1 - t0 < best_s) best_s = t1 - t0;
        if (t2 - t1 < best_n) best_n = t2 - t1;
    }
    for (int i = 0; i < nq; i++) {
        SunTrace t;
        double e[3];
        for (int k = 0; k < 3; k++) e[k] = pt[i][k] + si.sun_dir[k] * SUN_TRACE_LENGTH;
        sun_trace(m, s, pt[i], e, SUN_MASK_OPAQUE, &t);
        leaves += t.leaves;
    }
    /* the whole model rule without a cache (the light cache point's one or two traces, the sky-flag gate, the sun trace) */
    double tm0 = now_us();
    int nmodel_lit = 0;
    for (int i = 0; i < nq; i++) nmodel_lit += sun_model_visible(m, s, pt[i]) == SUN_LIT;
    double tm1 = now_us();
    /* a cache hit */
    SunCache *c = sun_cache_new(m, 16);
    for (int i = 0; i < 256 && i < nq; i++) sun_cache_visible(c, pt[i]);
    double t3 = now_us();
    int reps = 2000000;
    for (int i = 0; i < reps; i++) nlit += sun_cache_visible(c, pt[i & 255]);
    double t4 = now_us();
    /* P6-like: 400 props for 600 frames, 300 at rest, 100 moving 1..6 units a frame in a random direction, one lookup each a frame */
    sun_cache_clear(c);
    SunCacheStats c0;
    sun_cache_stats(c, &c0);
    double (*pos)[3] = malloc(sizeof *pos * 400), (*vel)[3] = malloc(sizeof *vel * 400);
    for (int i = 0; i < 400; i++) {
        memcpy(pos[i], pt[i % nq], sizeof pos[i]);
        double sp = i < 300 ? 0 : 1 + 5 * rndu(), a = rndu() * 6.283185307, z = (rndu() - 0.5) * 0.4;
        vel[i][0] = sp * cos(a); vel[i][1] = sp * sin(a); vel[i][2] = sp * z;
    }
    double t5 = now_us();
    for (int f = 0; f < 600; f++)
        for (int i = 0; i < 400; i++) {
            for (int k = 0; k < 3; k++) pos[i][k] += vel[i][k];
            nlit += sun_cache_visible(c, pos[i]);
        }
    double t6 = now_us();
    SunCacheStats c1;
    sun_cache_stats(c, &c1);
    printf("  %s bench: %d points near floors (%.0f%% lit), sun trace %.2f us with scratch, %.2f without, %.1f leaves visited; whole model rule "
           "uncached %.2f us (%.0f%% lit); a cache hit %.3f us;\n"
           "    400 props x 600 frames (100 moving 1-6 u/frame), the engine-keyed cache: %.1f%% hits, %.3f us per lookup, %.3f ms per frame\n",
           name, nq, 100.0 * nlit0 / (nq ? nq : 1), best_s / nq, best_n / nq, (double)leaves / nq, (tm1 - tm0) / nq, 100.0 * nmodel_lit / (nq ? nq : 1),
           (t4 - t3) / reps, 100.0 * (double)(c1.hits - c0.hits) / (double)(c1.lookups - c0.lookups), (t6 - t5) / (600.0 * 400), (t6 - t5) / 600 / 1000);
    (void)nlit;
    free(pos); free(vel);
    sun_cache_free(c);
    sun_scratch_free(s);
    ref_free(&r);
    free(pt);
    sun_map_free(m);
}

/* =================================================================================================================================== */
/* The engine's answers (tests/gmod_harness/photon_harness.lua, suntrace=1)                                                             */

static int compare_mode(const char *map, const char *lines) {
    size_t n;
    uint8_t *b = read_file(map, &n);
    if (!b) { printf("cannot read %s\n", map); return 2; }
    SunMap *m = load_mem(b, n, 1, map);
    if (!m) return 2;
    SunInfo si;
    sun_map_info(m, &si);
    Ref r;
    ref_load(&r, b);
    SunScratch *s = sun_scratch_new(m);
    FILE *f = fopen(lines, "r");
    if (!f) { printf("cannot read %s\n", lines); return 2; }
    char ln[1024];
    long tot = 0, agree = 0, raw_agree = 0, ss = 0, ss_agree = 0, eng_lit = 0, c_lit = 0, propblock = 0, ties = 0, model_diff = 0, open_n = 0, open_a = 0;
    long kind_n[2] = { 0, 0 }, kind_a[2] = { 0, 0 };
    printf("compare %s: sun dir %.6f %.6f %.6f (the core)\n", map, si.sun_dir[0], si.sun_dir[1], si.sun_dir[2]);
    while (fgets(ln, sizeof ln, f)) {
        char *q = strstr(ln, "PHOTON_SUN ");
        if (!q) continue;
        q += 11;
        if (!strncmp(q, "dir", 3) || !strncmp(q, "done", 4) || !strncmp(q, "start", 5)) { printf("  engine: %s", q); continue; }
        long idx;
        double p[3], rfrac, wfrac;
        int rsky, rss, wsky, wss;
        char kind[8], rtex[256], wtex[256];
        if (sscanf(q, "pt %ld %7s %lf %lf %lf %d %d %lf %255s %d %d %lf %255s", &idx, kind, &p[0], &p[1], &p[2], &rsky, &rss, &rfrac, rtex, &wsky, &wss,
                   &wfrac, wtex) != 13) continue;
        int got = sun_visible(m, s, p);
        RHit h;
        ref_sun(&r, &si, p, &h);
        tot++;
        int k = kind[0] == 'g';
        kind_n[k]++;
        eng_lit += wsky;
        c_lit += got;
        raw_agree += got == rsky;
        model_diff += sun_model_visible(m, s, p) != got;   /* the whole model rule (light cache point, sky flags) against the trace alone */
        if (!wss) { open_n++; open_a += got == wsky; }
        propblock += rsky != wsky;
        if (wss) { ss++; ss_agree += got == wsky; }
        if (got == wsky) { agree++; kind_a[k]++; continue; }
        if (h.graze) ties++;
        SunTrace t;
        double e[3];
        for (int j = 0; j < 3; j++) e[j] = p[j] + si.sun_dir[j] * SUN_TRACE_LENGTH;
        sun_trace(m, s, p, e, SUN_MASK_OPAQUE, &t);
        printf("  DIFF %ld %s (%.3f %.3f %.3f): engine %d (frac %.6f %s%s), core %d (hit %d brush %d disp %d frac %.6f flags %x), ref t %.6f t2 %.6f kind %d%s\n", idx, kind,
               p[0], p[1], p[2], wsky, wfrac, wtex, wss ? " startsolid" : "", got, t.hit, t.brush, t.disp, t.fraction, t.surf_flags, h.t, h.t2, h.kind,
               h.graze ? " near tie" : "");
    }
    fclose(f);
    printf("compare %s: %ld points, %ld agree (%.3f%%); fixed-height %ld/%ld, ground-relative %ld/%ld; start in solid %ld (%ld agree); "
           "lit: engine %ld, core %ld; near ties among the differences %ld; static props or entities changed the engine's raw answer at %ld (the core = raw at %ld); starts not in solid %ld/%ld; the model rule (light cache point, sky flags) differs from the trace alone at %ld\n",
           map, tot, agree, tot ? 100.0 * (double)agree / (double)tot : 0.0, kind_a[0], kind_n[0], kind_a[1], kind_n[1], ss, ss_agree, eng_lit, c_lit, ties, propblock, raw_agree, open_a, open_n, model_diff);
    sun_scratch_free(s);
    ref_free(&r);
    sun_map_free(m);
    free(b);
    return 0;
}

static int why_mode(const char *map, const double p[3]) {
    size_t n;
    uint8_t *b = read_file(map, &n);
    SunMap *m = b ? load_mem(b, n, 1, map) : NULL;
    if (!m) return 2;
    SunInfo si;
    sun_map_info(m, &si);
    Ref r;
    ref_load(&r, b);
    RHit h;
    ref_sun(&r, &si, p, &h);
    SunTrace t;
    double e[3];
    for (int j = 0; j < 3; j++) e[j] = p[j] + si.sun_dir[j] * SUN_TRACE_LENGTH;
    sun_trace(m, NULL, p, e, SUN_MASK_OPAQUE, &t);
    printf("core: hit %d brush %d side %d disp %d texinfo %d flags %x frac %.9f (%.3f units) startsolid %d allsolid %d leaves %d\n", t.hit, t.brush, t.side, t.disp,
           t.texinfo, t.surf_flags, t.fraction, t.fraction * SUN_TRACE_LENGTH, t.startsolid, t.allsolid, t.leaves);
    printf("ref: kind %d id %d sky %d t %.9f (%.3f units) t2 %.9f graze %d startsolid %d; contents at p %x\n", h.kind, h.id, h.sky, h.t, h.t * SUN_TRACE_LENGTH, h.t2,
           h.graze, h.startsolid, ref_contents(&r, p));
    /* every brush on the ray, in units along it */
    for (int i = 0; i < r.nb; i++) {
        const RBrush *br = &r.b[i];
        if (!(br->contents & SUN_MASK_OPAQUE) || !br->nsides) continue;
        double enter = -1e300, leave = 1e300;
        int lead = -1, inside = 1, miss = 0;
        for (int s = 0; s < br->nsides && !miss; s++) {
            const RPlane *pl = &br->pl[s];
            double d1 = pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d, d2 = pl->n[0] * e[0] + pl->n[1] * e[1] + pl->n[2] * e[2] - pl->d;
            if (d1 > 0) inside = 0;
            if (d1 > 0 && d2 > 0) miss = 1;
            if (miss || (d1 <= 0 && d2 <= 0)) continue;
            double t = d1 / (d1 - d2);
            if (d1 > d2) { if (t > enter) { enter = t; lead = s; } } else if (t < leave) leave = t;
        }
        if (miss || !(enter <= leave)) continue;
        printf("  brush %d contents %x %s enter %.3f leave %.3f lead flags %x\n", br->orig, br->contents, inside ? "START INSIDE" : "", enter * SUN_TRACE_LENGTH,
               leave * SUN_TRACE_LENGTH, lead >= 0 ? br->flags[lead] : -1);
    }
    ref_free(&r);
    sun_map_free(m);
    free(b);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--compare")) return compare_mode(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "--floorbox")) {   /* the box of the points near floors, for the harness's sample box */
        size_t n;
        uint8_t *b = read_file(argv[2], &n);
        SunMap *m = b ? load_mem(b, n, 1, argv[2]) : NULL;
        if (!m) return 2;
        SunInfo si;
        sun_map_info(m, &si);
        Ref r;
        ref_load(&r, b);
        double mn[3] = { 1e30, 1e30, 1e30 }, mx[3] = { -1e30, -1e30, -1e30 };
        for (int i = 0, got = 0; i < 2000000 && got < 20000; i++) {
            double p[3];
            if (!ground_point(m, &r, &si, p)) continue;
            got++;
            for (int k = 0; k < 3; k++) { mn[k] = fmin(mn[k], p[k]); mx[k] = fmax(mx[k], p[k]); }
        }
        printf("%.0f %.0f %.0f %.0f %.0f %.0f\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
        ref_free(&r);
        sun_map_free(m);
        free(b);
        return 0;
    }
    if (argc == 6 && !strcmp(argv[1], "--why")) { double p[3] = { atof(argv[3]), atof(argv[4]), atof(argv[5]) }; return why_mode(argv[2], p); }
    if (argc == 3 && !strcmp(argv[1], "--info")) {
        size_t n;
        uint8_t *b = read_file(argv[2], &n);
        SunMap *m = b ? load_mem(b, n, 1, argv[2]) : NULL;
        if (!m) return 2;
        SunInfo si;
        sun_map_info(m, &si);
        printf("%s: sun %d (%d skylights, lump %d v%d), normal %.9g %.9g %.9g, dir %.9g %.9g %.9g, intensity %.6g %.6g %.6g, style %d; %d brushes, %d sides, "
               "%d displacements (%d triangles), %d sky texinfo\n", argv[2], si.has_sun, si.nskylights, si.light_lump, si.light_lump_version, si.sun_normal[0],
               si.sun_normal[1], si.sun_normal[2], si.sun_dir[0], si.sun_dir[1], si.sun_dir[2], si.sun_intensity[0], si.sun_intensity[1], si.sun_intensity[2],
               si.sun_style, si.nbrushes, si.nsides, si.ndisps, si.ndisp_tris, si.nsky_texinfo);
        sun_map_free(m);
        free(b);
        return 0;
    }
    int nbench = 0, npts = 20000, hostile = 300, a = 1;
    const char *maps[4] = { NULL, NULL, NULL, NULL };
    int nm = 0;
    for (; a < argc; a++) {
        if (!strcmp(argv[a], "--bench") && a + 1 < argc) nbench = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--points") && a + 1 < argc) npts = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--hostile") && a + 1 < argc) hostile = atoi(argv[++a]);
        else if (nm < 4) maps[nm++] = argv[a];
    }
    printf("--- synthetic map\n");
    synth_tests();
    {
        size_t n;
        uint8_t *b = synth_sun_bsp(0, &n);
        read_tests("synth", b, n, hostile * 3);
        cache_tests("synth", b, n);
        free(b);
    }
    for (int i = 0; i < nm; i++) {
        size_t n;
        uint8_t *b = read_file(maps[i], &n);
        const char *name = strrchr(maps[i], '/') ? strrchr(maps[i], '/') + 1 : maps[i];
        if (!b) { CHECK(0, "cannot read %s", maps[i]); continue; }
        printf("--- %s\n", name);
        ref_compare(name, b, n, npts);
        read_tests(name, b, n, hostile);
        cache_tests(name, b, n);
        if (nbench) bench(name, b, n, nbench);
        free(b);
    }
    if (fails) { printf("SUNTEST FAIL (%d)\n", fails); return 1; }
    printf("SUNTEST OK\n");
    return 0;
}
