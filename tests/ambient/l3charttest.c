/* L3 charts (analysis/37, "per-surface 2D charts"): what would the re-bake cost stored on the surfaces instead of in 3D bricks? For every
 * lit flat face and every point / spot lamp whose reach and front side it is in: a coarse pass (4x the texel size) traces the lamp from
 * the face's texels; only if that finds both lit and shadowed texels does the full-resolution pass run, and the (face, lamp) pair is a chart.
 * Prints the chart count, texels (= bytes at L8), rays and time. Usage: l3charttest map.bsp texel_units */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/ambient/lg_light.h"
#include "../../src/ambient/lg_map.h"
#include "../../src/ambient/sun_vis.h"

typedef struct { int ofs, len, ver; char cc[4]; } Lump;
typedef struct { float n[3], d; int type; } Plane;
typedef struct { float tv[2][4], lv[2][4]; int flags, texdata; } TexInfo;
#pragma pack(push, 1)
typedef struct {
    uint16_t plane; uint8_t side, on_node; int32_t first_edge; int16_t nedges, texinfo, dispinfo, fog;
    uint8_t styles[4]; int32_t lightofs; float area; int32_t lm_mins[2], lm_size[2]; int32_t orig; uint16_t nprims, first_prim; uint32_t smoothing;
} Face;
#pragma pack(pop)
static unsigned char *g_b;
static long g_n;
static const void *lump(int i, int rec, int *n) {
    const Lump *l = (const Lump *)(g_b + 8 + 16 * i);
    *n = l->len > 0 && (long)l->ofs + l->len <= g_n ? l->len / rec : 0;
    return *n ? g_b + l->ofs : NULL;
}
static double now_ms(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); g_n = ftell(f); fseek(f, 0, SEEK_SET);
    g_b = malloc((size_t)g_n);
    if (fread(g_b, 1, (size_t)g_n, f) != (size_t)g_n) return 1;
    fclose(f);
    double T = atof(argv[2]), t0 = now_ms();
    LgMap *lm; SunMap *sm;
    if (lg_map_load(g_b, (size_t)g_n, 1, &lm) || sun_map_load(g_b, (size_t)g_n, 1, &sm)) return 1;
    SunScratch *scr = sun_scratch_new(sm);
    int nl, np, nv, ne, nse, nt, nf;
    const LgWorldLight *wl = lg_map_lights(lm, &nl);
    const Plane *pl = lump(1, sizeof(Plane), &np);
    const float (*vt)[3] = lump(3, 12, &nv);
    const uint16_t (*ed)[2] = lump(12, 4, &ne);
    const int32_t *se = lump(13, 4, &nse);
    const TexInfo *ti = lump(6, sizeof(TexInfo), &nt);
    const Face *fc = lump(7, sizeof(Face), &nf);
    double *reach = malloc(sizeof(double) * (size_t)nl);
    for (int i = 0; i < nl; i++) reach[i] = lg_window_radius(&wl[i], 0.01);
    long charts = 0, texels = 0, rays = 0, pairs = 0, faces = 0, tiles = 0;
    static unsigned char vis[1 << 22];
    for (int fi = 0; fi < nf; fi++) {
        const Face *F = &fc[fi];
        if (F->dispinfo >= 0 || F->texinfo < 0 || F->texinfo >= nt || F->nedges < 3 || F->nedges > 64) continue;
        if (ti[F->texinfo].flags & (0x4 | 0x400 | 0x80)) continue;
        double poly[64][3];
        for (int e = 0; e < F->nedges; e++) {
            int s = se[F->first_edge + e], ei = s >= 0 ? s : -s, vi = s >= 0 ? ed[ei][0] : ed[ei][1];
            for (int k = 0; k < 3; k++) poly[e][k] = vt[vi][k];
        }
        double n[3] = { pl[F->plane].n[0], pl[F->plane].n[1], pl[F->plane].n[2] };
        if (F->side) for (int k = 0; k < 3; k++) n[k] = -n[k];
        /* the face's plane basis: u along its first edge, v = n x u */
        double u[3] = { poly[1][0] - poly[0][0], poly[1][1] - poly[0][1], poly[1][2] - poly[0][2] }, ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (ul < 1e-3) continue;
        for (int k = 0; k < 3; k++) u[k] /= ul;
        double v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
        double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 }, c[3] = { 0, 0, 0 };
        for (int e = 0; e < F->nedges; e++) {
            double w[3] = { poly[e][0] - poly[0][0], poly[e][1] - poly[0][1], poly[e][2] - poly[0][2] };
            double a = w[0] * u[0] + w[1] * u[1] + w[2] * u[2], b = w[0] * v[0] + w[1] * v[1] + w[2] * v[2];
            if (a < lo[0]) lo[0] = a; if (a > hi[0]) hi[0] = a; if (b < lo[1]) lo[1] = b; if (b > hi[1]) hi[1] = b;
            for (int k = 0; k < 3; k++) c[k] += poly[e][k] / F->nedges;
        }
        double rad = 0.5 * sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]));
        faces++;
        for (int li = 0; li < nl; li++) {
            const LgWorldLight *L = &wl[li];
            if (L->type != LG_EMIT_POINT && L->type != LG_EMIT_SPOT) continue;
            double d[3] = { L->origin[0] - c[0], L->origin[1] - c[1], L->origin[2] - c[2] };
            double dist = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist > reach[li] + rad || (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) < -rad) continue;   /* out of reach, or wholly behind the face */
            pairs++;
            for (int pass = 0; pass < 2; pass++) {   /* 0: coarse (4 texels), 1: full */
                double st = pass ? T : 4 * T;
                int nu = (int)ceil((hi[0] - lo[0]) / st) + 1, nvv = (int)ceil((hi[1] - lo[1]) / st) + 1, seen = 0;
                long tx = 0;
                if ((long)nu * nvv > (1 << 22)) break;
                for (int j = 0; j < nvv; j++)
                    for (int i = 0; i < nu; i++) {
                        double a = lo[0] + (i + 0.5) * st, b = lo[1] + (j + 0.5) * st, p[3];
                        for (int k = 0; k < 3; k++) p[k] = poly[0][k] + u[k] * a + v[k] * b + n[k];
                        double o[3] = { L->origin[0], L->origin[1], L->origin[2] };
                        SunTrace tr;
                        sun_trace(sm, scr, p, o, SUN_MASK_OPAQUE, &tr);
                        rays++;
                        seen |= (tr.fraction >= 0.999) ? 1 : 2;
                        vis[(long)j * nu + i] = tr.fraction >= 0.999;
                        tx++;
                    }
                if (seen != 3) break;              /* no edge on this face for this lamp */
                if (pass) {
                    charts++; texels += tx;
                    /* sparse: 8x8 tiles holding an edge (a texel whose 4-neighbour differs), plus one ring of tiles around them */
                    int tu = (nu + 7) / 8, tv = (nvv + 7) / 8;
                    static unsigned char e[1 << 16], k2[1 << 16];
                    if ((long)tu * tv > (1 << 16)) { tiles += (long)tu * tv; continue; }
                    memset(e, 0, (size_t)tu * tv); memset(k2, 0, (size_t)tu * tv);
                    for (int j = 0; j < nvv; j++)
                        for (int i = 0; i < nu; i++) {
                            unsigned char a = vis[(long)j * nu + i];
                            if ((i + 1 < nu && vis[(long)j * nu + i + 1] != a) || (j + 1 < nvv && vis[(long)(j + 1) * nu + i] != a)) e[(j / 8) * tu + i / 8] = 1;
                        }
                    for (int y = 0; y < tv; y++)
                        for (int x = 0; x < tu; x++)
                            if (e[y * tu + x])
                                for (int dy = -1; dy <= 1; dy++)
                                    for (int dx = -1; dx <= 1; dx++)
                                        if (y + dy >= 0 && y + dy < tv && x + dx >= 0 && x + dx < tu) k2[(y + dy) * tu + x + dx] = 1;
                    for (int q = 0; q < tu * tv; q++) tiles += k2[q];
                }
            }
        }
    }
    printf("L3CHART %s texel %.0f: %ld faces, %ld face-lamp pairs, %ld charts with an edge, %ld texels (%.1f MB whole), %ld edge tiles of 8x8 (%.1f MB sparse), %ld rays, %.0f ms\n",
           argv[1], T, faces, pairs, charts, texels, texels / 1048576.0, tiles, tiles * 64 / 1048576.0, rays, now_ms() - t0);
    return 0;
}
