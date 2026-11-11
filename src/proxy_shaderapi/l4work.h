/* L4 in game (analysis/39): Photon's path-traced lightmaps for the current map, when photon_pathtrace is on.
 * A worker reads the map's BSP through the game's filesystem (AmbWorkFiles, as l3work), loads data/photon/bake/<map>_<key>_l4.lump if
 * present, else bakes it (src/ambient/l4_bake.c, passes on several low-priority threads, progress in data/photon/bake/l4_progress.txt) and
 * writes that file. The main thread (P6's DrawModelExecute hook) then copies the lump over the engine's in-memory lighting data
 * (worldbrush + 0x120, checked against the map's own lump) and calls R_RedownloadAllLightmaps (engine + 0xe5260), so the engine builds
 * Photon's luxels. The map file is never touched; switching the setting off restores the map's own lump the same way. */
#ifndef PHOTON_L4WORK_H
#define PHOTON_L4WORK_H
#include "ambwork.h"

void l4_work_frame(const AmbWorkFiles *files, const char *level, int want);   /* Present thread (lights.c): want 0 off, 1 path-traced, 2 toon steps */
void photon_l4_main_tick(void);                                              /* main thread, from P6's DrawModelExecute hook */
void l4_work_summary(const char *why);
int photon_l4_applied_mode(void);   /* the lightmaps in the engine now: 0 the map's own, 1 path-traced, 2 toon, 3 flat (PT-RT) */
#endif
