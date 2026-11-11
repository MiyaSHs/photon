/* P3 (a): ambient volume bake. Contract and layout in amb_bake.h; the reference is tools/lpv2_ambient_volume.py
 * (--mode leaflocal), and tests/ambient checks this file against it.
 *
 * BSP formats are the public SDK ones (refs/source-sdk-2013/src/public/bspfile.h): dplane_t 20 B, dnode_t 32 B,
 * dleaf_t v1 32 B / dleaf_version_0_t 56 B, dmodel_t 48 B, dleafambientindex_t 4 B, dleafambientlighting_t 28 B.
 *
 * LZMA: not supported. gm_construct and gm_flatgrass have no LZMA lump (no lump starts with the "LZMA" tag; checked
 * with the Python's own test), and the Python's LZMA path was never exercised. A lump this loader needs that is
 * compressed fails the load with AMB_ERR_LZMA instead of being misread. If community maps need it, the LZMA SDK's
 * public-domain LzmaDec.c can be vendored under src/ambient/lzma/ (header lzma_header_t, SDK lzmaDecoder.h:28).
 *
 * Ambient source selection follows the engine loader (engine.dll FUN_180116e20 -> FUN_1801172b0 / FUN_180117010,
 * ghidra/out/engine.dll/decomp.c:190670-191040) [confirmed decomp]:
 *  - leaf lump v0: one sample per leaf, the cube stored inline in the leaf, at fraction (128, 128, 128)
 *    (FUN_180117010 writes count 1, first = leaf index, x = y = z = 0x80).
 *  - leaf lump v1, lighting lump version 1 and a non-empty index lump: the indexed samples; `first` of a leaf with
 *    samples is the running sum of the counts (the stored value is ignored), as the Python does.
 *  - otherwise one sample per leaf from a 24-byte-per-leaf lighting lump, or grey 0.5 when that lump is empty.
 * Both test maps take the indexed path (lump 55/56 version 1), the only one the Python implements.
 *
 * Note on count-0 leaves [confirmed decomp]: FUN_1801172b0 writes the runtime {count, first} pair only for leaves with
 * samples (decomp.c:190924-190930) into a hunk allocation zeroed by FUN_18023aa90(.., 1) (memset when param_3 != 0),
 * so a count-0 leaf keeps first = 0 and the sampler FUN_180115950 returns a zero cube for it (it follows `first` only
 * when it is non-zero, decomp.c:189921-189929). The vrad reference leaf is therefore never used by this engine build.
 * This library keeps the Python's rule (follow the stored reference leaf) because the bake must match the Python; it
 * only matters for points inside solid leaves, which the bake never evaluates (solid voxels are filled by dilation). */
#include "amb_bake.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CONTENTS_SOLID = 0x1 };
enum { LUMP_ENTITIES = 0, LUMP_PLANES = 1, LUMP_NODES = 5, LUMP_LEAFS = 10, LUMP_MODELS = 14,
       LUMP_AMB_INDEX_HDR = 51, LUMP_AMB_INDEX = 52, LUMP_AMB_LIGHT_HDR = 55, LUMP_AMB_LIGHT = 56 };
enum { HEADER_BYTES = 8 + 64 * 16 };
/* dleaf_version_0_t: the cube follows leafWaterDataID at byte 28 (engine FUN_180117010 copies from record + 30, stride 56) */
enum { LEAF_V0_CUBE = 30 };
/* The engine's own map limits (engine.dll decomp 264608, 264737, 265771); a sample count and tree depth well above real maps.
 * Past these a map is refused, which falls back to the engine cube: a Workshop map is untrusted input on worker threads. */
enum { MAX_MAP_ITEMS = 65536, MAX_SAMPLES = 1048576, MAX_LEAF_SAMPLES = 256, MAX_TREE_DEPTH = 512 };

typedef struct { double n[3], d; int axis; } Plane;     /* axis 0..2: normal is exactly +axis (fast exact test) */
typedef struct { int plane, child[2]; } Node;
typedef struct { int contents, count, first, ref; } Leaf; /* first: running sum; ref: stored first (count 0) */
typedef struct { double pos[3], cube[AMB_VOXEL_FLOATS]; } Sample;

struct AmbMap {
    Plane *planes;
    Node *nodes;
    Leaf *leaves;
    Sample *samples;
    int nplanes, nnodes, nleaves, nsamples, head;
    AmbMapInfo info;
};

/* ---------------------------------------------------------------------------------------------------- loading -- */

typedef struct { const uint8_t *p; size_t ofs, len; int version; } Lump;   /* p: valid for what fetch() asked for, at most len bytes */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static int32_t rd_s32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}
static float rd_f32(const uint8_t *p) { uint32_t u = (uint32_t)rd_s32(p); float f; memcpy(&f, &u, 4); return f; }

/* Where the bytes come from: a whole file in memory (mem), or a reader that fetches each lump into a buffer of its own. The parse below
 * is the same code for both, so the two differ only in how a range turns into a pointer. */
enum { MAX_BUFS = 8 };
typedef struct {
    const uint8_t *mem;           /* the header, and the whole file when r is NULL */
    size_t len;
    const AmbReader *r;
    uint8_t head[HEADER_BYTES];
    uint8_t *bufs[MAX_BUFS];
    int nbufs;
    long long held;
    AmbReadStats st;
} Src;

/* The same intended race as AmbBakeParams.cancel: one int, a late read only costs one more chunk (tests/ambient/worktest.supp matches the name) */
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
    /* L4D2-format headers order the fields version, fileofs, filelen. The engine knows them by lump 0's "version" being its length
     * (engine.dll FUN_180112730: above 1000, every lump's three fields are rotated back). */
    int l4d2 = rd_s32(s->mem + 8 + 8) > 1000;
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

/* Makes the first n bytes of l readable at l->p (n <= l->len, checked by the caller's size rules before this is called) */
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

/* ColorRGBExp32 -> linear: m * 2^e with e signed (ColorRGBExp32ToVector; engine FUN_18029ffc0 uses a 2^e/255 table
 * times 255, the same value). ldexp is exact in double, as the Python's m * exp2(e). */
static void decode_cube(const uint8_t *c24, double out[AMB_VOXEL_FLOATS]) {
    for (int f = 0; f < AMB_FACES; f++)
        for (int c = 0; c < 3; c++) {
            double v = ldexp((double)c24[f * 4 + c], (int8_t)c24[f * 4 + 3]);
            /* far outside real data at both ends: one bad sample must not spread half-float inf through a cascade, and
             * denormals would slow every sum */
            out[f * 3 + c] = v > 1024.0 ? 1024.0 : v < 1e-30 ? 0.0 : v;
        }
}

/* Sample position: leaf mins + frac/255 * (maxs - mins), the Python's operation order. */
static void sample_pos(const uint8_t *leafrec, const uint8_t frac[3], double out[3]) {
    for (int a = 0; a < 3; a++) {
        double lo = rd_s16(leafrec + 8 + 2 * a), hi = rd_s16(leafrec + 14 + 2 * a);
        out[a] = lo + ((double)frac[a] / 255.0) * (hi - lo);
    }
}

/* Iterative walk from the head: a cycle or a tree deeper than any real map fails the load, so every later point walk is short */
static int tree_ok(const AmbMap *m) {
    if (m->head < 0) return 1;
    int *stack = malloc(sizeof(int) * 2 * (MAX_TREE_DEPTH + 2));
    uint8_t *seen = calloc((size_t)m->nnodes, 1);
    int ok = stack && seen, sp = 0;
    if (ok) { stack[0] = m->head; stack[1] = 0; sp = 1; }
    while (ok && sp) {
        sp--;
        int n = stack[sp * 2], depth = stack[sp * 2 + 1];
        if (depth > MAX_TREE_DEPTH || seen[n]) { ok = 0; break; }   /* a BSP node has one parent: a second visit is malformed */
        seen[n] = 1;
        for (int c = 0; c < 2; c++) {
            int ch = m->nodes[n].child[c];
            if (ch < 0) continue;
            if (sp > MAX_TREE_DEPTH) { ok = 0; break; }
            stack[sp * 2] = ch; stack[sp * 2 + 1] = depth + 1; sp++;
        }
    }
    free(stack);
    free(seen);
    return ok;
}

void amb_map_free(AmbMap *m) {
    if (!m) return;
    free(m->planes);
    free(m->nodes);
    free(m->leaves);
    free(m->samples);
    free(m);
}

/* s->mem holds at least the header, which starts with "VBSP". Every fetch comes after the size rule that bounds it. */
static int load(Src *s, int prefer_hdr, AmbMap **out) {
    const uint8_t *bsp = s->mem;
    Lump lp, ln, ll, lm, lidx, llit;
    int rc;
    if ((rc = locate(s, LUMP_PLANES, &lp)) || (rc = locate(s, LUMP_NODES, &ln)) || (rc = locate(s, LUMP_LEAFS, &ll)) ||
        (rc = locate(s, LUMP_MODELS, &lm)))
        return rc;
    int leaf_stride = ll.version == 0 ? 56 : ll.version == 1 ? 32 : 0;
    if (!leaf_stride || lp.len % 20 || ln.len % 32 || ll.len % (size_t)leaf_stride || lm.len < 48 || !ll.len ||
        lp.len / 20 > MAX_MAP_ITEMS || ln.len / 32 > MAX_MAP_ITEMS || ll.len / (size_t)leaf_stride > MAX_MAP_ITEMS)
        return AMB_ERR_FORMAT;
    if ((rc = fetch(s, &lp, lp.len)) || (rc = fetch(s, &ln, ln.len)) || (rc = fetch(s, &ll, ll.len)) || (rc = fetch(s, &lm, 48)))
        return rc;

    AmbMap *m = calloc(1, sizeof *m);
    if (!m) return AMB_ERR_NOMEM;
    m->nplanes = (int)(lp.len / 20);
    m->nnodes = (int)(ln.len / 32);
    m->nleaves = (int)(ll.len / (size_t)leaf_stride);
    m->planes = malloc(sizeof *m->planes * (size_t)(m->nplanes ? m->nplanes : 1));
    m->nodes = malloc(sizeof *m->nodes * (size_t)(m->nnodes ? m->nnodes : 1));
    m->leaves = malloc(sizeof *m->leaves * (size_t)m->nleaves);
    rc = AMB_ERR_NOMEM;
    if (!m->planes || !m->nodes || !m->leaves) goto fail;
    rc = AMB_ERR_FORMAT;

    for (int i = 0; i < m->nplanes; i++) {
        const uint8_t *r = lp.p + 20 * (size_t)i;
        Plane *pl = &m->planes[i];
        for (int a = 0; a < 3; a++) pl->n[a] = rd_f32(r + 4 * a);   /* float -> double, as the Python's astype */
        pl->d = rd_f32(r + 12);
        /* The fast path p[axis] * n[axis] - d equals the full dot product only when the other two components are
         * exactly 0 (then they add exact zeros), so it is enabled only for planes that are verified axial. */
        int t = rd_s32(r + 16);
        pl->axis = -1;
        if (t >= 0 && t < 3 && pl->n[(t + 1) % 3] == 0.0 && pl->n[(t + 2) % 3] == 0.0) pl->axis = t;
    }
    for (int i = 0; i < m->nnodes; i++) {
        const uint8_t *r = ln.p + 32 * (size_t)i;
        Node *nd = &m->nodes[i];
        nd->plane = rd_s32(r);
        nd->child[0] = rd_s32(r + 4);
        nd->child[1] = rd_s32(r + 8);
        if (nd->plane < 0 || nd->plane >= m->nplanes) goto fail;
        for (int c = 0; c < 2; c++) {
            int ch = nd->child[c];
            if (ch >= m->nnodes || (ch < 0 && -1 - ch >= m->nleaves)) goto fail;
        }
    }
    m->head = rd_s32(lm.p + 36);   /* model 0 = world */
    if (m->head >= m->nnodes || (m->head < 0 && -1 - m->head >= m->nleaves) || !tree_ok(m)) goto fail;
    for (int a = 0; a < 3; a++) {
        m->info.world_min[a] = rd_f32(lm.p + 4 * a);
        m->info.world_max[a] = rd_f32(lm.p + 12 + 4 * a);
    }
    for (int i = 0; i < m->nleaves; i++) {
        Leaf *lf = &m->leaves[i];
        lf->contents = rd_s32(ll.p + (size_t)leaf_stride * (size_t)i);
        lf->count = lf->first = lf->ref = 0;
    }

    /* Ambient lumps: HDR when asked for and lump 55 is non-empty (the Python's rule; the engine also keys on its HDR
     * mode). */
    int hdr = prefer_hdr != 0;
    if (hdr) {
        Lump t;
        if ((rc = locate(s, LUMP_AMB_LIGHT_HDR, &t))) goto fail;
        hdr = t.len != 0;
    }
    if ((rc = locate(s, hdr ? LUMP_AMB_INDEX_HDR : LUMP_AMB_INDEX, &lidx)) ||
        (rc = locate(s, hdr ? LUMP_AMB_LIGHT_HDR : LUMP_AMB_LIGHT, &llit)))
        goto fail;
    rc = AMB_ERR_FORMAT;
    int source;
    if (ll.version == 0) source = 1;
    else if (llit.version == 1 && lidx.len) source = 0;
    else source = llit.len ? 1 : 2;

    if (source == 0) {
        if (lidx.len != 4 * (size_t)m->nleaves || llit.len % 28) goto fail;
        if ((rc = fetch(s, &lidx, lidx.len))) goto fail;
        rc = AMB_ERR_FORMAT;
        size_t nlump = llit.len / 28, total = 0;
        for (int i = 0; i < m->nleaves; i++) {
            Leaf *lf = &m->leaves[i];
            lf->count = rd_u16(lidx.p + 4 * (size_t)i);
            lf->ref = rd_u16(lidx.p + 4 * (size_t)i + 2);
            lf->first = (int)total;
            total += (size_t)lf->count;
            if (lf->count > MAX_LEAF_SAMPLES) goto fail;
        }
        /* engine: "ambient cube out of bounds" if a leaf's range passes the lump; the Python needs equality */
        if (total > nlump || total > MAX_SAMPLES) goto fail;
        if ((rc = fetch(s, &llit, 28 * total))) goto fail;   /* only the samples the index names: the rest of the lump is never read */
        rc = AMB_ERR_FORMAT;
        m->nsamples = (int)total;
        m->samples = malloc(sizeof *m->samples * (total ? total : 1));
        if (!m->samples) { rc = AMB_ERR_NOMEM; goto fail; }
        for (int i = 0; i < m->nleaves; i++) {
            const Leaf *lf = &m->leaves[i];
            for (int k = 0; k < lf->count; k++) {
                const uint8_t *r = llit.p + 28 * (size_t)(lf->first + k);
                Sample *s = &m->samples[lf->first + k];
                decode_cube(r, s->cube);
                sample_pos(ll.p + (size_t)leaf_stride * (size_t)i, r + 24, s->pos);
            }
        }
    } else {
        /* one sample per leaf at the leaf centre fraction 128/255 (engine FUN_180117010 / the legacy branch of
         * FUN_1801172b0) */
        if (source == 1 && ll.version == 1 && llit.len != 24 * (size_t)m->nleaves) goto fail;
        if (source == 1 && ll.version == 1 && (rc = fetch(s, &llit, llit.len))) goto fail;
        m->nsamples = m->nleaves;
        m->samples = malloc(sizeof *m->samples * (size_t)m->nleaves);
        if (!m->samples) { rc = AMB_ERR_NOMEM; goto fail; }
        static const uint8_t mid[3] = { 128, 128, 128 };
        for (int i = 0; i < m->nleaves; i++) {
            const uint8_t *lr = ll.p + (size_t)leaf_stride * (size_t)i;
            Sample *s = &m->samples[i];
            m->leaves[i].count = 1;
            m->leaves[i].first = i;
            if (source == 2) for (int c = 0; c < AMB_VOXEL_FLOATS; c++) s->cube[c] = 0.5;
            else decode_cube(ll.version == 0 ? lr + LEAF_V0_CUBE : llit.p + 24 * (size_t)i, s->cube);
            sample_pos(lr, mid, s->pos);
        }
    }
    m->info.bsp_version = rd_s32(bsp + 4);
    m->info.leaf_lump_version = ll.version;
    m->info.hdr = ll.version == 0 ? 0 : hdr;
    m->info.ambient_source = source;
    m->info.nplanes = m->nplanes;
    m->info.nnodes = m->nnodes;
    m->info.nleaves = m->nleaves;
    m->info.nsamples = m->nsamples;
    *out = m;
    return AMB_OK;
fail:
    amb_map_free(m);
    return rc;
}

int amb_map_load(const void *bsp, size_t len, int prefer_hdr, AmbMap **out) {
    if (!out) return AMB_ERR_ARG;
    *out = NULL;
    if (!bsp || len < HEADER_BYTES || memcmp(bsp, "VBSP", 4) != 0) return AMB_ERR_FORMAT;
    Src s = { .mem = bsp, .len = len };
    return load(&s, prefer_hdr, out);
}

int amb_map_load_reader(const AmbReader *r, int prefer_hdr, AmbMap **out, AmbReadStats *stats) {
    if (stats) memset(stats, 0, sizeof *stats);
    if (!out) return AMB_ERR_ARG;
    *out = NULL;
    if (!r || !r->read || r->size < 0) return AMB_ERR_ARG;
    if (r->size < HEADER_BYTES || (unsigned long long)r->size > SIZE_MAX) return AMB_ERR_FORMAT;
    Src *s = calloc(1, sizeof *s);
    if (!s) return AMB_ERR_NOMEM;
    s->r = r;
    s->len = (size_t)r->size;
    s->mem = s->head;
    int rc = src_read(s, 0, s->head, HEADER_BYTES);
    if (rc == AMB_OK) rc = memcmp(s->head, "VBSP", 4) != 0 ? AMB_ERR_FORMAT : load(s, prefer_hdr, out);
    for (int i = 0; i < s->nbufs; i++) free(s->bufs[i]);
    if (stats) *stats = s->st;
    free(s);
    return rc;
}

static int same_d(const double *a, const double *b, size_t n) { return memcmp(a, b, n * sizeof *a) == 0; }
int amb_map_same(const AmbMap *a, const AmbMap *b) {
    if (!a || !b) return a == b;
    if (a->nplanes != b->nplanes || a->nnodes != b->nnodes || a->nleaves != b->nleaves || a->nsamples != b->nsamples || a->head != b->head) return 0;
    if (memcmp(&a->info, &b->info, sizeof a->info) != 0) return 0;   /* ints and floats only: no padding */
    for (int i = 0; i < a->nplanes; i++)
        if (!same_d(a->planes[i].n, b->planes[i].n, 3) || !same_d(&a->planes[i].d, &b->planes[i].d, 1) || a->planes[i].axis != b->planes[i].axis) return 0;
    for (int i = 0; i < a->nnodes; i++)
        if (a->nodes[i].plane != b->nodes[i].plane || a->nodes[i].child[0] != b->nodes[i].child[0] || a->nodes[i].child[1] != b->nodes[i].child[1]) return 0;
    for (int i = 0; i < a->nleaves; i++)
        if (a->leaves[i].contents != b->leaves[i].contents || a->leaves[i].count != b->leaves[i].count || a->leaves[i].first != b->leaves[i].first ||
            a->leaves[i].ref != b->leaves[i].ref) return 0;
    for (int i = 0; i < a->nsamples; i++)
        if (!same_d(a->samples[i].pos, b->samples[i].pos, 3) || !same_d(a->samples[i].cube, b->samples[i].cube, AMB_VOXEL_FLOATS)) return 0;
    return 1;
}

void amb_map_info(const AmbMap *m, AmbMapInfo *info) { *info = m->info; }

/* ------------------------------------------------------------------------------------------- the engine rule -- */

int amb_point_leaf(const AmbMap *m, const double p[3]) {
    int n = m->head;
    /* the load refused cycles and deep trees (tree_ok), so this bound is only a second line of defence */
    for (int steps = 0; n >= 0 && steps <= m->nnodes; steps++) {
        const Node *nd = &m->nodes[n];
        const Plane *pl = &m->planes[nd->plane];
        double d = pl->axis >= 0 ? p[pl->axis] * pl->n[pl->axis] - pl->d
                                 : pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d;
        n = d >= 0 ? nd->child[0] : nd->child[1];
    }
    return n < 0 ? -1 - n : 0;
}

int amb_point_solid(const AmbMap *m, const double p[3]) {
    return (m->leaves[amb_point_leaf(m, p)].contents & CONTENTS_SOLID) != 0;
}

int amb_segment_solid(const AmbMap *m, const double a[3], const double b[3]) {
    /* one pending far part per level: the tree is at most MAX_TREE_DEPTH deep (tree_ok), so this stack cannot overflow */
    struct { int node; double p[3], q[3]; } st[MAX_TREE_DEPTH + 2];
    int sp = 0, n = m->head;
    double p[3] = { a[0], a[1], a[2] }, q[3] = { b[0], b[1], b[2] };
    for (;;) {
        while (n >= 0) {
            const Node *nd = &m->nodes[n];
            const Plane *pl = &m->planes[nd->plane];
            double d1, d2;
            if (pl->axis >= 0) {
                d1 = p[pl->axis] * pl->n[pl->axis] - pl->d;
                d2 = q[pl->axis] * pl->n[pl->axis] - pl->d;
            } else {
                d1 = pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d;
                d2 = pl->n[0] * q[0] + pl->n[1] * q[1] + pl->n[2] * q[2] - pl->d;
            }
            if (d1 >= 0 && d2 >= 0) { n = nd->child[0]; continue; }
            if (d1 < 0 && d2 < 0) { n = nd->child[1]; continue; }
            /* crosses: the part from p to the plane on p's side now, the rest later */
            double t = d1 / (d1 - d2), mid[3];
            for (int k = 0; k < 3; k++) mid[k] = p[k] + t * (q[k] - p[k]);
            if (sp > MAX_TREE_DEPTH) return 1;   /* unreachable for a tree that passed tree_ok; blocked is the safe answer */
            st[sp].node = d1 >= 0 ? nd->child[1] : nd->child[0];
            memcpy(st[sp].p, mid, sizeof mid);
            memcpy(st[sp].q, q, sizeof st[sp].q);
            sp++;
            memcpy(q, mid, sizeof mid);
            n = d1 >= 0 ? nd->child[0] : nd->child[1];
        }
        if (m->leaves[-1 - n].contents & CONTENTS_SOLID) return 1;
        if (!sp) return 0;
        sp--;
        n = st[sp].node;
        memcpy(p, st[sp].p, sizeof p);
        memcpy(q, st[sp].q, sizeof q);
    }
}

/* Weighted mean of a leaf's samples, the Python's engine_cube: sum(cube * w) / max(sum(w), 1e-30), accumulated in
 * sample order in double. */
static int eval_leaf(const AmbMap *m, int leaf, const double p[3], double soft2, float out[AMB_VOXEL_FLOATS]) {
    const Leaf *lf = &m->leaves[leaf];
    if (lf->count == 0) lf = lf->ref < m->nleaves ? &m->leaves[lf->ref] : NULL;   /* bad reference: zero */
    if (!lf || lf->count == 0) {
        memset(out, 0, sizeof(float) * AMB_VOXEL_FLOATS);
        return 0;
    }
    double acc[AMB_VOXEL_FLOATS] = { 0 }, wsum = 0;
    const Sample *s = &m->samples[lf->first];
    for (int k = 0; k < lf->count; k++, s++) {
        double dx = s->pos[0] - p[0], dy = s->pos[1] - p[1], dz = s->pos[2] - p[2];
        double w = 1.0 / (dx * dx + dy * dy + dz * dz + soft2);
        for (int c = 0; c < AMB_VOXEL_FLOATS; c++) acc[c] += s->cube[c] * w;
        wsum += w;
    }
    double div = wsum > 1e-30 ? wsum : 1e-30;
    for (int c = 0; c < AMB_VOXEL_FLOATS; c++) out[c] = (float)(acc[c] / div);
    return lf->count;
}

int amb_eval_engine(const AmbMap *m, const double p[3], double soft, float out[AMB_VOXEL_FLOATS]) {
    int leaf = amb_point_leaf(m, p);
    if (m->leaves[leaf].count == 0) {
        memset(out, 0, sizeof(float) * AMB_VOXEL_FLOATS);
        return 0;
    }
    return eval_leaf(m, leaf, p, soft * soft, out);
}

int amb_eval_point(const AmbMap *m, const double p[3], double soft, float out[AMB_VOXEL_FLOATS]) {
    return eval_leaf(m, amb_point_leaf(m, p), p, soft * soft, out);
}

/* --------------------------------------------------------------------------------------------------- the bake -- */

void amb_params_default(AmbBakeParams *p) {
    memset(p, 0, sizeof *p);
    p->voxel_xy = 32;
    p->voxel_z = 32;
    p->soft = 32;
    p->blur_passes = 4;
    p->dilate_passes = 3;
}

int amb_box_from_bounds(const double bmin[3], const double bmax[3], double voxel_xy, double voxel_z, AmbBox *box) {
    if (!(voxel_xy > 0) || !(voxel_z > 0)) return AMB_ERR_ARG;
    for (int a = 0; a < 3; a++) {
        double st = a < 2 ? voxel_xy : voxel_z;
        double lo = floor(bmin[a] / st), hi = ceil(bmax[a] / st);
        /* the NaN-safe form; map bounds are untrusted, and BSP coordinates are 16-bit, so a million voxels is far out of range */
        if (!(lo >= -1e6 && lo <= 1e6 && hi >= -1e6 && hi <= 1e6)) return AMB_ERR_ARG;
        box->org[a] = (int)lo;
        box->dim[a] = (int)hi - box->org[a];
    }
    return AMB_OK;
}

int amb_region_margin(const AmbBakeParams *p) {
    return (p->blur_passes > 0 ? p->blur_passes : 0) + (p->dilate_passes > 0 ? p->dilate_passes : 0);
}

typedef struct {
    const AmbMap *m;
    const AmbBakeParams *p;
    AmbBox box;
    double st[3], soft2;
    size_t stride[3];           /* voxel index step per axis */
    const volatile int *cancel; /* p->cancel */
    const float *src;           /* current stage input / output volumes */
    float *dst;
    uint8_t *open;              /* 1 = centre not in a solid leaf */
    uint8_t *pass;              /* bit a: the face towards +a is passable */
    const uint8_t *msrc;        /* dilation filled-masks */
    uint8_t *mdst;
    int wrap;
} Bake;

static void centre(const Bake *b, int i, int j, int k, double P[3]) {
    int ijk[3] = { i, j, k };
    for (int a = 0; a < 3; a++) P[a] = ((double)(b->box.org[a] + ijk[a]) + 0.5) * b->st[a];
}

/* Tasks are rows along x: task r covers (j, k) = (r % dim[1], r / dim[1]). */
static int cancelled(const Bake *b) { return b->cancel && *b->cancel; }
static size_t row_start(const Bake *b, int r, int *j, int *k) {
    *j = r % b->box.dim[1];
    *k = r / b->box.dim[1];
    return (size_t)r * (size_t)b->box.dim[0];
}

static void task_eval(void *ctx, int r) {
    Bake *b = ctx;
    if (cancelled(b)) return;
    int j, k;
    size_t v = row_start(b, r, &j, &k);
    for (int i = 0; i < b->box.dim[0]; i++, v++) {
        double P[3];
        centre(b, i, j, k, P);
        int leaf = amb_point_leaf(b->m, P);
        float *o = b->dst + v * AMB_VOXEL_FLOATS;
        b->open[v] = !(b->m->leaves[leaf].contents & CONTENTS_SOLID);
        if (b->open[v]) eval_leaf(b->m, leaf, P, b->soft2, o);
        else memset(o, 0, sizeof(float) * AMB_VOXEL_FLOATS);
    }
}

/* The Python's passf: both voxels open, the +a neighbour inside the box, and the points centre + t * size[a] for
 * t = 0.25, 0.5, 0.75 not in solid leaves. Needs `open` of the whole box, hence its own stage after task_eval. */
static void task_faces(void *ctx, int r) {
    Bake *b = ctx;
    if (cancelled(b)) return;
    int j, k;
    size_t v = row_start(b, r, &j, &k);
    for (int i = 0; i < b->box.dim[0]; i++, v++) {
        int ijk[3] = { i, j, k };
        uint8_t bits = 0;
        if (b->open[v]) {
            double P[3];
            centre(b, i, j, k, P);
            for (int a = 0; a < 3; a++) {
                if (ijk[a] + 1 >= b->box.dim[a] || !b->open[v + b->stride[a]]) continue;
                int ok = 1;
                for (int t = 1; t <= 3 && ok; t++) {
                    double q[3] = { P[0], P[1], P[2] };
                    q[a] += 0.25 * t * b->st[a];
                    ok = !amb_point_solid(b->m, q);
                }
                bits |= (uint8_t)(ok << a);
            }
        }
        b->pass[v] = bits;
    }
}

/* One blur pass: open voxel = (self + neighbours across passable faces) / count, summed in the Python's order
 * (self, +x, -x, +y, -y, +z, -z). Solid voxels stay 0. */
static void task_blur(void *ctx, int r) {
    Bake *b = ctx;
    if (cancelled(b)) return;
    int j, k;
    size_t v = row_start(b, r, &j, &k);
    for (int i = 0; i < b->box.dim[0]; i++, v++) {
        const float *s = b->src + v * AMB_VOXEL_FLOATS;
        float *o = b->dst + v * AMB_VOXEL_FLOATS;
        if (!b->open[v]) { memcpy(o, s, sizeof(float) * AMB_VOXEL_FLOATS); continue; }
        int ijk[3] = { i, j, k };
        double acc[AMB_VOXEL_FLOATS];
        int cnt = 1;
        for (int c = 0; c < AMB_VOXEL_FLOATS; c++) acc[c] = s[c];
        for (int a = 0; a < 3; a++) {
            if (b->pass[v] >> a & 1) {
                const float *n = s + b->stride[a] * AMB_VOXEL_FLOATS;
                for (int c = 0; c < AMB_VOXEL_FLOATS; c++) acc[c] += n[c];
                cnt++;
            }
            if (ijk[a] > 0 && (b->pass[v - b->stride[a]] >> a & 1)) {
                const float *n = s - b->stride[a] * AMB_VOXEL_FLOATS;
                for (int c = 0; c < AMB_VOXEL_FLOATS; c++) acc[c] += n[c];
                cnt++;
            }
        }
        for (int c = 0; c < AMB_VOXEL_FLOATS; c++) o[c] = (float)(acc[c] / cnt);
    }
}

/* One dilation step: an unfilled voxel with filled 6-neighbours becomes their mean (order +x, -x, +y, -y, +z, -z as
 * the Python's np.roll loop). wrap: the neighbour index wraps around the box like np.roll; else it does not exist. */
static void task_dilate(void *ctx, int r) {
    Bake *b = ctx;
    if (cancelled(b)) return;
    int j, k;
    size_t v = row_start(b, r, &j, &k);
    for (int i = 0; i < b->box.dim[0]; i++, v++) {
        const float *s = b->src + v * AMB_VOXEL_FLOATS;
        float *o = b->dst + v * AMB_VOXEL_FLOATS;
        memcpy(o, s, sizeof(float) * AMB_VOXEL_FLOATS);
        b->mdst[v] = b->msrc[v];
        if (b->msrc[v]) continue;
        int ijk[3] = { i, j, k };
        double acc[AMB_VOXEL_FLOATS] = { 0 };
        int cnt = 0;
        for (int a = 0; a < 3; a++) {
            for (int sgn = 1; sgn >= -1; sgn -= 2) {
                int q = ijk[a] + sgn, d = b->box.dim[a];
                if (q < 0 || q >= d) {
                    if (!b->wrap) continue;
                    q = (q + d) % d;
                }
                size_t nv = v + (size_t)q * b->stride[a] - (size_t)ijk[a] * b->stride[a];
                if (!b->msrc[nv]) continue;
                const float *n = b->src + nv * AMB_VOXEL_FLOATS;
                for (int c = 0; c < AMB_VOXEL_FLOATS; c++) acc[c] += n[c];
                cnt++;
            }
        }
        if (cnt) {
            for (int c = 0; c < AMB_VOXEL_FLOATS; c++) o[c] = (float)(acc[c] / cnt);
            b->mdst[v] = 1;
        }
    }
}

static void run(const AmbBakeParams *p, int n, AmbTaskFn fn, void *ctx) {
    if (p->pfor) p->pfor(p->pool, n, fn, ctx);
    else for (int i = 0; i < n; i++) fn(ctx, i);
}

static int box_voxels(const AmbBox *box, size_t *n) {
    size_t t = 1;
    for (int a = 0; a < 3; a++) {
        if (box->dim[a] <= 0 || box->dim[a] > 1 << 20) return AMB_ERR_ARG;
        /* a NaN camera converted to int arrives here as INT_MIN: refuse it before any org arithmetic can overflow */
        if (box->org[a] < -(1 << 24) || box->org[a] > 1 << 24) return AMB_ERR_ARG;
        t *= (size_t)box->dim[a];
    }
    /* rows are int task indices and voxels index float arrays of 18 per voxel */
    if ((size_t)box->dim[1] * (size_t)box->dim[2] > INT32_MAX || t > SIZE_MAX / (sizeof(float) * AMB_VOXEL_FLOATS))
        return AMB_ERR_ARG;
    *n = t;
    return AMB_OK;
}

static int bake(const AmbMap *m, const AmbBakeParams *p, const AmbBox *box, int wrap, float *vol, uint8_t *open_out) {
    size_t nv;
    if (!m || !p || !box || !vol || !(p->voxel_xy > 0) || !(p->voxel_z > 0) || p->blur_passes < 0 ||
        p->dilate_passes < 0 || box_voxels(box, &nv))
        return AMB_ERR_ARG;
    Bake b = { .m = m, .p = p, .box = *box, .st = { p->voxel_xy, p->voxel_xy, p->voxel_z },
               .soft2 = p->soft * p->soft, .wrap = wrap, .cancel = p->cancel };
    b.stride[0] = 1;
    b.stride[1] = (size_t)box->dim[0];
    b.stride[2] = (size_t)box->dim[0] * (size_t)box->dim[1];
    float *tmp = malloc(nv * sizeof(float) * AMB_VOXEL_FLOATS);
    uint8_t *masks = malloc(nv * 4);
    if (!tmp || !masks) { free(tmp); free(masks); return AMB_ERR_NOMEM; }
    b.open = masks;
    b.pass = masks + nv;
    int rows = box->dim[1] * box->dim[2];

    /* ping-pong between the caller's buffer and tmp; `cur` holds the latest stage */
    float *cur = vol, *other = tmp;
    b.dst = cur;
    run(p, rows, task_eval, &b);
    run(p, rows, task_faces, &b);
    for (int it = 0; it < p->blur_passes; it++) {
        b.src = cur;
        b.dst = other;
        run(p, rows, task_blur, &b);
        other = cur;
        cur = b.dst;
    }
    uint8_t *ma = masks + 2 * nv, *mb = masks + 3 * nv;
    memcpy(ma, b.open, nv);
    for (int it = 0; it < p->dilate_passes; it++) {
        b.src = cur;
        b.dst = other;
        b.msrc = ma;
        b.mdst = mb;
        run(p, rows, task_dilate, &b);
        other = cur;
        cur = b.dst;
        uint8_t *t = ma; ma = mb; mb = t;
    }
    int rc = cancelled(&b) ? AMB_ERR_CANCELLED : AMB_OK;   /* a cancelled bake's output is partial: nothing is copied out */
    if (rc == AMB_OK) {
        if (cur != vol) memcpy(vol, cur, nv * sizeof(float) * AMB_VOXEL_FLOATS);
        if (open_out) memcpy(open_out, b.open, nv);
    }
    free(tmp);
    free(masks);
    return rc;
}

int amb_bake_box(const AmbMap *m, const AmbBakeParams *p, const AmbBox *box, float *vol, uint8_t *open_out) {
    return p ? bake(m, p, box, p->wrap_edges != 0, vol, open_out) : AMB_ERR_ARG;
}

int amb_bake_region(const AmbMap *m, const AmbBakeParams *p, const AmbBox *box, float *vol, uint8_t *open_out) {
    size_t nv, ng;
    if (!p || !box || !vol || box_voxels(box, &nv)) return AMB_ERR_ARG;
    int g = amb_region_margin(p);
    AmbBox grown = *box;
    for (int a = 0; a < 3; a++) {
        if (box->dim[a] > INT32_MAX - 2 * g) return AMB_ERR_ARG;
        grown.org[a] -= g;
        grown.dim[a] += 2 * g;
    }
    if (box_voxels(&grown, &ng)) return AMB_ERR_ARG;
    float *gv = malloc(ng * sizeof(float) * AMB_VOXEL_FLOATS);
    uint8_t *go = open_out ? malloc(ng) : NULL;
    if (!gv || (open_out && !go)) { free(gv); free(go); return AMB_ERR_NOMEM; }
    int rc = bake(m, p, &grown, 0, gv, go);
    if (rc == AMB_OK) {
        for (int k = 0; k < box->dim[2]; k++)
            for (int j = 0; j < box->dim[1]; j++) {
                size_t d = ((size_t)k * (size_t)box->dim[1] + (size_t)j) * (size_t)box->dim[0];
                size_t s = ((size_t)(k + g) * (size_t)grown.dim[1] + (size_t)(j + g)) * (size_t)grown.dim[0] + (size_t)g;
                memcpy(vol + d * AMB_VOXEL_FLOATS, gv + s * AMB_VOXEL_FLOATS,
                       (size_t)box->dim[0] * sizeof(float) * AMB_VOXEL_FLOATS);
                if (go) memcpy(open_out + d, go + s, (size_t)box->dim[0]);
            }
    }
    free(gv);
    free(go);
    return rc;
}

/* ---------------------------------------------------------------------------------------------- GPU packing -- */

uint16_t amb_f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint16_t sign = (uint16_t)(x >> 16 & 0x8000);
    uint32_t ax = x & 0x7fffffffu;
    if (ax >= 0x7f800000u)                                      /* inf, NaN (payload top bits kept, forced quiet) */
        return (uint16_t)(sign | 0x7c00 | (ax > 0x7f800000u ? 0x200 | (ax >> 13 & 0x3ff) : 0));
    if (ax >= 0x477ff000u) return (uint16_t)(sign | 0x7c00);   /* >= 65520 = half way past 65504: ties go to inf */
    if (ax >= 0x38800000u) {                                    /* half normal: rebias 127 -> 15, round 13 bits */
        uint32_t h = (ax - 0x38000000u) >> 13, rem = ax & 0x1fffu;
        if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++; /* a carry into the exponent is the right result */
        return (uint16_t)(sign | h);
    }
    if (ax < 0x33000000u) return sign;                          /* < 2^-25 (incl. float denormals): 0 */
    /* half subnormal: value = mant * 2^(e - 150), result = round(value * 2^24) = mant >> (126 - e) rounded */
    uint32_t mant = (ax & 0x7fffffu) | 0x800000u, e = ax >> 23, sh = 126 - e;   /* sh in 14..24 */
    uint32_t h = mant >> sh, rem = mant & ((1u << sh) - 1), half = 1u << (sh - 1);
    if (rem > half || (rem == half && (h & 1))) h++;            /* h = 0x400 is the smallest normal: correct */
    return (uint16_t)(sign | h);
}

float amb_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = h >> 10 & 0x1f, mant = h & 0x3ffu, x;
    if (e == 0x1f) x = sign | 0x7f800000u | mant << 13;
    else if (e) x = sign | (e + 112) << 23 | mant << 13;
    else if (!mant) x = sign;
    else {                                                      /* subnormal: normalise */
        e = 113;
        while (!(mant & 0x400)) { mant <<= 1; e--; }
        x = sign | e << 23 | (mant & 0x3ffu) << 13;
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}

static void put_texel(uint8_t *t, const float rgb[3]) {
    uint16_t q[4] = { amb_f32_to_f16(rgb[0]), amb_f32_to_f16(rgb[1]), amb_f32_to_f16(rgb[2]), 0x3c00 /* 1.0 */ };
    memcpy(t, q, sizeof q);   /* little-endian halves R, G, B, A = D3DFMT_A16B16G16R16F */
}

void amb_pack_atlas(const float *vol, const int dim[3], void *dst, size_t row_pitch, size_t slice_pitch) {
    uint8_t *base = dst;
    for (int k = 0; k < dim[2]; k++)
        for (int j = 0; j < dim[1]; j++) {
            uint8_t *row = base + (size_t)k * slice_pitch + (size_t)j * row_pitch;
            for (int i = 0; i < dim[0]; i++) {
                const float *v = vol + (((size_t)k * (size_t)dim[1] + (size_t)j) * (size_t)dim[0] + (size_t)i) *
                                           AMB_VOXEL_FLOATS;
                for (int f = 0; f < AMB_FACES; f++) put_texel(row + ((size_t)f * (size_t)dim[0] + (size_t)i) * 8, v + 3 * f);
            }
        }
}

static int wrap_index(int a, int d) { int r = a % d; return r < 0 ? r + d : r; }

int amb_write_toroidal(const float *vol, const AmbBox *region, const int clip_dim[3], void *atlas, size_t row_pitch,
                       size_t slice_pitch) {
    size_t nv;
    if (!vol || !region || !clip_dim || !atlas || box_voxels(region, &nv)) return AMB_ERR_ARG;
    for (int a = 0; a < 3; a++)
        if (clip_dim[a] <= 0 || region->dim[a] > clip_dim[a]) return AMB_ERR_ARG;
    uint8_t *base = atlas;
    for (int k = 0; k < region->dim[2]; k++) {
        int tz = wrap_index(region->org[2] + k, clip_dim[2]);
        for (int j = 0; j < region->dim[1]; j++) {
            int ty = wrap_index(region->org[1] + j, clip_dim[1]);
            uint8_t *row = base + (size_t)tz * slice_pitch + (size_t)ty * row_pitch;
            for (int i = 0; i < region->dim[0]; i++) {
                int tx = wrap_index(region->org[0] + i, clip_dim[0]);
                const float *v = vol + (((size_t)k * (size_t)region->dim[1] + (size_t)j) * (size_t)region->dim[0] +
                                        (size_t)i) * AMB_VOXEL_FLOATS;
                size_t band = (size_t)AMB_BAND_TEXELS(clip_dim[0]);
                for (int f = 0; f < AMB_FACES; f++) {
                    put_texel(row + ((size_t)f * band + (size_t)tx) * 8, v + 3 * f);
                    if (tx == 0) put_texel(row + ((size_t)f * band + (size_t)clip_dim[0]) * 8, v + 3 * f);   /* gutter */
                }
            }
        }
    }
    return AMB_OK;
}
