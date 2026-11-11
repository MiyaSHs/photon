/* P5 host tests of the light grid's pure core (src/ambient/lg_map.c, lg_light.c, lg_grid.c; analysis/18_p5_lights.md, acceptance d):
 *   map      the worldlight lumps and the tree of gm_construct / gm_flatgrass (counts of 12 section 7 row 1), memory and chunked reads equal,
 *            a version 1 light lump read like the version 0 one it was made from;
 *   convert  the engine's conversions (lg_light.h) on hand-made lights, against the formulas of FUN_180107ea0 / FUN_1801dcdd0;
 *   grid     camera walks, jumps, dynamic lights, style toggles and k changes on gm_construct, every frame: every window cell decoded from the
 *            texture image equals an independent brute-force reference (no buckets, no caching), every record equals lg_record of its
 *            light, pending cells hold no lights, every static record's refcount is the number of cells using it, the overflow totals are
 *            the reference's, and the shader's addressing (photon_lights_ps.h, in float) lands on the cell's and the records' texels;
 *   bench    the per-frame build with 96 dynamic lights moving every frame plus the map's static lights.
 *   P5b      the split (rho of every light equals a brute-force pass over every cell, no buckets; no window cell has more than k_static statics
 *            whose rho reaches it), every cell's fold list and the fold table equal the reference's, lg_fold_eval equals a double evaluation,
 *            and the walk: the lighting a pixel gets (per-pixel list + the fold at the pixel) is continuous across cell borders.
 * Modes: lgtest [--frames N] [--seeds N] [--bench N] <construct.bsp> [<flatgrass.bsp>];  lgtest --dump <map> <hdr>;  lgtest --pvsdist <map> <seed> <n>
 * (the last two print what tests/ambient/lg_reference.py prints, for scripts/run_ambient_test.sh to compare). Prints LGTEST OK. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ambient/lg_grid.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } fails++; } } while (0)

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

typedef struct { const uint8_t *b; size_t n; } Mem;
static int mem_read(void *ctx, long long ofs, void *buf, int n) {
    const Mem *m = ctx;
    if (ofs < 0 || (size_t)ofs > m->n || (size_t)n > m->n - (size_t)ofs) return 0;
    int k = n > 4096 ? 4096 : n;   /* short reads are refused by the reader: give all, but touch the chunking */
    memcpy(buf, m->b + ofs, (size_t)k);
    if (k < n) memcpy((uint8_t *)buf + k, m->b + ofs + k, (size_t)(n - k));
    return n;
}

static LgMap *load(const uint8_t *b, size_t n, int hdr) {
    LgMap *m = NULL;
    int rc = lg_map_load(b, n, hdr, &m);
    CHECK(rc == AMB_OK, "lg_map_load %d", rc);
    return m;
}

/* --- dump / pvsdist (lg_reference.py's formats) ---------------------------------------------------------------- */
static void dump(const LgMap *m) {
    int n;
    const LgWorldLight *w = lg_map_lights(m, &n);
    printf("lights %d\n", n);
    for (int i = 0; i < n; i++) {
        const LgWorldLight *l = &w[i];
        printf("wl %d %d %d %d %d %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", i, l->type, l->style,
               l->cluster, l->flags, l->texinfo, l->owner, l->origin[0], l->origin[1], l->origin[2], l->intensity[0], l->intensity[1],
               l->intensity[2], l->normal[0], l->normal[1], l->normal[2], l->stopdot, l->stopdot2, l->exponent, l->radius, l->constant_attn,
               l->linear_attn, l->quadratic_attn);
    }
}

static double radius255(const LgWorldLight *w) {
    if (w->radius > 0) return w->radius;
    double c = w->constant_attn, l = w->linear_attn, q = w->quadratic_attn;
    if (w->type == 0) { c = 0; l = 0; q = 1; }
    else if (c == 0 && l == 0 && q == 0) c = 1;
    double mx = w->intensity[0];
    if (w->intensity[1] > mx) mx = w->intensity[1];
    if (w->intensity[2] > mx) mx = w->intensity[2];
    double t = 255.0 * mx;
    if (q > 0) { double disc = l * l - 4 * q * (c - t); return disc >= 0 ? (sqrt(disc) - l) / (2 * q) : 0.0; }
    if (l > 0) return (t - c) / l;
    return t >= c ? 1e30 : 0.0;
}

static void pvsdist(const LgMap *m, uint64_t seed, int n, const double *box) {
    LgMapInfo mi;
    lg_map_info(m, &mi);
    if (box) for (int a = 0; a < 3; a++) { mi.world_min[a] = (float)box[a]; mi.world_max[a] = (float)box[3 + a]; }
    int nl;
    const LgWorldLight *w = lg_map_lights(m, &nl);
    int *counts = malloc((size_t)n * sizeof *counts), k = 0, tries = 0;
    uint64_t x = seed;
    while (k < n && tries < 100 * n) {
        tries++;
        double p[3];
        for (int a = 0; a < 3; a++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            double u = (double)(x >> 11) / 9007199254740992.0;
            p[a] = (double)mi.world_min[a] + u * ((double)mi.world_max[a] - (double)mi.world_min[a]);
        }
        int cl = lg_point_cluster(m, p);
        if (cl < 0) continue;
        int c = 0;
        for (int i = 0; i < nl; i++) {
            if (w[i].type != 0 && w[i].type != 1 && w[i].type != 2) continue;
            double dx = p[0] - w[i].origin[0], dy = p[1] - w[i].origin[1], dz = p[2] - w[i].origin[2];
            double d = sqrt(dx * dx + dy * dy + dz * dz);
            if (lg_cluster_sees(m, cl, w[i].cluster) && d - 64.0 <= radius255(&w[i])) c++;
        }
        counts[k++] = c;
    }
    long long chk = 0;
    for (int i = 0; i < k; i++) chk += (long long)(i + 1) * counts[i];
    int *s = malloc((size_t)k * sizeof *s);
    memcpy(s, counts, (size_t)k * sizeof *s);
    for (int i = 1; i < k; i++) { int v = s[i], j = i; while (j > 0 && s[j - 1] > v) { s[j] = s[j - 1]; j--; } s[j] = v; }
    printf("pvsdist points %d tries %d p50 %d p90 %d p99 %d max %d checksum %lld\n", k, tries, s[k * 50 / 100], s[k * 90 / 100], s[k * 99 / 100],
           s[k - 1], chk);
    free(s);
    free(counts);
}

/* --- map ------------------------------------------------------------------------------------------------------------------ */
static void test_map(const uint8_t *b, size_t n, int construct) {
    for (int hdr = 0; hdr < 2; hdr++) {
        LgMap *m = load(b, n, hdr), *r = NULL;
        if (!m) return;
        Mem mem = { b, n };
        AmbReader rd = { &mem, (long long)n, mem_read, NULL };
        AmbReadStats st;
        CHECK(lg_map_load_reader(&rd, hdr, &r, &st) == AMB_OK && r, "reader load");
        LgMapInfo a, c;
        lg_map_info(m, &a);
        int nl, nr;
        const LgWorldLight *wl = lg_map_lights(m, &nl);
        if (r) {
            lg_map_info(r, &c);
            const LgWorldLight *wr = lg_map_lights(r, &nr);
            CHECK(memcmp(&a, &c, sizeof a) == 0 && nl == nr && (!nl || memcmp(wl, wr, (size_t)nl * sizeof *wl) == 0), "reader != memory");
            CHECK(st.bytes_read < (long long)n, "the reader read the whole file (%lld of %zu)", st.bytes_read, n);
            lg_map_free(r);
        }
        int types[6] = { 0 }, st0 = 0, st32 = 0, st33 = 0;
        for (int i = 0; i < nl; i++) {
            if (wl[i].type >= 0 && wl[i].type < 6) types[wl[i].type]++;
            st0 += wl[i].style == 0; st32 += wl[i].style == 32; st33 += wl[i].style == 33;
        }
        if (construct) {
            CHECK(nl == 138 && types[1] == 108 && types[2] == 28 && types[3] == 1 && types[5] == 1 && types[0] == 0 && types[4] == 0,
                  "gm_construct hdr %d: %d lights, types %d/%d/%d/%d/%d/%d (want 138: 108 point, 28 spot, 1 sky, 1 sky ambient)", hdr, nl, types[0],
                  types[1], types[2], types[3], types[4], types[5]);
            CHECK(st0 == 107 && st32 == 30 && st33 == 1, "gm_construct styles %d/%d/%d (want 107/30/1)", st0, st32, st33);
            CHECK(a.nleaves == 5002 && a.pvs && a.nclusters > 0, "gm_construct: %d leaves (want 5002), pvs %d, %d clusters", a.nleaves, a.pvs, a.nclusters);
            CHECK(a.light_lump == (hdr ? 54 : 15), "lump %d", a.light_lump);
            if (hdr) printf("gm_construct: 138 lights (108 point, 28 spot, 1 skylight, 1 skyambient), styles 107/30/1, %d leaves, %d clusters\n",
                            a.nleaves, a.nclusters);
        } else {
            CHECK(nl == 3 && types[1] == 1 && types[3] == 1 && types[5] == 1, "gm_flatgrass: %d lights", nl);
        }
        lg_map_free(m);
    }
    /* a version 1 copy of lump 15 (12 zero bytes of shadow_cast_offset after the normal) reads as the original */
    LgMap *m0 = load(b, n, 0);
    if (!m0) return;
    int nl;
    const LgWorldLight *w0 = lg_map_lights(m0, &nl);
    int32_t ofs, len;
    memcpy(&ofs, b + 8 + 16 * 15, 4);
    memcpy(&len, b + 8 + 16 * 15 + 4, 4);
    size_t n1 = n + (size_t)nl * 100;
    uint8_t *c = malloc(n1);
    memcpy(c, b, n);
    for (int i = 0; i < nl; i++) {
        memcpy(c + n + 100 * (size_t)i, b + ofs + 88 * (size_t)i, 36);
        memset(c + n + 100 * (size_t)i + 36, 0x7f, 12);
        memcpy(c + n + 100 * (size_t)i + 48, b + ofs + 88 * (size_t)i + 36, 52);
    }
    int32_t o1 = (int32_t)n, l1 = nl * 100, v1 = 1;
    memcpy(c + 8 + 16 * 15, &o1, 4);
    memcpy(c + 8 + 16 * 15 + 4, &l1, 4);
    memcpy(c + 8 + 16 * 15 + 8, &v1, 4);
    LgMap *m1 = load(c, n1, 0);
    if (m1) {
        int n2;
        const LgWorldLight *w1 = lg_map_lights(m1, &n2);
        CHECK(n2 == nl && memcmp(w0, w1, (size_t)nl * sizeof *w0) == 0, "a version 1 light lump differs from its version 0 source");
        lg_map_free(m1);
    }
    free(c);
    lg_map_free(m0);
    (void)len;
}

/* --- conversions ------------------------------------------------------------------------------------------------------------- */
static void test_convert(void) {
    LgWorldLight w;
    LgDesc d;
    memset(&w, 0, sizeof w);
    w.type = LG_EMIT_POINT;
    w.intensity[0] = 100; w.intensity[1] = 50; w.intensity[2] = 25;
    w.quadratic_attn = 1;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.type == 1 && d.att0 == 0 && d.att2 == 1, "point");
    float mag = sqrtf(50.0f * 50.0f + 100.0f * 100.0f + 25.0f * 25.0f);
    float disc = 0.0f - (0.0f - mag * 33.333335876464844f) * 1.0f * 4.0f;
    CHECK(d.range == (sqrtf(disc) - 0.0f) * (0.5f / 1.0f), "point range %g", d.range);
    w.quadratic_attn = 0;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.att0 == 1.0f && d.att1 == 0 && d.att2 == 0, "att0 defaults to 1 (%g)", d.att0);
    w.linear_attn = 2; w.constant_attn = 1;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.att0 == 1 && d.range == (mag * 33.333335876464844f - 1.0f) / 2.0f, "linear range %g", d.range);
    w.radius = 300;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.range == 300, "radius wins");
    w.type = LG_EMIT_SPOT; w.stopdot = 0.9f; w.stopdot2 = 0.5f; w.exponent = 0;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.type == 3 && d.theta == (float)(2 * acos(0.9f)) && d.phi == (float)(2 * acos(0.5f)) && d.falloff == 1 &&
          d.thetadot == 0.9f && d.phidot == 0.5f, "spot cone %g %g", d.theta, d.phi);
    w.type = LG_EMIT_SKYLIGHT; w.constant_attn = w.linear_attn = 0;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.type == 2 && d.att0 == 1, "skylight");
    w.type = LG_EMIT_SURFACE;
    CHECK(lg_desc_from_worldlight(&w, &d) && d.type == 3 && d.theta == 3.14159274f && d.phi == 3.14159274f && d.att2 == 1 && d.att0 == 0,
          "surface light");
    w.type = LG_EMIT_SKYAMBIENT;
    CHECK(!lg_desc_from_worldlight(&w, &d), "sky ambient is not a light");
    w.type = LG_EMIT_QUAKE;
    CHECK(!lg_desc_from_worldlight(&w, &d), "quake light is not a light");

    float tab[256];
    lg_exptab_default(tab);
    LgDlight dl;
    memset(&dl, 0, sizeof dl);
    dl.origin[0] = 10; dl.r = 255; dl.g = 128; dl.b = 0; dl.exponent = 1; dl.radius = 200; dl.minlight = 0; dl.style = 0;
    LgWorldLight dw;
    lg_worldlight_from_dlight(&dl, tab, 1.0f, &dw);
    CHECK(dw.type == 1 && dw.intensity[0] == 255.0f * tab[129] && dw.radius == 200 && dw.quadratic_attn == 1.0f / (1.0f * 200.0f * 200.0f) &&
          dw.constant_attn == 0 && dw.linear_attn == 0, "dlight: q %g", dw.quadratic_attn);
    dl.minlight = 2.0f;
    lg_worldlight_from_dlight(&dl, tab, 1.0f, &dw);
    CHECK(dw.quadratic_attn == 1.0f / (2.0f * 200.0f * 200.0f), "dlight minlight");
    dl.minlight = 0; dl.radius = 0; dl.outer = 45; dl.inner = 30; dl.direction[2] = -1;
    lg_worldlight_from_dlight(&dl, tab, 1.0f, &dw);
    CHECK(dw.radius == 0.100000001f && dw.type == 2 && dw.stopdot == (float)cos(30 * 0.017453292519943295) &&
          dw.stopdot2 == (float)cos(45 * 0.017453292519943295) && dw.normal[2] == -1, "dlight spot");
    CHECK(lg_desc_from_worldlight(&dw, &d) && d.type == 3 && d.range == dw.radius, "dlight spot desc");
    int sv[128];
    for (int i = 0; i < 128; i++) sv[i] = 264;
    sv[32] = 0;
    CHECK(lg_style_scale(sv, 0) == 264.0f * 0.0037878789f && lg_style_scale(sv, 32) == 0 && lg_style_scale(sv, 200) == 1, "style scale");
    float rec[4][4];
    w.type = LG_EMIT_POINT; w.radius = 0; w.quadratic_attn = 1; w.intensity[0] = 10;
    lg_desc_from_worldlight(&w, &d);
    CHECK(lg_record(&d, 0.5f, 100.0f, rec) && rec[1][0] == 0.5f * 10.0f && rec[0][3] == 1.0f / (100.0f * 100.0f) && rec[1][3] == 0 && rec[3][2] == 1,
          "record");
    double r = lg_window_radius(&w, 0.01);
    double i = sqrt(10.0 * 10.0 + 50.0 * 50.0 + 25.0 * 25.0);
    CHECK(fabs(r - sqrt(i / 0.01)) < 1e-9, "window radius %g", r);
}

/* --- grid --------------------------------------------------------------------------------------------------------------------- */
static uint64_t g_rng;
static double rnd(void) { g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull; return (double)(g_rng >> 11) / 9007199254740992.0; }
static int imod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }
static int tor(const int c[3]) { return imod(c[0], LG_GX) + LG_GX * imod(c[1], LG_GY) + LG_GX * LG_GY * imod(c[2], LG_GZ); }

typedef struct { int id; float imp; } RC;
static int rc_cmp(const void *a, const void *b) {
    const RC *x = a, *y = b;
    if (x->imp != y->imp) return x->imp > y->imp ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}
typedef struct { int slot, cluster; LgDesc desc, scaled; double r; float scale; } TDyn;

static double box_dist(const int key[3], double s, const float p[3]) {
    double d2 = 0;
    for (int k = 0; k < 3; k++) {
        double lo = key[k] * s, hi = lo + s, v = p[k], d = 0;
        if (v < lo) d = lo - v;
        else if (v > hi) d = v - hi;
        d2 += d * d;
    }
    return sqrt(d2);
}

static int ref_dyn(const LgMap *m, const LgParams *p, const LgSnapshot *s, TDyn *out) {
    int n = 0;
    for (int which = 0; which < 2; which++)
        for (int i = 0; i < (which ? LG_NEL : LG_NDL); i++) {
            const LgDlight *dl = which ? &s->el[i] : &s->dl[i];
            if (which ? !(s->el_on && dl->radius > 0) : !(s->dl_mask >> i & 1)) continue;
            if (dl->flags & 0xe) continue;
            LgWorldLight w;
            lg_worldlight_from_dlight(dl, p->exptab, s->minlight_floor > 0.0f ? s->minlight_floor : p->minlight_floor, &w);
            TDyn *d = &out[n];
            if (!lg_desc_from_worldlight(&w, &d->desc)) continue;
            double o[3] = { w.origin[0], w.origin[1], w.origin[2] };
            d->cluster = lg_point_cluster(m, o);
            d->scale = lg_style_scale(s->style, w.style);
            if (d->cluster < 0 || !(d->scale > 0)) continue;
            d->slot = which ? LG_NDL + i : i;
            d->r = w.radius;
            d->scaled = d->desc;
            for (int k = 0; k < 3; k++) d->scaled.color[k] = d->scale * d->desc.color[k];
            n++;
        }
    return n;
}

/* The cell's list (ids: static index or 0x8000 | slot), overflow and fold list (lg_grid.h's rule, written out again without buckets or caching).
 * *npp_all: statics (any style) whose rho reaches the cell; *lost: candidates beyond the caps. */
typedef struct { int n, over, nfold, npp_all, lost; int ids[LG_KMAX]; uint16_t fold[LG_FOLD_MAX]; } RefCell;
static void ref_cell(const LgMap *m, const LgGrid *g, const LgParams *p, const TDyn *dyn, int nd, const LgSnapshot *s, int k, const int key[3], RefCell *o) {
    memset(o, 0, sizeof *o);
    double cs = p->cell;
    int clus[9], nc = 0;
    for (int q = 0; q < 9; q++) {
        double pt[3];
        for (int a = 0; a < 3; a++) pt[a] = (key[a] + 0.5 + (q == 0 ? 0.0 : ((((q - 1) >> a) & 1) ? 0.25 : -0.25))) * cs;
        int cl = lg_point_cluster(m, pt), dup = 0;
        if (cl < 0) continue;
        for (int j = 0; j < nc; j++) dup |= clus[j] == cl;
        if (!dup) clus[nc++] = cl;
    }
    if (!nc) return;
    int ns = lg_grid_static_count(g);
    RC *st = malloc(((size_t)ns + 1) * sizeof *st), dy[LG_NDYN];
    double *sd = malloc(((size_t)ns + 1) * sizeof *sd);
    int nst = 0, ndy = 0;
    for (int i = 0; i < ns; i++) {
        double r;
        int cl, sty;
        const LgDesc *d = lg_grid_static(g, i, &r, &cl, &sty);
        double dist = box_dist(key, cs, d->pos);
        int seen = 0;
        for (int j = 0; j < nc && cl >= 0; j++) seen |= lg_cluster_sees(m, clus[j], cl);
        if (dist > r || !seen) continue;
        float imp = (float)lg_importance(d, r, dist);
        if (imp > 0) { st[nst++] = (RC){ i, imp }; sd[i] = dist; }
    }
    qsort(st, (size_t)nst, sizeof *st, rc_cmp);
    for (int i = 0; i < nst; i++) o->npp_all += sd[st[i].id] < lg_grid_rho(g, st[i].id);
    if (nst > LG_STATIC_MAX) { o->lost += nst - LG_STATIC_MAX; nst = LG_STATIC_MAX; }
    int w = 0;
    for (int i = 0; i < nst; i++) {
        int sty;
        lg_grid_static(g, st[i].id, NULL, NULL, &sty);
        if ((unsigned)sty >= 128u || s->style[sty] != 0) st[w++] = st[i];
    }
    nst = w;
    for (int i = 0; i < nd; i++) {
        double dist = box_dist(key, cs, dyn[i].desc.pos);
        int seen = 0;
        for (int j = 0; j < nc; j++) seen |= lg_cluster_sees(m, clus[j], dyn[i].cluster);
        if (dist > dyn[i].r || !seen) continue;
        float imp = (float)lg_importance(&dyn[i].scaled, dyn[i].r, dist);
        if (imp > 0) dy[ndy++] = (RC){ 0x8000 | dyn[i].slot, imp };
    }
    qsort(dy, (size_t)ndy, sizeof *dy, rc_cmp);
    if (ndy > LG_DYN_MAX) { o->lost += ndy - LG_DYN_MAX; ndy = LG_DYN_MAX; }
    /* the per-pixel statics first, then the dynamic lights while there is room, in importance order */
    RC all[LG_KMAX];
    int na = 0, npp = 0, nks = 0;
    for (int i = 0; i < nst; i++) {
        if (!(sd[st[i].id] < lg_grid_rho(g, st[i].id))) continue;
        npp++;
        if (nks < k) all[na++] = st[i], nks++;
    }
    int nkd = ndy < k - nks ? ndy : k - nks;
    for (int i = 0; i < nkd; i++) all[na++] = dy[i];
    qsort(all, (size_t)na, sizeof *all, rc_cmp);
    o->n = na;
    for (int i = 0; i < na; i++) o->ids[i] = all[i].id;
    o->over = npp + ndy - na;
    for (int i = 0; i < nst; i++) {
        int kept = 0;
        for (int j = 0; j < na; j++) kept |= all[j].id == st[i].id;
        o->fold[o->nfold++] = (uint16_t)(st[i].id | (kept ? LG_FOLD_KEPT : 0));
    }
    for (int i = nkd; i < ndy; i++) o->fold[o->nfold++] = (uint16_t)(LG_FOLD_DYN | (dy[i].id & 0x7fff));
    free(st);
    free(sd);
}

/* W's gain of static light i (lg_grid.h), from its worldlight: the engine's radius (HDR lights: |I| / att = 1/66.67) over r, capped at 0.9 */
static float ref_we(const LgMap *m, const LgGrid *g, int i, double r) {
    int nw;
    const LgWorldLight *w = lg_map_lights(m, &nw);
    const LgDesc *d = lg_grid_static(g, i, NULL, NULL, NULL);
    for (int j = 0; j < nw; j++) {   /* the worldlight at the desc's position (the grid keeps the map's order, skipping non-grid lights) */
        if (w[j].origin[0] != d->pos[0] || w[j].origin[1] != d->pos[1] || w[j].origin[2] != d->pos[2]) continue;
        LgDesc dd;
        if (!lg_desc_from_worldlight(&w[j], &dd) || memcmp(&dd, d, sizeof dd) != 0) continue;
        LgMapInfo mi;
        lg_map_info(m, &mi);
        double re = lg_window_radius(&w[j], mi.light_lump == 54 ? 1.0 / 66.66667175292969 : 1.0 / 33.333335876464844), xe = re / r;
        if (!(xe < 0.9)) xe = 0.9;
        return (float)(1.0 / (1.0 - xe * xe * xe * xe));
    }
    return -1.0f;
}

/* lg_fold_eval written out in double (the reference for one entry list) */
static double win_d(double d2, double R) {
    if (!(R > 0)) return 0;
    double q = d2 / (R * R), w = 1 - q * q;
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    return w * w;
}
/* scale: the same sum with no window at all, the size float rounding is relative to (W - window(rho) and W itself cancel) */
static void ref_fold(const LgFoldLight *tab, int ntab, int nstatic, const uint16_t *cell, const float p[3], double out[6][3], double scale[6][3]) {
    memset(out, 0, 18 * sizeof(double));
    memset(scale, 0, 18 * sizeof(double));
    for (int j = 0; j < cell[0]; j++) {
        unsigned e = cell[2 + j];
        int dyn = (e & LG_FOLD_DYN) != 0, kept = !dyn && (e & LG_FOLD_KEPT), idx = dyn ? nstatic + (int)(e & 0x7fff) : (int)(e & 0x3fff);
        if (idx >= ntab) continue;
        const LgFoldLight *L = &tab[idx];
        double to[3] = { L->rec[0][0] - p[0], L->rec[0][1] - p[1], L->rec[0][2] - p[2] };
        double d2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
        if (d2 < 1e-6) d2 = 1e-6;
        double d = sqrt(d2), l[3] = { to[0] / d, to[1] / d, to[2] / d };
        double whole = win_d(d2, L->r);
        if (L->we > 0) { double q = d2 / ((double)L->r * L->r); whole = (1 - q * q) * L->we; whole = whole < 0 ? 0 : whole > 1 ? 1 : whole; }
        double w = whole - (kept ? win_d(d2, L->rho) : 0);
        if (!(whole > 0)) continue;
        double att = L->rec[3][0] + L->rec[3][1] * d + L->rec[3][2] * d2, a = 1 / (att > 1e-6 ? att : 1e-6);
        if (L->rec[1][3] > 0.5) {
            double cd = -(L->rec[2][0] * l[0] + L->rec[2][1] * l[1] + L->rec[2][2] * l[2]) - L->rec[3][3];
            double sp = pow(cd > 1e-4 ? cd : 1e-4, L->rec[2][3]);
            a *= sp > 1 ? 1 : sp;
        }
        for (int f = 0; f < 6; f++) {
            double dt = (f & 1) ? -l[f >> 1] : l[f >> 1];
            if (dt > 0) for (int c = 0; c < 3; c++) { out[f][c] += dt * a * (w > 0 ? w : 0) * L->rec[1][c]; scale[f][c] += dt * a * fabs(L->rec[1][c]); }
        }
    }
}

static void random_light(LgDlight *d, const double cam[3], int elight) {
    memset(d, 0, sizeof *d);
    for (int k = 0; k < 3; k++) d->origin[k] = (float)(cam[k] + (rnd() - 0.5) * (k == 2 ? 800 : 3000));
    d->radius = (float)(elight && rnd() < 0.15 ? 0 : 50 + rnd() * 600);
    d->r = (uint8_t)(rnd() * 255); d->g = (uint8_t)(rnd() * 255); d->b = (uint8_t)(rnd() * 255);
    d->exponent = (int8_t)(rnd() * 5 - 2);
    d->minlight = (float)(rnd() < 0.5 ? 0 : rnd() * 3);
    d->flags = rnd() < 0.1 ? 2 : 0;
    d->style = rnd() < 0.2 ? 32 : 0;
    if (rnd() < 0.3) { d->outer = (float)(20 + rnd() * 50); d->inner = d->outer * (float)rnd(); d->direction[2] = -1; }
}

static void check_frame(const LgMap *m, LgGrid *g, const LgParams *p, const LgSnapshot *s, const LgStats *st, const char *tag) {
    TDyn dyn[LG_NDYN];
    int nd = ref_dyn(m, p, s, dyn);
    int lo[3];
    lg_grid_window(g, lo);
    const float *img = lg_grid_image(g);
    int k = s->k > 0 ? s->k : p->k, refs[LG_NSTATIC_REC] = { 0 }, ocells = 0, olights = 0, bad = 0, flost = 0, fcells = 0, fentries = 0;
    int ntab, ns = lg_grid_static_count(g);
    const LgFoldLight *tab = lg_grid_fold_table(g, &ntab, NULL);
    float c63[4];
    lg_grid_constants(g, c63);
    for (int z = 0; z < LG_GZ; z++)
        for (int y = 0; y < LG_GY; y++)
            for (int x = 0; x < LG_GX; x++) {
                int key[3] = { lo[0] + x, lo[1] + y, lo[2] + z }, i = tor(key);
                const float *t = img + ((size_t)(i / LG_GX) * LG_TEX_W + 4 * (size_t)(i % LG_GX)) * 4;
                int n = (int)t[0], state = lg_grid_cell_state(g, key);
                if (state != 2) { if (n != 0 && bad++ < 3) CHECK(0, "%s: cell %d %d %d is pending but holds %d lights", tag, key[0], key[1], key[2], n); continue; }
                RefCell rf;
                ref_cell(m, g, p, dyn, nd, s, k, key, &rf);
                int rn = rf.n, *ids = rf.ids, over = rf.over;
                ocells += over > 0;
                olights += over;
                flost += rf.lost;
                fcells += rf.nfold > 0;
                fentries += rf.nfold;
                if (p->k_static && rf.npp_all > p->k_static && bad++ < 3)
                    CHECK(0, "%s: cell %d %d %d: %d statics reach it per pixel, k_static %d", tag, key[0], key[1], key[2], rf.npp_all, p->k_static);
                const uint16_t *fl = lg_grid_fold_image(g) + (size_t)(i / LG_GX) * LG_FOLD_ROW + (size_t)(i % LG_GX) * LG_FOLD_STRIDE;
                if ((fl[0] != rf.nfold || memcmp(fl + 2, rf.fold, (size_t)rf.nfold * sizeof *rf.fold) != 0) && bad++ < 3)
                    CHECK(0, "%s: cell %d %d %d: fold list of %d entries, the reference %d (first %04x / %04x)", tag, key[0], key[1], key[2], fl[0], rf.nfold,
                          fl[0] ? fl[2] : 0, rf.nfold ? rf.fold[0] : 0);
                for (int j = 0; j < rf.nfold; j++) {   /* the table entries the list names */
                    unsigned e = rf.fold[j];
                    LgFoldLight want;
                    if (e & LG_FOLD_DYN) {
                        int di = -1;
                        for (int q = 0; q < nd; q++) if (dyn[q].slot == (int)(e & 0x7fff)) di = q;
                        if (di < 0) continue;
                        lg_record(&dyn[di].desc, dyn[di].scale, 0.0f, want.rec);
                        want.r = want.rho = (float)dyn[di].r;
                        want.we = 0.0f;
                        e = (unsigned)(ns + (int)(e & 0x7fff));
                    } else {
                        double r;
                        int sty;
                        const LgDesc *d = lg_grid_static(g, (int)(e & 0x3fff), &r, NULL, &sty);
                        lg_record(d, lg_style_scale(s->style, sty), 0.0f, want.rec);
                        want.r = (float)r;
                        want.rho = (float)lg_grid_rho(g, (int)(e & 0x3fff));
                        want.we = ref_we(m, g, (int)(e & 0x3fff), r);
                        e &= 0x3fff;
                    }
                    if ((int)e >= ntab || memcmp(&tab[e], &want, sizeof want) != 0) {
                        if (bad++ < 3) CHECK(0, "%s: fold table entry %u differs from its light", tag, e);
                        break;
                    }
                }
                if (rf.nfold) {   /* the evaluation at a point of the cell, against the double one */
                    float pt[3];
                    for (int a = 0; a < 3; a++) pt[a] = (float)((key[a] + rnd()) * p->cell);
                    float got[6][3];
                    double want[6][3], scale[6][3];
                    lg_fold_eval(tab, ntab, ns, fl, pt, got);
                    ref_fold(tab, ntab, ns, fl, pt, want, scale);
                    for (int f = 0; f < 6; f++)
                        for (int c = 0; c < 3; c++)
                            if (fabs(got[f][c] - want[f][c]) > 2e-4 * fabs(want[f][c]) + 2e-6 * scale[f][c] + 1e-9 && bad++ < 3)
                                CHECK(0, "%s: cell %d %d %d: the fold's face %d channel %d is %g, the reference %g", tag, key[0], key[1], key[2], f, c,
                                      got[f][c], want[f][c]);
                }
                int ok = n == rn;
                for (int j = 0; j < n && ok && j < LG_KMAX; j++) {
                    int rec = (int)t[1 + j];
                    float want[4][4];
                    if (ids[j] & 0x8000) {
                        int slot = ids[j] & 0x7fff, di = -1;
                        for (int q = 0; q < nd; q++) if (dyn[q].slot == slot) di = q;
                        ok = rec == LG_NSTATIC_REC + slot && di >= 0;
                        if (ok) lg_record(&dyn[di].desc, dyn[di].scale, (float)dyn[di].r, want);
                    } else {
                        double r;
                        int sty;
                        const LgDesc *d = lg_grid_static(g, ids[j], &r, NULL, &sty);
                        ok = rec >= 0 && rec < LG_NSTATIC_REC;
                        (void)r;
                        if (ok) { refs[rec]++; lg_record(d, lg_style_scale(s->style, sty), (float)lg_grid_rho(g, ids[j]), want); }
                    }
                    if (ok) ok = memcmp(img + ((size_t)(LG_REC_ROW + rec / 32) * LG_TEX_W + 4 * (size_t)(rec % 32)) * 4, want, sizeof want) == 0;
                }
                if (!ok && bad++ < 3) CHECK(0, "%s: cell %d %d %d: %d lights, the reference %d (first ids %d / %d)", tag, key[0], key[1], key[2], n, rn,
                                            n ? (int)t[1] : -1, rn ? ids[0] : -1);
                /* the shader's addressing, in float, at a point inside this cell */
                float wp[3];
                for (int a = 0; a < 3; a++) wp[a] = (float)((key[a] + 0.25 + 0.5 * rnd()) * p->cell);
                float wc[3], tt[3];
                int inside = 1;
                for (int a = 0; a < 3; a++) {
                    wc[a] = floorf(wp[a] * c63[3]);
                    float rel = wc[a] - c63[a];
                    inside &= rel >= 0.0f && rel < (a == 2 ? 16.0f : 32.0f);
                    float sc = wc[a] * (a == 2 ? 1.0f / 16.0f : 1.0f / 32.0f);
                    tt[a] = sc - floorf(sc);
                }
                float u = tt[0] + 0.5f / 128.0f, v = (tt[1] * 32.0f + tt[2] * 512.0f) * (1.0f / 528.0f) + 0.5f / 528.0f;
                int tx = (int)floorf(u * 128.0f), ty = (int)floorf(v * 528.0f);
                if ((!inside || tx != 4 * (i % LG_GX) || ty != i / LG_GX) && bad++ < 3)
                    CHECK(0, "%s: the shader's cell texel for %d %d %d is (%d, %d), the cell's (%d, %d), inside %d", tag, key[0], key[1], key[2], tx, ty,
                          4 * (i % LG_GX), i / LG_GX, inside);
                for (int j = 0; j < n && j < LG_KMAX; j++) {
                    float q = (float)j + 1.0f, qt = floorf(q * 0.25f), comp = q - 4.0f * qt;
                    int col = (int)floorf((u + qt * (1.0f / 128.0f)) * 128.0f);
                    float pp = t[(int)qt * 4 + (int)comp - (j + 1 < 4 ? 0 : 0)];
                    float a = pp * (1.0f / 32.0f), fa = a - floorf(a);
                    float ur = fa + 0.5f / 128.0f, vr = (512.0f + 0.5f + (a - fa)) * (1.0f / 528.0f);
                    int rx = (int)floorf(ur * 128.0f), ry = (int)floorf(vr * 528.0f), rec = (int)t[1 + j];
                    if ((col != 4 * (i % LG_GX) + (int)qt || pp != (float)rec || rx != 4 * (rec % 32) || ry != LG_REC_ROW + rec / 32) && bad++ < 3)
                        CHECK(0, "%s: the shader's record %d of cell %d %d %d: index texel col %d, record texel (%d, %d), want record %d", tag, j,
                              key[0], key[1], key[2], col, rx, ry, rec);
                }
            }
    for (int sl = 0; sl < LG_NSTATIC_REC; sl++)
        if (refs[sl] != lg_grid_refcount(g, sl) && bad++ < 3) CHECK(0, "%s: record %d used by %d cells, refcount %d", tag, sl, refs[sl], lg_grid_refcount(g, sl));
    if (!st->pending) CHECK(st->overflow_cells == ocells && st->overflow_lights == olights, "%s: overflow %d cells / %d lights, the reference %d / %d", tag,
                            st->overflow_cells, st->overflow_lights, ocells, olights);
    if (!st->pending) CHECK(st->fold_cells == fcells && st->fold_entries == fentries && st->fold_lost == flost,
                            "%s: fold %d cells / %d entries / %d lost, the reference %d / %d / %d", tag, st->fold_cells, st->fold_entries, st->fold_lost,
                            fcells, fentries, flost);
    CHECK(st->record_overflow == 0, "%s: %d record overflows", tag, st->record_overflow);
}

static void test_grid(const uint8_t *b, size_t n, int seeds, int frames) {
    LgMap *m = load(b, n, 1);
    if (!m) return;
    LgMapInfo mi;
    lg_map_info(m, &mi);
    for (int seed = 0; seed < seeds; seed++) {
        LgParams p;
        lg_params_default(&p);
        p.budget = seed % 2 ? 700 : 0;
        if (seed % 4 == 3) p.k_static = 0;   /* P5's grid: no split, only what a full cell has no room for is folded */
        else if (seed % 4 == 2) p.k_static = 6;
        LgGrid *g = NULL;
        CHECK(lg_grid_new(m, &p, &g) == AMB_OK && g, "lg_grid_new");
        if (!g) break;
        g_rng = 0x5eed0000ull + (uint64_t)seed;
        LgSnapshot s;
        memset(&s, 0, sizeof s);
        for (int i = 0; i < 128; i++) s.style[i] = 264;
        double cam[3] = { -1500, -1000, -40 };
        if (seed % 3 == 1) { cam[0] = -6000; cam[1] = 3000; cam[2] = 300; }
        if (seed % 3 == 2) { cam[0] = -2400; cam[1] = -3700; cam[2] = -200; }   /* the ColourRoom: the split's own place */
        long overflow_seen = 0, dyn_seen = 0, fold_seen = 0;
        for (int f = 0; f < frames; f++) {
            double jump = rnd();
            for (int k = 0; k < 3; k++) cam[k] += jump < 0.03 ? (rnd() - 0.5) * 8000 : (rnd() - 0.5) * (k == 2 ? 60 : 300);
            if (cam[2] < -500) cam[2] = -500;
            if (cam[2] > 2000) cam[2] = 2000;
            memcpy(s.cam, cam, sizeof cam);
            if (f % 5 == 0 || rnd() < 0.3) {
                s.dl_mask = 0;
                int ndl = (int)(rnd() * 33), nel = (int)(rnd() * 65);
                for (int i = 0; i < LG_NDL; i++) { random_light(&s.dl[i], cam, 0); if (i < ndl) s.dl_mask |= 1u << i; }
                s.el_on = rnd() < 0.9;
                for (int i = 0; i < LG_NEL; i++) { random_light(&s.el[i], cam, 1); if (i >= nel) s.el[i].radius = 0; }
            } else if (rnd() < 0.5) {
                for (int i = 0; i < LG_NDL; i++) s.dl[i].origin[0] += (float)(rnd() * 20);
            }
            if (rnd() < 0.15) { s.style[32] = rnd() < 0.5 ? 0 : (int)(rnd() * 300); s.style[33] = rnd() < 0.5 ? 0 : 264; }
            if (rnd() < 0.05) s.k = (int)(rnd() * 16);
            if (rnd() < 0.1) s.minlight_floor = rnd() < 0.5 ? 0.00390625f : 0.0f;
            LgStats st;
            CHECK(lg_grid_frame(g, &s, &st) == AMB_OK, "frame");
            overflow_seen += st.overflow_lights;
            dyn_seen += st.dyn_lights;
            fold_seen += st.fold_entries;
            char tag[48];
            snprintf(tag, sizeof tag, "seed %d frame %d", seed, f);
            check_frame(m, g, &p, &s, &st, tag);
            if (fails > 30) break;
        }
        printf("grid seed %d: %d frames, budget %d, k_static %d, dynamic lights seen %ld, overflow lights seen %ld, fold entries seen %ld, %d static grid"
               " lights\n", seed, frames, p.budget, p.k_static, dyn_seen, overflow_seen, fold_seen, lg_grid_static_count(g));
        lg_grid_free(g);
    }
    lg_map_free(m);
}

/* --- P5b: the split against a brute-force pass over every cell the lights reach (no buckets, every light tested) -------------------------- */
static void test_split(const uint8_t *b, size_t n) {
    LgMap *m = load(b, n, 1);
    if (!m) return;
    LgParams p;
    lg_params_default(&p);
    LgGrid *g = NULL;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(lg_grid_new(m, &p, &g) == AMB_OK && g, "lg_grid_new");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!g) { lg_map_free(m); return; }
    double new_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6;
    int ns = lg_grid_static_count(g);
    double *rho = malloc((size_t)ns * sizeof *rho), *dist = malloc((size_t)ns * sizeof *dist), lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
    RC *c = malloc((size_t)ns * sizeof *c);
    for (int i = 0; i < ns; i++) {
        double r;
        const LgDesc *d = lg_grid_static(g, i, &r, NULL, NULL);
        rho[i] = r;
        for (int a = 0; a < 3; a++) { if (d->pos[a] - r < lo[a]) lo[a] = d->pos[a] - r; if (d->pos[a] + r > hi[a]) hi[a] = d->pos[a] + r; }
    }
    int k0[3], k1[3];
    long cells = 0, open = 0, over = 0;
    for (int a = 0; a < 3; a++) { k0[a] = (int)floor(lo[a] / p.cell); k1[a] = (int)floor(hi[a] / p.cell); }
    for (int z = k0[2]; z <= k1[2]; z++)
        for (int y = k0[1]; y <= k1[1]; y++)
            for (int x = k0[0]; x <= k1[0]; x++) {
                int key[3] = { x, y, z }, clus[9], nc = 0;
                cells++;
                for (int q = 0; q < 9; q++) {
                    double pt[3];
                    for (int a = 0; a < 3; a++) pt[a] = (key[a] + 0.5 + (q == 0 ? 0.0 : ((((q - 1) >> a) & 1) ? 0.25 : -0.25))) * p.cell;
                    int cl = lg_point_cluster(m, pt), dup = 0;
                    if (cl < 0) continue;
                    for (int j = 0; j < nc; j++) dup |= clus[j] == cl;
                    if (!dup) clus[nc++] = cl;
                }
                if (!nc) continue;
                open++;
                int nn = 0;
                for (int i = 0; i < ns; i++) {
                    double r;
                    int cl;
                    const LgDesc *d = lg_grid_static(g, i, &r, &cl, NULL);
                    double dd = box_dist(key, p.cell, d->pos);
                    int seen = 0;
                    for (int j = 0; j < nc && cl >= 0; j++) seen |= lg_cluster_sees(m, clus[j], cl);
                    if (dd > r || !seen) continue;
                    float imp = (float)lg_importance(d, r, dd);
                    if (imp > 0) { c[nn++] = (RC){ i, imp }; dist[i] = dd; }
                }
                if (nn <= p.k_static) continue;
                over++;
                qsort(c, (size_t)nn, sizeof *c, rc_cmp);
                for (int j = p.k_static; j < nn; j++) if (dist[c[j].id] < rho[c[j].id]) rho[c[j].id] = dist[c[j].id];
            }
    int diff = 0, split = 0;
    for (int i = 0; i < ns; i++) {
        double r;
        lg_grid_static(g, i, &r, NULL, NULL);
        split += rho[i] < r;
        if (rho[i] != lg_grid_rho(g, i) && diff++ < 3) CHECK(0, "split: light %d rho %.9g, the brute-force pass %.9g", i, lg_grid_rho(g, i), rho[i]);
    }
    printf("split: rho of all %d lights = the brute-force pass over %ld cells (%ld open, %ld with more than %d candidates); %d lights split;"
           " lg_grid_new %.1f ms\n", ns, cells, open, over, p.k_static, split, new_ms);
    CHECK(split > 50, "split: only %d lights split on gm_construct", split);
    free(rho); free(dist); free(c);
    lg_grid_free(g);
    lg_map_free(m);
}

/* --- P5b: the walk. The diffuse light a pixel at p with normal nrm gets: the per-pixel loop over p's cell (photon_lights_ps.h's math, N.L) plus,
 * with fold, the fold cube at p (the shader's n^2 weights), in luminance. walk() moves p along a line through cell borders every 0.5 units, or keeps
 * it and orbits dynamic light 0 round it a degree a frame, and returns the largest step relative to the mean, and where it was. */
static double pix_light(const LgGrid *g, const float p[3], const float nrm[3], int fold) {
    const float *img = lg_grid_image(g);
    int key[3];
    float c63[4];
    lg_grid_constants(g, c63);
    for (int a = 0; a < 3; a++) key[a] = (int)floorf(p[a] * c63[3]);
    int i = tor(key);
    const float *t = img + ((size_t)(i / LG_GX) * LG_TEX_W + 4 * (size_t)(i % LG_GX)) * 4;
    double sum = 0;
    for (int j = 0; j < (int)t[0]; j++) {
        int rec = (int)t[1 + j];
        const float *r = img + ((size_t)(LG_REC_ROW + rec / 32) * LG_TEX_W + 4 * (size_t)(rec % 32)) * 4;
        float to[3] = { r[0] - p[0], r[1] - p[1], r[2] - p[2] };
        float d2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
        if (d2 < 1e-6f) d2 = 1e-6f;
        float ood = 1.0f / sqrtf(d2), l[3] = { to[0] * ood, to[1] * ood, to[2] * ood };
        float da = 1.0f / fmaxf(r[12] + r[13] * d2 * ood + r[14] * d2, 1e-6f);
        float sp = r[7] > 0.5f ? fminf(fmaxf(powf(fmaxf(1e-4f, -(r[8] * l[0] + r[9] * l[1] + r[10] * l[2]) - r[15]), r[11]), 0.0f), 1.0f) : 1.0f;
        float rr = d2 * r[3], wnd = fminf(fmaxf(1.0f - rr * rr, 0.0f), 1.0f);
        float nl = fmaxf(0.0f, nrm[0] * l[0] + nrm[1] * l[1] + nrm[2] * l[2]);
        sum += (0.3 * r[4] + 0.59 * r[5] + 0.11 * r[6]) * nl * da * sp * wnd * wnd;
    }
    if (!fold) return sum;
    int ntab;
    const LgFoldLight *tab = lg_grid_fold_table(g, &ntab, NULL);
    float cube[6][3];
    lg_fold_eval(tab, ntab, lg_grid_static_count(g), lg_grid_fold_image(g) + (size_t)(i / LG_GX) * LG_FOLD_ROW + (size_t)(i % LG_GX) * LG_FOLD_STRIDE, p, cube);
    for (int a = 0; a < 3; a++) {
        const float *f = cube[2 * a + (nrm[a] < 0)];
        sum += (double)nrm[a] * nrm[a] * (0.3 * f[0] + 0.59 * f[1] + 0.11 * f[2]);
    }
    return sum;
}
static double g_walk_inner, g_walk_inner_at;   /* the walk's largest step inside a cell (a light's own shape: a cone edge, the near field), and where */
static double walk(LgGrid *g, LgSnapshot *s, const float a[3], const float b[3], const float nrm[3], int fold, int orbit, double *at, double *mean_out) {
    LgStats st;
    for (int w = 0; w < 600; w++) { lg_grid_frame(g, s, &st); if (!st.pending) break; }
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    int nstep = orbit ? 360 : (int)(sqrtf(dx * dx + dy * dy + dz * dz) / 0.5f);
    double prev = -1, mx = 0, sum = 0, inner = 0;
    float pprev[3] = { 0, 0, 0 };
    g_walk_inner_at = 0;
    for (int k = 0; k <= nstep; k++) {
        float p[3];
        if (orbit) {
            for (int q = 0; q < 3; q++) p[q] = a[q];
            double ang = k * 3.14159265358979 / 180.0;
            s->dl[0].origin[0] = (float)(a[0] + 60 * cos(ang));
            s->dl[0].origin[1] = (float)(a[1] + 60 * sin(ang));
            s->dl[0].origin[2] = a[2] + 40;
            lg_grid_frame(g, s, &st);
        } else
            for (int q = 0; q < 3; q++) p[q] = a[q] + (b[q] - a[q]) * (float)k / (float)nstep;
        double v = pix_light(g, p, nrm, fold);
        sum += v;
        int border = 0;   /* the step from pprev to p crosses a cell border in x (the walks run along x): the jump is measured at it, +- 0.001 */
        if (k && !orbit) border = floorf(p[0] / 128.0f) != floorf(pprev[0] / 128.0f);
        if (orbit && prev >= 0 && fabs(v - prev) > mx) { mx = fabs(v - prev); *at = k; }
        if (border) {
            float xb = 128.0f * floorf(p[0] / 128.0f), q0[3] = { xb - 0.001f, p[1], p[2] }, q1[3] = { xb + 0.001f, p[1], p[2] };
            double j = fabs(pix_light(g, q1, nrm, fold) - pix_light(g, q0, nrm, fold));
            if (j > mx) { mx = j; *at = xb; }
        }
        if (prev >= 0 && !border && !orbit && fabs(v - prev) > inner) { inner = fabs(v - prev); g_walk_inner_at = p[0]; }
        prev = v;
        memcpy(pprev, p, sizeof p);
    }
    double mean = sum / (nstep + 1);
    *mean_out = mean;
    g_walk_inner = mean > 0 ? inner / mean : 0;
    return mean > 0 ? mx / mean : 0;
}
static void test_walk(const uint8_t *b, size_t n) {
    LgMap *m = load(b, n, 1);
    if (!m) return;
    /* the ColourRoom, x across three cell borders (-2560, -2432, -2304), at 3 heights and depths; normals up, +x, -y */
    static const float nrms[3][3] = { { 0, 0, 1 }, { 0.6f, 0.48f, 0.64f }, { -0.48f, -0.6f, 0.64f } };   /* up, and two oblique ones (the cube is exact on axes) */
    static const char *names[3] = { "P5b", "no split, fold", "P5" };
    double worst[3] = { 0, 0, 0 }, orbit[2] = { 0, 0 }, inner[3] = { 0, 0, 0 };
    for (int mode = 0; mode < 3; mode++) {   /* 0 P5b (split + fold), 1 no split (only what a full cell drops is folded), 2 P5 (no split, nothing folded) */
        LgParams p;
        lg_params_default(&p);
        p.budget = 0;
        if (mode) p.k_static = 0;
        LgGrid *g = NULL;
        if (lg_grid_new(m, &p, &g) != AMB_OK) { CHECK(0, "walk: lg_grid_new"); continue; }
        LgSnapshot s;
        memset(&s, 0, sizeof s);
        for (int i = 0; i < 128; i++) s.style[i] = 264;
        s.cam[0] = -2400; s.cam[1] = -3700; s.cam[2] = -200;
        for (int h = 0; h < 3; h++)
            for (int q = 0; q < 3; q++) {
                float a[3] = { -2620, -3700 + 37.0f * h, -250 + 60.0f * h }, bb[3] = { -2240, -3700 + 37.0f * h, -250 + 60.0f * h };   /* x borders -2560, -2432, -2304 */
                double at = 0, mean;
                double r = walk(g, &s, a, bb, nrms[q], mode < 2, 0, &at, &mean);
                if (r > worst[mode]) worst[mode] = r;
                if (g_walk_inner > inner[mode]) inner[mode] = g_walk_inner;
                if (h == 1)
                    printf("walk %s: ColourRoom z %.0f, normal %.2f %.2f %.2f: mean %.4f, largest jump at a cell border %.3f%% (x %.0f), largest 0.5-unit step"
                           " inside a cell %.3f%% (x %.1f)\n", names[mode], a[2], nrms[q][0], nrms[q][1], nrms[q][2], mean, 100 * r, at, 100 * g_walk_inner,
                           g_walk_inner_at);
            }
        if (mode < 2) {   /* the overflow change: 8 dynamic lights round a point (more than its free slots), one of them orbiting */
            float c[3] = { -2400, -3663, -190 }, up[3] = { 0, 0, 1 }, side[3] = { 1, 0, 0 };
            s.dl_mask = 0xff;
            for (int i = 0; i < 8; i++) {
                LgDlight *d = &s.dl[i];
                memset(d, 0, sizeof *d);
                double ang = i * 3.14159265358979 / 4 + 0.2;
                d->origin[0] = (float)(c[0] + 60 * cos(ang)); d->origin[1] = (float)(c[1] + 60 * sin(ang)); d->origin[2] = c[2] + 40;
                d->radius = 250; d->r = d->g = d->b = 200;
            }
            double at = 0, mean;
            double r1 = walk(g, &s, c, c, up, 1, 1, &at, &mean);
            double r2 = walk(g, &s, c, c, side, 1, 1, &at, &mean);
            orbit[mode] = r1 > r2 ? r1 : r2;
            LgStats st;
            lg_grid_frame(g, &s, &st);
            printf("orbit %s: 8 dynamic lights round a ColourRoom point, one orbiting (overflow %d lights now): largest one-degree step %.2f%%"
                   " (normal up %.2f%%, +x %.2f%%)\n", names[mode], st.overflow_lights, 100 * orbit[mode], 100 * r1, 100 * r2);
            s.dl_mask = 0;
        }
        lg_grid_free(g);
    }
    printf("walk: worst jump at a cell border over 9 walks: P5b %.3f%%, no split %.3f%%, P5 %.3f%% (0.5-unit steps inside a cell: %.3f%%, %.3f%%,"
           " %.3f%%)\n", 100 * worst[0], 100 * worst[1], 100 * worst[2], 100 * inner[0], 100 * inner[1], 100 * inner[2]);
    CHECK(worst[0] < 0.001, "walk: P5b jumps %.3f%% at a cell border (a seam)", 100 * worst[0]);
    CHECK(worst[2] > 4 * worst[0], "walk: P5's steps (%.2f%%) are not larger than P5b's (%.2f%%): the walk proves nothing", 100 * worst[2], 100 * worst[0]);
    lg_map_free(m);
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static void bench(const uint8_t *b, size_t n, int frames, int elights) {
    LgMap *m = load(b, n, 1);
    if (!m) return;
    LgParams p;
    lg_params_default(&p);
    LgGrid *g = NULL;
    lg_grid_new(m, &p, &g);
    LgSnapshot s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 128; i++) s.style[i] = 264;
    g_rng = 77;
    double cam[3] = { -1500, -1000, -40 };
    memcpy(s.cam, cam, sizeof cam);
    s.dl_mask = 0xffffffffu;
    s.el_on = elights;
    for (int i = 0; i < LG_NDL; i++) { random_light(&s.dl[i], cam, 0); s.dl[i].flags = 0; s.dl[i].radius = 256; }
    for (int i = 0; i < LG_NEL; i++) { random_light(&s.el[i], cam, 1); s.el[i].flags = 0; s.el[i].radius = 256; }
    double *ms = malloc((size_t)frames * sizeof *ms);
    LgStats st;
    for (int w = 0; w < 120; w++) lg_grid_frame(g, &s, &st);   /* the window's static part in */
    int maxl = 0, nlights = 0;
    for (int f = 0; f < frames; f++) {
        s.cam[0] = cam[0] + 600.0 * sin(f * (2.0 * 3.14159265358979 / 1200.0));   /* walking back and forth, up to 3 u a frame */
        for (int i = 0; i < LG_NDL; i++) s.dl[i].origin[0] += (float)(rnd() * 8 - 4);
        for (int i = 0; i < LG_NEL; i++) s.el[i].origin[1] += (float)(rnd() * 8 - 4);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        lg_grid_frame(g, &s, &st);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms[f] = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6;
        if (st.max_n > maxl) maxl = st.max_n;
        nlights = st.dyn_lights + st.records_static;
    }
    qsort(ms, (size_t)frames, sizeof *ms, cmp_d);
    printf("bench: %d frames, %d lights in use (%d dynamic moving every frame, %d static records), max %d per cell, %d pending: build p50 %.3f ms,"
           " p99 %.3f ms, max %.3f ms\n", frames, nlights, st.dyn_lights, st.records_static, maxl, st.pending, ms[frames / 2], ms[frames * 99 / 100],
           ms[frames - 1]);
    free(ms);
    lg_grid_free(g);
    lg_map_free(m);
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "--dump") == 0) {
        size_t n;
        uint8_t *b = read_file(argv[2], &n);
        if (!b) return 2;
        LgMap *m = load(b, n, atoi(argv[3]));
        if (m) dump(m);
        lg_map_free(m);
        free(b);
        return fails ? 1 : 0;
    }
    if (argc >= 5 && strcmp(argv[1], "--pvsdist") == 0) {
        size_t n;
        uint8_t *b = read_file(argv[2], &n);
        if (!b) return 2;
        LgMap *m = load(b, n, 1);
        double box[6];
        for (int i = 0; i < 6 && argc >= 11; i++) box[i] = atof(argv[5 + i]);
        if (m) pvsdist(m, strtoull(argv[3], NULL, 10), atoi(argv[4]), argc >= 11 ? box : NULL);
        lg_map_free(m);
        free(b);
        return fails ? 1 : 0;
    }
    if (argc >= 6 && strcmp(argv[1], "--count") == 0) {   /* P5 (c): the grid's static lights at a world point, every style on, the defaults */
        size_t n;
        uint8_t *b = read_file(argv[2], &n);
        if (!b) return 2;
        LgMap *m = load(b, n, 1);
        LgParams p;
        lg_params_default(&p);
        p.budget = 0;
        LgGrid *g = NULL;
        if (!m || lg_grid_new(m, &p, &g) != AMB_OK) return 1;
        LgSnapshot s;
        memset(&s, 0, sizeof s);
        for (int i = 0; i < 128; i++) s.style[i] = 264;
        for (int k = 0; k < 3; k++) s.cam[k] = atof(argv[3 + k]);
        LgStats st;
        lg_grid_frame(g, &s, &st);
        int key[3];
        for (int k = 0; k < 3; k++) key[k] = (int)floor(s.cam[k] / p.cell);
        int i = tor(key);
        const float *t = lg_grid_image(g) + ((size_t)(i / LG_GX) * LG_TEX_W + 4 * (size_t)(i % LG_GX)) * 4;
        TDyn dyn[1];
        RefCell rf;
        ref_cell(m, g, &p, dyn, 0, &s, p.k, key, &rf);
        printf("count %s %s %s: cell %d %d %d holds %d grid lights (reference %d, %d dropped beyond k %d; %d folded or split, k_static %d)\n", argv[3],
               argv[4], argv[5], key[0], key[1], key[2], (int)t[0], rf.n, rf.over, p.k, rf.nfold, p.k_static);
        lg_grid_free(g);
        lg_map_free(m);
        free(b);
        return 0;
    }
    int frames = 40, seeds = 4, bframes = 0, a = 1, split_ref = 0;
    for (; a < argc && argv[a][0] == '-'; a += 2) {
        if (a + 1 >= argc) return 2;
        if (strcmp(argv[a], "--frames") == 0) frames = atoi(argv[a + 1]);
        else if (strcmp(argv[a], "--seeds") == 0) seeds = atoi(argv[a + 1]);
        else if (strcmp(argv[a], "--bench") == 0) bframes = atoi(argv[a + 1]);
        else if (strcmp(argv[a], "--split") == 0) split_ref = atoi(argv[a + 1]);
    }
    test_convert();
    for (int i = a; i < argc; i++) {
        size_t n;
        uint8_t *b = read_file(argv[i], &n);
        if (!b) { printf("FAIL cannot read %s\n", argv[i]); fails++; continue; }
        int construct = strstr(argv[i], "gm_construct") != NULL;
        test_map(b, n, construct);
        if (construct && seeds) test_grid(b, n, seeds, frames);
        if (construct && split_ref) { test_split(b, n); test_walk(b, n); }
        if (construct && bframes) { bench(b, n, bframes, 0); bench(b, n, bframes, 1); }
        free(b);
    }
    if (fails) { printf("LGTEST FAILED: %d\n", fails); return 1; }
    printf("LGTEST OK\n");
    return 0;
}
