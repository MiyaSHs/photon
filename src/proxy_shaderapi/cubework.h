/* P4a: the cubemap pick table's worker and its camera-centred window (analysis/17). The table (src/ambient/amb_pick.h) covers the ambient volume's
 * cascade-0 grid: 64 x 64 x 32 voxels of 32 units around the main view, moved in steps of 8 by the same scheduler (amb_clip.h), so a pick and an ambient
 * voxel are the same box of world space. Nothing reads it yet but the P4a probe (cubemap.c); P4b binds the second cubemap from it.
 *
 * THREADS, as ambwork.h: the draw thread (the one that calls shaderapi: Present and every draw) calls cube_work_frame, _lookup and _sample; worker 0 reads
 * the map and builds jobs; the main thread calls cube_work_shutdown.
 *
 * THE HAND-OFF. A worker builds a job into its own buffer, waits until the job is the queue's head (amb_clip_may_write), and copies it into the staging table
 * under g_stage_lock. The draw thread, in cube_work_frame, copies a written head from the staging table into the draw table it alone reads (one whole window
 * or one slab: 512 KiB at most, a memcpy), between amb_clip_begin_upload and _end_upload, and only if it gets the staging lock at once; otherwise it tries
 * at the next frame. So the draw table only ever holds finished jobs, and the valid box says which voxels those are. The head rule alone already keeps the
 * worker's writes and the draw thread's copies apart (only the head's texels are touched, by one side per job state, with g_wl ordering the states); the
 * staging lock is a second line, and removing it is not a mutant the test can catch (scripts/run_ambient_test.sh).
 *
 * MAP LIFETIME AND STOP: exactly ambwork.h's contract. A map change starts a new generation and retires the old map, freed only when the worker has
 * acknowledged a later generation. cube_work_shutdown sets the stop flag and the cancel flag (a build stops within one row of voxels, a load within one
 * read), waits a bounded time, never from DllMain; the worker holds a counted reference on this module and exits with FreeLibraryAndExitThread, holds a
 * reference on the filesystem's module around each load, makes no filesystem call after the stop flag (the open handle is leaked), and is idempotent.
 * Maps inside a GMA are read through AmbWorkFiles.open, which gives them a checked handle of their own or refuses them (ambvol.c). */
#ifndef PHOTON_CUBEWORK_H
#define PHOTON_CUBEWORK_H
#include <stddef.h>
#include <stdint.h>
#include "ambwork.h"
#include "../ambient/amb_pick.h"

enum { CUBE_X = 64, CUBE_Y = 64, CUBE_Z = 32, CUBE_VOXEL = 32, CUBE_STEP = 8,
       /* P4c: one coarse level over model 0's bounds, built after cascade 0 in slices between clip jobs; 256-unit voxels, doubled until the
        * level fits in CUBE_COARSE_MAX entries (8 MiB) */
       CUBE_COARSE_VOXEL = 256, CUBE_COARSE_MAX = 2 << 20, CUBE_COARSE_SLICE = 4 };

typedef struct CubeWorkStats {
    long generations, loads, load_failures, maps_freed, maps_in_gma, gma_refused, handles_leaked, build_failures;
    long jobs[2], jobs_cancelled, write_cancelled, uploads, stage_busy;
    double build_ms[2], build_ms_max[2], first_ms, read_ms, parse_ms;   /* first_ms: generation start to its first whole window in the draw table */
    long long bytes_read;
    int ncubes, max_backlog;
    long lookups, hits, coarse_hits;
    long long coarse_voxels;
    int coarse_voxel;
    double coarse_ms;
    char level[64];
} CubeWorkStats;

/* Before any other call. AMB_ERR_ARG if called twice without a shutdown in between, or for a NULL files */
int cube_work_init(const AmbWorkFiles *files);
/* Draw thread, once per frame: want (a map is loaded and the table is wanted), level (the map's path as the filesystem takes it), the main view (NULL if
 * unknown). A change of (want, level) starts a new generation. Also moves at most one finished job into the draw table. */
void cube_work_frame(int want, const char *level, const double *cam);
/* Draw thread: 1 and the entry of p's voxel if it is inside cascade 0's valid box, 2 and the coarse level's entry if it is inside that level
 * (complete only), else 0 */
int cube_work_lookup(const float p[3], uint32_t *entry);
/* Draw thread: the index of the current map's lump-42 sample with this origin, -1 if there is none or no map is loaded yet; *n = its sample count */
int cube_work_sample(const int origin[3], int *n);
/* Draw thread: sample i's origin; 0 if out of range or no map */
int cube_work_origin(int i, int o[3]);
/* Draw thread: 1 once the current generation's map is loaded (even with zero samples) */
int cube_work_map_ready(void);
void cube_work_shutdown(const char *why);
void cube_work_stats(CubeWorkStats *out);
void cube_work_summary(const char *why);
#ifdef CUBE_WORK_TEST_HOOKS                    /* tests/ambient/cubeworktest.c only */
extern DWORD cube_work_test_wait_ms;          /* the shutdown bound */
extern int cube_work_test_coarse_voxel;       /* the coarse level's first voxel size */
#endif
#endif
