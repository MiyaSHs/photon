/* P3g: see ambwork.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ambwork.h"
#include "log.h"

/* Test-only (scripts/run_ambient_test.sh): each removes one rule of the hand-off, and tests/ambient/worktest.c must then fail.
 * 1 a job writes the atlas before it is the head, 2 workers write without the atlas lock, 3 a retired map is freed without waiting for the workers,
 * 4 a re-upload leaves a written head behind, 5 no second look at the job with the atlas held, 6 a new generation does not cancel bakes, 7 a jump does not,
 * 8 bakes ignore the cancel flag, 9 no second worker, 10 a failed bake leaves the box as it is, 11 the fold uses the tool's leaf rule, 12 a failed second
 * worker is retried, 13 the file is closed after the stop flag, 14 worker 1 bakes behind a whole window, 15 no reference on the filesystem's module. */
#ifndef AMB_WORK_MUTANT
#define AMB_WORK_MUTANT 0
#endif

#define AMB_WORK_MUTANT_HEAD (AMB_WORK_MUTANT == 1)
#ifdef AMB_WORK_TEST_HOOKS
void (*amb_work_test_between_locks)(int worker);
volatile int amb_work_test_fail_bake;
#endif
enum { MAX_WORKERS = 2, MAX_RETIRED = 8, MAX_PATH_BYTES = 260, SHUTDOWN_WAIT_MS = 5000 };

typedef struct Worker {
    HANDLE h;
    int index;
    LONG ack;               /* under g_wl: the generation it last held nothing in */
    unsigned job;           /* under g_wl: the job in hand, 0 none */
    volatile int cancel;    /* written under g_wl; read by a bake within each row (AmbBakeParams.cancel) and by the file loop */
    HMODULE module;
} Worker;

static AmbWorkFiles g_files;
static AmbClip g_clip;
static uint8_t *g_atlas;                 /* the one CPU copy */
static SRWLOCK g_atlas_lock = SRWLOCK_INIT;
static SRWLOCK g_wl = SRWLOCK_INIT;      /* the queue, the generation, the map pointers, worker slots, stats. Order: atlas, then this. */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static int g_inited, g_stop, g_off;
static volatile LONG g_shut;
static LONG g_gen;
static Worker g_w[MAX_WORKERS];
static int g_nworkers;
static double g_qpc_ms;
static AmbWorkStats g_st;

static int g_load_want, g_load_hdr;
static LONG g_load_gen;
static char g_load_path[MAX_PATH_BYTES];
static AmbMap *g_loaded;                 /* parsed, not yet adopted by the draw thread */
static AmbMap *g_map;                    /* in use: written by the draw thread under g_wl, read by workers under g_wl */
static int g_failed;                     /* a bake failed for this map: the volume stays off until the next generation */
static struct { AmbMap *m; LONG stamp; } g_retired[MAX_RETIRED];

/* draw thread only */
static int g_key_want, g_key_hdr, g_failed_seen, g_worker1_failed;
static double g_last_tick_ms;
static char g_key_level[MAX_PATH_BYTES];
static unsigned g_up_id;
static int g_up_part;
static int g_a_valid;
static float g_a_origin[3], g_a[AMB_VOXEL_FLOATS];

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g_qpc_ms;
}

/* --- worker ------------------------------------------------------------------------------------------------------ */
/* The same intended race as AmbBakeParams.cancel: one int, a late read only costs another chunk */
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

static void do_load(Worker *w, const char *path, int hdr, LONG gen) {
    double t0 = now_ms();
    long long size = 0;
    void *pin = AMB_WORK_MUTANT == 15 ? NULL : g_files.pin();
    int gma = 0;
    void *f = g_files.open(path, &gma, &w->cancel);
    int opened = f != NULL;
    int rc = AMB_ERR_FORMAT;
    AmbMap *m = NULL;
    AmbReadStats rs = { 0, 0, 0 };
    LoadCtx lc = { f, 0.0 };
    /* past the shutdown bound the filesystem may be gone: no further call into it after the stop flag. The reader checks the cancel flag, which
     * the stop sets, before each read. */
    if (f && !stopping()) {
        size = g_files.size(f);
        AmbReader r = { &lc, size, load_read, &w->cancel };
        rc = amb_map_load_reader(&r, hdr, &m, &rs);
    }
    int leaked = 0;
    if (f) {
        if (AMB_WORK_MUTANT != 13 && stopping()) leaked = 1;
        else g_files.close(f);
    }
    if (pin) g_files.unpin(pin);
    double t1 = now_ms();
    int cancelled = worker_cancelled(w);
    if (cancelled) { amb_map_free(m); m = NULL; }
    AcquireSRWLockExclusive(&g_wl);
    int current = gen == g_gen && !g_stop;
    if (m && current) { g_loaded = m; m = NULL; rc = AMB_OK; }
    g_st.loads++;
    g_st.read_ms = lc.ms;
    g_st.parse_ms = t1 - t0 - lc.ms;
    g_st.bytes_read = rs.bytes_read;
    g_st.peak_bytes = rs.peak_bytes;
    g_st.handles_leaked += leaked;
    int refused = gma && !f && current && !cancelled;
    if (gma) {
        g_st.maps_in_gma++;
        g_st.gma_refused += refused;
        snprintf(g_st.last_gma_map, sizeof g_st.last_gma_map, "%.63s", path);
    }
    if (current && rc != AMB_OK && !refused) g_st.load_failures++;
    long gma_refused = g_st.gma_refused;
    ReleaseSRWLockExclusive(&g_wl);
    amb_map_free(m);   /* nobody else ever saw it */
    const char *where = gma ? " (inside a GMA)" : "";
    if (!current || cancelled) photon_log("ambvol: load of %s abandoned (a newer map or a stop)", path);
    else if (refused) photon_log("ambvol: %s is inside a GMA and has no checked handle of its own: it stays on the engine cube (GMA maps refused so far: %ld)", path, gma_refused);
    else if (rc != AMB_OK) photon_log("ambvol: %s%s: no volume for this map: %s, %lld of %lld bytes read, load error %d%s", path, where, opened ? "opened" : "cannot open",
                                      rs.bytes_read, size, rc, rc == AMB_ERR_LZMA ? " (an LZMA lump)" : "");
    else photon_log("ambvol: %s loaded%s: %lld of %lld bytes read in %d reads, %.0f ms reading, %.0f ms parsing, %lld bytes held at most, %s ambient", path, where,
                    rs.bytes_read, size, rs.reads, lc.ms, t1 - t0 - lc.ms, rs.peak_bytes, hdr ? "HDR" : "LDR");
    if (!current) return;
    WakeAllConditionVariable(&g_cv);
}

static void note_max(double *m, double v) { if (v > *m) *m = v; }

static void run_job(Worker *w, const AmbClipJob *j, const AmbMap *map) {
    AmbBakeParams p;
    amb_params_default(&p);
    p.voxel_xy = g_clip.voxel[0];
    p.voxel_z = g_clip.voxel[2];
    if (AMB_WORK_MUTANT != 8) p.cancel = &w->cancel;
    size_t voxels = (size_t)j->box.dim[0] * (size_t)j->box.dim[1] * (size_t)j->box.dim[2];
    float *vol = malloc(voxels * AMB_VOXEL_FLOATS * sizeof(float));
    double t0 = now_ms();
    int rc = vol ? amb_bake_region(map, &p, &j->box, vol, NULL) : AMB_ERR_NOMEM;
#ifdef AMB_WORK_TEST_HOOKS
    if (__atomic_exchange_n(&amb_work_test_fail_bake, 0, __ATOMIC_SEQ_CST)) rc = AMB_ERR_NOMEM;
#endif
    double t1 = now_ms(), t2 = t1;
    int wrote = 0;
    if (rc == AMB_OK) {
        /* our turn comes when every job before this one is in the texture */
        AcquireSRWLockExclusive(&g_wl);
        while (!AMB_WORK_MUTANT_HEAD && !g_stop && amb_clip_alive(&g_clip, j->id) && !amb_clip_may_write(&g_clip, j->id))
            SleepConditionVariableSRW(&g_cv, &g_wl, 100, 0);
        ReleaseSRWLockExclusive(&g_wl);
        t2 = now_ms();
#ifdef AMB_WORK_TEST_HOOKS
        if (amb_work_test_between_locks) amb_work_test_between_locks(w->index);
#endif
        if (AMB_WORK_MUTANT != 2) AcquireSRWLockExclusive(&g_atlas_lock);
        /* checked again with the atlas held: a jump between the two locks cancels this job, and its voxels must not land on top of the
         * whole window that replaces it */
        AcquireSRWLockExclusive(&g_wl);
        int go = !g_stop && (AMB_WORK_MUTANT == 5 || (AMB_WORK_MUTANT_HEAD ? amb_clip_alive(&g_clip, j->id) : amb_clip_may_write(&g_clip, j->id)));
        ReleaseSRWLockExclusive(&g_wl);
        if (go) {
            int wr = amb_write_toroidal(vol, &j->box, g_clip.dim, g_atlas, amb_clip_row_bytes(&g_clip), amb_clip_slice_bytes(&g_clip));
            /* a job cancelled during the write is simply forgotten: whatever replaces it rewrites these texels */
            AcquireSRWLockExclusive(&g_wl);
            if (wr == AMB_OK) { amb_clip_written(&g_clip, j->id); wrote = 1; }
            else rc = wr;
            ReleaseSRWLockExclusive(&g_wl);
        }
        if (AMB_WORK_MUTANT != 2) ReleaseSRWLockExclusive(&g_atlas_lock);
    }
    free(vol);
    double t3 = now_ms();
    AcquireSRWLockExclusive(&g_wl);
    if (rc == AMB_OK && wrote) {
        g_st.jobs[j->kind]++;
        g_st.bake_ms[j->kind] += t1 - t0;
        note_max(&g_st.bake_ms_max[j->kind], t1 - t0);
        g_st.head_wait_ms += t2 - t1;
        note_max(&g_st.write_ms_max, t3 - t2);
    } else if (rc == AMB_OK) {
        g_st.write_cancelled++;
    } else if (rc == AMB_ERR_CANCELLED) {
        g_st.jobs_cancelled++;
    } else if (!g_failed && amb_clip_alive(&g_clip, j->id)) {   /* not a job of an older map */
        g_failed = 1;
        photon_log("ambvol: bake of a %d x %d x %d box failed (%d): no volume for this map", j->box.dim[0], j->box.dim[1], j->box.dim[2], rc);
    }
    ReleaseSRWLockExclusive(&g_wl);
}

/* Under g_wl. Slabs behind an unfinished whole window would go with it if it is cancelled, as it is in a flight */
static int second_worker_wanted(void) {
    return amb_clip_backlog(&g_clip) >= 2 && (AMB_WORK_MUTANT == 14 || !amb_clip_whole_pending(&g_clip));
}

static DWORD WINAPI worker_main(LPVOID arg) {
    Worker *w = arg;
    if (g_files.thread_start) g_files.thread_start(w->index);
    AcquireSRWLockExclusive(&g_wl);
    for (;;) {
        w->ack = g_gen;
        w->job = 0;
        w->cancel = g_stop;
        if (g_stop) break;
        if (w->index == 0 && g_load_want) {
            char path[MAX_PATH_BYTES];
            memcpy(path, g_load_path, sizeof path);
            int hdr = g_load_hdr;
            LONG gen = g_load_gen;
            g_load_want = 0;
            ReleaseSRWLockExclusive(&g_wl);
            do_load(w, path, hdr, gen);
            AcquireSRWLockExclusive(&g_wl);
            continue;
        }
        AmbClipJob j;
        const AmbMap *map = g_map;
        if (map && !g_failed && (w->index == 0 || second_worker_wanted()) && amb_clip_take(&g_clip, &j)) {
            w->job = j.id;
            ReleaseSRWLockExclusive(&g_wl);
            run_job(w, &j, map);
            AcquireSRWLockExclusive(&g_wl);
            WakeAllConditionVariable(&g_cv);   /* it may have been the job another worker waits behind */
            continue;
        }
        SleepConditionVariableSRW(&g_cv, &g_wl, 250, 0);
    }
    ReleaseSRWLockExclusive(&g_wl);
    /* drops our module reference and exits without ever returning into this DLL's code */
    FreeLibraryAndExitThread(w->module, 0);
    return 0;
}

static int start_worker(int i) {
    Worker *w = &g_w[i];
    HMODULE self = NULL;   /* a counted reference for the thread (no _UNCHANGED_REFCOUNT flag) */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)worker_main, &self)) {
        photon_log("ambvol: no module reference for a worker (%lu)", (unsigned long)GetLastError());
        return 0;
    }
    AcquireSRWLockExclusive(&g_wl);
    w->index = i;
    w->module = self;
    w->ack = g_gen;
    w->job = 0;
    w->cancel = 0;
    ReleaseSRWLockExclusive(&g_wl);
    HANDLE h = CreateThread(NULL, 0, worker_main, w, 0, NULL);
    if (!h) {
        photon_log("ambvol: CreateThread failed (%lu)", (unsigned long)GetLastError());
        FreeLibrary(self);
        return 0;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    w->h = h;
    g_nworkers = i + 1;
    AcquireSRWLockExclusive(&g_wl);
    g_st.workers_started++;
    ReleaseSRWLockExclusive(&g_wl);
    photon_log("ambvol: worker %d started", i);
    return 1;
}

/* --- generations and map lifetime -------------------------------------------------------------------------------- */
static void retire(AmbMap *m) {
    if (!m) return;
    for (int i = 0; i < MAX_RETIRED; i++)
        if (!g_retired[i].m) { g_retired[i].m = m; g_retired[i].stamp = g_gen; return; }
    /* only with workers stuck through many map changes: a leak, never a free under a reader */
    photon_log("ambvol: retired map table full, a map is leaked");
}

static void new_generation(int want, const char *level, int hdr) {
    AcquireSRWLockExclusive(&g_wl);
    g_gen++;
    g_st.generations++;
    amb_clip_reset(&g_clip);
    retire(g_map);
    retire(g_loaded);
    g_map = g_loaded = NULL;
    g_failed = 0;
    g_load_want = want;
    if (want) {
        snprintf(g_load_path, sizeof g_load_path, "%s", level);
        g_load_hdr = hdr;
        g_load_gen = g_gen;
    }
    for (int i = 0; i < g_nworkers && AMB_WORK_MUTANT != 6; i++) g_w[i].cancel = 1;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_wl);
    g_failed_seen = 0;
    g_up_id = 0;
    g_a_valid = 0;
}

/* Frees what every worker has moved past */
static void reap(void) {
    AmbMap *dead[MAX_RETIRED];
    int n = 0;
    AcquireSRWLockExclusive(&g_wl);
    for (int i = 0; i < MAX_RETIRED; i++) {
        if (!g_retired[i].m) continue;
        int acked = 1;
        for (int k = 0; k < g_nworkers && AMB_WORK_MUTANT != 3; k++) acked &= g_w[k].ack - g_retired[i].stamp >= 0;
        if (acked) { dead[n++] = g_retired[i].m; g_retired[i].m = NULL; g_st.maps_freed++; }
    }
    ReleaseSRWLockExclusive(&g_wl);
    for (int i = 0; i < n; i++) amb_map_free(dead[i]);
}

/* --- draw thread ---------------------------------------------------------------------------------------------------- */
int amb_work_init(const AmbWorkFiles *files, const int dim[3], const int voxel[3], int step) {
    if (voxel[0] != voxel[1] || amb_clip_init_dims(&g_clip, dim, voxel, step)) return AMB_ERR_ARG;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1e3 / (double)f.QuadPart;
    g_files = *files;
    g_stop = g_off = g_failed = g_nworkers = 0;
    g_shut = 0;
    g_key_want = g_failed_seen = g_worker1_failed = 0;
    g_last_tick_ms = 0;
    g_key_level[0] = 0;
    g_up_id = 0;
    g_load_want = 0;
    g_a_valid = 0;
    memset(&g_st, 0, sizeof g_st);
    memset(g_w, 0, sizeof g_w);
    g_inited = 1;
    return AMB_OK;
}

void amb_work_frame(int want, const char *level, int hdr, const double *cam) {
    if (!g_inited || g_stop || g_off) return;
    want = want && level && level[0] && strlen(level) < MAX_PATH_BYTES;
    if (want != g_key_want || (want && (hdr != g_key_hdr || strcmp(level, g_key_level) != 0))) {
        if (want && !g_atlas) {
            g_atlas = calloc(1, amb_clip_atlas_bytes(&g_clip));
            if (!g_atlas) { g_off = 1; photon_log("ambvol: out of memory for the atlas: off"); return; }
        }
        new_generation(want, level, hdr);
        g_key_want = want;
        g_key_hdr = hdr;
        if (want) {
            snprintf(g_key_level, sizeof g_key_level, "%s", level);
            photon_log("ambvol: generation %ld: %s, %s ambient", (long)g_gen, level, hdr ? "HDR" : "LDR");
            if (!g_nworkers && !start_worker(0)) { g_off = 1; photon_log("ambvol: no worker: off"); return; }
        } else {
            g_key_level[0] = 0;
            photon_log("ambvol: generation %ld: not in game", (long)g_gen);
        }
    }
    int backlog = 0, second = 0;
    if (want) {
        AcquireSRWLockExclusive(&g_wl);
        if (g_loaded) {
            g_map = g_loaded;
            g_loaded = NULL;
            g_a_valid = 0;
        }
        if (g_failed && !g_failed_seen) {
            g_failed_seen = 1;
            if (AMB_WORK_MUTANT != 10) amb_clip_reset(&g_clip);
        }
        double now = now_ms();
        if (g_last_tick_ms > 0) amb_clip_tick(&g_clip, (now - g_last_tick_ms) * 1e-3);
        g_last_tick_ms = now;
        if (g_map && !g_failed && cam) {
            int fl = amb_clip_camera(&g_clip, cam);
            if (fl & AMB_CLIP_CANCELLED)
                for (int i = 0; i < g_nworkers; i++)
                    if (AMB_WORK_MUTANT != 7 && g_w[i].job && !amb_clip_alive(&g_clip, g_w[i].job)) g_w[i].cancel = 1;
            if (fl) WakeAllConditionVariable(&g_cv);
        }
        backlog = amb_clip_backlog(&g_clip);
        if (backlog > g_st.max_backlog) g_st.max_backlog = backlog;
        second = second_worker_wanted();
        ReleaseSRWLockExclusive(&g_wl);
    }
    if (second && AMB_WORK_MUTANT != 9 && g_nworkers == 1 && !g_worker1_failed && !start_worker(1)) {
        g_worker1_failed = AMB_WORK_MUTANT != 12;
        photon_log("ambvol: no second worker: staying on one");
    }
    reap();
}

int amb_work_has_data(void) {
    AmbClipJob j;
    AcquireSRWLockExclusive(&g_wl);
    int r = g_clip.valid || amb_clip_upload_ready(&g_clip, &j);
    ReleaseSRWLockExclusive(&g_wl);
    return r;
}

int amb_work_backlog(void) {
    AcquireSRWLockExclusive(&g_wl);
    int r = amb_clip_backlog(&g_clip);
    ReleaseSRWLockExclusive(&g_wl);
    return r;
}

int amb_work_upload(AmbWorkCopyFn copy, void *ctx) {
    if (!g_atlas) return 0;
    if (!TryAcquireSRWLockShared(&g_atlas_lock)) {
        g_st.atlas_busy++;
        return 0;
    }
    AmbClipJob j;
    AcquireSRWLockExclusive(&g_wl);
    int ready = !g_stop && amb_clip_upload_ready(&g_clip, &j);
    if (ready) amb_clip_begin_upload(&g_clip, j.id);   /* shrinks the box before any texel changes */
    ReleaseSRWLockExclusive(&g_wl);
    int did = 0;
    if (ready) {
        if (j.id != g_up_id) { g_up_id = j.id; g_up_part = 0; }
        AmbTexBox b[AMB_CLIP_MAX_BOXES];
        int n = amb_clip_boxes(&g_clip, &j, g_up_part, b), ok = 1;
        for (int i = 0; i < n && ok; i++) ok = copy(ctx, g_atlas, amb_clip_row_bytes(&g_clip), amb_clip_slice_bytes(&g_clip), &b[i]);
        if (ok) {
            did = 1;
            g_st.uploads++;
            if (++g_up_part == amb_clip_parts(&g_clip, &j)) {
                AcquireSRWLockExclusive(&g_wl);
                amb_clip_end_upload(&g_clip, j.id);   /* grows the box: the data is in */
                WakeAllConditionVariable(&g_cv);
                ReleaseSRWLockExclusive(&g_wl);
                g_up_id = 0;
            }
        } else g_st.copy_failures++;   /* the box stays shrunk; the part is tried again next frame */
    }
    ReleaseSRWLockShared(&g_atlas_lock);
    return did;
}

int amb_work_full_upload(AmbWorkCopyFn copy, void *ctx) {
    if (!g_atlas) return 1;
    if (!TryAcquireSRWLockShared(&g_atlas_lock)) {
        g_st.atlas_busy++;
        return 0;
    }
    AmbClipJob j;
    AcquireSRWLockExclusive(&g_wl);
    int head = AMB_WORK_MUTANT != 4 && !g_stop && amb_clip_upload_ready(&g_clip, &j);
    if (head) amb_clip_begin_upload(&g_clip, j.id);
    int needed = head || g_clip.valid;
    ReleaseSRWLockExclusive(&g_wl);
    int ok = 1;
    if (needed) {
        AmbTexBox all;
        amb_clip_all_box(&g_clip, &all);
        ok = copy(ctx, g_atlas, amb_clip_row_bytes(&g_clip), amb_clip_slice_bytes(&g_clip), &all);
        if (ok) g_st.full_uploads++; else g_st.copy_failures++;
    }
    if (ok && head) {
        AcquireSRWLockExclusive(&g_wl);
        amb_clip_end_upload(&g_clip, j.id);
        WakeAllConditionVariable(&g_cv);
        ReleaseSRWLockExclusive(&g_wl);
        g_up_id = 0;
    }
    ReleaseSRWLockShared(&g_atlas_lock);
    return ok;
}

float amb_work_weight(const float origin[3]) { return amb_clip_weight(&g_clip, origin); }

void amb_work_constants(float c[4][4]) {
    amb_clip_constants(&g_clip, c[0], c[1]);
    amb_clip_shader_box(&g_clip, c[2], c[3]);
}

void amb_work_fold(const float origin[3], float f, float cube[6][4]) {
    if (!g_map) return;
    if (g_a_valid && memcmp(origin, g_a_origin, sizeof g_a_origin) == 0) g_st.a_hits++;
    else {
        double p[3] = { origin[0], origin[1], origin[2] };
        if (AMB_WORK_MUTANT == 11) amb_eval_point(g_map, p, 1.0, g_a);
        else amb_eval_engine(g_map, p, 1.0, g_a);
        memcpy(g_a_origin, origin, sizeof g_a_origin);
        g_a_valid = 1;
        g_st.a_evals++;
    }
    amb_clip_fold((const float (*)[4])cube, g_a, f, cube);
}

int amb_work_map_loaded(void) { return g_map != NULL; }

void amb_work_trace_state(char *out, size_t n) {
    AcquireSRWLockExclusive(&g_wl);
    snprintf(out, n, "valid=%d box=%d,%d,%d..%d,%d,%d gain=%.3f wmin=%.0f,%.0f,%.0f wmax=%.0f,%.0f,%.0f hold=%d qn=%d jumps=%ld wholes=%ld", g_clip.valid, g_clip.vmin[0], g_clip.vmin[1], g_clip.vmin[2],
             g_clip.vmax[0], g_clip.vmax[1], g_clip.vmax[2], g_clip.wgain, g_clip.wmin[0], g_clip.wmin[1], g_clip.wmin[2], g_clip.wmax[0], g_clip.wmax[1], g_clip.wmax[2], g_clip.holding, g_clip.qn, g_clip.jumps, g_clip.wholes);
    ReleaseSRWLockExclusive(&g_wl);
}

void amb_work_trace_a(const float origin[3], float a[AMB_VOXEL_FLOATS]) {
    double p[3] = { origin[0], origin[1], origin[2] };
    if (g_map) amb_eval_engine(g_map, p, 1.0, a);
}

/* --- shutdown and reporting ----------------------------------------------------------------------------------------- */
void amb_work_shutdown(const char *why) {
    if (!g_inited || InterlockedExchange(&g_shut, 1)) return;
    AcquireSRWLockExclusive(&g_wl);
    g_stop = 1;
    for (int i = 0; i < g_nworkers; i++) g_w[i].cancel = 1;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_wl);
    int n = g_nworkers;
    HANDLE hs[MAX_WORKERS];
    for (int i = 0; i < n; i++) hs[i] = g_w[i].h;
    DWORD r = n ? WaitForMultipleObjects((DWORD)n, hs, TRUE, SHUTDOWN_WAIT_MS) : WAIT_OBJECT_0;
    for (int i = 0; i < n; i++) CloseHandle(hs[i]);
    int clean = r < WAIT_OBJECT_0 + (DWORD)(n ? n : 1);
    if (clean) {
        /* nothing can read these any more. After a timeout a worker might, so they stay: it holds its module reference anyway */
        AcquireSRWLockExclusive(&g_wl);
        amb_clip_reset(&g_clip);
        ReleaseSRWLockExclusive(&g_wl);
        amb_map_free(g_map);
        amb_map_free(g_loaded);
        g_map = g_loaded = NULL;
        for (int i = 0; i < MAX_RETIRED; i++) { amb_map_free(g_retired[i].m); g_retired[i].m = NULL; }
        free(g_atlas);
        g_atlas = NULL;
    }
    photon_log("ambvol: workers stopped at %s: %d %s", why ? why : "?", n,
               clean ? "exited, module references released" : "did not exit in time and keep their module references");
}

#ifdef AMB_WORK_TEST_HOOKS
void amb_work_test_timing(double hold_seconds, double survive_seconds) {
    g_clip.hold_seconds = hold_seconds;
    g_clip.survive_seconds = survive_seconds;
}
#endif

void amb_work_stats(AmbWorkStats *out) {
    AcquireSRWLockShared(&g_wl);
    *out = g_st;
    ReleaseSRWLockShared(&g_wl);
}

void amb_work_summary(const char *why) {
    AmbWorkStats s;
    amb_work_stats(&s);
    photon_log("ambvol jobs (%s): generations %ld, map loads %ld (failed %ld, read %.0f ms, parse %.0f ms), maps freed %ld, workers %ld, max backlog %ld; whole windows %ld (mean %.0f ms,"
               " max %.0f ms), slabs %ld (mean %.0f ms, max %.0f ms), %.0f ms waiting for the head, longest atlas write %.1f ms; cancelled in a bake %ld, at the write %ld; camera jumps %ld, steps"
               " back coalesced %ld; uploads %ld, full uploads %ld, atlas busy %ld, copy failures %ld; A evals %ld, cache hits %ld; maps inside a GMA %ld (on the cube %ld, last %s), handles left open at a"
               " stop %ld", why ? why : "?", s.generations, s.loads, s.load_failures,
               s.read_ms, s.parse_ms, s.maps_freed, s.workers_started, s.max_backlog, s.jobs[AMB_JOB_WHOLE],
               s.jobs[AMB_JOB_WHOLE] ? s.bake_ms[AMB_JOB_WHOLE] / (double)s.jobs[AMB_JOB_WHOLE] : 0.0, s.bake_ms_max[AMB_JOB_WHOLE], s.jobs[AMB_JOB_SLAB],
               s.jobs[AMB_JOB_SLAB] ? s.bake_ms[AMB_JOB_SLAB] / (double)s.jobs[AMB_JOB_SLAB] : 0.0, s.bake_ms_max[AMB_JOB_SLAB], s.head_wait_ms, s.write_ms_max, s.jobs_cancelled,
               s.write_cancelled, g_clip.jumps, g_clip.coalesced, s.uploads, s.full_uploads, s.atlas_busy, s.copy_failures, s.a_evals, s.a_hits, s.maps_in_gma, s.gma_refused,
               s.last_gma_map[0] ? s.last_gma_map : "none", s.handles_leaked);
}
