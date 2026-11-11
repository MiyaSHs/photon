/* L4: path-traced lightmaps in the map's own layout. See l4_bake.h; the model is tools/l4_bounce.c's (analysis/39). */
#include "l4_bake.h"
#include "lg_map.h"
#include "sun_vis.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int ofs, len, ver; char cc[4]; } Lump;
typedef struct { float n[3], d; int type; } Plane;
typedef struct { float tv[2][4], lv[2][4]; int flags, texdata; } TexInfo;
typedef struct { float refl[3]; int name, w, h, vw, vh; } TexData;
typedef struct { uint16_t plane; int16_t texinfo, dispinfo, bevel; } BrushSide;
#pragma pack(push, 1)
typedef struct {
    uint16_t plane; uint8_t side, on_node; int32_t first_edge; int16_t nedges, texinfo, dispinfo, fog;
    uint8_t styles[4]; int32_t lightofs; float area; int32_t lm_mins[2], lm_size[2]; int32_t orig; uint16_t nprims, first_prim; uint32_t smoothing;
} Face;
#pragma pack(pop)
enum { SURF_SKY = 0x4, SURF_NOLIGHT = 0x400, SURF_BUMPLIGHT = 0x800, CONTENTS_SOLID = 1 };

typedef struct {
    int ok;                 /* baked by this module (flat, lit, has luxels) */
    int w, h, nstyle, bump;
    double n[3];
    float *acc;             /* nstyle x w x h x 3: indirect sums over the paths so far */
    float *dir;             /* nstyle x nmap x w x h x 3: direct light per lightmap (nmap 4 for bump faces: flat + the 3 bump basis
                             * normals, vrad's layout), computed on the first pass (deterministic) */
    double bn[3][3];        /* bump faces: the three basis normals in world space */
    int samples;            /* paths per luxel accumulated so far */
    float *pos;             /* w x h x 3: the sample point (lifted 1 unit, moved out of solid) */
} FaceBake;

struct L4Bake {
    const uint8_t *bsp; size_t size;
    int bounces, hdr, lump_light, nf, np, nt, ntd, nbs, nlit;
    const Plane *planes; const TexInfo *ti; const TexData *td; const BrushSide *bs; const Face *faces; const uint8_t *light;
    LgMap *lm; SunMap *sm;
    const LgWorldLight *wl; int nl;
    FaceBake *fb;
};

static const void *lump(const L4Bake *b, int i, int rec, int *n) {
    const Lump *l = (const Lump *)(b->bsp + 8 + 16 * i);
    *n = l->len > 0 && (size_t)l->ofs + (size_t)l->len <= b->size ? l->len / rec : 0;
    return *n ? b->bsp + l->ofs : NULL;
}
static int solve3(const double m[3][3], const double r[3], double x[3]) {
    double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (fabs(d) < 1e-12) return 0;
    for (int c = 0; c < 3; c++) {
        double t[3][3];
        memcpy(t, m, sizeof t);
        for (int k = 0; k < 3; k++) t[k][c] = r[k];
        x[c] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / d;
    }
    return 1;
}
static int in_solid(const SunMap *sm, const double p[3]) {
    int lf = sun_point_leaf(sm, p);
    return lf < 0 || (sun_leaf_contents(sm, lf) & CONTENTS_SOLID);
}

int l4_bake_new(const uint8_t *bsp, size_t size, int bounces, L4Bake **out) {
    *out = NULL;
    if (size < 1036 || memcmp(bsp, "VBSP", 4) != 0) return 1;
    L4Bake *b = calloc(1, sizeof *b);
    if (!b) return 2;
    b->bsp = bsp; b->size = size; b->bounces = bounces < 0 ? 0 : bounces > 8 ? 8 : bounces;
    int nlh;
    lump(b, 53, 1, &nlh);
    b->hdr = nlh > 0;
    b->lump_light = b->hdr ? 53 : 8;
    b->planes = lump(b, 1, sizeof(Plane), &b->np);
    b->td = lump(b, 2, sizeof(TexData), &b->ntd);
    b->ti = lump(b, 6, sizeof(TexInfo), &b->nt);
    b->bs = lump(b, 19, sizeof(BrushSide), &b->nbs);
    b->faces = lump(b, b->hdr ? 58 : 7, sizeof(Face), &b->nf);
    if (b->hdr && !b->faces) b->faces = lump(b, 7, sizeof(Face), &b->nf);
    b->light = lump(b, b->lump_light, 1, &b->nlit);
    if (!b->planes || !b->ti || !b->faces || !b->light || lg_map_load(bsp, size, 1, &b->lm) || sun_map_load(bsp, size, 1, &b->sm)) { l4_bake_free(b); return 3; }
    b->wl = lg_map_lights(b->lm, &b->nl);
    b->fb = calloc((size_t)b->nf, sizeof *b->fb);
    if (!b->fb) { l4_bake_free(b); return 2; }
    for (int fi = 0; fi < b->nf; fi++) {
        const Face *fc = &b->faces[fi];
        FaceBake *F = &b->fb[fi];
        if (fc->lightofs < 0 || fc->dispinfo >= 0 || fc->texinfo < 0 || fc->texinfo >= b->nt || fc->plane >= b->np) continue;
        const TexInfo *t = &b->ti[fc->texinfo];
        if (t->flags & (SURF_SKY | SURF_NOLIGHT)) continue;
        int ns = 0;
        while (ns < 4 && fc->styles[ns] != 255) ns++;
        if (!ns) continue;
        F->w = fc->lm_size[0] + 1; F->h = fc->lm_size[1] + 1; F->nstyle = ns; F->bump = (t->flags & SURF_BUMPLIGHT) != 0;
        long need = (long)F->w * F->h * 4 * ns * (F->bump ? 4 : 1);
        if ((long)fc->lightofs + need > b->nlit) continue;
        double pn[3] = { b->planes[fc->plane].n[0], b->planes[fc->plane].n[1], b->planes[fc->plane].n[2] }, pd = b->planes[fc->plane].d;
        /* no flip for fc->side: the face's plane number already names its own facing plane (side == planenum & 1 on every face; flipping
 * again put the luxels behind ~1 in 5 side-1 faces, the dark walls of the 2026-11-09 mode screenshots) */
        memcpy(F->n, pn, sizeof F->n);
        if (F->bump) {   /* vrad's bump basis (tangent space) to world: S and T from the texture vectors, orthogonal to the normal */
            static const double basis[3][3] = { { 0.81649658092772603, 0, 0.57735026918962573 },
                                                { -0.40824829046386302, 0.70710678118654746, 0.57735026918962573 },
                                                { -0.40824829046386302, -0.70710678118654746, 0.57735026918962573 } };
            double S[3] = { t->tv[0][0], t->tv[0][1], t->tv[0][2] }, T[3] = { t->tv[1][0], t->tv[1][1], t->tv[1][2] };
            double ds = S[0] * pn[0] + S[1] * pn[1] + S[2] * pn[2], dt = T[0] * pn[0] + T[1] * pn[1] + T[2] * pn[2];
            for (int c = 0; c < 3; c++) { S[c] -= ds * pn[c]; T[c] -= dt * pn[c]; }
            double ls = sqrt(S[0] * S[0] + S[1] * S[1] + S[2] * S[2]), lt = sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
            if (ls > 1e-9 && lt > 1e-9) {
                for (int c = 0; c < 3; c++) { S[c] /= ls; T[c] /= lt; }
                double cr[3] = { S[1] * T[2] - S[2] * T[1], S[2] * T[0] - S[0] * T[2], S[0] * T[1] - S[1] * T[0] };
                if (cr[0] * pn[0] + cr[1] * pn[1] + cr[2] * pn[2] < 0) for (int c = 0; c < 3; c++) T[c] = -T[c];   /* left-handed */
                /* Source's texture T axis runs down the image (v grows downward), so the tangent-space +y is -T */
                for (int k = 0; k < 3; k++)
                    for (int c = 0; c < 3; c++) F->bn[k][c] = basis[k][0] * S[c] - basis[k][1] * T[c] + basis[k][2] * pn[c];
            } else F->bump = 0;
        }
        F->acc = calloc((size_t)ns * F->w * F->h * 3, sizeof *F->acc);
        F->dir = calloc((size_t)ns * (F->bump ? 4 : 1) * F->w * F->h * 3, sizeof *F->dir);
        F->pos = malloc(sizeof *F->pos * 3 * (size_t)F->w * F->h);
        if (!F->acc || !F->dir || !F->pos) { l4_bake_free(b); return 2; }
        /* luxel positions (as l3_lmpredict / vrad), lifted 1 unit; one in solid (a border luxel) moves toward the face's middle */
        double m[3][3] = { { t->lv[0][0], t->lv[0][1], t->lv[0][2] }, { t->lv[1][0], t->lv[1][1], t->lv[1][2] }, { pn[0], pn[1], pn[2] } };
        double mid[3], rm[3] = { fc->lm_mins[0] + 0.5 * (F->w - 1) - t->lv[0][3], fc->lm_mins[1] + 0.5 * (F->h - 1) - t->lv[1][3], pd };
        if (!solve3(m, rm, mid)) continue;
        for (int c = 0; c < 3; c++) mid[c] += pn[c];
        int good = 1;
        for (int j = 0; j < F->h && good; j++)
            for (int i = 0; i < F->w; i++) {
                double r[3] = { fc->lm_mins[0] + i - t->lv[0][3], fc->lm_mins[1] + j - t->lv[1][3], pd }, p[3];
                if (!solve3(m, r, p)) { good = 0; break; }
                for (int c = 0; c < 3; c++) p[c] += pn[c];
                for (int k = 0; k < 8 && in_solid(b->sm, p); k++)
                    for (int c = 0; c < 3; c++) p[c] += (mid[c] - p[c]) * 0.5;
                float *o = F->pos + 3 * (j * F->w + i);
                o[0] = (float)p[0]; o[1] = (float)p[1]; o[2] = (float)p[2];
            }
        F->ok = good;
    }
    *out = b;
    return 0;
}

int l4_bake_faces(const L4Bake *b) { return b->nf; }
long l4_bake_luxels(const L4Bake *b) {
    long n = 0;
    for (int i = 0; i < b->nf; i++) if (b->fb[i].ok) n += (long)b->fb[i].w * b->fb[i].h * b->fb[i].nstyle;
    return n;
}

typedef struct { L4Bake *b; SunScratch *scr; uint64_t rng; } Ctx;
static double rnd(Ctx *c) { c->rng ^= c->rng << 13; c->rng ^= c->rng >> 7; c->rng ^= c->rng << 17; return (double)(c->rng >> 11) / 9007199254740992.0; }

/* vrad's direct light of every worldlight of `style` at q (normal n), with shadow rays, RGB */
static void direct_rgb(Ctx *c, int style, const double q[3], const double n[3], double out[3]) {
    const L4Bake *b = c->b;
    out[0] = out[1] = out[2] = 0;
    for (int li = 0; li < b->nl; li++) {
        const LgWorldLight *L = &b->wl[li];
        if (L->style != style) continue;
        double f = 0;
        if (L->type == LG_EMIT_POINT || L->type == LG_EMIT_SPOT) {
            double v[3] = { L->origin[0] - q[0], L->origin[1] - q[1], L->origin[2] - q[2] };
            double d = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (d < 1e-3 || (L->radius > 0 && d > L->radius)) continue;
            double l[3] = { v[0] / d, v[1] / d, v[2] / d };
            double ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
            if (ndl <= 0) continue;
            double cc = L->constant_attn, li2 = L->linear_attn, qq = L->quadratic_attn;
            if (cc == 0 && li2 == 0 && qq == 0) cc = 1;
            f = ndl / (cc + li2 * d + qq * d * d);
            if (L->type == LG_EMIT_SPOT) {
                double dot2 = -(l[0] * L->normal[0] + l[1] * L->normal[1] + l[2] * L->normal[2]);
                if (dot2 <= L->stopdot2) continue;
                if (dot2 <= L->stopdot) f *= pow((dot2 - L->stopdot2) / (L->stopdot - L->stopdot2), L->exponent > 0 ? L->exponent : 1);
            }
            if (f <= 1e-6) continue;
            double o[3] = { L->origin[0], L->origin[1], L->origin[2] };
            SunTrace tr;
            sun_trace(b->sm, c->scr, q, o, SUN_MASK_OPAQUE, &tr);
            if (tr.fraction < 0.999) continue;
        } else if (L->type == LG_EMIT_SKYLIGHT) {
            double ndl = -(n[0] * L->normal[0] + n[1] * L->normal[1] + n[2] * L->normal[2]);
            if (ndl <= 0 || sun_visible(b->sm, c->scr, q) != SUN_LIT) continue;
            f = ndl;
        } else continue;
        for (int k = 0; k < 3; k++) out[k] += f * L->intensity[k];
    }
}
static void sky_rgb(const L4Bake *b, int style, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (int li = 0; li < b->nl; li++)
        if (b->wl[li].type == LG_EMIT_SKYAMBIENT && b->wl[li].style == style)
            for (int k = 0; k < 3; k++) out[k] += b->wl[li].intensity[k];
}
static void cos_dir(Ctx *c, const double n[3], double dir[3]) {
    double tx[3] = { fabs(n[0]) < 0.9 ? 1 : 0, fabs(n[0]) < 0.9 ? 0 : 1, 0 }, ty[3];
    double dd = tx[0] * n[0] + tx[1] * n[1] + tx[2] * n[2];
    for (int k = 0; k < 3; k++) tx[k] -= dd * n[k];
    double tl = sqrt(tx[0] * tx[0] + tx[1] * tx[1] + tx[2] * tx[2]);
    for (int k = 0; k < 3; k++) tx[k] /= tl;
    ty[0] = n[1] * tx[2] - n[2] * tx[1]; ty[1] = n[2] * tx[0] - n[0] * tx[2]; ty[2] = n[0] * tx[1] - n[1] * tx[0];
    double u1 = rnd(c), u2 = rnd(c), rr = sqrt(u1), ph = 6.283185307179586 * u2;
    double lx = rr * cos(ph), ly = rr * sin(ph), lz = sqrt(1 - u1 > 0 ? 1 - u1 : 0);
    for (int k = 0; k < 3; k++) dir[k] = lx * tx[k] + ly * ty[k] + lz * n[k];
}
/* indirect light at q (normal n) for `style`: one path of up to b->bounces surfaces */
static void path_rgb(Ctx *c, int style, const double q0[3], const double n0[3], const double sky[3], double out[3]) {
    const L4Bake *b = c->b;
    double q[3] = { q0[0], q0[1], q0[2] }, n[3] = { n0[0], n0[1], n0[2] }, thr[3] = { 1, 1, 1 };
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < b->bounces; k++) {
        double dir[3];
        cos_dir(c, n, dir);
        double e[3] = { q[0] + dir[0] * 32768, q[1] + dir[1] * 32768, q[2] + dir[2] * 32768 };
        SunTrace tr;
        sun_trace(b->sm, c->scr, q, e, SUN_MASK_OPAQUE, &tr);
        if (tr.fraction >= 0.999 || (tr.surf_flags & SURF_SKY)) { for (int m = 0; m < 3; m++) out[m] += thr[m] * sky[m]; return; }
        double y[3] = { q[0] + (e[0] - q[0]) * tr.fraction, q[1] + (e[1] - q[1]) * tr.fraction, q[2] + (e[2] - q[2]) * tr.fraction };
        double hn[3] = { -dir[0], -dir[1], -dir[2] }, refl[3] = { 0.3, 0.3, 0.3 };
        if (tr.hit == SUN_HIT_BRUSH && tr.side >= 0 && tr.side < b->nbs && b->bs[tr.side].plane < b->np) {
            const Plane *hp = &b->planes[b->bs[tr.side].plane];
            hn[0] = hp->n[0]; hn[1] = hp->n[1]; hn[2] = hp->n[2];
            if (hn[0] * dir[0] + hn[1] * dir[1] + hn[2] * dir[2] > 0) { hn[0] = -hn[0]; hn[1] = -hn[1]; hn[2] = -hn[2]; }
            int tix = tr.texinfo >= 0 ? tr.texinfo : b->bs[tr.side].texinfo;
            if (tix >= 0 && tix < b->nt && b->ti[tix].texdata >= 0 && b->ti[tix].texdata < b->ntd)
                for (int m = 0; m < 3; m++) refl[m] = b->td[b->ti[tix].texdata].refl[m];
        }
        for (int m = 0; m < 3; m++) thr[m] *= refl[m];
        double y1[3] = { y[0] + hn[0], y[1] + hn[1], y[2] + hn[2] }, d[3];
        direct_rgb(c, style, y1, hn, d);
        for (int m = 0; m < 3; m++) out[m] += thr[m] * d[m];
        memcpy(q, y1, sizeof q); memcpy(n, hn, sizeof n);
        if (thr[0] + thr[1] + thr[2] < 3e-3) return;
    }
}

void l4_bake_pass(L4Bake *b, int f0, int f1, int samples, uint64_t seed) {
    Ctx c = { b, sun_scratch_new(b->sm), seed | 1 };
    if (!c.scr) return;
    if (f1 > b->nf) f1 = b->nf;
    for (int fi = f0 < 0 ? 0 : f0; fi < f1; fi++) {
        FaceBake *F = &b->fb[fi];
        if (!F->ok) continue;
        for (int s = 0; s < F->nstyle; s++) {
            int style = b->faces[fi].styles[s];
            double sky[3];
            sky_rgb(b, style, sky);
            for (int i = 0; i < F->w * F->h; i++) {
                double q[3] = { F->pos[3 * i], F->pos[3 * i + 1], F->pos[3 * i + 2] }, d[3], acc[3] = { 0, 0, 0 };
                if (F->samples == 0) {   /* direct light once (no randomness): the flat normal, then each bump basis normal */
                    int nm = F->bump ? 4 : 1;
                    for (int bv = 0; bv < nm; bv++) {
                        direct_rgb(&c, style, q, bv ? F->bn[bv - 1] : F->n, d);
                        float *dd = F->dir + (((size_t)s * nm + bv) * F->w * F->h + i) * 3;
                        for (int m = 0; m < 3; m++) dd[m] = (float)d[m];
                    }
                }
                for (int k = 0; k < samples; k++) {
                    double p[3];
                    path_rgb(&c, style, q, F->n, sky, p);
                    for (int m = 0; m < 3; m++) acc[m] += p[m];
                }
                float *a = F->acc + ((size_t)s * F->w * F->h + i) * 3;
                for (int m = 0; m < 3; m++) a[m] += (float)acc[m];
            }
        }
        F->samples += samples;
    }
    sun_scratch_free(c.scr);
}

static void enc_rgbe(const double v[3], uint8_t o[4]) {   /* ColorRGBExp32: value = byte x 2^exp / 255 */
    double mx = v[0] > v[1] ? v[0] : v[1];
    if (v[2] > mx) mx = v[2];
    if (!(mx > 1e-9)) { o[0] = o[1] = o[2] = 0; o[3] = 0; return; }
    int e = (int)ceil(log2(mx));
    if (e < -128) e = -128;
    if (e > 127) e = 127;
    double sc = 255.0 / ldexp(1.0, e);
    for (int k = 0; k < 3; k++) { double c = v[k] * sc + 0.5; o[k] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c); }
    o[3] = (uint8_t)(int8_t)e;
}

static double dec_lum(const uint8_t *c) { double sc = ldexp(1.0, (int8_t)c[3]) / 255.0; return (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) * sc; }

uint8_t *l4_bake_lump(const L4Bake *b, size_t *n, int *lump_index) {
    uint8_t *out = malloc((size_t)b->nlit);
    if (!out) return NULL;
    memcpy(out, b->light, (size_t)b->nlit);
    /* exposure per light style (artistic-vision rule, analysis/39): the baked flat maps' mean luminance scaled to the stored one */
    double sb[256] = { 0 }, so[256] = { 0 }, scale[256];
    for (int fi = 0; fi < b->nf; fi++) {
        const FaceBake *F = &b->fb[fi];
        if (!F->ok || !F->samples) continue;
        long nl = (long)F->w * F->h;
        int nm = F->bump ? 4 : 1;
        for (int s = 0; s < F->nstyle; s++) {
            int st = b->faces[fi].styles[s];
            for (long i = 0; i < nl; i++) {
                const float *a = F->acc + ((size_t)s * nl + i) * 3, *d = F->dir + (((size_t)s * nm) * nl + i) * 3;
                sb[st] += 0.2126 * (d[0] + a[0] / F->samples) + 0.7152 * (d[1] + a[1] / F->samples) + 0.0722 * (d[2] + a[2] / F->samples);
                so[st] += dec_lum(b->light + b->faces[fi].lightofs + 4 * ((long)(s * nm) * nl + i));
            }
        }
    }
    for (int st = 0; st < 256; st++) scale[st] = sb[st] > 1e-9 && so[st] > 0 ? so[st] / sb[st] : 1.0;
    /* denoise: the indirect (path-traced, noisy, low-frequency) blurred within each face's luxel grid (5x5 Gaussian, renormalised at the
     * face's edges); the direct light (no noise, carries the shadow edges) is added back unblurred */
    static const float gk[5] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };
    size_t cap = 0;
    float *ind = NULL;
    for (int fi = 0; fi < b->nf; fi++) {
        const FaceBake *F = &b->fb[fi];
        if (!F->ok || !F->samples) continue;
        long nl = (long)F->w * F->h;
        int nm = F->bump ? 4 : 1;
        if ((size_t)nl * 3 > cap) { free(ind); cap = (size_t)nl * 3; ind = malloc(sizeof *ind * cap); if (!ind) { free(out); return NULL; } }
        for (int s = 0; s < F->nstyle; s++) {
            double k = scale[b->faces[fi].styles[s]];
            const float *A = F->acc + (size_t)s * nl * 3;
            for (int y = 0; y < F->h; y++)
                for (int x = 0; x < F->w; x++) {
                    float sum[3] = { 0, 0, 0 }, wsum = 0;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= F->w || yy >= F->h) continue;
                            float w2 = gk[dx + 2] * gk[dy + 2];
                            const float *a = A + ((size_t)yy * F->w + xx) * 3;
                            for (int m = 0; m < 3; m++) sum[m] += w2 * a[m];
                            wsum += w2;
                        }
                    for (int m = 0; m < 3; m++) ind[((size_t)y * F->w + x) * 3 + m] = sum[m] / (wsum * (float)F->samples);
                }
            for (int bv = 0; bv < nm; bv++)   /* indirect: the flat value in every map (v1); direct: per map */
                for (long i = 0; i < nl; i++) {
                    const float *a = ind + (size_t)i * 3, *d = F->dir + (((size_t)s * nm + bv) * nl + i) * 3;
                    double v[3] = { k * (d[0] + a[0]), k * (d[1] + a[1]), k * (d[2] + a[2]) };
                    enc_rgbe(v, out + b->faces[fi].lightofs + 4 * ((long)(s * nm + bv) * nl + i));
                }
        }
    }
    free(ind);
    *n = (size_t)b->nlit;
    if (lump_index) *lump_index = b->lump_light;
    return out;
}

void l4_bake_free(L4Bake *b) {
    if (!b) return;
    if (b->fb) for (int i = 0; i < b->nf; i++) { free(b->fb[i].acc); free(b->fb[i].dir); free(b->fb[i].pos); }
    free(b->fb);
    if (b->sm) sun_map_free(b->sm);
    if (b->lm) lg_map_free(b->lm);
    free(b);
}
