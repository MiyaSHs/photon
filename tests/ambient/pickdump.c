/* P4a host tool for the pick table (src/ambient/amb_pick.c) on a real map. Used by tools/p4/ (offline acceptance) and for timing.
 *   pickdump <map.bsp> box x0 y0 z0 x1 y1 z1 [--voxel 32] [--tests 8] [--threads N] [--out file]
 *       every voxel whose centre lies in the bounds; prints timing and flag statistics; --out writes b"PICK", i32 org[3], i32 dim[3], f64 voxel,
 *       u32 entries (x fastest)
 *   pickdump <map.bsp> points <in.txt> [--voxel 32] [--tests 8]
 *       one "x y z" per line: prints "x y z voxel_x voxel_y voxel_z A B flags nearest" (nearest: amb_cube_nearest at the point itself)
 *   pickdump <map.bsp> seg <in.txt>
 *       one "ax ay az bx by bz" per line: prints amb_segment_solid (1 blocked) per line */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/ambient/amb_pick.h"

typedef struct { int n; AmbTaskFn fn; void *ctx; int next; pthread_mutex_t mu; } Pool;
static void *pool_main(void *p) {
    Pool *pl = p;
    for (;;) {
        pthread_mutex_lock(&pl->mu);
        int i = pl->next++;
        pthread_mutex_unlock(&pl->mu);
        if (i >= pl->n) return NULL;
        pl->fn(pl->ctx, i);
    }
}
static int g_threads = 1;
static void pfor(void *pool, int n, AmbTaskFn fn, void *ctx) {
    (void)pool;
    Pool pl = { n, fn, ctx, 0, PTHREAD_MUTEX_INITIALIZER };
    pthread_t t[64];
    int k = g_threads < 1 ? 1 : g_threads > 64 ? 64 : g_threads;
    for (int i = 0; i < k; i++) pthread_create(&t[i], NULL, pool_main, &pl);
    for (int i = 0; i < k; i++) pthread_join(t[i], NULL);
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }

static uint8_t *slurp(const char *path, size_t *len) {
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

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see the file header\n"); return 2; }
    size_t len;
    uint8_t *bsp = slurp(argv[1], &len);
    if (!bsp) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    AmbMap *m = NULL;
    static AmbCubes c;
    static AmbPickTargets t;
    int rc = amb_map_load(bsp, len, 1, &m);
    if (rc == AMB_OK) rc = amb_cubes_load(bsp, len, &c);
    if (rc != AMB_OK) { fprintf(stderr, "load failed %d\n", rc); return 1; }
    amb_pick_targets(m, &c, &t);
    AmbPickParams p;
    amb_pick_params_default(&p);
    const char *out = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--voxel") && i + 1 < argc) p.voxel = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tests") && i + 1 < argc) p.max_tests = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    if (g_threads > 1) p.pfor = pfor;
    int solid_targets = 0;
    for (int i = 0; i < t.n; i++) solid_targets += t.solid[i];
    if (!strcmp(argv[2], "box") && argc >= 9) {
        double lo[3], hi[3];
        for (int k = 0; k < 3; k++) { lo[k] = atof(argv[3 + k]); hi[k] = atof(argv[6 + k]); }
        AmbBox box;
        for (int k = 0; k < 3; k++) {
            box.org[k] = (int)floor(lo[k] / p.voxel);
            box.dim[k] = (int)ceil(hi[k] / p.voxel) - box.org[k];
        }
        size_t nv = (size_t)box.dim[0] * box.dim[1] * box.dim[2];
        uint32_t *e = malloc(nv * sizeof *e);
        double t0 = now_s();
        rc = amb_pick_box(m, &c, &t, &p, &box, e);
        double t1 = now_s();
        long nsolid = 0, nmoved = 0, nafb = 0, nbfb = 0, nhid = 0, open = 0;
        for (size_t i = 0; i < nv; i++) {
            uint32_t f = e[i];
            if (f & AMB_PICK_SOLID) { nsolid++; continue; }
            open++;
            nmoved += !!(f & AMB_PICK_MOVED);
            nafb += !!(f & AMB_PICK_A_FALLBACK);
            nbfb += !!(f & AMB_PICK_B_FALLBACK);
            nhid += !!(f & AMB_PICK_NEAR_HIDDEN);
        }
        printf("pickdump box: rc %d, %d samples (%d targets nudged or solid), box org %d %d %d dim %d %d %d = %zu voxels, %.3f s on %d threads"
               " (%.2f us/voxel/thread)\n", rc, c.n, solid_targets, box.org[0], box.org[1], box.org[2], box.dim[0], box.dim[1], box.dim[2], nv,
               t1 - t0, g_threads, (t1 - t0) * 1e6 * g_threads / (double)nv);
        printf("pickdump stats: solid %ld, open %ld: moved %ld, nearest hidden %ld (%.3f%%), A fallback %ld (%.3f%%), B fallback %ld (%.3f%%)\n", nsolid,
               open, nmoved, nhid, open ? 100.0 * nhid / open : 0, nafb, open ? 100.0 * nafb / open : 0, nbfb, open ? 100.0 * nbfb / open : 0);
        if (out) {
            FILE *f = fopen(out, "wb");
            fwrite("PICK", 1, 4, f);
            fwrite(box.org, 4, 3, f);
            fwrite(box.dim, 4, 3, f);
            fwrite(&p.voxel, 8, 1, f);
            fwrite(e, 4, nv, f);
            fclose(f);
        }
        free(e);
    } else if (!strcmp(argv[2], "points") && argc >= 4) {
        FILE *f = fopen(argv[3], "r");
        double x, y, z;
        while (f && fscanf(f, "%lf %lf %lf", &x, &y, &z) == 3) {
            int a[3] = { (int)floor(x / p.voxel), (int)floor(y / p.voxel), (int)floor(z / p.voxel) };
            uint32_t e = amb_pick_voxel(m, &c, &t, &p, a);
            float fp[3] = { (float)x, (float)y, (float)z };
            printf("%.3f %.3f %.3f %d %d %d %d %d 0x%x %d\n", x, y, z, a[0], a[1], a[2], AMB_PICK_A(e), AMB_PICK_B(e), e >> 22, amb_cube_nearest(&c, fp));
        }
        if (f) fclose(f);
    } else if (!strcmp(argv[2], "seg") && argc >= 4) {
        FILE *f = fopen(argv[3], "r");
        double s[6];
        while (f && fscanf(f, "%lf %lf %lf %lf %lf %lf", &s[0], &s[1], &s[2], &s[3], &s[4], &s[5]) == 6) printf("%d\n", amb_segment_solid(m, s, s + 3));
        if (f) fclose(f);
    } else { fprintf(stderr, "bad mode\n"); return 2; }
    amb_map_free(m);
    free(bsp);
    return rc == AMB_OK ? 0 : 1;
}
