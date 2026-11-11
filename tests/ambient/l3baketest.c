/* L3b (analysis/37): the bake's numbers per map and voxel size, and a slice image for the eye.
 * Usage: l3baketest map.bsp voxel [slice_z cx cy half lamp out.pgm]: a horizontal slice at height slice_z (the voxel layer containing
 * it), centred on (cx, cy) with half width half: per voxel the lamp's signed distance (white lit, black shadowed, mid grey the edge)
 * where a brick holds that lamp; dark grey where a brick holds other lamps only; black outside the bake. Prints L3BAKE lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../src/ambient/l3_bake.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    L3Params p = { atof(argv[2]), getenv("L3_MIN_LIGHT") ? atof(getenv("L3_MIN_LIGHT")) : 0.01, NULL, getenv("L3_MIN_SHARE") ? atof(getenv("L3_MIN_SHARE")) : 0 };
    L3Bake *bk = NULL;
    L3Stats st;
    int rc = l3_bake(b, (size_t)n, &p, &bk, &st);
    if (rc) { printf("L3BAKE FAIL rc %d\n", rc); return 1; }
    printf("L3BAKE %s voxel %.0f: %ld faces, %ld shell voxels (%ld in solid, left out), %ld bricks (%ld with an edge), %ld brick lamps, %ld rays; "
           "%.0f ms shell, %.0f ms rays, %.0f ms sdf; %.1f MB (bricks %zu B each); edge-free slots dropped %ld, bricks %ld\n", argv[1], p.voxel, st.faces, st.shell_voxels, st.voxels_solid,
           st.bricks, st.edge_bricks, st.brick_lamps, st.rays, st.ms_shell, st.ms_rays, st.ms_sdf, bk->nbricks * (double)sizeof(L3Brick) / 1048576.0,
           sizeof(L3Brick), st.slots_dropped, st.bricks_dropped);
    {   /* L3c: the pack and its lookup must give back every brick's every voxel and lamps */
        L3Pack *pk = NULL;
        rc = bk->nbricks ? l3_pack(bk, &pk) : 0;
        long bad = 0, checked = 0;
        for (long i = 0; pk && i < bk->nbricks; i++) {
            const L3Brick *br = &bk->bricks[i];
            for (int k = 0; k < L3_B * L3_B * L3_B; k++) {
                uint8_t s[L3_LAMPS];
                int lm[L3_LAMPS];
                int ok = l3_pack_lookup(pk, br->x * L3_B + k % L3_B, br->y * L3_B + k / L3_B % L3_B, br->z * L3_B + k / (L3_B * L3_B), s, lm);
                checked++;
                if (!ok || memcmp(s, br->sdf[k], L3_LAMPS) != 0) bad++;
                else for (int q = 0; q < L3_LAMPS; q++) if (lm[q] != br->lamp[q]) { bad++; break; }
            }
        }
        if (pk)
            printf("L3BAKE pack: l1 %dx%dx%d (super-bricks from %d %d %d), %d pages (l2 %dx%dx%d), atlas %dx%dx%d, %.1f MB; lookup of %ld voxels: %ld wrong%s\n",
                   pk->l1_dim[0], pk->l1_dim[1], pk->l1_dim[2], pk->sb_min[0], pk->sb_min[1], pk->sb_min[2], pk->npages, L3_PX * L3_SB, L3_PY * L3_SB,
                   pk->pz * L3_SB, L3_AX * L3_B, L3_AY * L3_B, pk->az * L3_B,
                   ((double)pk->l1_dim[0] * pk->l1_dim[1] * pk->l1_dim[2] * 2 + (double)L3_PX * L3_SB * L3_PY * L3_SB * pk->pz * L3_SB * 2 +
                    (double)L3_AX * L3_B * L3_AY * L3_B * pk->az * L3_B * 4 + 16.0 * L3_LW * pk->lamp_rows) / 1048576.0, checked, bad, bad ? " FAIL" : " OK");
        if (pk) {   /* the cache file round trip */
            uint8_t *buf = NULL;
            size_t blen = 0;
            L3Pack *pl = NULL;
            int ok = l3_pack_save(pk, &buf, &blen) == 0 && l3_pack_load(buf, blen, &pl) == 0 && pl->nbricks == pk->nbricks &&
                     memcmp(pl->atlas, pk->atlas, (size_t)L3_AX * L3_B * L3_AY * L3_B * pk->az * L3_B * 4) == 0 &&
                     memcmp(pl->l2, pk->l2, (size_t)L3_PX * L3_SB * L3_PY * L3_SB * pk->pz * L3_SB * 2) == 0 &&
                     l3_pack_load(buf, blen - 1, &(L3Pack *){ NULL }) != 0;
            printf("L3BAKE cache file: %zu bytes, round trip %s\n", blen, ok ? "OK" : "FAIL");
            l3_pack_free(pl);
            free(buf);
            if (!ok) bad++;
        }
        l3_pack_free(pk);
        if (bad) return 1;
    }
    if (argc >= 9) {
        double z = atof(argv[3]), cx = atof(argv[4]), cy = atof(argv[5]), half = atof(argv[6]), V = p.voxel;
        int lamp = atoi(argv[7]), W = (int)(2 * half / V);
        unsigned char *img = calloc((size_t)W * W, 1);
        int vz = (int)floor(z / V);
        for (long i = 0; i < bk->nbricks; i++) {
            const L3Brick *br = &bk->bricks[i];
            if (vz < br->z * L3_B || vz >= (br->z + 1) * L3_B) continue;
            int s = -1;
            for (int k = 0; k < L3_LAMPS; k++) if (br->lamp[k] == lamp) s = k;
            for (int ly = 0; ly < L3_B; ly++)
                for (int lx = 0; lx < L3_B; lx++) {
                    int px = (int)floor(((br->x * L3_B + lx) * V - (cx - half)) / V), py = (int)floor(((cy + half) - (br->y * L3_B + ly) * V) / V);
                    if (px < 0 || py < 0 || px >= W || py >= W) continue;
                    int k = lx + L3_B * (ly + L3_B * (vz - br->z * L3_B));
                    img[py * W + px] = s >= 0 ? (unsigned char)(br->sdf[k][s] / 2 + 64) : 32;
                }
        }
        FILE *o = fopen(argv[8], "wb");
        fprintf(o, "P5\n%d %d\n255\n", W, W);
        fwrite(img, 1, (size_t)W * W, o);
        fclose(o);
        printf("L3BAKE slice z %.0f lamp %d -> %s (%dx%d)\n", z, lamp, argv[8], W, W);
    }
    l3_bake_free(bk);
    return 0;
}
