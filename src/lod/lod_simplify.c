/* F3: quadric-error half-edge collapse. See lod_simplify.h. */
#include "lod_simplify.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double q[10], w; } Quad;   /* symmetric 4x4: a2 ab ac ad b2 bc bd c2 cd d2; w: the weights summed (cost / w = mean squared distance) */

static void quad_plane(Quad *Q, double a, double b, double c, double d, double w) {
    Q->q[0] += w * a * a; Q->q[1] += w * a * b; Q->q[2] += w * a * c; Q->q[3] += w * a * d;
    Q->q[4] += w * b * b; Q->q[5] += w * b * c; Q->q[6] += w * b * d;
    Q->q[7] += w * c * c; Q->q[8] += w * c * d; Q->q[9] += w * d * d;
    Q->w += w;
}
static double quad_eval(const Quad *A, const Quad *B, const float *v) {
    double q[10];
    for (int i = 0; i < 10; i++) q[i] = A->q[i] + B->q[i];
    double x = v[0], y = v[1], z = v[2];
    double e = q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z +
               2 * q[8] * z + q[9];
    double w = A->w + B->w;
    return e > 0 && w > 0 ? e / w : 0;
}

typedef struct { double cost; uint32_t from, to, stamp; } Cand;
typedef struct { Cand *h; uint32_t n, cap; } Heap;
static int heap_push(Heap *H, Cand c) {
    if (H->n == H->cap) {
        uint32_t nc = H->cap ? H->cap * 2 : 1024;
        Cand *nh = realloc(H->h, sizeof *nh * nc);
        if (!nh) return -1;
        H->h = nh; H->cap = nc;
    }
    uint32_t i = H->n++;
    while (i && H->h[(i - 1) / 2].cost > c.cost) { H->h[i] = H->h[(i - 1) / 2]; i = (i - 1) / 2; }
    H->h[i] = c;
    return 0;
}
static Cand heap_pop(Heap *H) {
    Cand top = H->h[0], last = H->h[--H->n];
    uint32_t i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        double mc = last.cost;
        if (l < H->n && H->h[l].cost < mc) { m = l; mc = H->h[l].cost; }
        if (r < H->n && H->h[r].cost < mc) m = r;
        if (m == i) break;
        H->h[i] = H->h[m]; i = m;
    }
    if (H->n) H->h[i] = last;
    return top;
}

/* vertex -> triangles (CSR), built once; collapsed vertices keep their old lists, triangles are checked live through tri[] */
typedef struct {
    const float *pos;
    const uint32_t *cls;
    uint32_t nv, ntri;
    uint32_t *tri;           /* ntri x 3, live (remapped) */
    uint8_t *dead;           /* per triangle */
    uint32_t *vcount, *vcap;   /* per-vertex triangle lists (growable: a vertex inherits its collapsed neighbours' lists) */
    uint32_t **vlist;
    uint32_t *alias;         /* collapsed vertex -> the vertex it became */
    uint32_t *stamp;
    Quad *Q;
} Mesh;

static void tri_normal(const float *a, const float *b, const float *c, double n[3]) {
    double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    n[0] = u[1] * w[2] - u[2] * w[1]; n[1] = u[2] * w[0] - u[0] * w[2]; n[2] = u[0] * w[1] - u[1] * w[0];
}

static int list_add(Mesh *M, uint32_t v, uint32_t t) {
    if (M->vcount[v] == M->vcap[v]) {
        uint32_t nc = M->vcap[v] ? M->vcap[v] * 2 : 8;
        uint32_t *nl = realloc(M->vlist[v], sizeof *nl * nc);
        if (!nl) return -1;
        M->vlist[v] = nl; M->vcap[v] = nc;
    }
    M->vlist[v][M->vcount[v]++] = t;
    return 0;
}

/* would moving `from` onto `to` flip or degenerate a triangle that stays? */
static int collapse_ok(Mesh *M, uint32_t from, uint32_t to) {
    const float *pt = M->pos + 3 * to;
    for (uint32_t k = 0; k < M->vcount[from]; k++) {
        uint32_t t = M->vlist[from][k];
        if (M->dead[t]) continue;
        uint32_t *T = M->tri + 3 * t;
        if (T[0] == to || T[1] == to || T[2] == to) continue;   /* this one disappears */
        if (T[0] != from && T[1] != from && T[2] != from) continue;
        const float *p[3], *q[3];
        for (int i = 0; i < 3; i++) { p[i] = M->pos + 3 * T[i]; q[i] = T[i] == from ? pt : p[i]; }
        double n0[3], n1[3];
        tri_normal(p[0], p[1], p[2], n0);
        tri_normal(q[0], q[1], q[2], n1);
        double d = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2];
        double l0 = sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]), l1 = sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]);
        if (l1 < 1e-12 * (l0 + 1e-30) || d < 0.2 * l0 * l1) return 0;   /* degenerate, or turned by more than ~78 degrees */
    }
    return 1;
}

static int push_vertex_cands(Mesh *M, Heap *H, uint32_t v) {
    /* every live neighbour of v, both directions */
    for (uint32_t k = 0; k < M->vcount[v]; k++) {
        uint32_t t = M->vlist[v][k];
        if (M->dead[t]) continue;
        uint32_t *T = M->tri + 3 * t;
        for (int i = 0; i < 3; i++) {
            uint32_t a = T[i], b = T[(i + 1) % 3];
            if (a != v && b != v) continue;
            uint32_t o = a == v ? b : a;
            if (M->cls[v] != M->cls[o]) continue;
            Cand c1 = { quad_eval(&M->Q[v], &M->Q[o], M->pos + 3 * o), v, o, M->stamp[v] + M->stamp[o] };
            Cand c2 = { quad_eval(&M->Q[o], &M->Q[v], M->pos + 3 * v), o, v, M->stamp[v] + M->stamp[o] };
            if (heap_push(H, c1) || heap_push(H, c2)) return -1;
        }
    }
    return 0;
}

int lod_simplify(const float *pos, const uint32_t *cls, uint32_t nv, const uint32_t *idx, uint32_t ntri, const LodParams *p, uint32_t *out_idx,
                 LodResult *r) {
    memset(r, 0, sizeof *r);
    Mesh M;
    memset(&M, 0, sizeof M);
    M.pos = pos; M.cls = cls; M.nv = nv; M.ntri = ntri;
    Heap H = { 0 };
    int rc = -1;
    M.tri = malloc(sizeof *M.tri * 3 * (size_t)ntri);
    M.dead = calloc(ntri ? ntri : 1, 1);
    M.vcount = calloc(nv ? nv : 1, sizeof *M.vcount);
    M.vcap = calloc(nv ? nv : 1, sizeof *M.vcap);
    M.vlist = calloc(nv ? nv : 1, sizeof *M.vlist);
    M.alias = malloc(sizeof *M.alias * (nv ? nv : 1));
    M.stamp = calloc(nv ? nv : 1, sizeof *M.stamp);
    M.Q = calloc(nv ? nv : 1, sizeof *M.Q);
    if (!M.tri || !M.dead || !M.vcount || !M.vcap || !M.vlist || !M.alias || !M.stamp || !M.Q) goto out;
    memcpy(M.tri, idx, sizeof *M.tri * 3 * (size_t)ntri);
    for (uint32_t v = 0; v < nv; v++) M.alias[v] = v;
    uint32_t live = 0;
    for (uint32_t t = 0; t < ntri; t++) {
        uint32_t *T = M.tri + 3 * t;
        if (T[0] >= nv || T[1] >= nv || T[2] >= nv || T[0] == T[1] || T[1] == T[2] || T[0] == T[2]) { M.dead[t] = 1; continue; }
        live++;
        for (int i = 0; i < 3; i++) if (list_add(&M, T[i], t)) goto out;
        double n[3];
        tri_normal(pos + 3 * T[0], pos + 3 * T[1], pos + 3 * T[2], n);
        double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-20) continue;
        double a = n[0] / l, b = n[1] / l, c = n[2] / l, d = -(a * pos[3 * T[0]] + b * pos[3 * T[0] + 1] + c * pos[3 * T[0] + 2]);
        for (int i = 0; i < 3; i++) quad_plane(&M.Q[T[i]], a, b, c, d, l * 0.5);   /* area-weighted */
    }
    /* open edges: count each undirected edge's triangles through the vertex lists */
    for (uint32_t t = 0; t < ntri; t++) {
        if (M.dead[t]) continue;
        uint32_t *T = M.tri + 3 * t;
        for (int i = 0; i < 3; i++) {
            uint32_t a = T[i], b = T[(i + 1) % 3], shared = 0;
            for (uint32_t k = 0; k < M.vcount[a]; k++) {
                uint32_t u = M.vlist[a][k];
                if (u == t || M.dead[u]) continue;
                uint32_t *U = M.tri + 3 * u;
                if (U[0] == b || U[1] == b || U[2] == b) shared++;
            }
            if (shared) continue;
            /* a plane through the edge, perpendicular to the triangle */
            const float *pa = pos + 3 * a, *pb = pos + 3 * b;
            double n[3], e[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
            tri_normal(pos + 3 * T[0], pos + 3 * T[1], pos + 3 * T[2], n);
            double m[3] = { e[1] * n[2] - e[2] * n[1], e[2] * n[0] - e[0] * n[2], e[0] * n[1] - e[1] * n[0] };
            double l = sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), el = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            if (l < 1e-20) continue;
            double A = m[0] / l, B = m[1] / l, C = m[2] / l, D = -(A * pa[0] + B * pa[1] + C * pa[2]);
            quad_plane(&M.Q[a], A, B, C, D, p->border_weight * el * el);
            quad_plane(&M.Q[b], A, B, C, D, p->border_weight * el * el);
        }
    }
    for (uint32_t v = 0; v < nv; v++) if (push_vertex_cands(&M, &H, v)) goto out;
    double maxe2 = p->max_error > 0 ? p->max_error * p->max_error : INFINITY;
    while (H.n && (p->target_tris == 0 || live > p->target_tris)) {
        Cand c = heap_pop(&H);
        if (M.alias[c.from] != c.from || M.alias[c.to] != c.to || c.stamp != M.stamp[c.from] + M.stamp[c.to]) continue;   /* stale */
        if (c.cost > maxe2) break;
        if (!collapse_ok(&M, c.from, c.to)) { r->refused_flip++; continue; }
        /* collapse: triangles with both die, the rest move to `to` */
        for (uint32_t k = 0; k < M.vcount[c.from]; k++) {
            uint32_t t = M.vlist[c.from][k];
            if (M.dead[t]) continue;
            uint32_t *T = M.tri + 3 * t;
            if (T[0] != c.from && T[1] != c.from && T[2] != c.from) continue;
            if (T[0] == c.to || T[1] == c.to || T[2] == c.to) { M.dead[t] = 1; live--; continue; }
            for (int i = 0; i < 3; i++) if (T[i] == c.from) T[i] = c.to;
            if (list_add(&M, c.to, t)) goto out;
        }
        for (int i = 0; i < 10; i++) M.Q[c.to].q[i] += M.Q[c.from].q[i];
        M.Q[c.to].w += M.Q[c.from].w;
        M.alias[c.from] = c.to;
        M.stamp[c.to]++;
        r->collapses++;
        double e = sqrt(c.cost);
        if (e > r->error) r->error = e;
        if (push_vertex_cands(&M, &H, c.to)) goto out;
    }
    uint32_t w = 0;
    for (uint32_t t = 0; t < ntri; t++) {
        if (M.dead[t]) continue;
        memcpy(out_idx + 3 * w, M.tri + 3 * t, 3 * sizeof *out_idx);
        w++;
    }
    r->tris = w;
    rc = 0;
out:
    if (M.vlist) for (uint32_t v = 0; v < nv; v++) free(M.vlist[v]);
    free(M.tri); free(M.dead); free(M.vcount); free(M.vcap); free(M.vlist); free(M.alias); free(M.stamp); free(M.Q); free(H.h);
    return rc;
}
