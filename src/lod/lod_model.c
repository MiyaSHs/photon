/* F3: generated LOD levels for a one-LOD Source model. See lod_model.h; tools/f3_vtx_lod.py --quadric is the reference (same bytes). */
#include "lod_model.h"
#include "lod_simplify.h"
#include <stdlib.h>
#include <string.h>

/* optimize.h (public SDK 2013), packed */
enum { HDR_SZ = 36, BP_SZ = 8, MODEL_SZ = 8, LODH_SZ = 12, MESH_SZ = 9, SG_SZ = 25, STRIP_SZ = 27, VERT_SZ = 9, BSC_SZ = 8, MAXL = 8 };

typedef struct { int32_t ni, io, nv, vo; int16_t nbones; uint8_t flags; int32_t nbsc; const uint8_t *bsc; } Strip;
typedef struct { int32_t nv, ni, ns; uint8_t *v; uint16_t *idx; Strip *s; uint8_t flags; } Group;   /* v: nv x 9 bytes (Vertex_t) */
typedef struct { int32_t ng; Group *g; uint8_t flags; } Mesh;
typedef struct { int32_t nm; Mesh *m; float sw; } Lod;
typedef struct { int32_t nl; Lod l[MAXL]; } Model;
typedef struct { int32_t nmodel; Model *m; } Part;

static int32_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static int16_t rd16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }

/* every offset is checked against the buffer */
#define NEED(off, n) do { if ((off) < 0 || (size_t)(off) + (size_t)(n) > n_) goto bad; } while (0)

static void free_parts(Part *bp, int nbp) {
    if (!bp) return;
    for (int b = 0; b < nbp; b++) {
        for (int m = 0; m < bp[b].nmodel; m++)
            for (int l = 0; l < bp[b].m[m].nl; l++) {
                Lod *L = &bp[b].m[m].l[l];
                for (int k = 0; k < L->nm; k++) {
                    for (int g = 0; g < L->m[k].ng; g++) { free(L->m[k].g[g].v); free(L->m[k].g[g].idx); free(L->m[k].g[g].s); }
                    free(L->m[k].g);
                }
                free(L->m);
            }
        free(bp[b].m);
    }
    free(bp);
}

static int parse_vtx(const uint8_t *d, size_t n_, Part **out, int *nbp_out) {
    Part *bp = NULL;
    int nbp = 0;
    /* FileHeader_t: version +0, ..., checkSum +16, numLODs +20, materialReplacementListOffset +24, numBodyParts +28, bodyPartOffset +32 */
    if (n_ < HDR_SZ || rd32(d) != 7) return LOD_ERR_FORMAT;
    if (rd32(d + 20) != 1) return LOD_ERR_HAS_LODS;
    nbp = rd32(d + 28);
    int32_t bpo = rd32(d + 32);
    if (nbp < 0 || nbp > 4096) return LOD_ERR_FORMAT;
    bp = calloc((size_t)(nbp ? nbp : 1), sizeof *bp);
    if (!bp) return LOD_ERR_MEMORY;
    for (int b = 0; b < nbp; b++) {
        int32_t bo = bpo + BP_SZ * b;
        NEED(bo, BP_SZ);
        int32_t nm = rd32(d + bo), mo = rd32(d + bo + 4);
        if (nm < 0 || nm > 4096) goto bad;
        bp[b].nmodel = nm;
        bp[b].m = calloc((size_t)(nm ? nm : 1), sizeof *bp[b].m);
        if (!bp[b].m) goto mem;
        for (int m = 0; m < nm; m++) {
            int32_t mh = bo + mo + MODEL_SZ * m;
            NEED(mh, MODEL_SZ);
            int32_t nl = rd32(d + mh), lo = rd32(d + mh + 4);
            if (nl != 1) { free_parts(bp, nbp); return nl > 1 ? LOD_ERR_HAS_LODS : LOD_ERR_FORMAT; }
            bp[b].m[m].nl = 1;
            int32_t lh = mh + lo;
            NEED(lh, LODH_SZ);
            Lod *L = &bp[b].m[m].l[0];
            L->nm = rd32(d + lh);
            int32_t meo = rd32(d + lh + 4);
            L->sw = rdf(d + lh + 8);
            if (L->nm < 0 || L->nm > 65536) goto bad;
            L->m = calloc((size_t)(L->nm ? L->nm : 1), sizeof *L->m);
            if (!L->m) goto mem;
            for (int k = 0; k < L->nm; k++) {
                int32_t me = lh + meo + MESH_SZ * k;
                NEED(me, MESH_SZ);
                Mesh *M = &L->m[k];
                M->ng = rd32(d + me);
                int32_t sgo = rd32(d + me + 4);
                M->flags = d[me + 8];
                if (M->ng < 0 || M->ng > 65536) goto bad;
                M->g = calloc((size_t)(M->ng ? M->ng : 1), sizeof *M->g);
                if (!M->g) goto mem;
                for (int g = 0; g < M->ng; g++) {
                    int32_t so = me + sgo + SG_SZ * g;
                    NEED(so, SG_SZ);
                    Group *G = &M->g[g];
                    G->nv = rd32(d + so); int32_t vo = rd32(d + so + 4);
                    G->ni = rd32(d + so + 8); int32_t io = rd32(d + so + 12);
                    G->ns = rd32(d + so + 16); int32_t sto = rd32(d + so + 20);
                    G->flags = d[so + 24];
                    if (G->nv < 0 || G->ni < 0 || G->ns < 0 || G->nv > 65536 || G->ns > 65536) goto bad;
                    NEED(so + vo, (size_t)G->nv * VERT_SZ);
                    NEED(so + io, (size_t)G->ni * 2);
                    NEED(so + sto, (size_t)G->ns * STRIP_SZ);
                    G->v = malloc((size_t)(G->nv ? G->nv : 1) * VERT_SZ);
                    G->idx = malloc((size_t)(G->ni ? G->ni : 1) * 2);
                    G->s = calloc((size_t)(G->ns ? G->ns : 1), sizeof *G->s);
                    if (!G->v || !G->idx || !G->s) goto mem;
                    memcpy(G->v, d + so + vo, (size_t)G->nv * VERT_SZ);
                    memcpy(G->idx, d + so + io, (size_t)G->ni * 2);
                    for (int s = 0; s < G->ns; s++) {
                        int32_t sh = so + sto + STRIP_SZ * s;
                        Strip *S = &G->s[s];
                        S->ni = rd32(d + sh); S->io = rd32(d + sh + 4); S->nv = rd32(d + sh + 8); S->vo = rd32(d + sh + 12);
                        S->nbones = rd16(d + sh + 16); S->flags = d[sh + 18]; S->nbsc = rd32(d + sh + 19);
                        int32_t bsco = rd32(d + sh + 23);
                        if (S->nbsc < 0 || S->ni < 0 || S->io < 0 || S->io + S->ni > G->ni || S->vo < 0 || S->nv < 0 || S->vo + S->nv > G->nv) goto bad;
                        NEED(sh + bsco, (size_t)S->nbsc * BSC_SZ);
                        S->bsc = d + sh + bsco;   /* points into the input: valid for the call */
                    }
                }
            }
        }
    }
    *out = bp; *nbp_out = nbp;
    return LOD_OK;
bad:
    free_parts(bp, nbp);
    return LOD_ERR_FORMAT;
mem:
    free_parts(bp, nbp);
    return LOD_ERR_MEMORY;
}

/* a growable byte buffer */
typedef struct { uint8_t *p; size_t n, cap; int fail; } Buf;
static size_t buf_take(Buf *b, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->n + k) nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) { b->fail = 1; return 0; }
        b->p = np; b->cap = nc;
    }
    size_t at = b->n;
    memset(b->p + at, 0, k);
    b->n += k;
    return at;
}

static int write_vtx(const uint8_t *hdr, Part *bp, int nbp, int nlods, Buf *o) {
    buf_take(o, HDR_SZ);
    size_t bp_at = buf_take(o, (size_t)BP_SZ * nbp);
    for (int b = 0; b < nbp && !o->fail; b++) {
        size_t mo = buf_take(o, (size_t)MODEL_SZ * bp[b].nmodel);
        if (o->fail) break;
        wr32(o->p + bp_at + BP_SZ * b, bp[b].nmodel); wr32(o->p + bp_at + BP_SZ * b + 4, (int32_t)(mo - (bp_at + BP_SZ * b)));
        for (int m = 0; m < bp[b].nmodel && !o->fail; m++) {
            Model *Mo = &bp[b].m[m];
            size_t mh = mo + MODEL_SZ * m;
            size_t lo = buf_take(o, (size_t)LODH_SZ * Mo->nl);
            if (o->fail) break;
            wr32(o->p + mh, Mo->nl); wr32(o->p + mh + 4, (int32_t)(lo - mh));
            for (int l = 0; l < Mo->nl && !o->fail; l++) {
                Lod *L = &Mo->l[l];
                size_t lh = lo + LODH_SZ * l;
                size_t meo = buf_take(o, (size_t)MESH_SZ * L->nm);
                if (o->fail) break;
                wr32(o->p + lh, L->nm); wr32(o->p + lh + 4, (int32_t)(meo - lh)); memcpy(o->p + lh + 8, &L->sw, 4);
                for (int k = 0; k < L->nm && !o->fail; k++) {
                    Mesh *M = &L->m[k];
                    size_t me = meo + MESH_SZ * k;
                    size_t sgo = buf_take(o, (size_t)SG_SZ * M->ng);
                    if (o->fail) break;
                    wr32(o->p + me, M->ng); wr32(o->p + me + 4, (int32_t)(sgo - me)); o->p[me + 8] = M->flags;
                    for (int g = 0; g < M->ng && !o->fail; g++) {
                        Group *G = &M->g[g];
                        size_t so = sgo + SG_SZ * g;
                        size_t vo = buf_take(o, (size_t)VERT_SZ * G->nv);
                        if (o->fail) break;
                        memcpy(o->p + vo, G->v, (size_t)VERT_SZ * G->nv);
                        size_t io = buf_take(o, (size_t)2 * G->ni);
                        if (o->fail) break;
                        memcpy(o->p + io, G->idx, (size_t)2 * G->ni);
                        size_t sto = buf_take(o, (size_t)STRIP_SZ * G->ns);
                        if (o->fail) break;
                        for (int s = 0; s < G->ns && !o->fail; s++) {
                            Strip *S = &G->s[s];
                            size_t sh = sto + STRIP_SZ * s;
                            size_t bsco = buf_take(o, (size_t)BSC_SZ * S->nbsc);
                            if (o->fail) break;
                            memcpy(o->p + bsco, S->bsc, (size_t)BSC_SZ * S->nbsc);
                            wr32(o->p + sh, S->ni); wr32(o->p + sh + 4, S->io); wr32(o->p + sh + 8, S->nv); wr32(o->p + sh + 12, S->vo);
                            memcpy(o->p + sh + 16, &S->nbones, 2); o->p[sh + 18] = S->flags; wr32(o->p + sh + 19, S->nbsc);
                            wr32(o->p + sh + 23, (int32_t)(bsco - sh));
                        }
                        if (o->fail) break;
                        wr32(o->p + so, G->nv); wr32(o->p + so + 4, (int32_t)(vo - so)); wr32(o->p + so + 8, G->ni); wr32(o->p + so + 12, (int32_t)(io - so));
                        wr32(o->p + so + 16, G->ns); wr32(o->p + so + 20, (int32_t)(sto - so)); o->p[so + 24] = G->flags;
                    }
                }
            }
        }
    }
    while (!o->fail && o->n % 4) buf_take(o, 1);
    size_t mro = buf_take(o, (size_t)8 * nlods);
    if (o->fail) return LOD_ERR_MEMORY;
    memcpy(o->p, hdr, 20);
    wr32(o->p + 20, nlods); wr32(o->p + 24, (int32_t)mro); wr32(o->p + 28, nbp); wr32(o->p + 32, (int32_t)bp_at);
    return LOD_OK;
}

/* .mdl (studiohdr_t v48): bones +156/+160 (mstudiobone_t 216 bytes: parent +4, flags +160); body parts +232/+236 (16 bytes: nummodels +4,
 * modelindex +12); mstudiomodel_t 148 bytes (nummeshes +72, meshindex +76, vertexindex +84); mstudiomesh_t 116 bytes (vertexoffset +12,
 * numflexes +16). */
static int mdl_model(const uint8_t *m, size_t n_, int b, int k, int32_t *mo_out) {
    if (n_ < 240) return 0;
    int32_t nbp = rd32(m + 232), bpi = rd32(m + 236);
    if (b >= nbp) return 0;
    int32_t o = bpi + 16 * b;
    NEED(o, 16);
    if (k >= rd32(m + o + 4)) return 0;
    int32_t mo = o + rd32(m + o + 12) + 148 * k;
    NEED(mo, 148);
    *mo_out = mo;
    return 1;
bad:
    return 0;
}
static int mdl_mesh(const uint8_t *m, size_t n_, int32_t mo, int j, int32_t *base, int32_t *nflex) {
    int32_t nmesh = rd32(m + mo + 72), meshi = rd32(m + mo + 76);
    if (j >= nmesh) return 0;
    int32_t me = mo + meshi + 116 * j;
    NEED(me, 116);
    *base = rd32(m + mo + 84) / 48 + rd32(m + me + 12);
    *nflex = rd32(m + me + 16);
    return 1;
bad:
    return 0;
}
static int mdl_has_flex(const uint8_t *m, size_t n_) {
    int32_t nbp = n_ >= 240 ? rd32(m + 232) : 0;
    for (int b = 0; b < nbp; b++) {
        int32_t o = rd32(m + 236) + 16 * b;
        if (o < 0 || (size_t)o + 16 > n_) return 0;
        for (int k = 0; k < rd32(m + o + 4); k++) {
            int32_t mo, base, nf;
            if (!mdl_model(m, n_, b, k, &mo)) break;
            for (int j = 0; mdl_mesh(m, n_, mo, j, &base, &nf); j++) if (nf > 0) return 1;
        }
    }
    return 0;
}
static void mdl_set_lod_bones(uint8_t *m, size_t n_, int level, const uint8_t *used, int nused) {
    int32_t nb = rd32(m + 156), bi = rd32(m + 160);
    uint8_t *todo = calloc((size_t)(nb > 0 ? nb : 1), 1);
    if (!todo) return;
    for (int b = 0; b < nb && b < nused; b++) todo[b] = used[b];
    for (int again = 1; again;) {   /* mark, then walk up to the parents until nothing changes */
        again = 0;
        for (int b = 0; b < nb; b++) {
            if (!todo[b]) continue;
            todo[b] = 0;
            int32_t o = bi + 216 * b;
            if (o < 0 || (size_t)o + 216 > n_) continue;
            int32_t f = rd32(m + o + 160);
            if (f & (0x400 << level)) continue;
            wr32(m + o + 160, f | (0x400 << level));
            int32_t parent = rd32(m + o + 4);
            if (parent >= 0 && parent < nb) { todo[parent] = 1; again = 1; }
        }
    }
    free(todo);
}

/* one strip group through lod_simplify: classes = (strip, bones), output one strip per input strip, contiguous */
static int simplify_group(const Group *G, const float *pos, int32_t npos, int32_t base, double ratio, Group *O, double *err) {
    memset(O, 0, sizeof *O);
    O->flags = G->flags;
    int32_t *strip_of = calloc((size_t)(G->nv ? G->nv : 1), sizeof *strip_of);
    float *P = malloc(sizeof *P * 3 * (size_t)(G->nv ? G->nv : 1));
    uint32_t *C = malloc(sizeof *C * (size_t)(G->nv ? G->nv : 1));
    uint32_t *I = malloc(sizeof *I * (size_t)(G->ni ? G->ni : 1)), *Out = malloc(sizeof *Out * (size_t)(G->ni ? G->ni : 1));
    int32_t *keep = malloc(sizeof *keep * (size_t)(G->nv ? G->nv : 1)), *newi = malloc(sizeof *newi * (size_t)(G->nv ? G->nv : 1));
    uint64_t *ckey = malloc(sizeof *ckey * (size_t)(G->nv ? G->nv : 1));
    int rc = LOD_ERR_MEMORY;
    if (!strip_of || !P || !C || !I || !Out || !keep || !newi || !ckey) goto out;
    if (G->ns > 256) { rc = LOD_ERR_STRIPS; goto out; }
    for (int s = 0; s < G->ns; s++) {
        if (!(G->s[s].flags & 1)) { rc = LOD_ERR_STRIPS; goto out; }
        for (int i = G->s[s].vo; i < G->s[s].vo + G->s[s].nv; i++) strip_of[i] = s;
    }
    int32_t ni = 0;
    for (int s = 0; s < G->ns; s++) for (int t = 0; t < G->s[s].ni; t++) I[ni++] = G->idx[G->s[s].io + t];
    for (int i = 0; i < G->nv; i++) {
        const uint8_t *v = G->v + VERT_SZ * i;
        int32_t orig = v[4] | v[5] << 8;
        if (base + orig >= npos) { rc = LOD_ERR_FORMAT; goto out; }
        memcpy(P + 3 * i, pos + 3 * (size_t)(base + orig), 12);
        /* class: the strip and the bone bytes (weight indices, count, bone ids), packed into one key; equal keys share a class (the
         * numbering itself does not matter to lod_simplify, only equality) */
        C[i] = 0;
        ckey[i] = ((uint64_t)(uint8_t)strip_of[i] << 56) | (uint64_t)v[0] | ((uint64_t)v[1] << 8) | ((uint64_t)v[2] << 16) | ((uint64_t)v[3] << 24) |
                  ((uint64_t)v[6] << 32) | ((uint64_t)v[7] << 40) | ((uint64_t)v[8] << 48);   /* exact: at most 256 strips (checked above) */
    }
    /* dense class ids through an open-addressing table sized to a power of two above 2 x vertices */
    {
        uint32_t cap = 16;
        while (cap < 2u * (uint32_t)G->nv) cap <<= 1;
        uint64_t *hk = malloc(sizeof *hk * cap);
        uint32_t *hv = malloc(sizeof *hv * cap);
        uint8_t *hu = calloc(cap, 1);
        if (!hk || !hv || !hu) { free(hk); free(hv); free(hu); goto out; }
        uint32_t next = 0;
        for (int i = 0; i < G->nv; i++) {
            uint32_t h = (uint32_t)((ckey[i] * 0x9E3779B97F4A7C15ull) >> 40) & (cap - 1);
            while (hu[h] && hk[h] != ckey[i]) h = (h + 1) & (cap - 1);
            if (!hu[h]) { hu[h] = 1; hk[h] = ckey[i]; hv[h] = next++; }
            C[i] = hv[h];
        }
        free(hk); free(hv); free(hu);
    }
    for (int32_t t = 0; t < ni; t++) if (I[t] >= (uint32_t)G->nv) { rc = LOD_ERR_FORMAT; goto out; }
    LodParams lp = { (uint32_t)((double)(ni / 3) * ratio), 0, 100.0 };
    LodResult r;
    if (lod_simplify(P, C, (uint32_t)G->nv, I, (uint32_t)(ni / 3), &lp, Out, &r)) goto out;
    if (r.error > *err) *err = r.error;
    /* kept vertices ordered by (strip, original index) */
    int32_t nk = 0;
    for (int i = 0; i < G->nv; i++) newi[i] = -1;
    for (uint32_t t = 0; t < 3 * r.tris; t++) newi[Out[t]] = 0;
    for (int s = 0; s < G->ns; s++)
        for (int i = 0; i < G->nv; i++) if (strip_of[i] == s && newi[i] == 0) { keep[nk] = i; newi[i] = nk + 1; nk++; }
    for (int i = 0; i < G->nv; i++) if (newi[i] > 0) newi[i]--;
    O->nv = nk; O->ni = (int32_t)(3 * r.tris);
    O->v = malloc((size_t)(nk ? nk : 1) * VERT_SZ);
    O->idx = malloc((size_t)(O->ni ? O->ni : 1) * 2);
    O->s = calloc((size_t)(G->ns ? G->ns : 1), sizeof *O->s);
    if (!O->v || !O->idx || !O->s) goto out;
    for (int k = 0; k < nk; k++) memcpy(O->v + VERT_SZ * k, G->v + VERT_SZ * keep[k], VERT_SZ);
    int32_t w = 0;
    for (int s = 0; s < G->ns; s++) {
        int32_t io = w, vfirst = -1, vn = 0;
        for (int k = 0; k < nk; k++) if (strip_of[keep[k]] == s) { if (vfirst < 0) vfirst = k; vn++; }
        for (uint32_t t = 0; t < r.tris; t++) {
            if (strip_of[Out[3 * t]] != s) continue;
            for (int c = 0; c < 3; c++) O->idx[w++] = (uint16_t)newi[Out[3 * t + c]];
        }
        if (w == io) continue;
        Strip *S = &O->s[O->ns++];
        *S = G->s[s];
        S->ni = w - io; S->io = io; S->nv = vn; S->vo = vfirst < 0 ? 0 : vfirst;
    }
    rc = LOD_OK;
out:
    free(strip_of); free(P); free(C); free(I); free(Out); free(keep); free(newi); free(ckey);
    if (rc != LOD_OK) { free(O->v); free(O->idx); free(O->s); memset(O, 0, sizeof *O); }
    return rc;
}

int lod_model_build(const uint8_t *mdl, size_t mdl_n, const uint8_t *vvd, size_t vvd_n, const uint8_t *vtx, size_t vtx_n, const double *ratios,
                    int nratios, LodModelOut *out) {
    memset(out, 0, sizeof *out);
    if (nratios < 1 || nratios > MAXL - 1 || vvd_n < 64 || memcmp(vvd, "IDSV", 4) != 0 || mdl_n < 240) return LOD_ERR_FORMAT;
    if (rd32(vvd + 48) != 0) return LOD_ERR_FIXUPS;
    if (mdl_has_flex(mdl, mdl_n)) return LOD_ERR_FLEX;
    int32_t nv0 = rd32(vvd + 16), vdo = rd32(vvd + 56);
    if (nv0 <= 0) return LOD_ERR_EMPTY;
    if (vdo < 0 || (size_t)vdo + 48 * (size_t)nv0 > vvd_n) return LOD_ERR_FORMAT;
    float *pos = malloc(sizeof *pos * 3 * (size_t)nv0);
    if (!pos) return LOD_ERR_MEMORY;
    for (int32_t i = 0; i < nv0; i++) memcpy(pos + 3 * i, vvd + vdo + 48 * (size_t)i + 16, 12);
    Part *bp = NULL;
    int nbp = 0, rc = parse_vtx(vtx, vtx_n, &bp, &nbp);
    uint8_t *m2 = NULL, *used = NULL;
    int32_t nbones = rd32(mdl + 156);
    if (rc) goto out;
    rc = LOD_ERR_MEMORY;
    m2 = malloc(mdl_n);
    used = malloc((size_t)(nbones > 0 ? nbones : 1));
    if (!m2 || !used) goto out;
    memcpy(m2, mdl, mdl_n);
    for (int b = 0; b < nbp; b++) for (int k = 0; k < bp[b].nmodel; k++)
        for (int j = 0; j < bp[b].m[k].l[0].nm; j++) for (int g = 0; g < bp[b].m[k].l[0].m[j].ng; g++)
            out->tris[0] += (uint32_t)bp[b].m[k].l[0].m[j].g[g].ni / 3;
    for (int level = 1; level <= nratios; level++) {
        memset(used, 0, (size_t)(nbones > 0 ? nbones : 1));
        for (int b = 0; b < nbp; b++) {
            for (int k = 0; k < bp[b].nmodel; k++) {
                Model *Mo = &bp[b].m[k];
                Lod *L0 = &Mo->l[0], *L = &Mo->l[level];
                int32_t mo;
                if (!mdl_model(mdl, mdl_n, b, k, &mo)) { rc = LOD_ERR_FORMAT; goto out; }
                double err = 0;
                L->nm = L0->nm;
                L->m = calloc((size_t)(L0->nm ? L0->nm : 1), sizeof *L->m);
                Mo->nl = level + 1;
                if (!L->m) goto out;
                for (int j = 0; j < L0->nm; j++) {
                    int32_t base, nf;
                    if (!mdl_mesh(mdl, mdl_n, mo, j, &base, &nf)) { rc = LOD_ERR_FORMAT; goto out; }
                    Mesh *M0 = &L0->m[j], *M = &L->m[j];
                    M->flags = M0->flags; M->ng = M0->ng;
                    M->g = calloc((size_t)(M0->ng ? M0->ng : 1), sizeof *M->g);
                    if (!M->g) goto out;
                    for (int g = 0; g < M0->ng; g++) {
                        int r2 = simplify_group(&M0->g[g], pos, nv0, base, ratios[level - 1], &M->g[g], &err);
                        if (r2) { rc = r2; goto out; }
                        out->tris[level] += (uint32_t)M->g[g].ni / 3;
                        for (int v = 0; v < M->g[g].nv; v++) {   /* the bones this level's vertices are weighted to */
                            const uint8_t *vx = M->g[g].v + VERT_SZ * v;
                            const uint8_t *w = vvd + vdo + 48 * (size_t)(base + (vx[4] | vx[5] << 8));
                            float wt[3]; memcpy(wt, w, 12);
                            for (int c = 0; c < w[15] && c < 3; c++) if (wt[c] > 0 && (int8_t)w[12 + c] >= 0 && (int8_t)w[12 + c] < nbones) used[(int8_t)w[12 + c]] = 1;
                        }
                    }
                }
                float sw = (float)(200.0 * err);
                if (sw < Mo->l[level - 1].sw + 1.0f) sw = Mo->l[level - 1].sw + 1.0f;
                L->sw = sw;
                if (b == 0 && k == 0) out->switch_point[level] = sw;
            }
        }
        mdl_set_lod_bones(m2, mdl_n, level, used, nbones);
    }
    Buf o = { 0 };
    rc = write_vtx(vtx, bp, nbp, nratios + 1, &o);
    if (rc) { free(o.p); goto out; }
    out->vtx = o.p; out->vtx_n = o.n;
    out->vvd = malloc(vvd_n);
    if (!out->vvd) { rc = LOD_ERR_MEMORY; goto out; }
    memcpy(out->vvd, vvd, vvd_n);
    out->vvd_n = vvd_n;
    wr32(out->vvd + 12, nratios + 1);
    for (int l = 0; l < MAXL; l++) wr32(out->vvd + 16 + 4 * l, l <= nratios ? nv0 : 0);
    out->mdl = m2; out->mdl_n = mdl_n; m2 = NULL;
    out->levels = nratios;
    rc = LOD_OK;
out:
    free(pos); free(used); free(m2); free_parts(bp, nbp);
    if (rc) lod_model_free(out);
    return rc;
}

void lod_model_free(LodModelOut *out) {
    free(out->mdl); free(out->vvd); free(out->vtx);
    memset(out, 0, sizeof *out);
}
