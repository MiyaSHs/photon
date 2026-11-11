/* A small valid BSP for the pick table tests (P4a), built in memory: two open rooms split by a 64-unit solid wall with a doorway, and lump-42
 * cubemap samples on both sides. The geometry is simple enough for an exact reference line of sight (synth_pick_blocked).
 *   leaf 0  x < 8              open (room A)
 *   leaf 1  x >= 72            open (room B)
 *   leaf 2  8 <= x < 72, y < 0       solid
 *   leaf 3  8 <= x < 72, y >= 64     solid
 *   leaf 4  8 <= x < 72, 0 <= y < 64 open (the doorway)
 * Voxel centres (odd multiples of 16) fall inside the wall, at x = 24 (a nudge of 12 reaches room A: MOVED) and x = 40, 56 (none does: SOLID).
 * variant moves the samples, so two variants are two different maps. l4d2 = 1 writes the lump fields in L4D2's order; lzma = 1 tags lump 42
 * "LZMA"; cubes = -1 writes a lump 42 of 17 bytes (a funny size). */
#ifndef PHOTON_SYNTH_PICK_H
#define PHOTON_SYNTH_PICK_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline void sp_w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void sp_wf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); sp_w32(p, u); }
static inline void sp_w16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

enum { SP_PLANES = 4, SP_NODES = 4, SP_LEAVES = 5, SP_MAX_CUBES = 1100, SP_WALL_LO = 8, SP_WALL = 72, SP_DOOR_LO = 0, SP_DOOR_HI = 64 };

/* the variant's sample origins (at most SP_MAX_CUBES); returns the count */
static inline int synth_pick_cubes(int variant, int (*o)[3]) {
    static const int base[][3] = { { -400, 32, 40 }, { -200, -500, -60 }, { -900, 300, 200 }, { 300, 32, 40 }, { 600, -400, 0 },
                                   { 250, 700, -100 }, { -50, 900, 10 }, { 900, 900, 400 }, { 8, 32, 0 } };
    int n = (int)(sizeof base / sizeof base[0]);
    if (variant == 2) n = 1;   /* one sample: B is NONE */
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) o[i][k] = base[i][k] + (variant == 1 ? (k == 1 ? 96 : -48) : 0) * (i % 3 == 0 ? 1 : -1);
    return n;
}

/* 1 if the segment a-b has a point with 8 <= x < 72 and (y < 0 or y >= 64): the exact rule of the tree below (the doorway's own leaf is open) */
static inline int synth_pick_blocked(const double a[3], const double b[3]) {
    double dx = b[0] - a[0];
    double t0 = 0, t1 = 1;
    if (dx == 0) { if (a[0] < SP_WALL_LO || a[0] >= SP_WALL) return 0; }
    else {
        double ta = (SP_WALL_LO - a[0]) / dx, tb = (SP_WALL - a[0]) / dx;
        double lo = ta < tb ? ta : tb, hi = ta < tb ? tb : ta;
        if (lo > t0) t0 = lo;
        if (hi < t1) t1 = hi;
        if (t0 > t1) return 0;
        /* only the point x = 72 itself (a segment from the wall's far face going away from it): that point is in leaf 1, not the wall */
        if (t0 == t1 && a[0] + t0 * dx >= SP_WALL) return 0;
    }
    /* y is linear in t: it leaves [0, 64) inside [t0, t1] iff one of the ends does (the crossing point at x = 72 counts, as amb_segment_solid's crossing points do) */
    double y0 = a[1] + t0 * (b[1] - a[1]), y1 = a[1] + t1 * (b[1] - a[1]);
    return y0 < SP_DOOR_LO || y0 >= SP_DOOR_HI || y1 < SP_DOOR_LO || y1 >= SP_DOOR_HI;
}

static inline uint8_t *synth_pick_bsp(int variant, int l4d2, int lzma, int cubes, size_t *len) {
    int o[SP_MAX_CUBES][3];
    int nc = cubes >= 0 ? (cubes > 0 ? cubes : synth_pick_cubes(variant, o)) : 0;
    if (cubes > 0) for (int i = 0; i < nc; i++) { o[i][0] = -1000 + (i * 37) % 2000; o[i][1] = -1000 + (i * 91) % 2000; o[i][2] = (i * 13) % 200; }
    int clen = cubes < 0 ? 17 : nc * 16;
    enum { HDR = 8 + 64 * 16 + 4 };
    struct { int idx, size, version; } lumps[] = { { 1, SP_PLANES * 20, 0 }, { 5, SP_NODES * 32, 0 }, { 10, SP_LEAVES * 32, 1 }, { 14, 48, 0 },
                                                   { 42, clen, 0 } };
    size_t total = HDR;
    for (size_t i = 0; i < sizeof lumps / sizeof lumps[0]; i++) total += (size_t)lumps[i].size;
    uint8_t *b = calloc(1, total);
    if (!b) return NULL;
    memcpy(b, "VBSP", 4);
    sp_w32(b + 4, 20);
    size_t at = HDR;
    uint8_t *ptr[64] = { 0 };
    for (size_t i = 0; i < sizeof lumps / sizeof lumps[0]; i++) {
        uint8_t *h = b + 8 + 16 * lumps[i].idx;
        uint32_t f[3] = { (uint32_t)at, (uint32_t)lumps[i].size, (uint32_t)lumps[i].version };
        if (l4d2) { sp_w32(h, f[2]); sp_w32(h + 4, f[0]); sp_w32(h + 8, f[1]); }
        else { sp_w32(h, f[0]); sp_w32(h + 4, f[1]); sp_w32(h + 8, f[2]); }
        ptr[lumps[i].idx] = b + at;
        at += (size_t)lumps[i].size;
    }
    if (l4d2) sp_w32(b + 8 + 8, 2000);   /* lump 0's "version" field holds a length above 1000: the L4D2 tag (lump 0 itself is empty here) */
    /* planes: x = 8, x = 72, y = 0, y = 64 */
    static const struct { int axis; float d; } pl[SP_PLANES] = { { 0, SP_WALL_LO }, { 0, SP_WALL }, { 1, SP_DOOR_LO }, { 1, SP_DOOR_HI } };
    for (int i = 0; i < SP_PLANES; i++) {
        uint8_t *p = ptr[1] + 20 * i;
        sp_wf(p + 4 * pl[i].axis, 1.0f);
        sp_wf(p + 12, pl[i].d);
        sp_w32(p + 16, (uint32_t)pl[i].axis);
    }
    /* nodes (front child, back child; leaf i is -1 - i): 0: x>=8 ? 1 : leaf 0; 1: x>=72 ? leaf 1 : 2; 2: y>=0 ? 3 : leaf 2; 3: y>=64 ? leaf 3 : leaf 4 */
    static const int nd[SP_NODES][3] = { { 0, 1, -1 }, { 1, -2, 2 }, { 2, 3, -3 }, { 3, -4, -5 } };
    for (int i = 0; i < SP_NODES; i++) {
        uint8_t *p = ptr[5] + 32 * i;
        sp_w32(p, (uint32_t)nd[i][0]); sp_w32(p + 4, (uint32_t)nd[i][1]); sp_w32(p + 8, (uint32_t)nd[i][2]);
    }
    static const short bounds[SP_LEAVES][6] = { { -8000, -8000, -8000, 8, 8000, 8000 }, { 72, -8000, -8000, 8000, 8000, 8000 },
                                                { 8, -8000, -8000, 72, 0, 8000 }, { 8, 64, -8000, 72, 8000, 8000 }, { 8, 0, -8000, 72, 64, 8000 } };
    static const int solid[SP_LEAVES] = { 0, 0, 1, 1, 0 };
    for (int i = 0; i < SP_LEAVES; i++) {
        uint8_t *p = ptr[10] + 32 * i;
        sp_w32(p, (uint32_t)solid[i]);
        for (int k = 0; k < 6; k++) sp_w16(p + 8 + 2 * k, bounds[i][k]);
    }
    uint8_t *p = ptr[14];
    for (int a = 0; a < 3; a++) { sp_wf(p + 4 * a, -8000.0f); sp_wf(p + 12 + 4 * a, 8000.0f); }
    sp_w32(p + 36, 0);
    for (int i = 0; i < nc; i++) {
        uint8_t *r = ptr[42] + 16 * i;
        for (int k = 0; k < 3; k++) sp_w32(r + 4 * k, (uint32_t)o[i][k]);
        sp_w32(r + 12, (uint32_t)(i % 7));
    }
    if (lzma && clen >= 4) memcpy(ptr[42], "LZMA", 4);
    *len = total;
    return b;
}
#endif
