/* L3b (analysis/37): see l3_bake.h. */
#include "l3_bake.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "amb_bake.h"
#include "lg_light.h"
#include "lg_map.h"
#include "sun_vis.h"

typedef struct { int ofs, len, ver; char cc[4]; } Lump;
typedef struct { float n[3], d; int type; } Plane;
typedef struct { float tv[2][4], lv[2][4]; int flags, texdata; } TexInfo;
#pragma pack(push, 1)
typedef struct {
    uint16_t plane; uint8_t side, on_node; int32_t first_edge; int16_t nedges, texinfo, dispinfo, fog;
    uint8_t styles[4]; int32_t lightofs; float area; int32_t lm_mins[2], lm_size[2]; int32_t orig; uint16_t nprims, first_prim; uint32_t smoothing;
} Face;
#pragma pack(pop)
enum { SURF_SKY = 0x4, SURF_NOLIGHT = 0x400, SURF_NODRAW = 0x80, MAXPOLY = 64 };

typedef struct { const unsigned char *b; size_t len; } Bsp;
static const void *lump(const Bsp *m, int i, int rec, int *n) {
    const Lump *l = (const Lump *)(m->b + 8 + 16 * i);
    *n = 0;
    if (l->len <= 0 || l->ofs < 0 || (size_t)l->ofs + (size_t)l->len > m->len || memcmp(m->b + l->ofs, "LZMA", 4) == 0) return NULL;
    *n = l->len / rec;
    return m->b + l->ofs;
}

static double now_ms(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

/* --- the shell: voxel key -> normal, open addressing ------------------------------------------------------------------------------ */
typedef struct { uint64_t key; float n[3], h; int air; } Vox;   /* n, h: its face's normal and the centre's height above that face; air: 0 not yet known, 1 air, 2 in solid */
typedef struct { Vox *v; size_t cap, n; } VoxMap;
static uint64_t vkey(int x, int y, int z) { return ((uint64_t)(uint32_t)(x + (1 << 20)) << 42) | ((uint64_t)(uint32_t)(y + (1 << 20)) << 21) | (uint64_t)(uint32_t)(z + (1 << 20)); }
static void vunkey(uint64_t k, int *x, int *y, int *z) { *x = (int)(k >> 42) - (1 << 20); *y = (int)((k >> 21) & 0x1fffff) - (1 << 20); *z = (int)(k & 0x1fffff) - (1 << 20); }
static size_t hslot(uint64_t k, size_t cap) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (size_t)k & (cap - 1); }
static int vm_grow(VoxMap *m) {
    size_t nc = m->cap ? m->cap * 2 : 1 << 16;
    Vox *nv = calloc(nc, sizeof *nv);
    if (!nv) return 0;
    for (size_t i = 0; i < m->cap; i++)
        if (m->v[i].key) { size_t h = hslot(m->v[i].key, nc); while (nv[h].key) h = (h + 1) & (nc - 1); nv[h] = m->v[i]; }
    free(m->v);
    m->v = nv; m->cap = nc;
    return 1;
}
static int vm_add(VoxMap *m, uint64_t k, const float n[3], float hgt) {
    if ((m->n + 1) * 2 > m->cap && !vm_grow(m)) return 0;
    size_t h = hslot(k, m->cap);
    while (m->v[h].key && m->v[h].key != k) h = (h + 1) & (m->cap - 1);
    if (m->v[h].key) return 1;   /* first face wins its normal */
    m->v[h].key = k; memcpy(m->v[h].n, n, sizeof m->v[h].n); m->v[h].h = hgt; m->v[h].air = 0; m->n++;
    return 1;
}
static Vox *vm_get(const VoxMap *m, uint64_t k) {
    if (!m->cap) return NULL;
    size_t h = hslot(k, m->cap);
    while (m->v[h].key) { if (m->v[h].key == k) return &m->v[h]; h = (h + 1) & (m->cap - 1); }
    return NULL;
}

static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static float lum3(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

/* the lamp's unoccluded light at p (no N.L): luminance x cone / attenuation, as tools/l3_lmpredict.c (vrad's direct term) */
static double lamp_at(const LgWorldLight *w, const double p[3], double widen) {
    double v[3] = { p[0] - w->origin[0], p[1] - w->origin[1], p[2] - w->origin[2] };
    double d = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    double c = w->constant_attn, l = w->linear_attn, q = w->quadratic_attn;
    if (c == 0 && l == 0 && q == 0) c = 1;
    double r = 1.0 / fmax(c + l * d + q * d * d, 1e-3);
    if (w->type == LG_EMIT_SPOT && d > widen) {
        double dot2 = (v[0] * w->normal[0] + v[1] * w->normal[1] + v[2] * w->normal[2]) / d + widen / d;
        if (dot2 <= w->stopdot2) return 0;
        if (dot2 < w->stopdot) r *= pow((dot2 - w->stopdot2) / fmax(w->stopdot - w->stopdot2, 1e-4), w->exponent > 0 ? w->exponent : 1);
    }
    return r * lum3(w->intensity);
}

/* --- visibility cache: (voxel, lamp) -> 0 shadow, 1 lit, 2 not a shell voxel (absent or in solid); traced once --------------------- */
typedef struct { uint64_t key; uint8_t val; } VisEnt;
typedef struct { VisEnt *v; size_t cap, n; } VisMap;
typedef struct { VoxMap *vm; VisMap *vc; const SunMap *sm; SunScratch *scr; const LgWorldLight *wl; double V; L3Stats *st; int dbg; } VisCtx;
static int vis_grow(VisMap *m) {
    size_t nc = m->cap ? m->cap * 2 : 1 << 18;
    VisEnt *nv = calloc(nc, sizeof *nv);
    if (!nv) return 0;
    for (size_t i = 0; i < m->cap; i++)
        if (m->v[i].key) { size_t h = hslot(m->v[i].key, nc); while (nv[h].key) h = (h + 1) & (nc - 1); nv[h] = m->v[i]; }
    free(m->v);
    m->v = nv; m->cap = nc;
    return 1;
}
static int vis_of(VisCtx *c, int x, int y, int z, int lamp) {   /* -1: out of memory */
    Vox *vx = vm_get(c->vm, vkey(x, y, z));   /* not a shell voxel, or in solid: no trace, no cache entry */
    if (!vx) return 2;
    if (!vx->air) {
        double q[3] = { (x + 0.5) * c->V, (y + 0.5) * c->V, (z + 0.5) * c->V };
        int lf = sun_point_leaf(c->sm, q);
        vx->air = lf < 0 || (sun_leaf_contents(c->sm, lf) & 1) ? 2 : 1;
        if (vx->air == 2) c->st->voxels_solid++;
    }
    if (vx->air == 2) return 2;
    uint64_t k = vkey(x, y, z) * 0x9e3779b97f4a7c15ull ^ ((uint64_t)(lamp + 1) << 1) ^ 1;
    if (c->vc->cap) {
        size_t h = hslot(k, c->vc->cap);
        while (c->vc->v[h].key) { if (c->vc->v[h].key == k) return c->vc->v[h].val; h = (h + 1) & (c->vc->cap - 1); }
    }
    int val;
    {
        /* on the surface, as the lightmap sees it: the centre moved along its face's normal to 1 unit above the face (the centre sits up to 1.5
         * voxels above it, so a low occluder's shadow would land off the lightmap's) */
        double q[3] = { (x + 0.5) * c->V, (y + 0.5) * c->V, (z + 0.5) * c->V }, drop = vx->h - 1.0;
        if (drop > 0) for (int k2 = 0; k2 < 3; k2++) q[k2] -= vx->n[k2] * drop;
        double o[3] = { c->wl[lamp].origin[0], c->wl[lamp].origin[1], c->wl[lamp].origin[2] };
        SunTrace t;
        sun_trace(c->sm, c->scr, q, o, SUN_MASK_OPAQUE, &t);
        c->st->rays++;
        val = t.fraction >= 0.999;
    }
    if ((c->vc->n + 1) * 2 > c->vc->cap && !vis_grow(c->vc)) return -1;
    size_t h = hslot(k, c->vc->cap);
    while (c->vc->v[h].key) h = (h + 1) & (c->vc->cap - 1);
    c->vc->v[h].key = k; c->vc->v[h].val = (uint8_t)val; c->vc->n++;
    return val;
}

int l3_bake(const void *bsp, size_t len, const L3Params *p, L3Bake **out, L3Stats *st) {
    *out = NULL;
    memset(st, 0, sizeof *st);
    if (!bsp || len < 1036 || memcmp(bsp, "VBSP", 4) != 0 || !(p->voxel >= 1)) return AMB_ERR_ARG;
    Bsp m = { bsp, len };
    int np, nv, ne, nse, nt, nf;
    const Plane *planes = lump(&m, 1, sizeof(Plane), &np);
    const float (*verts)[3] = lump(&m, 3, 12, &nv);
    const uint16_t (*edges)[2] = lump(&m, 12, 4, &ne);
    const int32_t *surfedges = lump(&m, 13, 4, &nse);
    const TexInfo *ti = lump(&m, 6, sizeof(TexInfo), &nt);
    const Face *faces = lump(&m, 7, sizeof(Face), &nf);
    if (!planes || !verts || !edges || !surfedges || !ti || !faces) return AMB_ERR_FORMAT;
    LgMap *lm = NULL;
    SunMap *sm = NULL;
    int rc = lg_map_load(bsp, len, 1, &lm);
    if (rc == AMB_OK) rc = sun_map_load(bsp, len, 1, &sm);
    if (rc != AMB_OK) { lg_map_free(lm); return rc; }
    SunScratch *scr = sun_scratch_new(sm);
    int nl = 0;
    const LgWorldLight *wl = lg_map_lights(lm, &nl);
    const double V = p->voxel;
    VoxMap vm = { 0 };
    L3Bake *bk = NULL;

    /* ---- the shell */
    double t0 = now_ms();
    for (int fi = 0; fi < nf; fi++) {
        const Face *fc = &faces[fi];
        if (fc->dispinfo >= 0 || fc->texinfo < 0 || fc->texinfo >= nt || fc->plane >= np || fc->nedges < 3 || fc->nedges > MAXPOLY) continue;
        if (ti[fc->texinfo].flags & (SURF_SKY | SURF_NOLIGHT | SURF_NODRAW)) continue;
        double poly[MAXPOLY][3];
        int ok = 1;
        for (int e = 0; e < fc->nedges && ok; e++) {
            int si = fc->first_edge + e;
            if (si < 0 || si >= nse) { ok = 0; break; }
            int se = surfedges[si], ei = se >= 0 ? se : -se;
            if (ei >= ne) { ok = 0; break; }
            int vi = se >= 0 ? edges[ei][0] : edges[ei][1];
            if (vi >= nv) { ok = 0; break; }
            for (int k = 0; k < 3; k++) poly[e][k] = verts[vi][k];
        }
        if (!ok) continue;
        st->faces++;
        double n[3] = { planes[fc->plane].n[0], planes[fc->plane].n[1], planes[fc->plane].n[2] }, d = planes[fc->plane].d;
        /* no flip for fc->side: the face's plane number already names its own facing plane (side == planenum & 1 on every face; flipping
 * again put the luxels behind ~1 in 5 side-1 faces, the dark walls of the 2026-11-09 mode screenshots) */
        float nf3[3] = { (float)n[0], (float)n[1], (float)n[2] };
        double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
        for (int e = 0; e < fc->nedges; e++)
            for (int k = 0; k < 3; k++) { lo[k] = fmin(lo[k], poly[e][k]); hi[k] = fmax(hi[k], poly[e][k]); }
        int a[3], b[3];
        for (int k = 0; k < 3; k++) { a[k] = (int)floor((lo[k] - 1.5 * V) / V); b[k] = (int)floor((hi[k] + 1.5 * V) / V); }
        for (int z = a[2]; z <= b[2]; z++)
            for (int y = a[1]; y <= b[1]; y++)
                for (int x = a[0]; x <= b[0]; x++) {
                    double c[3] = { (x + 0.5) * V, (y + 0.5) * V, (z + 0.5) * V };
                    double h = n[0] * c[0] + n[1] * c[1] + n[2] * c[2] - d;
                    if (h <= 0 || h > 1.5 * V) continue;   /* in front, within 1.5 voxels */
                    int inside = 1;
                    for (int e = 0; e < fc->nedges && inside; e++) {   /* convex polygon, half a voxel of margin */
                        const double *p0 = poly[e], *p1 = poly[(e + 1) % fc->nedges];
                        double ed[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, w[3] = { c[0] - p0[0], c[1] - p0[1], c[2] - p0[2] };
                        double cr[3] = { ed[1] * w[2] - ed[2] * w[1], ed[2] * w[0] - ed[0] * w[2], ed[0] * w[1] - ed[1] * w[0] };
                        double el = sqrt(ed[0] * ed[0] + ed[1] * ed[1] + ed[2] * ed[2]);
                        if (cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] < -0.5 * V * el) inside = 0;
                    }
                    if (inside && !vm_add(&vm, vkey(x, y, z), nf3, (float)h)) { rc = AMB_ERR_NOMEM; goto done; }
                }
    }
    st->shell_voxels = (long)vm.n;
    st->ms_shell = now_ms() - t0;

    /* ---- bricks: one per occupied L3_B^3 cell, its voxels' local index */
    {
        VoxMap bm = { 0 };   /* brick key -> (index in .n[0]) */
        long nb = 0;
        for (size_t i = 0; i < vm.cap; i++) {
            if (!vm.v[i].key) continue;
            int x, y, z;
            vunkey(vm.v[i].key, &x, &y, &z);
            uint64_t k = vkey(floordiv(x, L3_B), floordiv(y, L3_B), floordiv(z, L3_B));
            if (!vm_get(&bm, k)) { float idx[3] = { (float)nb, 0, 0 }; if (!vm_add(&bm, k, idx, 0)) { free(bm.v); rc = AMB_ERR_NOMEM; goto done; } nb++; }
        }
        bk = calloc(1, sizeof *bk);
        if (bk) bk->bricks = calloc((size_t)(nb ? nb : 1), sizeof *bk->bricks);
        if (!bk || !bk->bricks) { free(bm.v); rc = AMB_ERR_NOMEM; goto done; }
        bk->voxel = V;
        bk->nbricks = nb;
        for (size_t i = 0; i < bm.cap; i++)
            if (bm.v[i].key) { L3Brick *br = &bk->bricks[(long)bm.v[i].n[0]]; vunkey(bm.v[i].key, &br->x, &br->y, &br->z); }
        free(bm.v);
        st->bricks = nb;
    }

    /* ---- per brick: its lamps; which bricks hold a visibility edge (or neighbour one); their signed distances with a halo */
    {
        static const int NV = L3_B * L3_B * L3_B;
        enum { R = L3_DMAX, H = L3_B + 2 * L3_DMAX };   /* the halo: an edge up to L3_DMAX voxels into a neighbour brick still counts */
        double tr = 0, tsdf = 0;
        char *lamp_solid = calloc((size_t)(nl ? nl : 1), 1);
        unsigned char *edge = calloc((size_t)(bk->nbricks ? bk->nbricks : 1), 1), *keep = calloc((size_t)(bk->nbricks ? bk->nbricks : 1), 1);
        unsigned char *loc = malloc((size_t)H * H * H);
        VisMap vc = { 0 };
        VoxMap bm = { 0 };
        if (!lamp_solid || !edge || !keep || !loc) { free(lamp_solid); free(edge); free(keep); free(loc); rc = AMB_ERR_NOMEM; goto done; }
        for (int li = 0; li < nl; li++) {   /* a lamp whose origin is in solid would shadow everything: left out */
            double o[3] = { wl[li].origin[0], wl[li].origin[1], wl[li].origin[2] };
            int lf = sun_point_leaf(sm, o);
            lamp_solid[li] = lf < 0 || (sun_leaf_contents(sm, lf) & 1);
        }
        VisCtx vx = { &vm, &vc, sm, scr, wl, V, st, 0 };
        const double bdiag = L3_B * V * 0.8660254 + 1;
        double t1 = now_ms();
        for (long bi = 0; bi < bk->nbricks && rc == AMB_OK; bi++) {
            if (p->cancel && *p->cancel) { rc = AMB_ERR_CANCELLED; break; }
            L3Brick *br = &bk->bricks[bi];
            float idx3[3] = { (float)bi, 0, 0 };
            if (!vm_add(&bm, vkey(br->x, br->y, br->z), idx3, 0)) { rc = AMB_ERR_NOMEM; break; }
            double c[3] = { (br->x * L3_B + L3_B * 0.5) * V, (br->y * L3_B + L3_B * 0.5) * V, (br->z * L3_B + L3_B * 0.5) * V };
            double best[L3_LAMPS];
            for (int k = 0; k < L3_LAMPS; k++) { br->lamp[k] = -1; best[k] = 0; }
            for (int li = 0; li < nl; li++) {
                const LgWorldLight *w = &wl[li];
                if ((w->type != LG_EMIT_POINT && w->type != LG_EMIT_SPOT) || lamp_solid[li]) continue;
                double reach = lg_window_radius(w, 0.01);
                double v[3] = { c[0] - w->origin[0], c[1] - w->origin[1], c[2] - w->origin[2] };
                if (sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) > reach + bdiag) continue;
                double sv = lamp_at(w, c, bdiag);
                if (sv < p->min_light) continue;
                int at = L3_LAMPS;
                while (at > 0 && best[at - 1] < sv) at--;
                if (at >= L3_LAMPS) continue;
                for (int k = L3_LAMPS - 1; k > at; k--) { best[k] = best[k - 1]; br->lamp[k] = br->lamp[k - 1]; }
                best[at] = sv; br->lamp[at] = (int16_t)li;
            }
            for (int s2 = 0; s2 < L3_LAMPS && !edge[bi]; s2++) {
                if (br->lamp[s2] < 0) continue;
                int seen = 0;
                for (int k = 0; k < NV && !edge[bi]; k++) {
                    int v2 = vis_of(&vx, br->x * L3_B + k % L3_B, br->y * L3_B + (k / L3_B) % L3_B, br->z * L3_B + k / (L3_B * L3_B), br->lamp[s2]);
                    if (v2 < 0) { rc = AMB_ERR_NOMEM; break; }
                    if (v2 < 2) { seen |= 1 << v2; if (seen == 3) edge[bi] = 1; }
                }
            }
            st->brick_lamps += (br->lamp[0] >= 0) + (br->lamp[1] >= 0) + (br->lamp[2] >= 0) + (br->lamp[3] >= 0);
        }
        tr = now_ms() - t1;
        /* keep: an edge brick and its 26 neighbours (an edge within L3_DMAX of their voxels) */
        for (long bi = 0; bi < bk->nbricks && rc == AMB_OK; bi++) {
            if (!edge[bi]) continue;
            st->edge_bricks++;
            const L3Brick *br = &bk->bricks[bi];
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const Vox *nb = vm_get(&bm, vkey(br->x + dx, br->y + dy, br->z + dz));
                        if (nb) keep[(long)nb->n[0]] = 1;
                    }
        }
        double t2 = now_ms();
        /* the halo offsets within L3_DMAX + 0.5, sorted by distance */
        struct { int d; float r; } off[(2 * R + 1) * (2 * R + 1) * (2 * R + 1)];
        int noff = 0;
        for (int dz = -R; dz <= R; dz++)
            for (int dy = -R; dy <= R; dy++)
                for (int dx = -R; dx <= R; dx++) {
                    double dd = sqrt((double)(dx * dx + dy * dy + dz * dz));
                    if (dd >= R + 0.5 || dd == 0) continue;
                    int at = noff++;
                    while (at > 0 && off[at - 1].r > dd) { off[at] = off[at - 1]; at--; }
                    off[at].d = dx + H * (dy + H * dz); off[at].r = (float)dd;
                }
        for (long bi = 0; bi < bk->nbricks && rc == AMB_OK; bi++) {
            if (!keep[bi]) continue;
            if (p->cancel && *p->cancel) { rc = AMB_ERR_CANCELLED; break; }
            L3Brick *br = &bk->bricks[bi];
            int ox = br->x * L3_B - R, oy = br->y * L3_B - R, oz = br->z * L3_B - R;
            for (int s2 = 0; s2 < L3_LAMPS && rc == AMB_OK; s2++) {
                if (br->lamp[s2] < 0) { for (int k = 0; k < NV; k++) br->sdf[k][s2] = 255; continue; }
                for (int k = 0; k < H * H * H; k++) {   /* the brick and its halo: 0 shadow, 1 lit, 2 not in the shell */
                    int v2 = vis_of(&vx, ox + k % H, oy + (k / H) % H, oz + k / (H * H), br->lamp[s2]);
                    if (v2 < 0) { rc = AMB_ERR_NOMEM; break; }
                    loc[k] = (unsigned char)v2;
                }
                float dist[L3_B * L3_B * L3_B];
                unsigned char have[L3_B * L3_B * L3_B];
                for (int k = 0; k < NV; k++) {
                    int kx = k % L3_B + R, ky = (k / L3_B) % L3_B + R, kz = k / (L3_B * L3_B) + R;
                    int me = loc[kx + H * (ky + H * kz)];
                    have[k] = me < 2;
                    if (me == 2) continue;
                    double dm = R + 0.5;
                    int base = kx + H * (ky + H * kz);
                    for (int q = 0; q < noff; q++) {   /* nearest first: the first voxel of the other visibility is the distance */
                        int o2 = loc[base + off[q].d];
                        if (o2 < 2 && o2 != me) { dm = off[q].r; break; }
                    }
                    double sd = (dm - 0.5) * (me ? 1 : -1);
                    dist[k] = (float)(sd > R ? R : sd < -R ? -R : sd);
                }
                /* voxels outside the shell take a neighbouring shell voxel's value, grown one ring per pass (26-neighbourhood) */
                for (int pass = 0, grew = 1; pass < L3_B && grew; pass++) {
                    unsigned char nh[L3_B * L3_B * L3_B];
                    memcpy(nh, have, sizeof nh);
                    grew = 0;
                    for (int k = 0; k < NV; k++) {
                        if (have[k]) continue;
                        int kx = k % L3_B, ky = (k / L3_B) % L3_B, kz = k / (L3_B * L3_B);
                        for (int dz = -1; dz <= 1 && !nh[k]; dz++)
                            for (int dy = -1; dy <= 1 && !nh[k]; dy++)
                                for (int dx = -1; dx <= 1 && !nh[k]; dx++) {
                                    int x2 = kx + dx, y2 = ky + dy, z2 = kz + dz;
                                    if (x2 < 0 || y2 < 0 || z2 < 0 || x2 >= L3_B || y2 >= L3_B || z2 >= L3_B) continue;
                                    int j = x2 + L3_B * (y2 + L3_B * z2);
                                    if (have[j]) { dist[k] = dist[j]; nh[k] = 1; grew = 1; }
                                }
                    }
                    memcpy(have, nh, sizeof nh);
                }
                for (int k = 0; k < NV; k++) {
                    if (!have[k]) dist[k] = (float)R;
                    int byte = (int)lrint(128.0 + dist[k] * 127.0 / R);
                    br->sdf[k][s2] = (uint8_t)(byte < 0 ? 0 : byte > 255 ? 255 : byte);
                }
            }
        }
        tsdf = now_ms() - t2;
        if (rc == AMB_OK) {   /* only the kept bricks stay, with only their lamp slots that hold an edge */
            long w = 0;
            for (long bi = 0; bi < bk->nbricks; bi++) {
                if (!keep[bi]) continue;
                L3Brick *br = &bk->bricks[bi];
                int any = 0;
                for (int s2 = 0; s2 < L3_LAMPS; s2++) {
                    if (br->lamp[s2] < 0) continue;
                    int edge_slot = 0;   /* a slot clamped at +-L3_DMAX everywhere: sharp = blurry there, nothing to correct */
                    for (int k = 0; k < NV && !edge_slot; k++) edge_slot = br->sdf[k][s2] != 0 && br->sdf[k][s2] != 255;
                    if (edge_slot && p->min_share > 0) {   /* a lamp that gives under min_share of the light here: sharpening its edge barely shows */
                        double cc[3] = { (br->x * L3_B + L3_B * 0.5) * V, (br->y * L3_B + L3_B * 0.5) * V, (br->z * L3_B + L3_B * 0.5) * V }, tot = 0;
                        for (int li = 0; li < nl; li++)
                            if ((wl[li].type == LG_EMIT_POINT || wl[li].type == LG_EMIT_SPOT) && wl[li].style == 0) tot += lamp_at(&wl[li], cc, 0);
                        double mine = lamp_at(&wl[br->lamp[s2]], cc, 0);
                        if (tot > 0 && mine < p->min_share * tot) { edge_slot = 0; st->slots_weak++; }
                    }
                    if (!edge_slot) { br->lamp[s2] = -1; for (int k = 0; k < NV; k++) br->sdf[k][s2] = 255; st->slots_dropped++; }
                    else any = 1;
                }
                if (!any) { st->bricks_dropped++; continue; }
                if (w != bi) bk->bricks[w] = *br;
                w++;
            }
            bk->nbricks = w;
            st->bricks = w;
        }
        st->ms_rays = tr;
        st->ms_sdf = tsdf;
        free(lamp_solid); free(edge); free(keep); free(loc); free(vc.v); free(bm.v);
    }

done:
    free(vm.v);
    sun_scratch_free(scr);
    sun_map_free(sm);
    lg_map_free(lm);
    if (rc != AMB_OK) { l3_bake_free(bk); return rc; }
    *out = bk;
    return AMB_OK;
}

void l3_bake_free(L3Bake *b) {
    if (!b) return;
    free(b->bricks);
    free(b);
}

/* --- L3c: packing for the GPU (l3_bake.h) ------------------------------------------------------------------------------------------ */
void l3_pack_free(L3Pack *p) {
    if (!p) return;
    free(p->l1); free(p->l2); free(p->atlas); free(p->lamps); free(p);
}
int l3_pack(const L3Bake *b, L3Pack **out) {
    *out = NULL;
    if (!b || b->nbricks <= 0 || b->nbricks >= 65535) return AMB_ERR_ARG;
    L3Pack *p = calloc(1, sizeof *p);
    if (!p) return AMB_ERR_NOMEM;
    p->voxel = b->voxel;
    p->nbricks = (int)b->nbricks;
    int lo[3] = { 1 << 30, 1 << 30, 1 << 30 }, hi[3] = { -(1 << 30), -(1 << 30), -(1 << 30) };
    for (long i = 0; i < b->nbricks; i++) {
        int c[3] = { floordiv(b->bricks[i].x, L3_SB), floordiv(b->bricks[i].y, L3_SB), floordiv(b->bricks[i].z, L3_SB) };
        for (int k = 0; k < 3; k++) { if (c[k] < lo[k]) lo[k] = c[k]; if (c[k] > hi[k]) hi[k] = c[k]; }
    }
    for (int k = 0; k < 3; k++) { p->sb_min[k] = lo[k]; p->l1_dim[k] = hi[k] - lo[k] + 1; }
    size_t n1 = (size_t)p->l1_dim[0] * p->l1_dim[1] * p->l1_dim[2];
    p->l1 = calloc(n1, sizeof *p->l1);
    if (!p->l1 || n1 > (1u << 24)) { l3_pack_free(p); return AMB_ERR_NOMEM; }
    /* pages: one per occupied super-brick, in brick order of first appearance */
    int np = 0;
    for (long i = 0; i < b->nbricks; i++) {
        const L3Brick *br = &b->bricks[i];
        size_t c = (size_t)(floordiv(br->x, L3_SB) - lo[0]) + (size_t)p->l1_dim[0] * ((size_t)(floordiv(br->y, L3_SB) - lo[1]) + (size_t)p->l1_dim[1] * (size_t)(floordiv(br->z, L3_SB) - lo[2]));
        if (!p->l1[c]) { if (np >= 65534) { l3_pack_free(p); return AMB_ERR_NOMEM; } p->l1[c] = (uint16_t)(++np); }
    }
    p->npages = np;
    p->pz = (np + L3_PX * L3_PY - 1) / (L3_PX * L3_PY);
    int w2 = L3_PX * L3_SB, h2 = L3_PY * L3_SB, d2 = p->pz * L3_SB;
    p->l2 = calloc((size_t)w2 * h2 * d2, sizeof *p->l2);
    p->az = (p->nbricks + L3_AX * L3_AY - 1) / (L3_AX * L3_AY);
    int wa = L3_AX * L3_B, ha = L3_AY * L3_B, da = p->az * L3_B;
    p->atlas = calloc((size_t)wa * ha * da, 4);
    p->lamp_rows = (p->nbricks + L3_LW - 1) / L3_LW;
    p->lamps = malloc(sizeof(float) * 4 * (size_t)L3_LW * (size_t)p->lamp_rows);
    if (!p->l2 || !p->atlas || !p->lamps) { l3_pack_free(p); return AMB_ERR_NOMEM; }
    for (size_t i = 0; i < 4 * (size_t)L3_LW * (size_t)p->lamp_rows; i++) p->lamps[i] = -1;
    for (int i = 0; i < p->nbricks; i++) {
        const L3Brick *br = &b->bricks[i];
        int sb[3] = { floordiv(br->x, L3_SB), floordiv(br->y, L3_SB), floordiv(br->z, L3_SB) };
        int g = p->l1[(size_t)(sb[0] - lo[0]) + (size_t)p->l1_dim[0] * ((size_t)(sb[1] - lo[1]) + (size_t)p->l1_dim[1] * (size_t)(sb[2] - lo[2]))] - 1;
        int gx = g % L3_PX, gy = g / L3_PX % L3_PY, gz = g / (L3_PX * L3_PY);
        int tx = gx * L3_SB + (br->x - sb[0] * L3_SB), ty = gy * L3_SB + (br->y - sb[1] * L3_SB), tz = gz * L3_SB + (br->z - sb[2] * L3_SB);
        p->l2[(size_t)tx + (size_t)w2 * ((size_t)ty + (size_t)h2 * (size_t)tz)] = (uint16_t)(i + 1);
        int ax = i % L3_AX, ay = i / L3_AX % L3_AY, az = i / (L3_AX * L3_AY);
        for (int k = 0; k < L3_B * L3_B * L3_B; k++) {
            int lx = k % L3_B, ly = k / L3_B % L3_B, lz = k / (L3_B * L3_B);
            uint8_t *t = p->atlas + 4 * ((size_t)(ax * L3_B + lx) + (size_t)wa * ((size_t)(ay * L3_B + ly) + (size_t)ha * (size_t)(az * L3_B + lz)));
            t[0] = br->sdf[k][2]; t[1] = br->sdf[k][1]; t[2] = br->sdf[k][0]; t[3] = br->sdf[k][3];   /* B G R A */
        }
        for (int s = 0; s < L3_LAMPS; s++) p->lamps[4 * (size_t)i + (size_t)s] = (float)br->lamp[s];
    }
    *out = p;
    return AMB_OK;
}
int l3_pack_lookup(const L3Pack *p, int x, int y, int z, uint8_t sdf[L3_LAMPS], int lamp[L3_LAMPS]) {
    int bx = floordiv(x, L3_B), by = floordiv(y, L3_B), bz = floordiv(z, L3_B);
    int sx = floordiv(bx, L3_SB) - p->sb_min[0], sy = floordiv(by, L3_SB) - p->sb_min[1], sz = floordiv(bz, L3_SB) - p->sb_min[2];
    if (sx < 0 || sy < 0 || sz < 0 || sx >= p->l1_dim[0] || sy >= p->l1_dim[1] || sz >= p->l1_dim[2]) return 0;
    int g = p->l1[(size_t)sx + (size_t)p->l1_dim[0] * ((size_t)sy + (size_t)p->l1_dim[1] * (size_t)sz)];
    if (!g) return 0;
    g--;
    int w2 = L3_PX * L3_SB, h2 = L3_PY * L3_SB;
    int tx = g % L3_PX * L3_SB + (bx - floordiv(bx, L3_SB) * L3_SB), ty = g / L3_PX % L3_PY * L3_SB + (by - floordiv(by, L3_SB) * L3_SB),
        tz = g / (L3_PX * L3_PY) * L3_SB + (bz - floordiv(bz, L3_SB) * L3_SB);
    int i = p->l2[(size_t)tx + (size_t)w2 * ((size_t)ty + (size_t)h2 * (size_t)tz)];
    if (!i) return 0;
    i--;
    int wa = L3_AX * L3_B, ha = L3_AY * L3_B;
    int ax = i % L3_AX * L3_B + (x - bx * L3_B), ay = i / L3_AX % L3_AY * L3_B + (y - by * L3_B), az = i / (L3_AX * L3_AY) * L3_B + (z - bz * L3_B);
    const uint8_t *t = p->atlas + 4 * ((size_t)ax + (size_t)wa * ((size_t)ay + (size_t)ha * (size_t)az));
    sdf[0] = t[2]; sdf[1] = t[1]; sdf[2] = t[0]; sdf[3] = t[3];
    for (int s = 0; s < L3_LAMPS; s++) lamp[s] = (int)p->lamps[4 * (size_t)i + (size_t)s];
    return 1;
}

/* --- the cache file: "PL3C", version 1, then the header ints, voxel, and the four arrays ------------------------------------------- */
typedef struct { char magic[4]; int32_t version, sb_min[3], l1_dim[3], npages, pz, nbricks, az, lamp_rows; double voxel; } L3FileHdr;
static size_t sz_l1(const L3Pack *p) { return (size_t)p->l1_dim[0] * p->l1_dim[1] * p->l1_dim[2] * 2; }
static size_t sz_l2(const L3Pack *p) { return (size_t)L3_PX * L3_SB * L3_PY * L3_SB * (size_t)p->pz * L3_SB * 2; }
static size_t sz_at(const L3Pack *p) { return (size_t)L3_AX * L3_B * L3_AY * L3_B * (size_t)p->az * L3_B * 4; }
static size_t sz_lm(const L3Pack *p) { return (size_t)L3_LW * (size_t)p->lamp_rows * 16; }
int l3_pack_save(const L3Pack *p, uint8_t **buf, size_t *len) {
    L3FileHdr h = { { 'P', 'L', '3', 'C' }, 1, { p->sb_min[0], p->sb_min[1], p->sb_min[2] }, { p->l1_dim[0], p->l1_dim[1], p->l1_dim[2] }, p->npages, p->pz,
                    p->nbricks, p->az, p->lamp_rows, p->voxel };
    size_t n = sizeof h + sz_l1(p) + sz_l2(p) + sz_at(p) + sz_lm(p);
    uint8_t *b = malloc(n), *w = b;
    if (!b) return AMB_ERR_NOMEM;
    memcpy(w, &h, sizeof h); w += sizeof h;
    memcpy(w, p->l1, sz_l1(p)); w += sz_l1(p);
    memcpy(w, p->l2, sz_l2(p)); w += sz_l2(p);
    memcpy(w, p->atlas, sz_at(p)); w += sz_at(p);
    memcpy(w, p->lamps, sz_lm(p));
    *buf = b; *len = n;
    return AMB_OK;
}
int l3_pack_load(const uint8_t *buf, size_t len, L3Pack **out) {
    *out = NULL;
    L3FileHdr h;
    if (len < sizeof h) return AMB_ERR_FORMAT;
    memcpy(&h, buf, sizeof h);
    if (memcmp(h.magic, "PL3C", 4) != 0 || h.version != 1) return AMB_ERR_FORMAT;
    for (int k = 0; k < 3; k++) if (h.l1_dim[k] <= 0 || h.l1_dim[k] > 4096) return AMB_ERR_FORMAT;
    if (h.npages <= 0 || h.npages > 65534 || h.nbricks <= 0 || h.nbricks > 65534 || h.pz != (h.npages + L3_PX * L3_PY - 1) / (L3_PX * L3_PY) ||
        h.az != (h.nbricks + L3_AX * L3_AY - 1) / (L3_AX * L3_AY) || h.lamp_rows != (h.nbricks + L3_LW - 1) / L3_LW || !(h.voxel >= 1))
        return AMB_ERR_FORMAT;
    L3Pack *p = calloc(1, sizeof *p);
    if (!p) return AMB_ERR_NOMEM;
    memcpy(p->sb_min, h.sb_min, sizeof p->sb_min); memcpy(p->l1_dim, h.l1_dim, sizeof p->l1_dim);
    p->npages = h.npages; p->pz = h.pz; p->nbricks = h.nbricks; p->az = h.az; p->lamp_rows = h.lamp_rows; p->voxel = h.voxel;
    if (len != sizeof h + sz_l1(p) + sz_l2(p) + sz_at(p) + sz_lm(p)) { free(p); return AMB_ERR_FORMAT; }
    p->l1 = malloc(sz_l1(p)); p->l2 = malloc(sz_l2(p)); p->atlas = malloc(sz_at(p)); p->lamps = malloc(sz_lm(p));
    if (!p->l1 || !p->l2 || !p->atlas || !p->lamps) { l3_pack_free(p); return AMB_ERR_NOMEM; }
    const uint8_t *r = buf + sizeof h;
    memcpy(p->l1, r, sz_l1(p)); r += sz_l1(p);
    memcpy(p->l2, r, sz_l2(p)); r += sz_l2(p);
    memcpy(p->atlas, r, sz_at(p)); r += sz_at(p);
    memcpy(p->lamps, r, sz_lm(p));
    *out = p;
    return AMB_OK;
}
