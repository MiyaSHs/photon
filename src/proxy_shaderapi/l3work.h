/* L3c (analysis/37): the re-bake's worker. One thread ("photon-l3"): on a map (or voxel size) change it reads the map's BSP through the
 * game's filesystem (AmbWorkFiles, as lgwork: maps inside a GMA too), loads data/photon/bake/<map>_<key>_<voxel>.l3 if present, else bakes
 * (src/ambient/l3_bake.c), packs and writes that file, and publishes the pack with the map's worldlights.
 * The draw thread asks with l3_work_frame (Present) and takes the result with l3_work_take (device thread, try-lock, never waits; it then
 * owns it). A change of map or voxel size cancels a bake in progress. Never from DllMain; the worker holds a module reference. */
#ifndef PHOTON_L3WORK_H
#define PHOTON_L3WORK_H
#include "ambwork.h"
#include "../ambient/l3_bake.h"
#include "../ambient/lg_map.h"

typedef struct L3Ready {
    L3Pack *pack;                 /* NULL: this map has nothing to correct */
    LgWorldLight *lights;
    int nlights, voxel;
    char level[64];
} L3Ready;

void l3_work_frame(const AmbWorkFiles *files, const char *level, int voxel);   /* voxel 0: not wanted */
L3Ready *l3_work_take(void);       /* the newest result, or NULL; the caller frees it with l3_ready_free */
void l3_ready_free(L3Ready *r);
void l3_work_summary(const char *why);
#endif
