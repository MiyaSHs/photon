/* PBR packs: the map's world materials' pack maps as two DXT1 atlases (pbrpack.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    PBR_RMH_TILE = 64, PBR_NRM_TILE = 128,                 /* tile sides (32 x 32 tiles per atlas) */
    PBR_RMH_SIDE = 32 * 64, PBR_NRM_SIDE = 32 * 128,      /* atlas sides */
    PBR_RMH_BYTES = PBR_RMH_SIDE * PBR_RMH_SIDE / 2, PBR_NRM_BYTES = PBR_NRM_SIDE * PBR_NRM_SIDE / 2,   /* DXT1: half a byte a texel */
    PBR_TILE_HAS_NORMAL = 1 << 16                          /* in tile[]: the normal atlas holds this tile too */
};
typedef struct PbrAtlas {
    int ntd, *tile;            /* per texdata: tile index (| PBR_TILE_HAS_NORMAL), -1 none */
    int ntiles, missing, small, with_normal, gmas;   /* gmas: pack GMA files read (data/photon/pbr, pbr_packs.txt) */
    uint8_t *rmh, *nrm;        /* DXT1 blocks, row-major, PBR_RMH_BYTES / PBR_NRM_BYTES */
} PbrAtlas;
PbrAtlas *pbr_atlas_build(const uint8_t *bsp, size_t size);   /* NULL: no texdata; ntiles 0: no pack installed for this map */
void pbr_atlas_free(PbrAtlas *a);
