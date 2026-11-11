/* P5: see lg_map.h. The lump reader follows amb_bake.c's (the same checks and the L4D2 header rule), kept separate so the light grid's
 * map can be reviewed and tested on its own. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lg_map.h"

#ifndef LG_MAP_MUTANT
#define LG_MAP_MUTANT 0     /* test only (scripts/run_ambient_test.sh): 1 the PVS row is read one cluster off, 2 v1 lights keep shadow_cast_offset */
#endif

enum { HEADER_BYTES = 8 + 64 * 16, MAX_PLANES = 65536, MAX_NODES = 65536, MAX_LEAVES = 65536, MAX_VIS_BYTES = 64 << 20,
       L_PLANES = 1, L_VIS = 4, L_NODES = 5, L_LEAVES = 10, L_MODELS = 14, L_LIGHTS = 15, L_LIGHTS_HDR = 54 };

typedef struct { double n[3], d; } Plane;
typedef struct { int plane, child[2]; } Node;
typedef struct { int contents, cluster; } Leaf;

struct LgMap {
    LgMapInfo info;
    Plane *planes;
    Node *nodes;
    Leaf *leaves;
    int head;
    uint8_t *pvs;             /* nclusters rows of rowbytes */
    size_t rowbytes;
    LgWorldLight *lights;
};

typedef struct { const uint8_t *p; size_t ofs, len; int version; } Lump;

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static int32_t rd_s32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}
static float rd_f32(const uint8_t *p) { uint32_t u = (uint32_t)rd_s32(p); float f; memcpy(&f, &u, 4); return f; }

enum { MAX_BUFS = 8 };
typedef struct {
    const uint8_t *mem;
    size_t len;
    const AmbReader *r;
    uint8_t head[HEADER_BYTES];
    uint8_t *bufs[MAX_BUFS];
    int nbufs;
    long long held;
    AmbReadStats st;
} Src;

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

static int fetch(Src *s, Lump *l, size_t n) {
    if (!s->r) return AMB_OK;
    if (s->nbufs == MAX_BUFS) return AMB_ERR_ARG;
    uint8_t *b = malloc(n ? n : 1);
    if (!b) return AMB_ERR_NOMEM;
    s->bufs[s->nbufs++] = b;
    s->held += (long long)n;
    if (s->held > s->st.peak_bytes) s->st.peak_bytes = s->held;
    l->p = b;
    return src_read(s, l->ofs, b, n);
}

void lg_map_free(LgMap *m) {
    if (!m) return;
    free(m->planes);
    free(m->nodes);
    free(m->leaves);
    free(m->pvs);
    free(m->lights);
    free(m);
}

static int parse(Src *s, int prefer_hdr, LgMap **out) {
    *out = NULL;
    if (s->len < HEADER_BYTES || memcmp(s->mem, "VBSP", 4) != 0) return AMB_ERR_FORMAT;
    LgMap *m = calloc(1, sizeof *m);
    if (!m) return AMB_ERR_NOMEM;
    m->info.bsp_version = rd_s32(s->mem + 4);
    Lump lp, ln, ll, lm, lv;
    int rc;
    if ((rc = locate(s, L_PLANES, &lp)) || (rc = locate(s, L_NODES, &ln)) || (rc = locate(s, L_LEAVES, &ll)) || (rc = locate(s, L_MODELS, &lm))) goto fail;
    rc = AMB_ERR_FORMAT;
    size_t leafsz = ll.version == 0 ? 56 : 32;
    if (ll.version != 0 && ll.version != 1) goto fail;
    if (lp.len % 20 || ln.len % 32 || ll.len % leafsz || lm.len < 48) goto fail;
    size_t np = lp.len / 20, nn = ln.len / 32, nl = ll.len / leafsz;
    if (!np || !nl || np > MAX_PLANES || nn > MAX_NODES || nl > MAX_LEAVES) goto fail;
    if ((rc = fetch(s, &lp, lp.len)) || (rc = fetch(s, &ln, ln.len)) || (rc = fetch(s, &ll, ll.len)) || (rc = fetch(s, &lm, 48))) goto fail;
    rc = AMB_ERR_NOMEM;
    m->planes = malloc(np * sizeof *m->planes);
    m->nodes = malloc((nn ? nn : 1) * sizeof *m->nodes);
    m->leaves = malloc(nl * sizeof *m->leaves);
    if (!m->planes || !m->nodes || !m->leaves) goto fail;
    rc = AMB_ERR_FORMAT;
    for (size_t i = 0; i < np; i++) {
        const uint8_t *p = lp.p + 20 * i;
        for (int k = 0; k < 3; k++) m->planes[i].n[k] = rd_f32(p + 4 * k);
        m->planes[i].d = rd_f32(p + 12);
    }
    for (size_t i = 0; i < nn; i++) {
        const uint8_t *p = ln.p + 32 * i;
        m->nodes[i].plane = rd_s32(p);
        m->nodes[i].child[0] = rd_s32(p + 4);
        m->nodes[i].child[1] = rd_s32(p + 8);
        if (m->nodes[i].plane < 0 || (size_t)m->nodes[i].plane >= np) goto fail;
        for (int c = 0; c < 2; c++) {
            int ch = m->nodes[i].child[c];
            if (ch >= 0 ? (size_t)ch >= nn : (size_t)(-1 - (long long)ch) >= nl) goto fail;
        }
    }
    for (size_t i = 0; i < nl; i++) {
        const uint8_t *p = ll.p + leafsz * i;
        m->leaves[i].contents = rd_s32(p);
        m->leaves[i].cluster = rd_s16(p + 4);
    }
    for (int k = 0; k < 3; k++) { m->info.world_min[k] = rd_f32(lm.p + 4 * k); m->info.world_max[k] = rd_f32(lm.p + 12 + 4 * k); }
    m->head = rd_s32(lm.p + 36);
    if (m->head >= 0 ? (size_t)m->head >= (nn ? nn : 1) || !nn : (size_t)(-1 - (long long)m->head) >= nl) goto fail;
    m->info.nplanes = (int)np;
    m->info.nnodes = (int)nn;
    m->info.nleaves = (int)nl;
    m->info.leaf_version = ll.version;

    /* visibility: [numclusters][bitofs PVS, PAS] then Quake RLE rows; anything odd leaves the map without a PVS (all visible) */
    int vrc = locate(s, L_VIS, &lv);
    if (vrc == AMB_ERR_CANCELLED || vrc == AMB_ERR_READ) { rc = vrc; goto fail; }
    if (vrc == AMB_OK && lv.len >= 4 && lv.len <= MAX_VIS_BYTES) {
        if ((rc = fetch(s, &lv, lv.len))) goto fail;
        int nc = rd_s32(lv.p);
        if (nc > 0 && nc <= LG_MAX_CLUSTERS && (size_t)nc * 8 + 4 <= lv.len) {
            size_t rb = ((size_t)nc + 7) / 8;
            uint8_t *pvs = calloc((size_t)nc, rb);
            int ok = pvs != NULL;
            for (int c = 0; ok && c < nc; c++) {
                int32_t ofs = rd_s32(lv.p + 4 + 8 * (size_t)(LG_MAP_MUTANT == 1 && c + 1 < nc ? c + 1 : c));
                if (ofs < 0 || (size_t)ofs >= lv.len) { ok = 0; break; }
                size_t i = (size_t)ofs, o = 0;
                uint8_t *row = pvs + (size_t)c * rb;
                while (o < rb) {
                    if (i >= lv.len) { ok = 0; break; }
                    uint8_t b = lv.p[i++];
                    if (b) { row[o++] = b; continue; }
                    if (i >= lv.len) { ok = 0; break; }
                    size_t run = lv.p[i++];
                    if (run > rb - o) run = rb - o;   /* a run past the row's end is clipped, as the engine's decompressor stops at the row */
                    o += run;                         /* calloc'ed: zeros */
                }
            }
            if (ok) { m->pvs = pvs; m->rowbytes = rb; m->info.pvs = 1; m->info.nclusters = nc; }
            else free(pvs);
        }
    }
    if (!m->info.pvs) {
        int mx = -1;
        for (size_t i = 0; i < nl; i++) if (m->leaves[i].cluster > mx) mx = m->leaves[i].cluster;
        m->info.nclusters = mx + 1;
    }

    /* worldlights: the engine's lump choice */
    Lump lw;
    int which = 0;
    if (prefer_hdr) {
        int r2 = locate(s, L_LIGHTS_HDR, &lw);
        if (r2 == AMB_ERR_CANCELLED || r2 == AMB_ERR_READ) { rc = r2; goto fail; }
        if (r2 == AMB_ERR_LZMA && lw.len) { m->info.lights_lzma = 1; which = -1; }
        else if (r2 == AMB_OK && lw.len) which = L_LIGHTS_HDR;
    }
    if (!which) {
        int r2 = locate(s, L_LIGHTS, &lw);
        if (r2 == AMB_ERR_CANCELLED || r2 == AMB_ERR_READ) { rc = r2; goto fail; }
        if (r2 == AMB_ERR_LZMA && lw.len) m->info.lights_lzma = 1;
        else if (r2 == AMB_OK && lw.len) which = L_LIGHTS;
    }
    if (which > 0) {
        size_t rs = lw.version == 1 ? 100 : 88;
        rc = AMB_ERR_FORMAT;
        if ((lw.version != 0 && lw.version != 1) || lw.len % rs || lw.len / rs > LG_MAX_WORLDLIGHTS) goto fail;
        size_t n = lw.len / rs;
        if ((rc = fetch(s, &lw, lw.len))) goto fail;
        rc = AMB_ERR_NOMEM;
        m->lights = calloc(n ? n : 1, sizeof *m->lights);
        if (!m->lights) goto fail;
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = lw.p + rs * i;
            uint8_t rec[88];
            if (rs == 100 && LG_MAP_MUTANT != 2) { memcpy(rec, p, 36); memcpy(rec + 36, p + 48, 52); }   /* drop shadow_cast_offset */
            else memcpy(rec, p, 88);
            LgWorldLight *w = &m->lights[i];
            for (int k = 0; k < 3; k++) { w->origin[k] = rd_f32(rec + 4 * k); w->intensity[k] = rd_f32(rec + 12 + 4 * k); w->normal[k] = rd_f32(rec + 24 + 4 * k); }
            w->cluster = rd_s32(rec + 36); w->type = rd_s32(rec + 40); w->style = rd_s32(rec + 44);
            w->stopdot = rd_f32(rec + 48); w->stopdot2 = rd_f32(rec + 52); w->exponent = rd_f32(rec + 56); w->radius = rd_f32(rec + 60);
            w->constant_attn = rd_f32(rec + 64); w->linear_attn = rd_f32(rec + 68); w->quadratic_attn = rd_f32(rec + 72);
            w->flags = rd_s32(rec + 76); w->texinfo = rd_s32(rec + 80); w->owner = rd_s32(rec + 84);
        }
        m->info.nlights = (int)n;
        m->info.light_lump = which;
        m->info.light_lump_version = lw.version;
    }
    *out = m;
    return AMB_OK;
fail:
    lg_map_free(m);
    return rc;
}

static void src_done(Src *s, AmbReadStats *stats) {
    for (int i = 0; i < s->nbufs; i++) free(s->bufs[i]);
    if (stats) *stats = s->st;
}

int lg_map_load(const void *bsp, size_t len, int prefer_hdr, LgMap **out) {
    if (!out) return AMB_ERR_ARG;
    *out = NULL;
    if (!bsp) return AMB_ERR_ARG;
    Src s;
    memset(&s, 0, sizeof s);
    s.mem = bsp;
    s.len = len;
    return parse(&s, prefer_hdr, out);
}

int lg_map_load_reader(const AmbReader *r, int prefer_hdr, LgMap **out, AmbReadStats *stats) {
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
    src_done(&s, stats);
    return rc;
}

void lg_map_info(const LgMap *m, LgMapInfo *info) { *info = m->info; }

const LgWorldLight *lg_map_lights(const LgMap *m, int *n) {
    *n = m->info.nlights;
    return m->lights;
}

int lg_point_leaf(const LgMap *m, const double p[3]) {
    int node = m->head;
    for (int guard = 0; node >= 0 && guard <= m->info.nnodes; guard++) {
        const Node *nd = &m->nodes[node];
        const Plane *pl = &m->planes[nd->plane];
        double d = pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d;
        node = nd->child[d >= 0 ? 0 : 1];
    }
    return node >= 0 ? 0 : -1 - node;   /* a cycle (refused at load only if out of range) ends in leaf 0, the solid leaf */
}

int lg_point_cluster(const LgMap *m, const double p[3]) {
    const Leaf *l = &m->leaves[lg_point_leaf(m, p)];
    return l->contents & 1 ? -1 : l->cluster;   /* CONTENTS_SOLID */
}

int lg_cluster_sees(const LgMap *m, int a, int b) {
    if (a < 0 || b < 0) return 0;
    if (!m->info.pvs) return 1;
    if (a >= m->info.nclusters || b >= m->info.nclusters) return 0;
    return (m->pvs[(size_t)a * m->rowbytes + (size_t)(b >> 3)] >> (b & 7)) & 1;
}
