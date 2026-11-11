/* L4 (analysis/39): bake a whole map with l4_bake on threads, in passes, and compare with the map's own lightmap.
 * Usage: l4baketest map.bsp threads passes samples_per_pass bounces [out.bsp]
 * Prints per pass the time and the fit of the baked luminance to the stored one (style slot 0, every luxel of every baked face); with
 * out.bsp, writes a copy of the map whose lighting lump is the bake (same size and layout: for a local visual check only; a map with a
 * changed lump fails a server's map check, so it is never a delivery format). */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/ambient/l4_bake.h"

typedef struct { L4Bake *b; int t, nt, samples, pass; } Job;
static void *work(void *arg) {
    Job *j = arg;
    int nf = l4_bake_faces(j->b), chunk = 64;
    for (int f0 = j->t * chunk; f0 < nf; f0 += j->nt * chunk)   /* interleaved chunks: the threads share the map's mix of faces */
        l4_bake_pass(j->b, f0, f0 + chunk, j->samples, 0x9E3779B97F4A7C15ull * (uint64_t)(j->pass * 1009 + f0 + 1));
    return NULL;
}
static double dec(const uint8_t *c);
#pragma pack(push, 1)
typedef struct {
    uint16_t plane; uint8_t side, on_node; int32_t first_edge; int16_t nedges, texinfo, dispinfo, fog;
    uint8_t styles[4]; int32_t lightofs; float area; int32_t lm_mins[2], lm_size[2]; int32_t orig; uint16_t nprims, first_prim; uint32_t smoothing;
} Face;
#pragma pack(pop)
typedef struct { float tv[2][4], lv[2][4]; int flags, texdata; } TexInfo;
/* the fit per category: [style 0 / other] x [flat face, bump face's flat map, bump face's three direction maps] */
static void fit_by_kind(const uint8_t *bsp, const uint8_t *lump, const uint8_t *orig, int hdr) {
    const int32_t *Lf = (const int32_t *)(bsp + 8 + 16 * (hdr ? 58 : 7)), *Lt = (const int32_t *)(bsp + 8 + 16 * 6);
    int nf = Lf[1] / (int)sizeof(Face), nt = Lt[1] / (int)sizeof(TexInfo);
    const Face *faces = (const Face *)(bsp + Lf[0]);
    const TexInfo *ti = (const TexInfo *)(bsp + Lt[0]);
    double S[6][5] = { { 0 } };
    long M[6] = { 0 };
    for (int fi = 0; fi < nf; fi++) {
        const Face *fc = &faces[fi];
        if (fc->lightofs < 0 || fc->texinfo < 0 || fc->texinfo >= nt) continue;
        int bump = (ti[fc->texinfo].flags & 0x800) != 0;
        long nl = (long)(fc->lm_size[0] + 1) * (fc->lm_size[1] + 1);
        for (int s2 = 0; s2 < 4 && fc->styles[s2] != 255; s2++)
            for (int bv = 0; bv < (bump ? 4 : 1); bv++) {
                int kind = (fc->styles[s2] == 0 ? 0 : 3) + (!bump ? 0 : bv == 0 ? 1 : 2);
                for (long i = 0; i < nl; i++) {
                    long o = fc->lightofs + 4 * ((long)(s2 * (bump ? 4 : 1) + bv) * nl + i);
                    if (memcmp(lump + o, orig + o, 4) == 0) continue;
                    double x = dec(lump + o), y = dec(orig + o);
                    S[kind][0] += x; S[kind][1] += y; S[kind][2] += x * x; S[kind][3] += x * y; S[kind][4] += y * y; M[kind]++;
                }
            }
    }
    static const char *const names[6] = { "style 0, flat face", "style 0, bump flat map", "style 0, bump direction maps",
                                           "other styles, flat face", "other styles, bump flat map", "other styles, bump direction maps" };
    for (int k = 0; k < 6; k++) {
        long m = M[k];
        if (!m) continue;
        double vx = S[k][2] - S[k][0] * S[k][0] / m, vy = S[k][4] - S[k][1] * S[k][1] / m, cxy = S[k][3] - S[k][0] * S[k][1] / m;
        printf("L4BAKE   %-34s %8ld luxels: stored = %.3f x baked + %.4f, r %.3f; means stored %.4f baked %.4f\n", names[k], m, vx > 0 ? cxy / vx : 0,
               m ? (S[k][1] - (vx > 0 ? cxy / vx : 0) * S[k][0]) / m : 0, vx > 0 && vy > 0 ? cxy / sqrt(vx * vy) : 0, S[k][1] / m, S[k][0] / m);
    }
}
static double now(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec + t.tv_nsec * 1e-9; }
static double dec(const uint8_t *c) { double s = ldexp(1.0, (int8_t)c[3]) / 255.0; return 0.2126 * c[0] * s + 0.7152 * c[1] * s + 0.0722 * c[2] * s; }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s map.bsp threads passes samples bounces [out.bsp]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *bsp = malloc((size_t)n);
    if (!bsp || fread(bsp, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    int nt = atoi(argv[2]), passes = atoi(argv[3]), samples = atoi(argv[4]), bounces = atoi(argv[5]);
    L4Bake *b;
    double t0 = now();
    if (l4_bake_new(bsp, (size_t)n, bounces, &b)) { fprintf(stderr, "l4_bake_new failed\n"); return 1; }
    printf("L4BAKE %s: %d faces, %ld luxels x styles, setup %.1f s\n", argv[1], l4_bake_faces(b), l4_bake_luxels(b), now() - t0);
    int lump_index = 0;
    for (int p = 0; p < passes; p++) {
        double t1 = now();
        pthread_t th[64];
        Job jb[64];
        for (int t = 0; t < nt && t < 64; t++) { jb[t] = (Job){ b, t, nt, samples, p }; pthread_create(&th[t], NULL, work, &jb[t]); }
        for (int t = 0; t < nt && t < 64; t++) pthread_join(th[t], NULL);
        size_t ln;
        uint8_t *lump = l4_bake_lump(b, &ln, &lump_index);
        /* the fit over every luxel whose bytes changed (baked faces), against the map's own lump */
        const int32_t *L = (const int32_t *)(bsp + 8 + 16 * lump_index);
        const uint8_t *orig = bsp + L[0];
        double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
        long m = 0;
        for (size_t i = 0; i + 4 <= ln; i += 4) {
            if (memcmp(lump + i, orig + i, 4) == 0) continue;
            double x = dec(lump + i), y = dec(orig + i);
            sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y; m++;
        }
        double vx = sxx - sx * sx / (m ? m : 1), vy = syy - sy * sy / (m ? m : 1), cxy = sxy - sx * sy / (m ? m : 1);
        printf("L4BAKE pass %d: %d paths per luxel so far, %.1f s (%d threads); fit over %ld changed luxels: stored = %.3f x baked + %.4f, r %.3f\n",
               p + 1, (p + 1) * samples, now() - t1, nt, m, vx > 0 ? cxy / vx : 0, m ? (sy - (vx > 0 ? cxy / vx : 0) * sx) / m : 0,
               vx > 0 && vy > 0 ? cxy / sqrt(vx * vy) : 0);
        if (p == passes - 1) fit_by_kind(bsp, lump, orig, lump_index == 53);
        if (p == passes - 1 && argc > 6) {
            memcpy(bsp + L[0], lump, ln);
            FILE *o = fopen(argv[6], "wb");
            if (o) { fwrite(bsp, 1, (size_t)n, o); fclose(o); printf("L4BAKE wrote %s\n", argv[6]); }
        }
        free(lump);
    }
    printf("L4BAKE total %.1f s\n", now() - t0);
    l4_bake_free(b);
    return 0;
}
