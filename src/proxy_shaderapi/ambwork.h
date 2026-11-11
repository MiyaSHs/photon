/* P3g: the ambient volume's map loader and bake workers (analysis/15d section 8, items 3, 5, 6). No D3D and no engine calls: ambvol.c
 * passes the camera in and does the texture writes through a callback, which is what lets tests/ambient/worktest.c run this file
 * under ThreadSanitizer on a shim of the few Win32 calls it makes.
 *
 * THREADS
 *   Draw thread: amb_work_frame, _upload, _full_upload, _has_data, _weight, _constants, _fold. It owns the valid box and the map pointer
 *     draws read, so those need no lock.
 *   Workers (1, a second only while 2 or more jobs are behind): read and parse the map (worker 0), bake jobs, write the CPU atlas.
 *   Main thread: amb_work_shutdown.
 *
 * THE HAND-OFF. The CPU atlas has exactly one owner at a time, by an SRW lock (g_atlas_lock): a worker holds it only around
 * amb_write_toroidal, the draw thread only tries for it and skips the frame if it is busy, so a frame never waits on a bake. On top of
 * that, order: a worker may write a job only while that job is the queue's head (amb_clip_may_write, checked again with the lock held,
 * since a jump can cancel the job in between), and the next job becomes head only when the draw thread has finished uploading this one.
 * So the atlas never holds data newer than head, and the texture is only ever written with what the state machine says is finished.
 * Bakes themselves touch no shared memory: each writes its own malloc'ed floats from an immutable map.
 *
 * MAP LIFETIME. A map change bumps the generation and retires the old map with that stamp; the draw thread stops using it at once.
 * Workers acknowledge the current generation under the lock each time they hold no job, and the retired map is freed by the draw thread
 * only when every started worker has acknowledged its stamp.
 *
 * SHUTDOWN. amb_work_shutdown sets the stop flag, sets every cancel flag (a bake stops within one row, a map load within one read of at
 * most 1 MB: amb_map_load_reader checks the flag before each), and waits for the threads with a bound. It must not run from DllMain: thread exit needs the loader lock. Idempotent.
 * A worker that outlives the bound can only be inside a filesystem call or a bake. Each worker holds its own module reference and exits with
 * FreeLibraryAndExitThread, so this DLL stays mapped under it, and worker 0 holds a second one on the filesystem's module for as long as a load runs, so a
 * stuck call has code to return to. After the stop flag it makes no further filesystem call (the open handle is leaked: the process is exiting), since
 * the filesystem may have been shut down by then. Nothing else is promised past the bound.
 *
 * READS. Only the lumps the map needs (amb_map_load_reader), wherever the file lives. A map inside a GMA is not read through the filesystem: all files of a
 * GMA share one OS handle, and each of their reads is an unlocked seek then read on it, so a worker reading beside the main thread could scramble both
 * (15d section 8). ambvol.c's open gives such a map a read-only handle of its own on the .gma instead, checked against the GMA's index, or refuses it
 * (*gma = 1 and NULL), which leaves it on the engine cube with a counter and a log line. */
#ifndef PHOTON_AMBWORK_H
#define PHOTON_AMBWORK_H
#include <stddef.h>
#include <stdint.h>
#include "../ambient/amb_clip.h"

typedef struct AmbWorkFiles {         /* the game's filesystem, called from worker 0 */
    void *(*pin)(void);                  /* a counted reference on the module the calls below run in, around each load */
    void (*unpin)(void *pin);
    /* NULL: cannot be opened, or inside a GMA and refused (no handle of its own could be made and checked). *gma: 1 for a map inside a GMA. */
    void *(*open)(const char *path, int *gma, const volatile int *cancel);   /* cancel: the load's flag, for any reading open does itself */
    long long (*size)(void *file);
    int (*read_at)(void *file, long long ofs, void *buf, int n);   /* bytes [ofs, ofs + n), n <= 1 MB; returns the bytes read */
    void (*close)(void *file);
    void (*thread_start)(int index);     /* may be NULL: called once on each worker, to name it */
} AmbWorkFiles;

/* Copies one box of the atlas to the texture, on the draw thread. Returns 1 on success. */
typedef int (*AmbWorkCopyFn)(void *ctx, const uint8_t *atlas, size_t row_bytes, size_t slice_bytes, const AmbTexBox *box);

typedef struct AmbWorkStats {
    long generations, loads, load_failures, maps_freed, workers_started, max_backlog;
    long maps_in_gma, gma_refused, handles_leaked;   /* maps_in_gma: loads of maps inside a GMA (read or not); gma_refused: of those, left on the cube */
    char last_gma_map[64];
    long long bytes_read, peak_bytes;   /* the last load: what it read, and the most it held in lump buffers */
    long jobs[2], jobs_cancelled, write_cancelled;   /* jobs[AMB_JOB_WHOLE / AMB_JOB_SLAB] written to the atlas */
    double bake_ms[2], bake_ms_max[2], write_ms_max, head_wait_ms, read_ms, parse_ms;
    long uploads, full_uploads, atlas_busy, copy_failures;
    long a_evals, a_hits;
} AmbWorkStats;

/* Before any other call. AMB_ERR_ARG for impossible dimensions. The atlas is allocated when the first map is wanted. */
int amb_work_init(const AmbWorkFiles *files, const int dim[3], const int voxel[3], int step);
/* Once per frame. want: in game and the setting on. level: the map's path as the filesystem takes it. hdr: which ambient lumps. A change
 * of (want, level, hdr) starts a new generation. cam: the main view's position, NULL if unknown this frame. */
void amb_work_frame(int want, const char *level, int hdr, const double *cam);
/* The atlas holds something the texture should get: a re-upload or an upload is worth creating the texture for */
int amb_work_has_data(void);
/* At most one job part (one slab's boxes). Returns 1 if a part went up. */
int amb_work_upload(AmbWorkCopyFn copy, void *ctx);
/* After the texture was re-created: the whole atlas, taking a written head job along so that the box matches. Returns 1 when the texture
 * matches the valid box, 0 to try again later (a worker holds the atlas, or the copy failed). */
int amb_work_full_upload(AmbWorkCopyFn copy, void *ctx);
float amb_work_weight(const float origin[3]);
void amb_work_constants(float c[4][4]);       /* c59, c60, c61, c62 */
/* c33..c38 = lerp(C, max(0, C - 1.5 A), f), A = the engine's leaf ambient at origin, cached until the origin changes */
void amb_work_fold(const float origin[3], float f, float cube[6][4]);
int amb_work_backlog(void);
void amb_work_shutdown(const char *why);
void amb_work_summary(const char *why);
void amb_work_stats(AmbWorkStats *out);
/* Tests only (PHOTON_TEST_AMBTRACE): the window's state as text; the engine rule's A at origin, zeros without a map. Draw thread. */
void amb_work_trace_state(char *out, size_t n);
void amb_work_trace_a(const float origin[3], float a[AMB_VOXEL_FLOATS]);
int amb_work_map_loaded(void);                /* draw thread */
#ifdef AMB_WORK_TEST_HOOKS                    /* tests/ambient/worktest.c only */
extern void (*amb_work_test_between_locks)(int worker);   /* in a job's write path, after its turn came and before it holds the atlas */
extern volatile int amb_work_test_fail_bake;   /* the next bake returns AMB_ERR_NOMEM */
void amb_work_test_timing(double hold_seconds, double survive_seconds);   /* after init: a small test window's bakes are far shorter */
#endif
#endif
