/* P4a: see cubework.h. The structure follows ambwork.c (P3g) on purpose, so the two can be reviewed against each other; this one has one worker, and its
 * "upload" is a copy into a CPU table rather than a texture. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cubework.h"
#include "log.h"

/* Test-only (scripts/run_ambient_test.sh): each removes one rule, and tests/ambient/cubeworktest.c must then fail.
 * 1 a job is staged before it is the head, 3 a retired map is freed without waiting for the worker,
 * 4 a new generation does not cancel a build, 5 the file is closed after the stop flag, 6 no reference on the filesystem's module, 7 a lookup ignores
 * the valid box. 8 the coarse level is never built. */
#ifndef CUBE_WORK_MUTANT
#define CUBE_WORK_MUTANT 0
#endif

enum { MAX_RETIRED = 8, MAX_PATH_BYTES = 260, NVOX = CUBE_X * CUBE_Y * CUBE_Z };
#ifdef CUBE_WORK_TEST_HOOKS
DWORD cube_work_test_wait_ms = 5000;
int cube_work_test_coarse_voxel = CUBE_COARSE_VOXEL;
#define COARSE_VOXEL0 cube_work_test_coarse_voxel
#define SHUTDOWN_WAIT_MS cube_work_test_wait_ms
#else
#define SHUTDOWN_WAIT_MS 5000u
#define COARSE_VOXEL0 CUBE_COARSE_VOXEL
#endif

typedef struct CubeMap {
    AmbMap *m;
    AmbCubes c;
    AmbPickTargets t;
    /* P4c, the coarse map-wide level: written by the worker slice by slice, z fastest-changing last; immutable once cdone (set under g_wl),
     * and only then read by the draw thread */
    uint32_t *coarse;
    AmbBox cbox;
    int cvox, cnext, cdone;
    double cms;
} CubeMap;

typedef struct Worker {
    HANDLE h;
    LONG ack;               /* under g_wl: the generation it last held nothing in */
    unsigned job;           /* under g_wl */
    volatile int cancel;    /* written under g_wl; read by a build within each row and by the file loop */
    HMODULE module;
} Worker;

static AmbWorkFiles g_files;
static AmbClip g_clip;
static uint32_t *g_stage;                /* the worker's side */
static uint32_t *g_table;                /* the draw thread's side */
static SRWLOCK g_stage_lock = SRWLOCK_INIT;
static SRWLOCK g_wl = SRWLOCK_INIT;      /* queue, generation, map pointers, the worker slot, stats. Order: stage, then this. */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static int g_inited, g_stop, g_off, g_started, g_clean = 1;
static volatile LONG g_shut;
static LONG g_gen;
static Worker g_w;
static double g_qpc_ms;
static CubeWorkStats g_st;

static int g_load_want;
static LONG g_load_gen;
static char g_load_path[MAX_PATH_BYTES];
static CubeMap *g_loaded;                /* parsed, not yet adopted */
static CubeMap *g_map;                   /* in use: written by the draw thread under g_wl, read by the worker under g_wl */
static int g_failed;
static struct { CubeMap *m; LONG stamp; } g_retired[MAX_RETIRED];

/* draw thread only */
static int g_key_want;
static char g_key_level[MAX_PATH_BYTES];
static double g_gen_t0, g_last_tick_ms;
static int g_first_done;
static long g_lookups, g_hits, g_coarse_hits;   /* copied into g_st once a frame */
static const CubeMap *g_dc;              /* the coarse level the draw thread uses: the current map's, once complete */

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g_qpc_ms;
}

static void map_free(CubeMap *m) {
    if (!m) return;
    amb_map_free(m->m);
    free(m->coarse);
    free(m);
}

/* The coarse level's grid: model 0's bounds at CUBE_COARSE_VOXEL units, the voxel doubled until it fits in CUBE_COARSE_MAX entries */
static void coarse_alloc(CubeMap *m) {
    AmbMapInfo mi;
    amb_map_info(m->m, &mi);
    for (int v = COARSE_VOXEL0; v <= 8192; v *= 2) {
        AmbBox b;
        long long n = 1;
        for (int k = 0; k < 3; k++) {
            if (!(mi.world_min[k] > -1e6f && mi.world_max[k] < 1e6f && mi.world_max[k] > mi.world_min[k])) return;
            b.org[k] = (int)floor(mi.world_min[k] / v);
            b.dim[k] = (int)ceil(mi.world_max[k] / v) - b.org[k];
            n *= b.dim[k];
        }
        if (n > CUBE_COARSE_MAX) continue;
        m->coarse = malloc((size_t)n * sizeof *m->coarse);
        if (!m->coarse) return;
        m->cbox = b;
        m->cvox = v;
        return;
    }
}

/* --- worker ------------------------------------------------------------------------------------------------------ */
/* The same intended race as AmbPickParams.cancel (tests/ambient/worktest.supp matches the name) */
static __attribute__((noinline)) int worker_cancelled(const Worker *w) { return w->cancel; }

static int stopping(void) {
    AcquireSRWLockExclusive(&g_wl);
    int s = g_stop;
    ReleaseSRWLockExclusive(&g_wl);
    return s;
}

typedef struct { void *f; double ms; } LoadCtx;
static int load_read(void *ctx, long long ofs, void *buf, int n) {
    LoadCtx *c = ctx;
    double t = now_ms();
    int r = g_files.read_at(c->f, ofs, buf, n);
    c->ms += now_ms() - t;
    return r;
}

static void do_load(Worker *w, const char *path, LONG gen) {
    double t0 = now_ms();
    long long size = 0;
    void *pin = CUBE_WORK_MUTANT == 6 ? NULL : g_files.pin();
    int gma = 0;
    void *f = g_files.open(path, &gma, &w->cancel);
    int opened = f != NULL, rc = AMB_ERR_FORMAT;
    CubeMap *m = calloc(1, sizeof *m);
    AmbReadStats rs = { 0, 0, 0 }, rc2 = { 0, 0, 0 };
    LoadCtx lc = { f, 0.0 };
    if (!m) rc = AMB_ERR_NOMEM;
    else if (f && !stopping()) {
        size = g_files.size(f);
        AmbReader r = { &lc, size, load_read, &w->cancel };
        rc = amb_map_load_reader(&r, 0, &m->m, &rs);
        if (rc == AMB_OK && !stopping()) rc = amb_cubes_load_reader(&r, &m->c, &rc2);
        else if (rc == AMB_OK) rc = AMB_ERR_CANCELLED;
        if (rc == AMB_OK) { amb_pick_targets(m->m, &m->c, &m->t); coarse_alloc(m); }
    }
    int leaked = 0;
    if (f) {
        if (CUBE_WORK_MUTANT != 5 && stopping()) leaked = 1;
        else g_files.close(f);
    }
    if (pin) g_files.unpin(pin);
    double t1 = now_ms();
    int cancelled = worker_cancelled(w);
    if (cancelled || rc != AMB_OK) { map_free(m); m = NULL; }
    AcquireSRWLockExclusive(&g_wl);
    int current = gen == g_gen && !g_stop;
    int ncubes = m ? m->c.n : 0;
    if (m && current) { g_loaded = m; m = NULL; }
    g_st.loads++;
    g_st.read_ms = lc.ms;
    g_st.parse_ms = t1 - t0 - lc.ms;
    g_st.bytes_read = rs.bytes_read + rc2.bytes_read;
    g_st.handles_leaked += leaked;
    int refused = gma && !f && current && !cancelled;
    if (gma) { g_st.maps_in_gma++; g_st.gma_refused += refused; }
    if (current && rc != AMB_OK && !refused && !cancelled) g_st.load_failures++;
    if (current && rc == AMB_OK) g_st.ncubes = ncubes;
    ReleaseSRWLockExclusive(&g_wl);
    map_free(m);   /* nobody else ever saw it */
    if (!current || cancelled) photon_log("cubework: load of %s abandoned (a newer map or a stop)", path);
    else if (refused) photon_log("cubework: %s is inside a GMA and has no checked handle of its own: no pick table for it", path);
    else if (rc != AMB_OK) photon_log("cubework: %s: no pick table for this map: %s, %lld of %lld bytes read, error %d", path, opened ? "opened" : "cannot open",
                                      rs.bytes_read + rc2.bytes_read, size, rc);
    else photon_log("cubework: %s loaded%s: %d cubemap samples, %lld of %lld bytes read, %.0f ms reading, %.0f ms parsing", path, gma ? " (inside a GMA)" : "",
                    ncubes, rs.bytes_read + rc2.bytes_read, size, lc.ms, t1 - t0 - lc.ms);
    if (current) WakeAllConditionVariable(&g_cv);
}

static void note_max(double *m, double v) { if (v > *m) *m = v; }

static void run_job(Worker *w, const AmbClipJob *j, const CubeMap *map) {
    AmbPickParams p;
    amb_pick_params_default(&p);
    p.voxel = CUBE_VOXEL;
    if (CUBE_WORK_MUTANT != 4) p.cancel = &w->cancel;
    size_t n = (size_t)j->box.dim[0] * (size_t)j->box.dim[1] * (size_t)j->box.dim[2];
    uint32_t *buf = malloc(n * sizeof *buf);
    double t0 = now_ms();
    int rc = buf ? amb_pick_box(map->m, &map->c, &map->t, &p, &j->box, buf) : AMB_ERR_NOMEM;
    double t1 = now_ms();
    int wrote = 0;
    if (rc == AMB_OK) {
        AcquireSRWLockExclusive(&g_wl);
        while (CUBE_WORK_MUTANT != 1 && !g_stop && amb_clip_alive(&g_clip, j->id) && !amb_clip_may_write(&g_clip, j->id))
            SleepConditionVariableSRW(&g_cv, &g_wl, 100, 0);
        ReleaseSRWLockExclusive(&g_wl);
        AcquireSRWLockExclusive(&g_stage_lock);
        AcquireSRWLockExclusive(&g_wl);
        int go = !g_stop && (CUBE_WORK_MUTANT == 1 ? amb_clip_alive(&g_clip, j->id) : amb_clip_may_write(&g_clip, j->id));
        ReleaseSRWLockExclusive(&g_wl);
        if (go) {
            static const int dim[3] = { CUBE_X, CUBE_Y, CUBE_Z };
            int wr = amb_pick_write_toroidal(buf, &j->box, dim, g_stage);
            AcquireSRWLockExclusive(&g_wl);
            if (wr == AMB_OK) { amb_clip_written(&g_clip, j->id); wrote = 1; }
            else rc = wr;
            ReleaseSRWLockExclusive(&g_wl);
        }
        ReleaseSRWLockExclusive(&g_stage_lock);
    }
    free(buf);
    AcquireSRWLockExclusive(&g_wl);
    if (rc == AMB_OK && wrote) {
        g_st.jobs[j->kind]++;
        g_st.build_ms[j->kind] += t1 - t0;
        note_max(&g_st.build_ms_max[j->kind], t1 - t0);
    } else if (rc == AMB_OK) g_st.write_cancelled++;
    else if (rc == AMB_ERR_CANCELLED) g_st.jobs_cancelled++;
    else if (!g_failed && amb_clip_alive(&g_clip, j->id)) {
        g_failed = 1;
        g_st.build_failures++;
        photon_log("cubework: build of a %d x %d x %d box failed (%d): no pick table for this map", j->box.dim[0], j->box.dim[1], j->box.dim[2], rc);
    }
    ReleaseSRWLockExclusive(&g_wl);
}

/* One slice of the coarse level (CUBE_COARSE_SLICE layers), between clip jobs: a slice is about a tenth of a second on the VM, so a slab
 * never waits longer than that behind it */
static void coarse_slice(Worker *w, CubeMap *map) {
    AmbPickParams p;
    amb_pick_params_default(&p);
    p.voxel = map->cvox;
    p.cancel = &w->cancel;
    AmbBox b = map->cbox;
    b.org[2] += map->cnext;
    b.dim[2] = map->cbox.dim[2] - map->cnext < CUBE_COARSE_SLICE ? map->cbox.dim[2] - map->cnext : CUBE_COARSE_SLICE;
    double t0 = now_ms();
    int rc = amb_pick_box(map->m, &map->c, &map->t, &p, &b, map->coarse + (size_t)map->cnext * (size_t)map->cbox.dim[0] * (size_t)map->cbox.dim[1]);
    double t1 = now_ms();
    AcquireSRWLockExclusive(&g_wl);
    if (rc == AMB_OK) {
        map->cnext += b.dim[2];
        map->cms += t1 - t0;
        if (map->cnext >= map->cbox.dim[2]) {
            map->cdone = 1;
            g_st.coarse_ms = map->cms;
            g_st.coarse_voxels = (long long)map->cbox.dim[0] * map->cbox.dim[1] * map->cbox.dim[2];
            g_st.coarse_voxel = map->cvox;
        }
    } else if (rc != AMB_ERR_CANCELLED) {
        free(map->coarse);   /* the draw thread never saw it: cdone is still 0 */
        map->coarse = NULL;
    }
    ReleaseSRWLockExclusive(&g_wl);
    if (map->cdone)
        photon_log("cubework: coarse level built: %d x %d x %d voxels of %d units (%.1f MiB) in %.0f ms", map->cbox.dim[0], map->cbox.dim[1],
                   map->cbox.dim[2], map->cvox, (double)g_st.coarse_voxels * 4.0 / 1048576.0, map->cms);
}

static DWORD WINAPI worker_main(LPVOID arg) {
    Worker *w = arg;
    if (g_files.thread_start) g_files.thread_start(2);   /* 0 and 1 are the ambient workers */
    AcquireSRWLockExclusive(&g_wl);
    for (;;) {
        w->ack = g_gen;
        w->job = 0;
        w->cancel = g_stop;
        if (g_stop) break;
        if (g_load_want) {
            char path[MAX_PATH_BYTES];
            memcpy(path, g_load_path, sizeof path);
            LONG gen = g_load_gen;
            g_load_want = 0;
            ReleaseSRWLockExclusive(&g_wl);
            do_load(w, path, gen);
            AcquireSRWLockExclusive(&g_wl);
            continue;
        }
        AmbClipJob j;
        const CubeMap *map = g_map;
        if (map && !g_failed && amb_clip_take(&g_clip, &j)) {
            w->job = j.id;
            ReleaseSRWLockExclusive(&g_wl);
            run_job(w, &j, map);
            AcquireSRWLockExclusive(&g_wl);
            WakeAllConditionVariable(&g_cv);
            continue;
        }
        /* after cascade 0 is in (its valid box), with no clip job waiting */
        if (map && !g_failed && map->coarse && !map->cdone && g_clip.valid && CUBE_WORK_MUTANT != 8) {
            ReleaseSRWLockExclusive(&g_wl);
            coarse_slice(w, (CubeMap *)map);
            AcquireSRWLockExclusive(&g_wl);
            continue;
        }
        SleepConditionVariableSRW(&g_cv, &g_wl, 250, 0);
    }
    ReleaseSRWLockExclusive(&g_wl);
    FreeLibraryAndExitThread(w->module, 0);
    return 0;
}

static int start_worker(void) {
    HMODULE self = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)worker_main, &self)) {
        photon_log("cubework: no module reference for the worker (%lu)", (unsigned long)GetLastError());
        return 0;
    }
    AcquireSRWLockExclusive(&g_wl);
    g_w.module = self;
    g_w.ack = g_gen;
    g_w.job = 0;
    g_w.cancel = 0;
    ReleaseSRWLockExclusive(&g_wl);
    HANDLE h = CreateThread(NULL, 0, worker_main, &g_w, 0, NULL);
    if (!h) {
        photon_log("cubework: CreateThread failed (%lu)", (unsigned long)GetLastError());
        FreeLibrary(self);
        return 0;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    g_w.h = h;
    g_started = 1;
    photon_log("cubework: worker started");
    return 1;
}

/* --- generations ------------------------------------------------------------------------------------------------- */
static void retire(CubeMap *m) {
    if (!m) return;
    for (int i = 0; i < MAX_RETIRED; i++)
        if (!g_retired[i].m) { g_retired[i].m = m; g_retired[i].stamp = g_gen; return; }
    photon_log("cubework: retired map table full, a map is leaked");
}

static void new_generation(int want, const char *level) {
    AcquireSRWLockExclusive(&g_wl);
    g_gen++;
    g_st.generations++;
    amb_clip_reset(&g_clip);
    retire(g_map);
    retire(g_loaded);
    g_map = g_loaded = NULL;
    g_failed = 0;
    g_st.ncubes = 0;
    g_load_want = want;
    if (want) {
        snprintf(g_load_path, sizeof g_load_path, "%s", level);
        g_load_gen = g_gen;
    }
    if (g_started && CUBE_WORK_MUTANT != 4) g_w.cancel = 1;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_wl);
    g_gen_t0 = now_ms();
    g_first_done = 0;
    g_dc = NULL;
}

static void reap(void) {
    CubeMap *dead[MAX_RETIRED];
    int n = 0;
    AcquireSRWLockExclusive(&g_wl);
    for (int i = 0; i < MAX_RETIRED; i++) {
        if (!g_retired[i].m) continue;
        if (CUBE_WORK_MUTANT == 3 || !g_started || g_w.ack - g_retired[i].stamp >= 0) {
            dead[n++] = g_retired[i].m;
            g_retired[i].m = NULL;
            g_st.maps_freed++;
        }
    }
    ReleaseSRWLockExclusive(&g_wl);
    for (int i = 0; i < n; i++) map_free(dead[i]);
}

/* The written head into the draw table: the job's voxels only, at their toroidal places */
static void upload(void) {
    if (!TryAcquireSRWLockShared(&g_stage_lock)) {
        AcquireSRWLockExclusive(&g_wl);
        g_st.stage_busy++;
        ReleaseSRWLockExclusive(&g_wl);
        return;
    }
    AmbClipJob j;
    AcquireSRWLockExclusive(&g_wl);
    int ready = !g_stop && amb_clip_upload_ready(&g_clip, &j);
    if (ready) amb_clip_begin_upload(&g_clip, j.id);
    ReleaseSRWLockExclusive(&g_wl);
    if (ready) {
        for (int z = 0; z < j.box.dim[2]; z++)
            for (int y = 0; y < j.box.dim[1]; y++) {
                int tz = ((j.box.org[2] + z) % CUBE_Z + CUBE_Z) % CUBE_Z, ty = ((j.box.org[1] + y) % CUBE_Y + CUBE_Y) % CUBE_Y;
                size_t row = ((size_t)tz * CUBE_Y + (size_t)ty) * CUBE_X;
                for (int x = 0; x < j.box.dim[0]; x++) {
                    size_t i = row + (size_t)(((j.box.org[0] + x) % CUBE_X + CUBE_X) % CUBE_X);
                    g_table[i] = g_stage[i];
                }
            }
        AcquireSRWLockExclusive(&g_wl);
        amb_clip_end_upload(&g_clip, j.id);
        g_st.uploads++;
        if (j.kind == AMB_JOB_WHOLE && !g_first_done) { g_first_done = 1; g_st.first_ms = now_ms() - g_gen_t0; }
        WakeAllConditionVariable(&g_cv);
        ReleaseSRWLockExclusive(&g_wl);
    }
    ReleaseSRWLockShared(&g_stage_lock);
}

/* --- draw thread ----------------------------------------------------------------------------------------------------- */
int cube_work_init(const AmbWorkFiles *files) {
    /* again only after a shutdown whose worker exited (tests): a worker that outlived the bound may still touch this state */
    if (!files || (g_inited && !(g_shut && g_clean))) return AMB_ERR_ARG;
    static const int dim[3] = { CUBE_X, CUBE_Y, CUBE_Z }, voxel[3] = { CUBE_VOXEL, CUBE_VOXEL, CUBE_VOXEL };
    if (amb_clip_init_dims(&g_clip, dim, voxel, CUBE_STEP)) return AMB_ERR_ARG;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1e3 / (double)f.QuadPart;
    g_files = *files;
    g_stop = g_off = g_failed = g_started = 0;
    g_map = g_loaded = NULL;
    g_first_done = 0;
    g_lookups = g_hits = g_coarse_hits = 0;
    g_dc = NULL;
    g_shut = 0;
    g_key_want = 0;
    g_key_level[0] = 0;
    g_load_want = 0;
    g_last_tick_ms = 0;
    memset(&g_st, 0, sizeof g_st);
    memset(&g_w, 0, sizeof g_w);
    g_inited = 1;
    return AMB_OK;
}

void cube_work_frame(int want, const char *level, const double *cam) {
    if (!g_inited || g_stop || g_off) return;
    want = want && level && level[0] && strlen(level) < MAX_PATH_BYTES;
    if (want != g_key_want || (want && strcmp(level, g_key_level) != 0)) {
        if (want && !g_table) {
            g_stage = calloc(NVOX, sizeof *g_stage);
            g_table = calloc(NVOX, sizeof *g_table);
            if (!g_stage || !g_table) { g_off = 1; photon_log("cubework: out of memory for the tables: off"); return; }
        }
        new_generation(want, level);
        g_key_want = want;
        if (want) {
            snprintf(g_key_level, sizeof g_key_level, "%s", level);
            AcquireSRWLockExclusive(&g_wl);
            snprintf(g_st.level, sizeof g_st.level, "%.63s", level);
            ReleaseSRWLockExclusive(&g_wl);
            photon_log("cubework: generation %ld: %s", (long)g_gen, level);
            if (!g_started && !start_worker()) { g_off = 1; photon_log("cubework: no worker: off"); return; }
        } else {
            g_key_level[0] = 0;
            photon_log("cubework: generation %ld: not in game", (long)g_gen);
        }
    }
    if (want) {
        AcquireSRWLockExclusive(&g_wl);
        if (g_loaded) { g_map = g_loaded; g_loaded = NULL; }
        if (g_map && g_map->cdone && g_dc != g_map) g_dc = g_map;
        if (g_failed) amb_clip_reset(&g_clip);
        double now = now_ms();
        if (g_last_tick_ms > 0) amb_clip_tick(&g_clip, (now - g_last_tick_ms) * 1e-3);
        g_last_tick_ms = now;
        if (g_map && !g_failed && cam) {
            int fl = amb_clip_camera(&g_clip, cam);
            if ((fl & AMB_CLIP_CANCELLED) && g_w.job && !amb_clip_alive(&g_clip, g_w.job)) g_w.cancel = 1;
            if (fl) WakeAllConditionVariable(&g_cv);
        }
        int backlog = amb_clip_backlog(&g_clip);
        if (backlog > g_st.max_backlog) g_st.max_backlog = backlog;
        g_st.lookups = g_lookups;
        g_st.hits = g_hits;
        g_st.coarse_hits = g_coarse_hits;
        ReleaseSRWLockExclusive(&g_wl);
        upload();
    }
    reap();
}

int cube_work_lookup(const float p[3], uint32_t *entry) {
    if (!g_inited || !g_table) return 0;
    g_lookups++;
    int a[3];
    for (int k = 0; k < 3; k++) {
        double v = (double)p[k] / CUBE_VOXEL;
        if (!(fabs(v) < 1e6)) return 0;
        a[k] = (int)floor(v);
    }
    int inside = 1;
    if (CUBE_WORK_MUTANT != 7) {
        inside = g_clip.valid;
        for (int k = 0; k < 3 && inside; k++) if (a[k] < g_clip.vmin[k] || a[k] >= g_clip.vmax[k]) inside = 0;
    }
    if (!inside) {   /* P4c: the coarse level */
        const CubeMap *c = g_dc;
        if (!c) return 0;
        int q[3];
        for (int k = 0; k < 3; k++) {
            q[k] = (int)floor((double)p[k] / c->cvox) - c->cbox.org[k];
            if (q[k] < 0 || q[k] >= c->cbox.dim[k]) return 0;
        }
        *entry = c->coarse[((size_t)q[2] * (size_t)c->cbox.dim[1] + (size_t)q[1]) * (size_t)c->cbox.dim[0] + (size_t)q[0]];
        g_coarse_hits++;
        return 2;
    }
    size_t i = ((size_t)(((a[2] % CUBE_Z) + CUBE_Z) % CUBE_Z) * CUBE_Y + (size_t)(((a[1] % CUBE_Y) + CUBE_Y) % CUBE_Y)) * CUBE_X +
               (size_t)(((a[0] % CUBE_X) + CUBE_X) % CUBE_X);
    *entry = g_table[i];
    g_hits++;
    return 1;
}

int cube_work_sample(const int origin[3], int *n) {
    const CubeMap *m = g_map;   /* written only by this thread */
    if (n) *n = m ? m->c.n : 0;
    if (!m) return -1;
    for (int i = 0; i < m->c.n; i++)
        if (m->c.origin[i][0] == origin[0] && m->c.origin[i][1] == origin[1] && m->c.origin[i][2] == origin[2]) return i;
    return -1;
}

int cube_work_map_ready(void) { return g_map != NULL; }

int cube_work_origin(int i, int o[3]) {
    const CubeMap *m = g_map;
    if (!m || i < 0 || i >= m->c.n) return 0;
    for (int k = 0; k < 3; k++) o[k] = m->c.origin[i][k];
    return 1;
}

/* --- shutdown and reporting ---------------------------------------------------------------------------------------------- */
void cube_work_shutdown(const char *why) {
    if (!g_inited || InterlockedExchange(&g_shut, 1)) return;
    AcquireSRWLockExclusive(&g_wl);
    g_stop = 1;
    g_w.cancel = 1;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_wl);
    int n = g_started;
    HANDLE h = g_w.h;
    DWORD r = n ? WaitForMultipleObjects(1, &h, TRUE, SHUTDOWN_WAIT_MS) : WAIT_OBJECT_0;
    if (n) CloseHandle(h);
    int clean = r == WAIT_OBJECT_0;
    g_clean = clean;
    if (clean) {
        AcquireSRWLockExclusive(&g_wl);
        amb_clip_reset(&g_clip);
        ReleaseSRWLockExclusive(&g_wl);
        map_free(g_map);
        map_free(g_loaded);
        g_map = g_loaded = NULL;
        for (int i = 0; i < MAX_RETIRED; i++) { map_free(g_retired[i].m); g_retired[i].m = NULL; }
        free(g_stage);
        free(g_table);
        g_stage = g_table = NULL;
    }
    photon_log("cubework: worker stopped at %s: %s", why ? why : "?",
               !n ? "never started" : clean ? "exited, module reference released" : "did not exit in time and keeps its module reference");
}

void cube_work_stats(CubeWorkStats *out) {
    AcquireSRWLockShared(&g_wl);
    *out = g_st;
    ReleaseSRWLockShared(&g_wl);
}

void cube_work_summary(const char *why) {
    CubeWorkStats s;
    cube_work_stats(&s);
    photon_log("cubework (%s): %s, %d samples; generations %ld, loads %ld (failed %ld, read %.0f ms, parse %.0f ms, %lld bytes), maps freed %ld; first whole"
               " window in the table %.0f ms after the map change; whole windows %ld (mean %.0f ms, max %.0f ms), slabs %ld (mean %.1f ms, max %.1f ms),"
               " cancelled %ld, at the write %ld, build failures %ld, uploads %ld, staging busy %ld, max backlog %d; lookups %ld, in the box %ld, in the coarse"
               " level %ld (%lld voxels of %d units, %.0f ms); maps inside a GMA %ld (refused %ld), handles left open at a stop %ld", why ? why : "?", s.level[0] ? s.level : "no map", s.ncubes, s.generations, s.loads,
               s.load_failures, s.read_ms, s.parse_ms, s.bytes_read, s.maps_freed, s.first_ms, s.jobs[AMB_JOB_WHOLE],
               s.jobs[AMB_JOB_WHOLE] ? s.build_ms[AMB_JOB_WHOLE] / (double)s.jobs[AMB_JOB_WHOLE] : 0.0, s.build_ms_max[AMB_JOB_WHOLE], s.jobs[AMB_JOB_SLAB],
               s.jobs[AMB_JOB_SLAB] ? s.build_ms[AMB_JOB_SLAB] / (double)s.jobs[AMB_JOB_SLAB] : 0.0, s.build_ms_max[AMB_JOB_SLAB], s.jobs_cancelled,
               s.write_cancelled, s.build_failures, s.uploads, s.stage_busy, s.max_backlog, s.lookups, s.hits, s.coarse_hits, s.coarse_voxels, s.coarse_voxel, s.coarse_ms, s.maps_in_gma, s.gma_refused, s.handles_leaked);
}
