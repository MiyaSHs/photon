/* P3 (a): checks src/ambient/amb_bake.c against tools/lpv2_ambient_volume.py (dumps from py_reference.py) and its own
 * contracts. Run through scripts/run_ambient_test.sh.
 *   ambtest <map.bsp> [--points ref.bin] [--bake ref.bin ...] [--centre x y z] [--small]
 * --small: the ASan/UBSan pass (small boxes, no timing). Exit code 0 only if every check holds. */
#define _POSIX_C_SOURCE 200809L
#include "../../src/ambient/amb_bake.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_fails++; } } while (0)

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { printf("cannot read %s\n", path); exit(2); }
    fclose(f);
    *len = (size_t)n;
    return p;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

/* --- a pthread AmbParallelFor: one job at a time, tasks claimed with an atomic counter ------------------------- */
typedef struct {
    pthread_t th[16];
    int n;
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    long gen;
    int quit, running;
    int total;
    int next;
    AmbTaskFn fn;
    void *ctx;
} Pool;
static void work(Pool *P) {
    for (;;) {
        int i = __atomic_fetch_add(&P->next, 1, __ATOMIC_RELAXED);
        if (i >= P->total) return;
        P->fn(P->ctx, i);
    }
}
static void *worker(void *arg) {
    Pool *P = arg;
    long seen = 0;
    pthread_mutex_lock(&P->mu);
    for (;;) {
        while (P->gen == seen && !P->quit) pthread_cond_wait(&P->go, &P->mu);
        if (P->quit) break;
        seen = P->gen;
        pthread_mutex_unlock(&P->mu);
        work(P);
        pthread_mutex_lock(&P->mu);
        if (--P->running == 0) pthread_cond_signal(&P->done);
    }
    pthread_mutex_unlock(&P->mu);
    return NULL;
}
static void pool_for(void *pool, int n, AmbTaskFn fn, void *ctx) {
    Pool *P = pool;
    pthread_mutex_lock(&P->mu);
    P->total = n; P->next = 0; P->fn = fn; P->ctx = ctx; P->running = P->n; P->gen++;
    pthread_cond_broadcast(&P->go);
    while (P->running) pthread_cond_wait(&P->done, &P->mu);
    pthread_mutex_unlock(&P->mu);
}
static void pool_start(Pool *P, int n) {
    memset(P, 0, sizeof *P);
    P->n = n;
    pthread_mutex_init(&P->mu, NULL);
    pthread_cond_init(&P->go, NULL);
    pthread_cond_init(&P->done, NULL);
    for (int i = 0; i < n; i++) pthread_create(&P->th[i], NULL, worker, P);
}
static void pool_stop(Pool *P) {
    pthread_mutex_lock(&P->mu);
    P->quit = 1;
    pthread_cond_broadcast(&P->go);
    pthread_mutex_unlock(&P->mu);
    for (int i = 0; i < P->n; i++) pthread_join(P->th[i], NULL);
}

/* --- reference comparisons ------------------------------------------------------------------------------------ */
static int over_tol(double a, double b) { return fabs(a - b) > 1e-3 * fmax(fabs(a), fabs(b)) + 1e-6; }

static void check_points(const AmbMap *m, const char *path) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    CHECK(memcmp(d, "AMBP", 4) == 0, "points header");
    int N;
    memcpy(&N, d + 4, 4);
    const uint8_t *pp = d + 8, *pl = pp + (size_t)N * 24, *pc = pl + (size_t)N * 4, *pv = pc + (size_t)N * 4;
    long bad_leaf = 0, bad_count = 0, over = 0, zero = 0;
    double maxrel = 0;
    for (int i = 0; i < N; i++) {
        double p[3], val[18];
        int leaf, cnt;
        float o[18];
        memcpy(p, pp + (size_t)i * 24, 24);
        memcpy(&leaf, pl + (size_t)i * 4, 4);
        memcpy(&cnt, pc + (size_t)i * 4, 4);
        memcpy(val, pv + (size_t)i * 144, 144);
        bad_leaf += amb_point_leaf(m, p) != leaf;
        int c = amb_eval_point(m, p, 1.0, o);
        bad_count += c != cnt;
        zero += cnt == 0;
        for (int k = 0; k < 18; k++) {
            over += over_tol(o[k], val[k]);
            double den = fmax(fabs(o[k]), fabs(val[k]));
            if (den > 1e-3) maxrel = fmax(maxrel, fabs(o[k] - val[k]) / den);
        }
    }
    printf("  points: %d (%ld with no samples): leaf mismatches %ld, count mismatches %ld, values over tolerance %ld, max rel %.2e\n",
           N, zero, bad_leaf, bad_count, over, maxrel);
    CHECK(N > 0 && bad_leaf == 0 && bad_count == 0 && over == 0, "points differ from the Python");
    free(d);
}

static void check_bake(const AmbMap *m, const char *path) {
    size_t len;
    uint8_t *d = read_file(path, &len);
    CHECK(memcmp(d, "AMBV", 4) == 0, "bake header");
    int org[3], dim[3], blur;
    double vxy, vz, soft;
    memcpy(org, d + 4, 12);
    memcpy(dim, d + 16, 12);
    memcpy(&vxy, d + 28, 8);
    memcpy(&vz, d + 36, 8);
    memcpy(&soft, d + 44, 8);
    memcpy(&blur, d + 52, 4);
    size_t nv = (size_t)dim[0] * (size_t)dim[1] * (size_t)dim[2];
    const float *rv = (const float *)(void *)(d + 56);   /* header size keeps it 4-aligned */
    const uint8_t *ro = d + 56 + nv * 72;
    CHECK(len == 56 + nv * 73, "bake file size");
    AmbBakeParams P;
    amb_params_default(&P);
    P.voxel_xy = vxy; P.voxel_z = vz; P.soft = soft; P.blur_passes = blur; P.wrap_edges = 1;
    Pool pool;
    pool_start(&pool, 8);
    P.pfor = pool_for; P.pool = &pool;
    AmbBox box;
    memcpy(box.org, org, 12);
    memcpy(box.dim, dim, 12);
    float *v = malloc(nv * 72);
    uint8_t *o = malloc(nv);
    double t0 = now();
    int rc = amb_bake_box(m, &P, &box, v, o);
    double t1 = now();
    CHECK(rc == AMB_OK, "bake rc %d", rc);
    size_t over = 0, odiff = 0;
    double maxrel = 0;
    for (size_t i = 0; i < nv; i++) {
        odiff += o[i] != ro[i];
        for (int k = 0; k < 18; k++) {
            double a = v[i * 18 + (size_t)k], b = rv[i * 18 + (size_t)k];
            over += over_tol(a, b);
            double den = fmax(fabs(a), fabs(b));
            if (den > 1e-3) maxrel = fmax(maxrel, fabs(a - b) / den);
        }
    }
    printf("  bake %s: %dx%dx%d @ %g/%g soft %g blur %d: open mismatches %zu, values over 1e-3 rel %zu, max rel %.2e; %.0f ms (8 threads; indicative)\n",
           strrchr(path, '/') ? strrchr(path, '/') + 1 : path, dim[0], dim[1], dim[2], vxy, vz, soft, blur, odiff, over, maxrel, (t1 - t0) * 1e3);
    CHECK(odiff == 0 && over == 0, "bake differs from the Python");
    pool_stop(&pool);
    free(v); free(o); free(d);
}

/* --- own contracts --------------------------------------------------------------------------------------------- */
static void check_determinism(const AmbMap *m, const AmbBox *box) {
    size_t nv = (size_t)box->dim[0] * (size_t)box->dim[1] * (size_t)box->dim[2];
    float *a = malloc(nv * 72), *b = malloc(nv * 72);
    AmbBakeParams P;
    amb_params_default(&P);
    amb_bake_box(m, &P, box, a, NULL);
    int ok = 1;
    int counts[] = { 1, 2, 8 };
    for (int t = 0; t < 3; t++) {
        Pool pool;
        pool_start(&pool, counts[t]);
        P.pfor = pool_for; P.pool = &pool;
        amb_bake_box(m, &P, box, b, NULL);
        int same = memcmp(a, b, nv * 72) == 0;
        printf("  determinism: inline vs %d thread(s): %s\n", counts[t], same ? "bit-identical" : "DIFFERENT");
        ok &= same;
        pool_stop(&pool);
    }
    CHECK(ok, "thread count changes the result");
    free(a); free(b);
}

static void check_regions(const AmbMap *m, const AmbBox *big) {
    AmbBakeParams P;
    amb_params_default(&P);
    size_t nb = (size_t)big->dim[0] * (size_t)big->dim[1] * (size_t)big->dim[2];
    float *bv = malloc(nb * 72);
    CHECK(amb_bake_region(m, &P, big, bv, NULL) == AMB_OK, "region bake");
    /* sub-boxes at the corners and the middle, incl. 1-voxel slabs: bit-equal to the same voxels of the big bake */
    long diffs = 0;
    int cases = 0;
    for (int c = 0; c < 8; c++) {
        AmbBox s;
        for (int a = 0; a < 3; a++) {
            s.dim[a] = c == 7 ? big->dim[a] / 2 : (c == a ? 1 : big->dim[a] / 2);
            int ofs = c == 6 ? big->dim[a] / 4 : (c >> a & 1) ? big->dim[a] - s.dim[a] : 0;
            s.org[a] = big->org[a] + ofs;
        }
        size_t ns = (size_t)s.dim[0] * (size_t)s.dim[1] * (size_t)s.dim[2];
        float *sv = malloc(ns * 72);
        CHECK(amb_bake_region(m, &P, &s, sv, NULL) == AMB_OK, "sub-region bake");
        for (int k = 0; k < s.dim[2]; k++)
            for (int j = 0; j < s.dim[1]; j++)
                for (int i = 0; i < s.dim[0]; i++) {
                    size_t si = (((size_t)k * (size_t)s.dim[1] + (size_t)j) * (size_t)s.dim[0] + (size_t)i) * 18;
                    size_t bi = (((size_t)(k + s.org[2] - big->org[2]) * (size_t)big->dim[1] + (size_t)(j + s.org[1] - big->org[1])) *
                                     (size_t)big->dim[0] + (size_t)(i + s.org[0] - big->org[0])) * 18;
                    diffs += memcmp(sv + si, bv + bi, 72) != 0;
                }
        free(sv);
        cases++;
    }
    /* a margin too small must show: the check above would pass trivially on uniform data */
    AmbBakeParams Q = P;
    AmbBox g = *big;
    for (int a = 0; a < 3; a++) { g.org[a] -= 1; g.dim[a] += 2; }
    size_t ng = (size_t)g.dim[0] * (size_t)g.dim[1] * (size_t)g.dim[2];
    float *gv = malloc(ng * 72);
    amb_bake_box(m, &Q, &g, gv, NULL);
    long thin = 0;
    for (int k = 0; k < big->dim[2]; k++)
        for (int j = 0; j < big->dim[1]; j++)
            for (int i = 0; i < big->dim[0]; i++) {
                size_t gi = (((size_t)(k + 1) * (size_t)g.dim[1] + (size_t)(j + 1)) * (size_t)g.dim[0] + (size_t)(i + 1)) * 18;
                size_t bi = (((size_t)k * (size_t)big->dim[1] + (size_t)j) * (size_t)big->dim[0] + (size_t)i) * 18;
                thin += memcmp(gv + gi, bv + bi, 72) != 0;
            }
    free(gv);
    printf("  regions: %d sub-boxes (slabs, corners, middle): %ld voxels differ from the big region; a 1-voxel margin instead differs at %ld voxels (must be > 0)\n",
           cases, diffs, thin);
    CHECK(diffs == 0, "a region bake depends on the box");
    CHECK(thin > 0, "the margin check has no power on this box");
    free(bv);
}

static void check_half(void) {
    long rt = 0, nearest = 0;
    for (uint32_t h = 0; h < 65536; h++) {
        uint16_t hh = (uint16_t)h;
        float f = amb_f16_to_f32(hh);
        if ((h & 0x7c00) == 0x7c00 && (h & 0x3ff)) { rt += !isnan(f); continue; }
        rt += amb_f32_to_f16(f) != hh;
    }
    /* random float patterns: the result is a closest half, and ties go to the even one */
    uint64_t state = 88172645463325252ull;
    for (long n = 0; n < 4000000; n++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        uint32_t x = (uint32_t)(state >> 32);
        float f;
        memcpy(&f, &x, 4);
        if (isnan(f) || isinf(f) || fabsf(f) >= 65520.0f) continue;
        uint16_t h = amb_f32_to_f16(f);
        double err = fabs((double)amb_f16_to_f32(h) - (double)f);
        uint16_t mag = h & 0x7fff;
        double up = mag < 0x7bff ? fabs((double)amb_f16_to_f32((uint16_t)((h & 0x8000) | (mag + 1))) - (double)f) : 1e30;
        double dn = mag > 0 ? fabs((double)amb_f16_to_f32((uint16_t)((h & 0x8000) | (mag - 1))) - (double)f) : 1e30;
        if (err > up || err > dn || (err == up && (h & 1)) || (err == dn && (h & 1))) nearest++;
    }
    struct { float f; uint16_t h; } edge[] = { { 0.0f, 0 }, { -0.0f, 0x8000 }, { 1.0f, 0x3c00 }, { 65504.0f, 0x7bff }, { 65520.0f, 0x7c00 },
                                               { 1e30f, 0x7c00 }, { 5.9604644775390625e-08f, 1 }, { 2.98023223876953125e-08f, 0 },
                                               { 6.103515625e-05f, 0x400 }, { (float)INFINITY, 0x7c00 } };
    int e = 0;
    for (size_t i = 0; i < sizeof edge / sizeof edge[0]; i++) e += amb_f32_to_f16(edge[i].f) != edge[i].h;
    printf("  half: round trip failures %ld of 65536, not-nearest %ld of ~4M random floats, edge cases wrong %d\n", rt, nearest, e);
    CHECK(rt == 0 && nearest == 0 && e == 0, "half conversion");
}

/* Software fetch of the toroidal atlas exactly as amb_bake.h tells the shader to read it (linear filter, WRAP on V, W) */
static void fetch(const uint8_t *atlas, const int X, const int Y, const int Z, size_t rp, size_t sp, int face, const double vox[3], float out[3]) {
    double band = (vox[0] - 0.5) / X;
    band = (band - floor(band)) * X + 0.5;
    double tu = (double)face * (double)(X + 1) + band - 0.5, tv = vox[1] - 0.5, tw = vox[2] - 0.5;
    int iu = (int)floor(tu), iv = (int)floor(tv), iw = (int)floor(tw);
    double fu = tu - iu, fv = tv - iv, fw = tw - iw;
    double acc[3] = { 0, 0, 0 };
    for (int c = 0; c < 8; c++) {
        int u = iu + (c & 1), v = (((iv + (c >> 1 & 1)) % Y) + Y) % Y, w = (((iw + (c >> 2 & 1)) % Z) + Z) % Z;
        double wt = ((c & 1) ? fu : 1 - fu) * ((c >> 1 & 1) ? fv : 1 - fv) * ((c >> 2 & 1) ? fw : 1 - fw);
        uint16_t q[3];
        memcpy(q, atlas + (size_t)w * sp + (size_t)v * rp + (size_t)u * 8, 6);
        for (int k = 0; k < 3; k++) acc[k] += wt * (double)amb_f16_to_f32(q[k]);
    }
    for (int k = 0; k < 3; k++) out[k] = (float)acc[k];
}

/* The clipmap: a cascade written whole, scrolled by slabs only, must equal a fresh write of the new window byte for
 * byte, and the shader's fetch must equal a direct trilinear of the baked volume, across the seams. */
static void check_toroidal(const AmbMap *m, const int centre[3], int X, int Y, int Z) {
    AmbBakeParams P;
    amb_params_default(&P);
    int clip[3] = { X, Y, Z };
    size_t rp = (size_t)(6 * AMB_BAND_TEXELS(X)) * 8, sp = rp * (size_t)Y, bytes = sp * (size_t)Z;
    uint8_t *A = calloc(1, bytes), *B = calloc(1, bytes);
    /* x origin chosen so the U seam falls mid-window after the +3 scroll below, whatever the map's centre */
    int cx = (int)floor((double)centre[0] / X) * X - 3;
    AmbBox w0 = { { cx - X / 2, centre[1] - Y / 2, centre[2] - Z / 2 }, { X, Y, Z } };
    size_t nv = (size_t)X * (size_t)Y * (size_t)Z;
    float *v0 = malloc(nv * 72);
    CHECK(amb_bake_region(m, &P, &w0, v0, NULL) == AMB_OK, "cascade bake");
    CHECK(amb_write_toroidal(v0, &w0, clip, A, rp, sp) == AMB_OK, "cascade write");
    /* scroll: +3 x, -2 y, +1 z, each axis as its own slab bake and write */
    AmbBox w1 = w0;
    int step[3] = { 3, -2, 1 };
    for (int a = 0; a < 3; a++) {
        AmbBox s = w1;
        int n = abs(step[a]);
        s.dim[a] = n;
        s.org[a] = step[a] > 0 ? w1.org[a] + w1.dim[a] : w1.org[a] - n;
        size_t ns = (size_t)s.dim[0] * (size_t)s.dim[1] * (size_t)s.dim[2];
        float *sv = malloc(ns * 72);
        CHECK(amb_bake_region(m, &P, &s, sv, NULL) == AMB_OK, "slab bake");
        CHECK(amb_write_toroidal(sv, &s, clip, A, rp, sp) == AMB_OK, "slab write");
        free(sv);
        w1.org[a] += step[a];
    }
    float *v1 = malloc(nv * 72);
    CHECK(amb_bake_region(m, &P, &w1, v1, NULL) == AMB_OK, "fresh bake");
    CHECK(amb_write_toroidal(v1, &w1, clip, B, rp, sp) == AMB_OK, "fresh write");
    int same = memcmp(A, B, bytes) == 0;
    /* fetch vs direct trilinear, at random points of the window minus a 1-voxel rim, half of them at the U seam */
    uint64_t st = 0x2545F4914F6CDD1Dull;
    long over = 0, n = 0, seam = 0;
    double maxrel = 0;
    for (int it = 0; it < 200000; it++) {
        double vox[3];
        int ci[3];
        double fr[3];
        for (int a = 0; a < 3; a++) {
            st ^= st << 13; st ^= st >> 7; st ^= st << 17;
            double r = (double)(st >> 11) / 9007199254740992.0;
            double lo = w1.org[a] + 0.5 + 1, hi = w1.org[a] + clip[a] - 0.5 - 1;
            vox[a] = lo + r * (hi - lo);
            if (a == 0 && (it & 1)) {
                double k = ceil((lo - 0.5) / X) * X + 0.5;   /* voxel coordinate where band_u wraps */
                if (k - 0.6 > lo && k + 0.6 < hi) { vox[0] = k + r - 0.5; seam++; }
            }
            double t = vox[a] - 0.5 - w1.org[a];
            ci[a] = (int)floor(t);
            fr[a] = t - ci[a];
        }
        for (int f = 0; f < 6; f++) {
            float got[3];
            fetch(B, X, Y, Z, rp, sp, f, vox, got);
            double want[3] = { 0, 0, 0 };
            for (int c = 0; c < 8; c++) {
                int i = ci[0] + (c & 1), j = ci[1] + (c >> 1 & 1), k = ci[2] + (c >> 2 & 1);
                double wt = ((c & 1) ? fr[0] : 1 - fr[0]) * ((c >> 1 & 1) ? fr[1] : 1 - fr[1]) * ((c >> 2 & 1) ? fr[2] : 1 - fr[2]);
                const float *q = v1 + (((size_t)k * (size_t)Y + (size_t)j) * (size_t)X + (size_t)i) * 18 + f * 3;
                for (int e = 0; e < 3; e++) want[e] += wt * q[e];
            }
            for (int e = 0; e < 3; e++) {
                double d = fabs(got[e] - want[e]), den = fmax(fabs(got[e]), fabs(want[e]));
                n++;
                if (d > 2e-3 * den + 1e-3) over++;
                if (den > 1e-2) maxrel = fmax(maxrel, d / den);
            }
        }
    }
    long gut = 0;
    for (int k = 0; k < Z; k++)
        for (int j = 0; j < Y; j++)
            for (int f = 0; f < 6; f++) {
                const uint8_t *g = B + (size_t)k * sp + (size_t)j * rp + ((size_t)f * (size_t)(X + 1) + (size_t)X) * 8;
                const uint8_t *t0 = B + (size_t)k * sp + (size_t)j * rp + ((size_t)f * (size_t)(X + 1)) * 8;
                gut += memcmp(g, t0, 8) != 0;
            }
    printf("  toroidal %dx%dx%d: scrolled = fresh write: %s; fetch vs direct trilinear: %ld samples (%ld points at the U seam), over tolerance %ld, max rel %.2e; gutter texels differing from texel 0: %ld\n",
           X, Y, Z, same ? "byte-identical" : "DIFFERENT", n, seam, over, maxrel, gut);
    CHECK(same, "slab scrolling differs from a fresh write");
    CHECK(over == 0, "the shader fetch differs from the volume");
    CHECK(gut == 0 && seam > 0, "gutter");
    free(A); free(B); free(v0); free(v1);
}

/* Garbled maps must fail or load and walk safely (run under ASan/UBSan): mutated headers, lump bytes, truncations */
static void check_robust(const uint8_t *bsp, size_t len) {
    uint8_t *c = malloc(len);
    uint64_t st = 0x9E3779B97F4A7C15ull;
    int loaded = 0, refused = 0;
    for (int it = 0; it < 400; it++) {
        memcpy(c, bsp, len);
        size_t l = len;
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        int kind = (int)(st % 4);
        static const int lumps[] = { 1, 5, 10, 14, 51, 52, 55, 56 };
        int lump = lumps[(st >> 8) % 8];
        uint8_t *h = c + 8 + 16 * lump;
        int32_t ofs, ln;
        memcpy(&ofs, h, 4);
        memcpy(&ln, h + 4, 4);
        if (kind == 0) { int32_t v = (int32_t)(st >> 20); memcpy(h + ((st >> 16) % 3) * 4, &v, 4); }   /* header field */
        else if (kind == 1) l = (st >> 12) % len;                                                      /* truncation */
        else if (ofs > 0 && ln > 0 && (size_t)ofs + (size_t)ln <= len) {                              /* lump bytes */
            for (int b = 0; b < 200; b++) {
                st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                c[(size_t)ofs + (st >> 8) % (size_t)ln] = (uint8_t)st;
            }
        }
        AmbMap *m = NULL;
        int rc = amb_map_load(c, l, 1, &m);
        if (rc == AMB_OK) {
            loaded++;
            for (int p = 0; p < 200; p++) {
                st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                double q[3] = { (double)(int16_t)st, (double)(int16_t)(st >> 16), (double)(int16_t)(st >> 32) };
                float o[18];
                amb_eval_point(m, q, 1.0, o);
            }
            AmbBakeParams P;
            amb_params_default(&P);
            AmbBox b = { { -4, -4, -4 }, { 8, 8, 8 } };
            static float v[8 * 8 * 8 * 18];
            amb_bake_box(m, &P, &b, v, NULL);
            amb_map_free(m);
        } else refused++;
    }
    AmbMap *m0 = NULL;
    int bad = amb_map_load(NULL, 0, 1, &m0) != AMB_ERR_FORMAT || amb_map_load(bsp, 10, 1, &m0) != AMB_ERR_FORMAT || amb_map_load(bsp, len, 1, NULL) != AMB_ERR_ARG;
    printf("  robustness: 400 mutated maps: %d loaded and walked safely, %d refused; argument checks %s\n", loaded, refused, bad ? "WRONG" : "ok");
    CHECK(!bad && refused > 0, "robustness");
    free(c);
}

/* Leaf lump v0 (BSP v19): no Python path to compare with, so the cube is decoded here from the SDK layout (byte 30, engine
 * FUN_180117010) and read back through the rule at each open leaf's own sample point. A value check, because such a map loads fine. */
static void check_v0(const uint8_t *bsp, size_t len, const AmbMap *m, int centre[3]) {
    AmbMapInfo I;
    amb_map_info(m, &I);
    CHECK(I.leaf_lump_version == 0 && I.ambient_source == 1, "not a v0 map: lump version %d, source %d", I.leaf_lump_version, I.ambient_source);
    int32_t ofs, ln;
    memcpy(&ofs, bsp + 8 + 16 * 10, 4);
    memcpy(&ln, bsp + 8 + 16 * 10 + 4, 4);
    CHECK((size_t)ofs + (size_t)ln <= len && ln % 56 == 0, "leaf lump");
    int checked = 0, wrong = 0;
    float top = 0;
    for (int i = 0; i < ln / 56; i++) {
        const uint8_t *r = bsp + ofs + 56 * (size_t)i;
        int32_t contents;
        memcpy(&contents, r, 4);
        if (contents & 1) continue;
        double p[3];
        for (int a = 0; a < 3; a++) {
            int16_t lo, hi;
            memcpy(&lo, r + 8 + 2 * a, 2);
            memcpy(&hi, r + 14 + 2 * a, 2);
            p[a] = lo + (128.0 / 255.0) * (hi - lo);
        }
        if (amb_point_leaf(m, p) != i) continue;   /* a sample point outside its own convex leaf: another leaf answers */
        float o[18];
        amb_eval_point(m, p, 1.0, o);
        if (!checked) for (int a = 0; a < 3; a++) centre[a] = (int)floor(p[a] / 32);   /* an open spot for the box checks below */
        checked++;
        for (int f = 0; f < 6; f++)
            for (int c = 0; c < 3; c++) {
                const uint8_t *e = r + 30 + f * 4;
                double want = ldexp((double)e[c], (int8_t)e[3]);
                if (want > 1024.0) want = 1024.0;
                if (over_tol(o[f * 3 + c], want)) { wrong++; }
                if (o[f * 3 + c] > top) top = o[f * 3 + c];
            }
    }
    printf("  leaf lump v0: %d open leaves checked, %d values wrong, largest %.3f\n", checked, wrong, top);
    CHECK(checked > 0 && wrong == 0, "v0 cubes differ from the layout at byte 30");
}

/* The limits a Workshop map must not get past, and the two control paths */
static void check_guards(const AmbMap *m, const int centre[3]) {
    AmbBakeParams P;
    amb_params_default(&P);
    AmbBox b = { { centre[0], centre[1], centre[2] }, { 4, 4, 4 } };
    int rc_null = amb_bake_region(m, &P, &b, NULL, NULL);
    float v[4 * 4 * 4 * 18];
    AmbBox wild = { { -2147483647 - 1, 0, 0 }, { 4, 4, 4 } };   /* what a NaN camera becomes through a float to int conversion */
    int rc_wild = amb_bake_region(m, &P, &wild, v, NULL);
    volatile int stop = 1;
    P.cancel = &stop;
    int rc_cancel = amb_bake_region(m, &P, &b, v, NULL);
    P.cancel = NULL;
    double nan = strtod("nan", NULL), hi[3] = { 1, 1, 1 }, lo[3] = { nan, 0, 0 };
    AmbBox out;
    int rc_nan = amb_box_from_bounds(lo, hi, 32, 32, &out);
    printf("  guards: NULL output %d, wild origin %d, cancel %d, NaN bounds %d\n", rc_null, rc_wild, rc_cancel, rc_nan);
    CHECK(rc_null == AMB_ERR_ARG && rc_wild == AMB_ERR_ARG && rc_cancel == AMB_ERR_CANCELLED && rc_nan == AMB_ERR_ARG, "guards");
}

static void timing(const AmbMap *m, const int centre[3]) {
    AmbBakeParams P;
    amb_params_default(&P);
    AmbBox cas = { { centre[0] - 32, centre[1] - 32, centre[2] - 16 }, { 64, 64, 32 } };
    AmbBox slab = cas;
    slab.dim[0] = 1;
    AmbBox slab8 = cas;
    slab8.dim[0] = 8;
    float *v = malloc(64 * 64 * 32 * 72);
    int counts[] = { 1, 8 };
    for (int t = 0; t < 2; t++) {
        Pool pool;
        pool_start(&pool, counts[t]);
        P.pfor = pool_for; P.pool = &pool;
        double best[3] = { 1e9, 1e9, 1e9 };
        const AmbBox *bx[3] = { &cas, &slab8, &slab };
        for (int b = 0; b < 3; b++)
            for (int r = 0; r < 3; r++) {
                double a = now();
                amb_bake_region(m, &P, bx[b], v, NULL);
                best[b] = fmin(best[b], now() - a);
            }
        pool_stop(&pool);
        printf("  timing, %d thread(s), best of 3 (indicative, the machine is shared): cascade 64x64x32 %.1f ms (%.2f us/voxel incl. margin), 8-voxel slab %.1f ms, 1-voxel slab %.1f ms\n",
               counts[t], best[0] * 1e3, best[0] * 1e6 / (78.0 * 78 * 46), best[1] * 1e3, best[2] * 1e3);
    }
    free(v);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: ambtest <map.bsp> [--points ref] [--bake ref ...] [--centre x y z] [--small]\n"); return 2; }
    size_t len;
    uint8_t *bsp = read_file(argv[1], &len);
    AmbMap *m = NULL;
    int rc = amb_map_load(bsp, len, 1, &m);
    printf("%s: load rc %d\n", argv[1], rc);
    if (rc != AMB_OK) return 1;
    AmbMapInfo I;
    amb_map_info(m, &I);
    printf("  bsp v%d, leaf lump v%d, %s, ambient source %d: %d planes, %d nodes, %d leaves, %d samples\n", I.bsp_version,
           I.leaf_lump_version, I.hdr ? "HDR" : "LDR", I.ambient_source, I.nplanes, I.nnodes, I.nleaves, I.nsamples);
    int small = 0, v0 = 0;
    int centre[3] = { 0, 0, 0 };
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--centre") && i + 3 < argc) for (int a = 0; a < 3; a++) centre[a] = atoi(argv[i + 1 + a]);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--small")) small = 1;
        else if (!strcmp(argv[i], "--v0")) v0 = 1;
        else if (!strcmp(argv[i], "--points") && i + 1 < argc) check_points(m, argv[++i]);
        else if (!strcmp(argv[i], "--bake") && i + 1 < argc) check_bake(m, argv[++i]);
    }
    if (v0) check_v0(bsp, len, m, centre);
    check_guards(m, centre);
    AmbBox dbox = { { centre[0] - 16, centre[1] - 16, centre[2] - 8 }, { 32, 32, 16 } };
    check_determinism(m, &dbox);
    AmbBox rbox = { { centre[0] - 12, centre[1] - 12, centre[2] - 6 }, { 24, 24, 12 } };
    check_regions(m, &rbox);
    check_half();
    if (small) check_toroidal(m, centre, 24, 20, 12);
    else check_toroidal(m, centre, 64, 64, 32);
    check_robust(bsp, len);
    if (!small) timing(m, centre);
    amb_map_free(m);
    free(bsp);
    printf(g_fails ? "AMBTEST FAIL (%d)\n" : "AMBTEST OK\n", g_fails);
    return g_fails ? 1 : 0;
}
