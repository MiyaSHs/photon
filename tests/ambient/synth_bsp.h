/* A tiny valid BSP built in memory, so the clipmap tests need no game files. Three leaves: an open room for x < 0, an open room for
 * x >= 0 and y >= 1024, and solid for x >= 0 and y < 1024, with samples whose inverse-distance mix differs from voxel to voxel.
 * l4d2 = 1 writes the lump fields in Left 4 Dead 2's order (version, fileofs, filelen), which amb_map_load must undo. */
#ifndef PHOTON_SYNTH_BSP_H
#define PHOTON_SYNTH_BSP_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void synth_w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void synth_wf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); synth_w32(p, u); }
static void synth_w16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

enum { SYNTH_LEAVES = 3, SYNTH_NSAMPLES = 9, SYNTH_ENTITIES = 1200 };

/* Returns a malloc'ed file, *len its size. An LZMA tag on the planes lump with lzma = 1. variant shifts the colours: another map. */
static uint8_t *synth_bsp_variant(int l4d2, int lzma, int variant, size_t *len) {
    enum { HDR = 8 + 64 * 16 + 4 };
    static const struct { int idx, size, version; } lumps[] = {
        { 0, SYNTH_ENTITIES, 0 }, { 1, 2 * 20, 0 }, { 5, 2 * 32, 0 }, { 10, SYNTH_LEAVES * 32, 1 }, { 14, 48, 0 },
        { 52, SYNTH_LEAVES * 4, 0 }, { 56, SYNTH_NSAMPLES * 28, 1 } };
    size_t total = HDR;
    for (size_t i = 0; i < sizeof lumps / sizeof lumps[0]; i++) total += (size_t)lumps[i].size;
    uint8_t *b = calloc(1, total);
    if (!b) return NULL;
    memcpy(b, "VBSP", 4);
    synth_w32(b + 4, 20);
    size_t at = HDR;
    uint8_t *ptr[64] = { 0 };
    for (size_t i = 0; i < sizeof lumps / sizeof lumps[0]; i++) {
        uint8_t *h = b + 8 + 16 * lumps[i].idx;
        uint32_t f[3] = { (uint32_t)at, (uint32_t)lumps[i].size, (uint32_t)lumps[i].version };
        if (l4d2) { synth_w32(h, f[2]); synth_w32(h + 4, f[0]); synth_w32(h + 8, f[1]); }
        else { synth_w32(h, f[0]); synth_w32(h + 4, f[1]); synth_w32(h + 8, f[2]); }
        ptr[lumps[i].idx] = b + at;
        at += (size_t)lumps[i].size;
    }
    memset(ptr[0], ' ', SYNTH_ENTITIES);
    /* planes: x = 0, y = 1024 */
    uint8_t *p = ptr[1];
    synth_wf(p, 1.0f); synth_w32(p + 16, 0);
    p += 20;
    synth_wf(p + 4, 1.0f); synth_wf(p + 12, 1024.0f); synth_w32(p + 16, 1);
    /* node 0: x >= 0 -> node 1, else leaf 0; node 1: y >= 1024 -> leaf 1, else leaf 2 (leaf i is child -1 - i) */
    p = ptr[5];
    synth_w32(p, 0); synth_w32(p + 4, 1); synth_w32(p + 8, (uint32_t)-1);
    p += 32;
    synth_w32(p, 1); synth_w32(p + 4, (uint32_t)-2); synth_w32(p + 8, (uint32_t)-3);
    /* leaves: contents at 0, mins at 8, maxs at 14 */
    static const short bounds[SYNTH_LEAVES][6] = { { -8000, -8000, -8000, 0, 8000, 8000 }, { 0, 1024, -8000, 8000, 8000, 8000 },
                                                   { 0, -8000, -8000, 8000, 1024, 8000 } };
    for (int i = 0; i < SYNTH_LEAVES; i++) {
        p = ptr[10] + 32 * i;
        synth_w32(p, i == 2 ? 1 : 0);
        for (int k = 0; k < 6; k++) synth_w16(p + 8 + 2 * k, bounds[i][k]);
    }
    /* model 0: bounds and head node 0 */
    p = ptr[14];
    for (int a = 0; a < 3; a++) { synth_wf(p + 4 * a, -8000.0f); synth_wf(p + 12 + 4 * a, 8000.0f); }
    synth_w32(p + 36, 0);
    /* index: count, first; samples: 6 x rgbe, then the position fractions */
    static const int counts[SYNTH_LEAVES] = { 5, 4, 0 };
    int s = 0;
    for (int i = 0; i < SYNTH_LEAVES; i++) {
        synth_w16(ptr[52] + 4 * i, counts[i]);
        synth_w16(ptr[52] + 4 * i + 2, i == 2 ? 0 : s);
        for (int k = 0; k < counts[i]; k++, s++) {
            uint8_t *r = ptr[56] + 28 * s;
            for (int f = 0; f < 6; f++) {
                r[f * 4 + 0] = (uint8_t)(20 + 23 * s + 7 * f + variant);
                r[f * 4 + 1] = (uint8_t)(200 - 19 * s + 11 * f);
                r[f * 4 + 2] = (uint8_t)(90 + 31 * k + 5 * f);
                r[f * 4 + 3] = (uint8_t)(int8_t)-6;
            }
            r[24] = (uint8_t)(100 + 25 * k + 3 * s);
            r[25] = (uint8_t)(120 + 17 * s - 9 * k);
            r[26] = (uint8_t)(110 + 11 * k + 5 * s);
        }
    }
    if (lzma) memcpy(ptr[1], "LZMA", 4);
    *len = total;
    return b;
}
static inline uint8_t *synth_bsp(int l4d2, int lzma, size_t *len) { return synth_bsp_variant(l4d2, lzma, 0, len); }
#endif
