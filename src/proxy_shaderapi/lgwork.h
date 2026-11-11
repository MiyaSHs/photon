/* P5 (analysis/18_p5_lights.md): the light grid's worker. One thread ("photon-lights") reads the map (worldlights, tree, PVS: src/ambient/lg_map.h)
 * and builds the grid (src/ambient/lg_grid.h) once a frame from the latest engine snapshot. No D3D and no engine calls: lights.c passes the snapshot
 * in and does the texture writes through a callback, so tests/ambient/lgworktest.c runs this file under ThreadSanitizer on the Win32 shim.
 *
 * THREADS
 *   Draw thread: lg_work_frame (generation and the snapshot) and lg_work_upload (copies what was published into the texture).
 *   Worker: the map load and every lg_grid_frame. It alone owns the map and the grid, so they need no lock and nobody else frees them.
 *   Main thread: lg_work_shutdown.
 *
 * THE HAND-OFF. The draw thread copies its snapshot into a slot under g_wl (latest wins; a snapshot the worker never took is counted as
 * skipped) and wakes the worker. After a build the worker publishes under g_pub: the rows that changed are copied into the published image, their
 * bits ORed into the published dirty set, the constants and the generation stamped. lg_work_upload only tries for g_pub and skips the frame if the
 * worker holds it, so the draw thread never waits; it uploads the dirty rows and clears them. A publish of an older generation is never uploaded,
 * and a new generation's first publish carries every row.
 *
 * THE FOLD (P5b, analysis/18): the grid's fold image rows travel with the texture rows they belong to (the same dirty bits), and its fold table
 * with them when it changed; lg_work_upload copies both into the draw thread's own copy in the same step as the texture, so a draw's fold always
 * matches the grid its pixels read. lg_work_fold evaluates the origin's cell, cached by origin in 256 direct-mapped entries; an entry stands while
 * its window, its cell's fold row and the table entries its cell lists are unchanged (each carries the copy version it last changed at).
 *
 * MAP LIFETIME AND STOP: a map change bumps the generation; the worker drops its grid and map at once (only it ever read them) and loads the new
 * map. lg_work_shutdown sets the stop and cancel flags (a load stops within one read), waits a bounded time, never from DllMain; the worker holds a
 * counted reference on this module and exits with FreeLibraryAndExitThread, holds one on the filesystem's module around a load, and makes no
 * filesystem call after the stop flag (the open handle is leaked). Idempotent. ambwork.h's contract, as cubework.h's. */
#ifndef PHOTON_LGWORK_H
#define PHOTON_LGWORK_H
#include <stddef.h>
#include <stdint.h>
#include "ambwork.h"
#include "../ambient/lg_grid.h"
#include "../ambient/sun_vis.h"

typedef struct LgWorkStats {
    long generations, loads, load_failures, loads_cancelled, maps_in_gma, gma_refused, handles_leaked;
    long builds, build_failures, snapshots, snapshots_skipped, publishes, uploads, upload_busy, upload_failures;
    long frames_fresh, frames_stale;     /* frames whose texture holds the latest snapshot, or an older one (a publish behind, or busy) */
    long fold_copies, fold_evals, fold_hits;   /* draw thread: uploads that carried fold data, fold evaluations, cache hits */
    double read_ms, parse_ms, build_ms_sum, build_ms_max;
    int nlights, nstatic;
    long sun_loads, sun_failures;     /* P6: sun maps loaded with the map (lg_work_want_sun), and those that failed */
    LgStats last;
    char level[64];
} LgWorkStats;

/* Copies the image rows marked in rows[] into the texture (draw thread). Returns 1 on success. */
typedef int (*LgWorkCopyFn)(void *ctx, const float *image, const uint8_t rows[LG_TEX_H]);

/* Before any other call. AMB_ERR_ARG for a NULL files or bad params, or when called again before a clean shutdown */
int lg_work_init(const AmbWorkFiles *files, const LgParams *p);
/* Draw thread, once a frame: want (in game and the setting on), the map's path, hdr (which light lump), and this frame's snapshot (NULL: none) */
void lg_work_frame(int want, const char *level, int hdr, const LgSnapshot *snap);
/* Draw thread: uploads what the worker published since the last upload. 1 if the texture now holds the current generation's grid (c63 set),
 * 0 if there is none yet or the upload failed. */
int lg_work_upload(LgWorkCopyFn copy, void *ctx, float c63[4]);
/* Draw thread: the texture was lost (device reset): the next upload carries every row of the last publish */
void lg_work_texture_lost(void);
/* Draw thread: the folded light at origin (lg_fold_eval of its cell, 6 faces rgb), for the grid the texture holds. 1 and out (zero outside the
 * window) when the texture holds this generation's grid, else 0 and out zero. */
int lg_work_fold(const float origin[3], float out[6][3]);
/* P6 sun (analysis/22): before lg_work_init, 1 makes every map load also load its SunMap (src/ambient/sun_vis.h) through the same reader */
void lg_work_want_sun(int on);
/* Draw thread: the current generation's SunMap, or NULL (none yet, none for this map, sun off). The draw thread owns it: valid until its next
 * lg_work_frame that changes the map, or lg_work_shutdown. *serial (may be NULL) changes whenever the returned map changes (0 for NULL), so a
 * cache built on a map can tell it is stale even if a new map lands at the same address. Never waits (a try-lock on the publish). */
const SunMap *lg_work_sun(long *serial);
/* L1 (analysis/35): the map's point and spot lights the camera's cluster sees (PVS) and that reach within LG_LAMP_RANGE of the camera,
 * nearest first, with this frame's style scale applied to the colour. The worker rebuilds the list with every grid build; the draw thread
 * takes the latest with a try-lock (never waits) and keeps its own copy. Returns the count (0..LG_LAMPS_MAX); *seq changes with every copy. */
enum { LG_LAMPS_MAX = 64, LG_LAMP_RANGE = 2048 };
typedef struct LgLamp {
    float origin[3], color[3], dir[3];   /* colour = intensity x style scale (linear, the grid's units); dir: a spot's axis */
    float stopdot, stopdot2, exponent, reach;
    float att[3];                        /* constant, linear, quadratic */
    int type, index;                     /* LG_EMIT_POINT / LG_EMIT_SPOT; the worldlight's index (stable for the map) */
} LgLamp;
int lg_work_lamps(const LgLamp **out, long *seq);
void lg_work_shutdown(const char *why);
void lg_work_stats(LgWorkStats *out);
void lg_work_summary(const char *why);
/* Build times of the last LG_WORK_TIMES builds (worker clock, ms), for the summary's p50 / p99; returns the count */
enum { LG_WORK_TIMES = 4096 };
int lg_work_times(float *out, int cap);
#ifdef LG_WORK_TEST_HOOKS
extern DWORD lg_work_test_wait_ms;
extern DWORD lg_work_test_sun_delay_ms;
#endif
#endif
