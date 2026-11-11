/* P6 sun visibility: see sun_vis.h and analysis/22_p6_sun.md. The lump reader follows lg_map.c's (the same checks, the L4D2 header rule,
 * chunked reads through an AmbReader), but frees each lump as soon as it is parsed, so the peak held is one lump. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sun_vis.h"

#ifndef SUN_VIS_MUTANT
#define SUN_VIS_MUTANT 0   /* test only (scripts/run_sun_test.sh): each number removes one rule, tests/ambient/suntest.c must catch it */
#endif

enum { HEADER_BYTES = 8 + 64 * 16,
       L_PLANES = 1, L_TEXINFO = 6, L_VERTS = 3, L_NODES = 5, L_FACES = 7, L_LEAVES = 10, L_EDGES = 12, L_SURFEDGES = 13, L_MODELS = 14,
       L_LIGHTS = 15, L_LEAFBRUSHES = 17, L_BRUSHES = 18, L_SIDES = 19, L_DISPINFO = 26, L_DISPVERTS = 33, L_LIGHTS_HDR = 54, L_FACES_HDR = 58,
       /* caps (public SDK bspfile.h MAX_MAP_*, rounded up) */
       MAX_PLANES = 65536, MAX_NODES = 65536, MAX_LEAVES = 65536, MAX_TEXINFO = 65536, MAX_LEAFBRUSHES = 65536, MAX_BRUSHES = 65536,
       MAX_SIDES = 262144, MAX_FACES = 262144, MAX_EDGES = 1 << 20, MAX_SURFEDGES = 1 << 21, MAX_VERTS = 1 << 20, MAX_DISPS = 8192,
       MAX_DISPVERTS = 8192 * 289, MAX_LIGHTS = 8192, MAX_LEAFDISPS = 1 << 22,
       MIN_POWER = 2, MAX_POWER = 4, CHUNK_QUADS = 4,
       SURF_NORAY_COLL = 0x8 };

typedef struct { double n[3], d; } Plane;
typedef struct { int plane, child[2]; } Node;
typedef struct { int contents, flags, firstbrush, nbrushes, firstdisp, ndisps; } Leaf;
typedef struct { int firstside, nsides, contents, isbox; } Brush;
typedef struct { int plane, texinfo, bevel; } Side;
typedef struct {
    int power, w;                 /* w = 2^power + 1 vertices per row */
    int contents, flags, texinfo;  /* texinfo: the base face's */
    float mins[3], maxs[3];
    float (*v)[3];                /* w * w vertices, row i (from corner 0 toward corner 1) has w vertices toward the edge 3-2 */
    int nch;                      /* chunks per side, (w - 1) / CHUNK_QUADS rounded up */
    float (*cbox)[6];             /* per chunk: mins, maxs */
} Disp;

struct SunMap {
    SunInfo info;
    Plane *planes;
    Node *nodes;
    Leaf *leaves;
    uint16_t *leafbrushes;
    int nleafbrushes;
    Brush *brushes;
    Side *sides;
    int *texflags;
    Disp *disps;
    int *leafdisps;
    int head;
};

struct SunScratch { const SunMap *m; uint32_t stamp; uint32_t *bmark, *dmark; };

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* lump reading                                                                                                                         */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static int32_t rd_s32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}
static float rd_f32(const uint8_t *p) { uint32_t u = (uint32_t)rd_s32(p); float f; memcpy(&f, &u, 4); return f; }

typedef struct {
    const uint8_t *mem;
    size_t len;
    const AmbReader *r;
    uint8_t head[HEADER_BYTES];
    long long held;
    AmbReadStats st;
} Src;

typedef struct { const uint8_t *p; uint8_t *own; size_t ofs, len; int version; } Lump;

/* the same intended race as AmbReader.cancel elsewhere (tests/ambient/worktest.supp matches the name) */
static int reader_cancelled(const AmbReader *r) { return r->cancel && *r->cancel; }

static int src_read(Src *s, size_t ofs, void *dst, size_t n) {
    uint8_t *d = dst;
    while (n) {
        if (reader_cancelled(s->r)) return AMB_ERR_CANCELLED;
        int k = n < AMB_READ_CHUNK ? (int)n : AMB_READ_CHUNK;
        int got = s->r->read(s->r->ctx, (long long)ofs, d, k);
        s->st.reads++;
        if (got != k) return AMB_ERR_READ;
        s->st.bytes_read += k;
        ofs += (size_t)k;
        d += k;
        n -= (size_t)k;
    }
    return AMB_OK;
}

static int locate(Src *s, int idx, Lump *out) {
    memset(out, 0, sizeof *out);
    const uint8_t *h = s->mem + 8 + 16 * idx;
    int l4d2 = rd_s32(s->mem + 8 + 8) > 1000;   /* the engine's rule, amb_bake.c */
    int32_t ofs = rd_s32(l4d2 ? h + 4 : h), n = rd_s32(l4d2 ? h + 8 : h + 4);
    out->version = rd_s32(l4d2 ? h : h + 8);
    if (ofs < 0 || n < 0 || (size_t)ofs > s->len || (size_t)n > s->len - (size_t)ofs) return AMB_ERR_FORMAT;
    out->ofs = (size_t)ofs;
    out->len = (size_t)n;
    out->p = s->r ? NULL : s->mem + ofs;
    uint8_t tag[4];
    const uint8_t *t = out->p;
    if (n >= 4 && s->r) {
        int rc = src_read(s, (size_t)ofs, tag, 4);
        if (rc) return rc;
        t = tag;
    }
    if (n >= 4 && memcmp(t, "LZMA", 4) == 0) return AMB_ERR_LZMA;
    return AMB_OK;
}

/* the first n bytes of the lump into memory (a reader load allocates; drop() frees) */
static int fetch(Src *s, Lump *l, size_t n) {
    if (!s->r) return AMB_OK;
    uint8_t *b = malloc(n ? n : 1);
    if (!b) return AMB_ERR_NOMEM;
    l->own = b;
    l->p = b;
    s->held += (long long)n;
    if (s->held > s->st.peak_bytes) s->st.peak_bytes = s->held;
    return src_read(s, l->ofs, b, n);
}

static void drop(Src *s, Lump *l) {
    if (l->own) { s->held -= (long long)l->len; free(l->own); }
    l->own = NULL;
    l->p = NULL;
}

/* locate + size check + fetch for a lump of fixed-size records: *n records, at most cap; the lump's length must be a multiple of rs */
static int get_records(Src *s, int idx, size_t rs, size_t cap, Lump *l, size_t *n) {
    int rc = locate(s, idx, l);
    if (rc) return rc;
    if (l->len % rs || l->len / rs > cap) return AMB_ERR_FORMAT;
    *n = l->len / rs;
    if (s->r && l->len > (size_t)AMB_READ_MAX * 4) return AMB_ERR_FORMAT;
    return fetch(s, l, l->len);
}

void sun_map_free(SunMap *m) {
    if (!m) return;
    free(m->planes);
    free(m->nodes);
    free(m->leaves);
    free(m->leafbrushes);
    free(m->brushes);
    free(m->sides);
    free(m->texflags);
    if (m->disps) for (int i = 0; i < m->info.ndisps; i++) { free(m->disps[i].v); free(m->disps[i].cbox); }
    free(m->disps);
    free(m->leafdisps);
    free(m);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* displacements                                                                                                                        */

/* Every leaf of model 0's tree that a box touches (with the node test d >= 0 front, as amb_point_leaf); count or fill. Iterative. */
static int box_leaves(const SunMap *m, const float mn[3], const float mx[3], int disp, int *count, int **cursor) {
    int stack[512], sp = 0, steps = 0;
    stack[sp++] = m->head;
    while (sp) {
        int node = stack[--sp];
        if (++steps > 4 * (m->info.nnodes + m->info.nleaves) + 16) return AMB_ERR_FORMAT;   /* a cycle in a hostile tree */
        if (node < 0) {
            int leaf = -1 - node;
            if (count) count[leaf]++;
            else { int *c = &cursor[0][leaf]; m->leafdisps[*c] = disp; (*c)++; }
            continue;
        }
        const Node *nd = &m->nodes[node];
        const Plane *pl = &m->planes[nd->plane];
        /* the box's extreme distances from the plane */
        double dmax = -pl->d, dmin = -pl->d;
        for (int k = 0; k < 3; k++) {
            double a = pl->n[k] * mn[k], b = pl->n[k] * mx[k];
            dmax += a > b ? a : b;
            dmin += a < b ? a : b;
        }
        if (sp + 2 > (int)(sizeof stack / sizeof stack[0])) return AMB_ERR_FORMAT;
        if (dmax >= 0) stack[sp++] = nd->child[0];
        if (dmin < 0) stack[sp++] = nd->child[1];
    }
    return AMB_OK;
}

/* A displacement's vertices, the public SDK's CCoreDispInfo::GenerateDispSurf in float (no elevation, no subdivision): corner 0 is the
 * face corner nearest startPosition (FindSurfPointStartIndex, first minimum), the rows go from corner 0 toward corner 1, a row's
 * vertices from the edge 0-1 toward the edge 3-2, each displaced by its dispvert's vector times its distance. */
static void disp_build_verts(Disp *d, const float corner[4][3], const float start[3], const uint8_t *dv) {
    int mi = 0;
    float best = 999999999.0f;
    for (int i = 0; i < 4; i++) {
        float e[3] = { start[0] - corner[i][0], start[1] - corner[i][1], start[2] - corner[i][2] };
        float l = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
        if (l < best) { best = l; mi = i; }
    }
    if (SUN_VIS_MUTANT == 11) mi = 0;   /* the corner nearest startPosition not used */
    float p[4][3];
    for (int i = 0; i < 4; i++) memcpy(p[i], corner[(i + mi) % 4], sizeof p[i]);
    int w = d->w;
    float oo = 1.0f / (float)(w - 1);
    float e0[3], e1[3];
    for (int k = 0; k < 3; k++) { e0[k] = (p[1][k] - p[0][k]) * oo; e1[k] = (p[2][k] - p[3][k]) * oo; }
    for (int i = 0; i < w; i++) {
        float a[3], b[3], seg[3];
        for (int k = 0; k < 3; k++) {
            a[k] = e0[k] * (float)i + p[0][k];
            b[k] = e1[k] * (float)i + p[3][k];
            seg[k] = (b[k] - a[k]) * oo;
        }
        for (int j = 0; j < w; j++) {
            const uint8_t *r = dv + 20 * (size_t)(i * w + j);
            float dist = rd_f32(r + 12);
            for (int k = 0; k < 3; k++) d->v[i * w + j][k] = a[k] + seg[k] * (float)j + rd_f32(r + 4 * k) * dist;
        }
    }
    for (int k = 0; k < 3; k++) { d->mins[k] = 1e30f; d->maxs[k] = -1e30f; }
    d->nch = (w - 1 + CHUNK_QUADS - 1) / CHUNK_QUADS;
    for (int c = 0; c < d->nch * d->nch; c++) for (int k = 0; k < 3; k++) { d->cbox[c][k] = 1e30f; d->cbox[c][3 + k] = -1e30f; }
    for (int i = 0; i < w; i++)
        for (int j = 0; j < w; j++) {
            const float *v = d->v[i * w + j];
            /* a vertex on a chunk border belongs to both chunks */
            for (int ci = (i ? (i - 1) / CHUNK_QUADS : 0); ci <= (i < w - 1 ? i / CHUNK_QUADS : d->nch - 1) && ci < d->nch; ci++)
                for (int cj = (j ? (j - 1) / CHUNK_QUADS : 0); cj <= (j < w - 1 ? j / CHUNK_QUADS : d->nch - 1) && cj < d->nch; cj++) {
                    float *b = d->cbox[ci * d->nch + cj];
                    for (int k = 0; k < 3; k++) { if (v[k] < b[k]) b[k] = v[k]; if (v[k] > b[3 + k]) b[3 + k] = v[k]; }
                }
            for (int k = 0; k < 3; k++) { if (v[k] < d->mins[k]) d->mins[k] = v[k]; if (v[k] > d->maxs[k]) d->maxs[k] = v[k]; }
        }
}

static int load_disps(Src *s, SunMap *m) {
    Lump ld, lv, lf, le, lse, lvt;
    size_t nd, nv, nf, ne, nse, nvt;
    int rc = get_records(s, L_DISPINFO, 176, MAX_DISPS, &ld, &nd);
    if (rc) return rc;
    if (nd == 0) { drop(s, &ld); return AMB_OK; }
    m->disps = calloc(nd, sizeof *m->disps);
    if (!m->disps) { drop(s, &ld); return AMB_ERR_NOMEM; }
    m->info.ndisps = (int)nd;
    memset(&lv, 0, sizeof lv); memset(&lf, 0, sizeof lf); memset(&le, 0, sizeof le); memset(&lse, 0, sizeof lse); memset(&lvt, 0, sizeof lvt);
    int fidx = L_FACES;
    if ((rc = locate(s, L_FACES, &lf))) goto out;
    if (lf.len == 0) fidx = L_FACES_HDR;
    if ((rc = get_records(s, fidx, 56, MAX_FACES, &lf, &nf))) goto out;
    if ((rc = get_records(s, L_EDGES, 4, MAX_EDGES, &le, &ne))) goto out;
    if ((rc = get_records(s, L_SURFEDGES, 4, MAX_SURFEDGES, &lse, &nse))) goto out;
    if ((rc = get_records(s, L_VERTS, 12, MAX_VERTS, &lvt, &nvt))) goto out;
    if ((rc = get_records(s, L_DISPVERTS, 20, MAX_DISPVERTS, &lv, &nv))) goto out;
    for (size_t i = 0; i < nd; i++) {
        const uint8_t *r = ld.p + 176 * i;
        Disp *d = &m->disps[i];
        int vstart = rd_s32(r + 12), power = rd_s32(r + 20), mintess = rd_s32(r + 24), face = rd_u16(r + 36);
        d->contents = rd_s32(r + 32);
        d->texinfo = -1;
        d->flags = (mintess & (int)0x80000000) ? (mintess & 0x7fffffff) : 0;
        rc = AMB_ERR_FORMAT;
        if (power < MIN_POWER || power > MAX_POWER) goto out;
        d->power = power;
        d->w = (1 << power) + 1;
        size_t nverts = (size_t)d->w * (size_t)d->w;
        if (vstart < 0 || (size_t)vstart > nv || nverts > nv - (size_t)vstart || (size_t)face >= nf) goto out;
        const uint8_t *f = lf.p + 56 * (size_t)face;
        int firstedge = rd_s32(f + 4), numedges = rd_s16(f + 8);
        d->texinfo = rd_s16(f + 10);
        if (numedges != 4 || firstedge < 0 || (size_t)firstedge > nse || 4 > nse - (size_t)firstedge) goto out;
        float corner[4][3], start[3];
        for (int c = 0; c < 4; c++) {
            int se = rd_s32(lse.p + 4 * ((size_t)firstedge + (size_t)c));
            long long ei = se >= 0 ? se : -(long long)se;
            if ((size_t)ei >= ne) goto out;
            int vi = rd_u16(le.p + 4 * (size_t)ei + (se >= 0 ? 0 : 2));
            if ((size_t)vi >= nvt) goto out;
            for (int k = 0; k < 3; k++) corner[c][k] = rd_f32(lvt.p + 12 * (size_t)vi + 4 * (size_t)k);
        }
        for (int k = 0; k < 3; k++) start[k] = rd_f32(r + 4 * k);
        rc = AMB_ERR_NOMEM;
        d->v = malloc(nverts * sizeof *d->v);
        int nch = (d->w - 1 + CHUNK_QUADS - 1) / CHUNK_QUADS;
        d->cbox = malloc((size_t)(nch * nch) * sizeof *d->cbox);
        if (!d->v || !d->cbox) goto out;
        disp_build_verts(d, (const float (*)[3])corner, start, lv.p + 20 * (size_t)vstart);
        m->info.ndisp_tris += 2 * (d->w - 1) * (d->w - 1);
    }
    /* leaf lists: every leaf a displacement's box (grown by 1 unit) touches */
    {
        int nl = m->info.nleaves;
        int *count = calloc((size_t)nl, sizeof *count), *cur = NULL;
        rc = AMB_ERR_NOMEM;
        if (!count) goto out;
        for (size_t i = 0; i < nd && rc != AMB_ERR_FORMAT; i++) {
            float mn[3], mx[3];
            for (int k = 0; k < 3; k++) { mn[k] = m->disps[i].mins[k] - 1; mx[k] = m->disps[i].maxs[k] + 1; }
            if (box_leaves(m, mn, mx, (int)i, count, NULL)) rc = AMB_ERR_FORMAT;
        }
        long long total = 0;
        for (int l = 0; l < nl; l++) total += count[l];
        if (rc == AMB_ERR_FORMAT || total > MAX_LEAFDISPS) { free(count); rc = AMB_ERR_FORMAT; goto out; }
        m->leafdisps = malloc((size_t)(total ? total : 1) * sizeof *m->leafdisps);
        cur = malloc((size_t)nl * sizeof *cur);
        if (!m->leafdisps || !cur) { free(count); free(cur); rc = AMB_ERR_NOMEM; goto out; }
        int at = 0;
        for (int l = 0; l < nl; l++) { m->leaves[l].firstdisp = at; m->leaves[l].ndisps = count[l]; cur[l] = at; at += count[l]; }
        rc = AMB_OK;
        for (size_t i = 0; i < nd && rc == AMB_OK; i++) {
            float mn[3], mx[3];
            for (int k = 0; k < 3; k++) { mn[k] = m->disps[i].mins[k] - 1; mx[k] = m->disps[i].maxs[k] + 1; }
            rc = box_leaves(m, mn, mx, (int)i, NULL, &cur);
        }
        free(count);
        free(cur);
    }
out:
    drop(s, &ld); drop(s, &lv); drop(s, &lf); drop(s, &le); drop(s, &lse); drop(s, &lvt);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* the parse                                                                                                                            */

static int load_lights(Src *s, SunMap *m, int prefer_hdr) {
    Lump lw;
    int which = 0;
    if (prefer_hdr && SUN_VIS_MUTANT != 10) {
        int r2 = locate(s, L_LIGHTS_HDR, &lw);
        if (r2 == AMB_ERR_CANCELLED || r2 == AMB_ERR_READ) return r2;
        if (r2 == AMB_ERR_LZMA && lw.len) { m->info.lights_lzma = 1; which = -1; }
        else if (r2 == AMB_OK && lw.len) which = L_LIGHTS_HDR;
    }
    if (!which) {
        int r2 = locate(s, L_LIGHTS, &lw);
        if (r2 == AMB_ERR_CANCELLED || r2 == AMB_ERR_READ) return r2;
        if (r2 == AMB_ERR_LZMA && lw.len) m->info.lights_lzma = 1;
        else if (r2 == AMB_OK && lw.len) which = L_LIGHTS;
    }
    if (which <= 0) return AMB_OK;
    size_t rs = lw.version == 1 ? 100 : 88;
    if ((lw.version != 0 && lw.version != 1) || lw.len % rs || lw.len / rs > MAX_LIGHTS) return AMB_ERR_FORMAT;
    int rc = fetch(s, &lw, lw.len);
    if (rc) { drop(s, &lw); return rc; }
    size_t n = lw.len / rs;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = lw.p + rs * i;
        uint8_t rec[88];
        if (rs == 100) { memcpy(rec, p, 36); memcpy(rec + 36, p + 48, 52); }   /* drop shadow_cast_offset, as lg_map.c */
        else memcpy(rec, p, 88);
        if (rd_s32(rec + 40) != 3) continue;   /* emit_skylight */
        if (m->info.nskylights++) continue;
        for (int k = 0; k < 3; k++) {
            m->info.sun_intensity[k] = rd_f32(rec + 12 + 4 * k);
            m->info.sun_normal[k] = rd_f32(rec + 24 + 4 * k);
            m->info.sun_dir[k] = -m->info.sun_normal[k];
        }
        m->info.sun_style = rd_s32(rec + 44);
    }
    m->info.has_sun = m->info.nskylights > 0;
    m->info.nlights = (int)n;
    m->info.light_lump = which;
    m->info.light_lump_version = lw.version;
    drop(s, &lw);
    return AMB_OK;
}

static int parse(Src *s, int prefer_hdr, SunMap **out) {
    *out = NULL;
    if (s->len < HEADER_BYTES || memcmp(s->mem, "VBSP", 4) != 0) return AMB_ERR_FORMAT;
    SunMap *m = calloc(1, sizeof *m);
    if (!m) return AMB_ERR_NOMEM;
    m->info.bsp_version = rd_s32(s->mem + 4);
    Lump l;
    size_t n;
    int rc;
    memset(&l, 0, sizeof l);
#define GET(idx, rs, cap) do { if ((rc = get_records(s, idx, rs, cap, &l, &n))) goto fail; } while (0)
#define ALLOC(ptr, cnt) do { (ptr) = calloc((cnt) ? (cnt) : 1, sizeof *(ptr)); if (!(ptr)) { rc = AMB_ERR_NOMEM; goto fail; } } while (0)
    GET(L_PLANES, 20, MAX_PLANES);
    if (!n) { rc = AMB_ERR_FORMAT; goto fail; }
    ALLOC(m->planes, n);
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) m->planes[i].n[k] = rd_f32(l.p + 20 * i + 4 * k);
        m->planes[i].d = rd_f32(l.p + 20 * i + 12);
    }
    m->info.nplanes = (int)n;
    drop(s, &l);

    GET(L_TEXINFO, 72, MAX_TEXINFO);
    ALLOC(m->texflags, n);
    for (size_t i = 0; i < n; i++) {
        m->texflags[i] = rd_s32(l.p + 72 * i + 64);
        if (m->texflags[i] & SUN_SURF_SKY) m->info.nsky_texinfo++;
    }
    m->info.ntexinfo = (int)n;
    drop(s, &l);

    /* leaves before nodes, so a node's leaf children can be range-checked */
    if ((rc = locate(s, L_LEAVES, &l))) goto fail;
    {
        size_t rs = l.version == 0 ? 56 : 32;
        rc = AMB_ERR_FORMAT;
        if ((l.version != 0 && l.version != 1) || l.len % rs || !l.len || l.len / rs > MAX_LEAVES) goto fail;
        m->info.leaf_version = l.version;
        n = l.len / rs;
        if ((rc = fetch(s, &l, l.len))) goto fail;
        ALLOC(m->leaves, n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = l.p + rs * i;
            m->leaves[i].contents = rd_s32(p);
            m->leaves[i].flags = (rd_u16(p + 6) >> 9) & 0x7f;   /* area:9, flags:7 */
            m->leaves[i].firstbrush = rd_u16(p + 24);
            m->leaves[i].nbrushes = rd_u16(p + 26);
        }
        m->info.nleaves = (int)n;
        drop(s, &l);
    }

    GET(L_NODES, 32, MAX_NODES);
    ALLOC(m->nodes, n);
    rc = AMB_ERR_FORMAT;
    for (size_t i = 0; i < n; i++) {
        Node *nd = &m->nodes[i];
        nd->plane = rd_s32(l.p + 32 * i);
        nd->child[0] = rd_s32(l.p + 32 * i + 4);
        nd->child[1] = rd_s32(l.p + 32 * i + 8);
        if (nd->plane < 0 || nd->plane >= m->info.nplanes) goto fail;
        for (int c = 0; c < 2; c++) {
            int ch = nd->child[c];
            if (ch >= 0 ? (size_t)ch >= n : -1 - (long long)ch >= m->info.nleaves) goto fail;
        }
    }
    m->info.nnodes = (int)n;
    drop(s, &l);

    if ((rc = locate(s, L_MODELS, &l))) goto fail;
    rc = AMB_ERR_FORMAT;
    if (l.len < 48) goto fail;
    if ((rc = fetch(s, &l, 48))) goto fail;
    l.len = 48;
    for (int k = 0; k < 3; k++) { m->info.world_min[k] = rd_f32(l.p + 4 * k); m->info.world_max[k] = rd_f32(l.p + 12 + 4 * k); }
    m->head = rd_s32(l.p + 36);
    drop(s, &l);
    rc = AMB_ERR_FORMAT;
    if (m->head >= 0 ? (m->head >= m->info.nnodes) : (-1 - (long long)m->head >= m->info.nleaves)) goto fail;

    GET(L_SIDES, 8, MAX_SIDES);
    ALLOC(m->sides, n);
    rc = AMB_ERR_FORMAT;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = l.p + 8 * i;
        m->sides[i].plane = rd_u16(p);
        m->sides[i].texinfo = rd_s16(p + 2);
        m->sides[i].bevel = p[6];   /* the low byte: "bevel" (a later "thin" byte shares the old short) */
        if (m->sides[i].plane >= m->info.nplanes) goto fail;
    }
    m->info.nsides = (int)n;
    drop(s, &l);

    GET(L_BRUSHES, 12, MAX_BRUSHES);
    ALLOC(m->brushes, n);
    rc = AMB_ERR_FORMAT;
    for (size_t i = 0; i < n; i++) {
        Brush *b = &m->brushes[i];
        b->firstside = rd_s32(l.p + 12 * i);
        b->nsides = rd_s32(l.p + 12 * i + 4);
        b->contents = rd_s32(l.p + 12 * i + 8);
        if (b->firstside < 0 || b->nsides < 0 || b->firstside > m->info.nsides || b->nsides > m->info.nsides - b->firstside) goto fail;
        /* the engine keeps a brush of 6 axial sides, one per direction, as a box (its own clip, FUN_18017e890) */
        int dirs = 0;
        for (int k = 0; k < b->nsides && b->nsides == 6; k++) {
            const Plane *pl = &m->planes[m->sides[b->firstside + k].plane];
            for (int a = 0; a < 3; a++)
                if (fabs(pl->n[a]) == 1 && pl->n[(a + 1) % 3] == 0 && pl->n[(a + 2) % 3] == 0) dirs |= 1 << (2 * a + (pl->n[a] < 0));
        }
        b->isbox = dirs == 0x3f;
    }
    m->info.nbrushes = (int)n;
    drop(s, &l);

    GET(L_LEAFBRUSHES, 2, MAX_LEAFBRUSHES);
    ALLOC(m->leafbrushes, n);
    rc = AMB_ERR_FORMAT;
    for (size_t i = 0; i < n; i++) {
        m->leafbrushes[i] = rd_u16(l.p + 2 * i);
        if (m->leafbrushes[i] >= m->info.nbrushes) goto fail;
    }
    m->nleafbrushes = (int)n;
    drop(s, &l);
    for (int i = 0; i < m->info.nleaves; i++)
        if (m->leaves[i].firstbrush + m->leaves[i].nbrushes > m->nleafbrushes) goto fail;

    if ((rc = load_disps(s, m))) goto fail;
    if ((rc = load_lights(s, m, prefer_hdr))) goto fail;
#undef GET
#undef ALLOC
    *out = m;
    return AMB_OK;
fail:
    drop(s, &l);
    sun_map_free(m);
    return rc;
}

int sun_map_load(const void *bsp, size_t len, int prefer_hdr, SunMap **out) {
    if (!out) return AMB_ERR_ARG;
    *out = NULL;
    if (!bsp) return AMB_ERR_ARG;
    Src s;
    memset(&s, 0, sizeof s);
    s.mem = bsp;
    s.len = len;
    return parse(&s, prefer_hdr, out);
}

int sun_map_load_reader(const AmbReader *r, int prefer_hdr, SunMap **out, AmbReadStats *stats) {
    if (!out) return AMB_ERR_ARG;
    *out = NULL;
    if (!r || !r->read || r->size < 0) return AMB_ERR_ARG;
    Src s;
    memset(&s, 0, sizeof s);
    s.r = r;
    s.len = (size_t)r->size;
    int rc = AMB_ERR_FORMAT;
    if (s.len >= HEADER_BYTES && (rc = src_read(&s, 0, s.head, HEADER_BYTES)) == AMB_OK) {
        s.mem = s.head;
        rc = parse(&s, prefer_hdr, out);
    }
    if (stats) *stats = s.st;
    return rc;
}

void sun_map_info(const SunMap *m, SunInfo *info) { *info = m->info; }

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* scratch                                                                                                                              */

SunScratch *sun_scratch_new(const SunMap *m) {
    SunScratch *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m;
    s->bmark = calloc((size_t)(m->info.nbrushes ? m->info.nbrushes : 1), sizeof *s->bmark);
    s->dmark = calloc((size_t)(m->info.ndisps ? m->info.ndisps : 1), sizeof *s->dmark);
    if (!s->bmark || !s->dmark) { sun_scratch_free(s); return NULL; }
    return s;
}

void sun_scratch_free(SunScratch *s) {
    if (!s) return;
    free(s->bmark);
    free(s->dmark);
    free(s);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* the trace                                                                                                                            */

typedef struct {
    const SunMap *m;
    SunScratch *s;
    double a[3], b[3], delta[3];
    int mask;
    SunTrace *tr;
    double leftsolid;   /* fractionleftsolid: the largest exit of the brushes the trace started in (as the engine's clip computes it) */
} Ctx;

/* A brush against the whole ray (Quake 2's CM_ClipBoxToBrush for a point, which the engine's world trace keeps; bevel sides skipped for rays) */
static void clip_brush(Ctx *c, int bi) {
    const SunMap *m = c->m;
    const Brush *br = &m->brushes[bi];
    if (!br->nsides) return;
    double enter = -1, leave = 1, leave_box = 1;
    int startout = 0, getout = 0, lead = -1;
    for (int i = 0; i < br->nsides; i++) {
        const Side *sd = &m->sides[br->firstside + i];
        if (sd->bevel) continue;
        const Plane *pl = &m->planes[sd->plane];
        double d1 = pl->n[0] * c->a[0] + pl->n[1] * c->a[1] + pl->n[2] * c->a[2] - pl->d;
        double d2 = pl->n[0] * c->b[0] + pl->n[1] * c->b[1] + pl->n[2] * c->b[2] - pl->d;
        if (d2 > 0) getout = 1;
        if (d1 > 0) startout = 1;
        if (d1 > 0 && (d2 >= SUN_DIST_EPSILON || d2 >= d1)) return;   /* in front of this side all the way: misses the brush */
        if (d1 <= 0 && d2 <= 0) continue;
        if (d1 > d2) {   /* entering */
            double f = (d1 - SUN_DIST_EPSILON) / (d1 - d2);
            if (f < 0) f = 0;
            if (f > enter) { enter = f; lead = br->firstside + i; }
        } else {         /* leaving */
            double f = (d1 + SUN_DIST_EPSILON) / (d1 - d2), fb = (d1 - SUN_DIST_EPSILON) / (d1 - d2);
            if (f > 1) f = 1;
            if (f < leave) leave = f;
            if (fb < leave_box) leave_box = fb;   /* a box is grown by DIST_EPSILON: left just past its side */
        }
    }
    SunTrace *tr = c->tr;
    /* The start inside this brush, or (once the trace has started in solid) a brush entered before the start's solid is left: part of the
     * start's solid, not a hit; it moves fractionleftsolid to its own exit, and one that is never left stops the trace with no surface.
     * The engine's box clip (FUN_18017e890: startsolid, or the flag and enter < fractionleftsolid) [confirmed decomp]; the same rule for
     * other brushes, and the order the brushes are met in, fit every start-in-solid point measured but a few (analysis/22 section 3). */
    double exitf = br->isbox ? leave_box : leave;
    int absorb = !startout || (tr->startsolid && enter < leave && enter < c->leftsolid && SUN_VIS_MUTANT != 15);
    if (absorb) {
        tr->startsolid = 1;
        if (SUN_VIS_MUTANT == 4 && !startout) { tr->fraction = 0; tr->hit = SUN_HIT_BRUSH; tr->brush = bi; tr->surf_flags = 0; return; }
        if (br->isbox ? exitf >= 1 : (!startout && !getout)) {
            tr->allsolid = 1; tr->fraction = 0; tr->hit = SUN_HIT_NONE; tr->brush = bi; tr->side = -1; tr->texinfo = -1; tr->surf_flags = 0;
        } else if (exitf > c->leftsolid) c->leftsolid = exitf;
        return;
    }
    if (enter < leave && enter > -1 && enter < tr->fraction) {
        if (SUN_VIS_MUTANT == 3) {   /* the hit's surface taken from the brush's first side */
            lead = br->firstside;
        }
        tr->fraction = enter < 0 ? 0 : enter;
        tr->hit = SUN_HIT_BRUSH;
        tr->brush = bi;
        tr->side = lead;
        tr->disp = -1;
        int ti = lead >= 0 ? m->sides[lead].texinfo : -1;
        tr->texinfo = ti;
        tr->surf_flags = ti >= 0 && ti < m->info.ntexinfo ? m->texflags[ti] : 0;
    }
}

/* The public SDK's IntersectRayWithTriangle for a ray (no extents): -1 for a miss, else t clamped to [0, 1]; one-sided culls a triangle
 * whose normal (v2 - v1) x (v3 - v1) does not face against the ray. */
static double ray_tri(const Ctx *c, const float *v1, const float *v2, const float *v3) {
    double e1[3], e2[3], org[3], dce2[3], oce1[3];
    for (int k = 0; k < 3; k++) { e1[k] = (double)v2[k] - v1[k]; e2[k] = (double)v3[k] - v1[k]; org[k] = c->a[k] - v1[k]; }
    const double *D = c->delta;
    if (SUN_VIS_MUTANT != 5) {
        double nrm[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        if (nrm[0] * D[0] + nrm[1] * D[1] + nrm[2] * D[2] >= 0) return -1;
    }
    dce2[0] = D[1] * e2[2] - D[2] * e2[1]; dce2[1] = D[2] * e2[0] - D[0] * e2[2]; dce2[2] = D[0] * e2[1] - D[1] * e2[0];
    double den = dce2[0] * e1[0] + dce2[1] * e1[1] + dce2[2] * e1[2];
    if (fabs(den) < 1e-6) return -1;
    den = 1.0 / den;
    double u = (dce2[0] * org[0] + dce2[1] * org[1] + dce2[2] * org[2]) * den;
    if (u < 0 || u > 1) return -1;
    oce1[0] = org[1] * e1[2] - org[2] * e1[1]; oce1[1] = org[2] * e1[0] - org[0] * e1[2]; oce1[2] = org[0] * e1[1] - org[1] * e1[0];
    double v = (oce1[0] * D[0] + oce1[1] * D[1] + oce1[2] * D[2]) * den;
    if (v < 0 || v + u > 1) return -1;
    double t = (oce1[0] * e2[0] + oce1[1] * e2[1] + oce1[2] * e2[2]) * den;
    const double boxt = 1e-3;
    if (t < -boxt || t > 1 + boxt) return -1;
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

/* the segment [0, tmax] of the ray against a box: 1 if it may touch it */
static int ray_box(const Ctx *c, const float *mn, const float *mx, double tmax) {
    double t0 = 0, t1 = tmax;
    for (int k = 0; k < 3; k++) {
        double lo = mn[k] - 0.5, hi = mx[k] + 0.5, o = c->a[k], d = c->delta[k];
        if (d == 0) { if (o < lo || o > hi) return 0; continue; }
        double inv = 1.0 / d, ta = (lo - o) * inv, tb = (hi - o) * inv;
        if (ta > tb) { double t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return 0;
    }
    return 1;
}

static void clip_disp(Ctx *c, int di) {
    const Disp *d = &c->m->disps[di];
    if (!(d->contents & c->mask) || !(d->contents & SUN_MASK_OPAQUE) || (d->flags & SURF_NORAY_COLL)) return;
    if (!ray_box(c, d->mins, d->maxs, c->tr->fraction)) return;
    int w = d->w, best_set = 0;
    double best = c->tr->fraction;
    const float *bt[3] = { 0, 0, 0 };
    for (int ci = 0; ci < d->nch; ci++)
        for (int cj = 0; cj < d->nch; cj++) {
            const float *b = d->cbox[ci * d->nch + cj];
            if (!ray_box(c, b, b + 3, best)) continue;
            int i1 = (ci + 1) * CHUNK_QUADS < w - 1 ? (ci + 1) * CHUNK_QUADS : w - 1;
            int j1 = (cj + 1) * CHUNK_QUADS < w - 1 ? (cj + 1) * CHUNK_QUADS : w - 1;
            for (int iv = ci * CHUNK_QUADS; iv < i1; iv++)
                for (int iu = cj * CHUNK_QUADS; iu < j1; iu++) {
                    int x = iv * w + iu, t[2][3];
                    /* BuildRayLeafList: the quad's box, grown by DISPCOLL_DIST_EPSILON, against the whole ray */
                    float qb[6];
                    const float *q4[4] = { d->v[x], d->v[x + 1], d->v[x + w], d->v[x + w + 1] };
                    for (int k = 0; k < 3; k++) {
                        float lo = q4[0][k], hi = q4[0][k];
                        for (int j = 1; j < 4; j++) { if (q4[j][k] < lo) lo = q4[j][k]; if (q4[j][k] > hi) hi = q4[j][k]; }
                        qb[k] = lo - 0.03125f + 0.5f;   /* ray_box grows by 0.5 */
                        qb[3 + k] = hi + 0.03125f - 0.5f;
                    }
                    if (SUN_VIS_MUTANT != 14 && !ray_box(c, qb, qb + 3, 1)) continue;
                    if ((x % 2 == 1) != (SUN_VIS_MUTANT == 12)) {   /* GenerateCollisionSurface: odd index, top left to bottom right */
                        t[0][0] = x; t[0][1] = x + w; t[0][2] = x + 1;
                        t[1][0] = x + 1; t[1][1] = x + w; t[1][2] = x + w + 1;
                    } else {            /* even: bottom left to top right */
                        t[0][0] = x; t[0][1] = x + w; t[0][2] = x + w + 1;
                        t[1][0] = x; t[1][1] = x + w + 1; t[1][2] = x + 1;
                    }
                    for (int k = 0; k < 2; k++) {
                        /* AABBTree_TreeTrisRayTest passes the vertices as 0, 2, 1 */
                        double f = ray_tri(c, d->v[t[k][0]], d->v[t[k][2]], d->v[t[k][1]]);
                        if (f >= 0 && f < best) { best = f; best_set = 1; bt[0] = d->v[t[k][0]]; bt[1] = d->v[t[k][2]]; bt[2] = d->v[t[k][1]]; }
                    }
                }
        }
    if (best_set) {
        SunTrace *tr = c->tr;
        tr->fraction = best;
        tr->hit = SUN_HIT_DISP;
        tr->disp = di;
        tr->brush = tr->side = tr->texinfo = -1;
        tr->surf_flags = 0;
        tr->disp_texinfo = d->texinfo;
        double e1[3], e2[3], n[3];
        for (int k = 0; k < 3; k++) { e1[k] = bt[1][k] - bt[0][k]; e2[k] = bt[2][k] - bt[0][k]; }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int k = 0; k < 3; k++) tr->disp_n[k] = l > 0 ? (float)(n[k] / l) : 0;
    }
}

static void trace_leaf(Ctx *c, int leaf) {
    const SunMap *m = c->m;
    const Leaf *lf = &m->leaves[leaf];
    SunScratch *s = c->s;
    c->tr->leaves++;
    /* No test of the leaf's own contents: vbsp adds func_detail brushes to the leaves they cross without splitting them, so an empty
     * (or fog, 0x100) leaf can list a solid detail brush (gm_construct has many), and the engine's world trace does stop at them. */
    if (SUN_VIS_MUTANT != 13 || (lf->contents & c->mask)) {
        for (int i = 0; i < lf->nbrushes; i++) {
            int bi = m->leafbrushes[lf->firstbrush + i];
            const Brush *br = &m->brushes[bi];
            if (!(br->contents & c->mask) && SUN_VIS_MUTANT != 2) continue;
            if (s) { if (s->bmark[bi] == s->stamp) continue; s->bmark[bi] = s->stamp; }
            clip_brush(c, bi);
            if (c->tr->fraction == 0) return;
        }
    }
    if (SUN_VIS_MUTANT == 1) return;   /* displacements ignored */
    for (int i = 0; i < lf->ndisps; i++) {
        int di = m->leafdisps[lf->firstdisp + i];
        if (s) { if (s->dmark[di] == s->stamp) continue; s->dmark[di] = s->stamp; }
        clip_disp(c, di);
    }
}

/* One pass of the tree walk from a to b. Returns where the brushes the start is inside of are left (exact, the largest) in *leftsolid. */
static void trace_once(const SunMap *m, SunScratch *s, const double a[3], const double b[3], int mask, SunTrace *tr, double *leftsolid) {
    memset(tr, 0, sizeof *tr);
    tr->fraction = 1;
    tr->brush = tr->side = tr->disp = tr->texinfo = -1;
    if (s && s->m != m) s = NULL;
    if (s && ++s->stamp == 0) {   /* the stamps wrapped: start the marks again */
        memset(s->bmark, 0, (size_t)(m->info.nbrushes ? m->info.nbrushes : 1) * sizeof *s->bmark);
        memset(s->dmark, 0, (size_t)(m->info.ndisps ? m->info.ndisps : 1) * sizeof *s->dmark);
        s->stamp = 1;
    }
    Ctx c = { m, s, { a[0], a[1], a[2] }, { b[0], b[1], b[2] }, { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, mask, tr, 0 };
    /* Quake 2's CM_RecursiveHullCheck for a point, iterative: a node the segment lies on one side of passes it to that child; a crossing
     * splits it at the plane, the near part extended and the far part started DIST_EPSILON past it, near part first */
    struct { int node; double f1, f2; } st[512];
    int sp = 0, steps = 0;
    st[sp].node = m->head; st[sp].f1 = 0; st[sp].f2 = 1; sp++;
    while (sp) {
        sp--;
        int node = st[sp].node;
        double f1 = st[sp].f1, f2 = st[sp].f2;
        if (tr->fraction <= f1) continue;   /* already hit something nearer */
        if (++steps > 4 * (m->info.nnodes + m->info.nleaves) + 16) break;   /* a cycle in a hostile tree */
        if (node < 0) { trace_leaf(&c, -1 - node); continue; }
        const Node *nd = &m->nodes[node];
        const Plane *pl = &m->planes[nd->plane];
        double p1[3], p2[3];
        for (int k = 0; k < 3; k++) { p1[k] = a[k] + f1 * c.delta[k]; p2[k] = a[k] + f2 * c.delta[k]; }
        double t1 = pl->n[0] * p1[0] + pl->n[1] * p1[1] + pl->n[2] * p1[2] - pl->d;
        double t2 = pl->n[0] * p2[0] + pl->n[1] * p2[1] + pl->n[2] * p2[2] - pl->d;
        if (sp + 2 > (int)(sizeof st / sizeof st[0])) break;
        if (t1 >= 0 && t2 >= 0) { st[sp].node = nd->child[0]; st[sp].f1 = f1; st[sp].f2 = f2; sp++; continue; }
        if (t1 < 0 && t2 < 0) { st[sp].node = nd->child[1]; st[sp].f1 = f1; st[sp].f2 = f2; sp++; continue; }
        int side;
        double frac, frac2;
        if (SUN_VIS_MUTANT == 7) {   /* no split: the whole part goes to the start's side */
            st[sp].node = nd->child[t1 >= 0 ? 0 : 1]; st[sp].f1 = f1; st[sp].f2 = f2; sp++; continue;
        }
        if (t1 < t2) {
            double idist = 1.0 / (t1 - t2);
            side = 1;
            frac2 = (t1 + SUN_DIST_EPSILON) * idist;
            frac = (t1 - SUN_DIST_EPSILON) * idist;
        } else if (t1 > t2) {
            double idist = 1.0 / (t1 - t2);
            side = 0;
            frac2 = (t1 - SUN_DIST_EPSILON) * idist;
            frac = (t1 + SUN_DIST_EPSILON) * idist;
        } else {
            side = 0;
            frac = 1;
            frac2 = 0;
        }
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        if (frac2 < 0) frac2 = 0;
        if (frac2 > 1) frac2 = 1;
        double midf = f1 + (f2 - f1) * frac, midf2 = f1 + (f2 - f1) * frac2;
        /* far part pushed first, so the near part is traced first */
        st[sp].node = nd->child[side ^ 1]; st[sp].f1 = midf2; st[sp].f2 = f2; sp++;
        st[sp].node = nd->child[side]; st[sp].f1 = f1; st[sp].f2 = midf; sp++;
    }
    *leftsolid = c.leftsolid;
}

void sun_trace(const SunMap *m, SunScratch *s, const double a[3], const double b[3], int mask, SunTrace *tr) {
    double left;
    trace_once(m, s, a, b, mask, tr, &left);
}

int sun_visible(const SunMap *m, SunScratch *s, const double p[3]) {
    if (!m->info.has_sun) return SUN_NONE;
    double dir[3];
    for (int k = 0; k < 3; k++) dir[k] = SUN_VIS_MUTANT == 8 ? m->info.sun_normal[k] : m->info.sun_dir[k];
    double b[3] = { p[0] + dir[0] * SUN_TRACE_LENGTH, p[1] + dir[1] * SUN_TRACE_LENGTH, p[2] + dir[2] * SUN_TRACE_LENGTH };
    SunTrace tr;
    sun_trace(m, s, p, b, SUN_MASK_OPAQUE, &tr);
    return (tr.surf_flags & SUN_SURF_SKY) ? SUN_LIT : SUN_SHADOW;
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* the engine's model rule: the light cache's point, the leaf's sky flags, the trace                                                    */

int sun_point_leaf(const SunMap *m, const double p[3]) {
    int node = m->head;
    for (int guard = 0; node >= 0 && guard <= m->info.nnodes; guard++) {
        const Node *nd = &m->nodes[node];
        const Plane *pl = &m->planes[nd->plane];
        double d = pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d;
        node = nd->child[d >= 0 ? 0 : 1];
    }
    return node >= 0 ? 0 : -1 - node;
}

int sun_leaf_contents(const SunMap *m, int leaf) { return leaf >= 0 && leaf < m->info.nleaves ? m->leaves[leaf].contents : 0; }
int sun_leaf_flags(const SunMap *m, int leaf) { return leaf >= 0 && leaf < m->info.nleaves ? m->leaves[leaf].flags : 0; }

/* FUN_18017d680 with eps 0.5: along the origin's path through the tree, a point on the other side of a node's plane is moved to eps past it
 * on the origin's side (float, as the engine) */
static void nudge(const SunMap *m, const float o[3], float p[3]) {
    int node = m->head;
    const float eps = 0.5f;
    for (int guard = 0; node >= 0 && guard <= m->info.nnodes; guard++) {
        const Node *nd = &m->nodes[node];
        const Plane *pl = &m->planes[nd->plane];
        float n[3] = { (float)pl->n[0], (float)pl->n[1], (float)pl->n[2] }, d = (float)pl->d;
        float dO = n[0] * o[0] + n[1] * o[1] + n[2] * o[2] - d;
        float dP = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] - d;
        if (dO >= 0) {
            node = nd->child[0];
            if (dP < 0) { float f = eps - dP; for (int k = 0; k < 3; k++) p[k] += n[k] * f; }
        } else {
            node = nd->child[1];
            if (dP > 0) { float f = dP + eps; for (int k = 0; k < 3; k++) p[k] -= n[k] * f; }
        }
    }
}

static int clear_to(const SunMap *m, SunScratch *s, const float o[3], const float p[3], int *startsolid) {
    double a[3] = { o[0], o[1], o[2] }, b[3] = { p[0], p[1], p[2] };
    SunTrace t;
    sun_trace(m, s, a, b, SUN_MASK_OPAQUE, &t);
    if (startsolid) *startsolid = t.startsolid;
    return t.fraction >= 1;
}

/* The light cache's cell of a float coordinate: (|x| truncated) >> shift, complemented for x < 0 (FUN_1801d7400's centre cell) */
static int centre_cell(float x, int shift) {
    int u = (int)fabsf(x) >> shift;
    return x < 0 ? ~u : u;
}

int sun_lightcache_point(const SunMap *m, SunScratch *s, const double origin[3], double out[3]) {
    float o[3] = { (float)origin[0], (float)origin[1], (float)origin[2] };
    int ux = centre_cell(o[0], 3), uy = centre_cell(o[1], 3), uz = centre_cell(o[2], 4);
    float c[3] = { ((float)(ux * 8) + 8.0f + (float)(ux * 8)) * 0.5f, (float)(uy * 8) + 4.0f, ((float)(uz * 16) + 16.0f + (float)(uz * 16)) * 0.5f };
    double od[3] = { o[0], o[1], o[2] }, cd[3] = { c[0], c[1], c[2] };
    int lo = sun_point_leaf(m, od), lc = sun_point_leaf(m, cd), how = SUN_POINT_ORIGIN;
    float *res = o;
    int second = 0;
    if (SUN_VIS_MUTANT == 16) { for (int k = 0; k < 3; k++) out[k] = o[k]; return SUN_POINT_ORIGIN; }
    if (lc != lo) {
        if (sun_leaf_contents(m, lc) & SUN_MASK_OPAQUE) second = 1;   /* the centre is in another, solid leaf */
        else nudge(m, o, c);
    }
    if (!second) {
        int ss;
        int clear = clear_to(m, s, o, c, &ss);
        if (ss) { res = o; how = SUN_POINT_ORIGIN; goto done; }
        if (clear) { res = c; how = SUN_POINT_CENTRE; goto done; }
    }
    /* the centre's x and y at the origin's height */
    {
        float c2[3] = { ((float)(ux * 8) + 8.0f + (float)(ux * 8)) * 0.5f, (float)(uy * 8) + 4.0f, o[2] };
        nudge(m, o, c2);
        if (clear_to(m, s, o, c2, NULL)) { for (int k = 0; k < 3; k++) out[k] = c2[k]; return SUN_POINT_CENTRE_XY; }
        res = o;
        how = SUN_POINT_ORIGIN;
    }
done:
    for (int k = 0; k < 3; k++) out[k] = res[k];
    return how;
}

int sun_model_visible(const SunMap *m, SunScratch *s, const double origin[3]) {
    if (!m->info.has_sun) return SUN_NONE;
    double p[3];
    sun_lightcache_point(m, s, origin, p);
    /* FUN_1801d6c70: the sun only for a point whose leaf has the 3D or 2D sky in its PVS (LEAF_FLAGS_SKY | LEAF_FLAGS_SKY2D) */
    if (!(sun_leaf_flags(m, sun_point_leaf(m, p)) & 5) && SUN_VIS_MUTANT != 17) return SUN_SHADOW;
    return sun_visible(m, s, p);
}

/* ---------------------------------------------------------------------------------------------------------------------------------- */
/* the cache: LightcacheGet's own key                                                                                                   */

struct SunCache {
    const SunMap *m;
    SunScratch *s;
    int shift;
    uint64_t mask;
    uint64_t *e;             /* (key << 2) | 2 (valid) | lit; 0 is empty */
    SunCacheStats st;
};

SunCache *sun_cache_new(const SunMap *m, int log2_entries) {
    if (!m || log2_entries < 1 || log2_entries > 26) return NULL;
    SunCache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->m = m;
    c->shift = 64 - log2_entries;
    c->mask = (1ull << log2_entries) - 1;
    c->e = calloc((size_t)1 << log2_entries, sizeof *c->e);
    c->s = sun_scratch_new(m);
    if (!c->e || !c->s) { sun_cache_free(c); return NULL; }
    return c;
}

void sun_cache_free(SunCache *c) {
    if (!c) return;
    sun_scratch_free(c->s);
    free(c->e);
    free(c);
}

void sun_cache_clear(SunCache *c) { memset(c->e, 0, (size_t)(c->mask + 1) * sizeof *c->e); }
void sun_cache_stats(const SunCache *c, SunCacheStats *st) { *st = c->st; }

int sun_cache_key(const SunMap *m, const double origin[3], uint64_t *key) {
    float o[3] = { (float)origin[0], (float)origin[1], (float)origin[2] };
    for (int k = 0; k < 3; k++) if (!(o[k] > -32768.0f && o[k] < 32767.0f)) return 0;   /* NaN and far points: no key */
    /* FUN_1801dafa0: ((int)x + 0x8000) >> 3, the same for y, ((int)z + 0x8000) >> 4, and the origin's leaf */
    uint64_t kx = (uint64_t)(((int)o[0] + 0x8000) >> 3), ky = (uint64_t)(((int)o[1] + 0x8000) >> 3), kz = (uint64_t)(((int)o[2] + 0x8000) >> 4);
    double od[3] = { o[0], o[1], o[2] };
    uint64_t leaf = (uint64_t)sun_point_leaf(m, od) & 0xffff;
    *key = kx << 41 | ky << 28 | kz << 16 | leaf;   /* 13 + 13 + 12 + 16 bits */
    return 1;
}

int sun_cache_visible(SunCache *c, const double origin[3]) {
    if (!c->m->info.has_sun) return SUN_NONE;
    c->st.lookups++;
    uint64_t key;
    if (!sun_cache_key(c->m, origin, &key)) { c->st.traces++; return sun_model_visible(c->m, c->s, origin); }
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    size_t slot = (size_t)((h * 0xBF58476D1CE4E5B9ull) >> c->shift);
    uint64_t e = c->e[slot];
    if (e && ((e >> 2) == key || SUN_VIS_MUTANT == 9)) { c->st.hits++; return (int)(e & 1); }
    c->st.traces++;
    int v = sun_model_visible(c->m, c->s, origin);
    c->e[slot] = key << 2 | 2 | (uint64_t)(v == SUN_LIT);
    return v;
}

int sun_map_disp_count(const SunMap *m) { return m->disps ? m->info.ndisps : 0; }
int sun_map_disp_grid(const SunMap *m, int i, const float (**v)[3], int *w, int *texinfo) {
    if (!m->disps || i < 0 || i >= m->info.ndisps) return 0;
    *v = (const float (*)[3])m->disps[i].v; *w = m->disps[i].w; *texinfo = m->disps[i].texinfo;
    return 1;
}
