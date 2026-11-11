/* P5: see lgwork.h. The structure follows cubework.c on purpose (one worker, generations, the stop contract), so the two can be reviewed
 * against each other; the difference is that the draw thread never reads the map or the grid, only the published image. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lgwork.h"
#include "log.h"

/* Test-only (scripts/run_ambient_test.sh): each removes one rule, and tests/ambient/lgworktest.c must then fail.
 * 1 a publish of an older generation is uploaded, 2 the publish is written without its lock, 3 the dirty rows are cleared before the upload,
 * 4 a new generation does not cancel a load, 5 the file is closed after the stop flag, 6 no reference on the filesystem's module, 7 the fold rows are
 * not copied with the texture's, 8 a changed fold table is not published, 9 a cached fold ignores a change of one of its lights,
 * 10 a sun map of an older generation is handed out, 11 the draw thread keeps the old map's sun after a generation change */
#ifndef LG_WORK_MUTANT
#define LG_WORK_MUTANT 0
#endif

enum { MAX_PATH_BYTES = 260, IMAGE_FLOATS = LG_TEX_W * LG_TEX_H * 4, ROW_FLOATS = LG_TEX_W * 4, FOLD_U16 = LG_FOLD_ROWS * LG_FOLD_ROW,
       FTAB_MAX = LG_NSTATIC_MAX + LG_NDYN };
#ifdef LG_WORK_TEST_HOOKS
DWORD lg_work_test_wait_ms = 5000;
DWORD lg_work_test_sun_delay_ms;   /* a pause before the sun is published, so a map change can overtake it */
#define SHUTDOWN_WAIT_MS lg_work_test_wait_ms
#else
#define SHUTDOWN_WAIT_MS 5000u
#endif

typedef struct Worker {
    HANDLE h;
    volatile int cancel;    /* written under g_wl; read by a load within each read */
    HMODULE module;
} Worker;

static AmbWorkFiles g_files;
static LgParams g_params;
static SRWLOCK g_wl = SRWLOCK_INIT;      /* generation, load request, snapshot slot, stats */
static SRWLOCK g_pub = SRWLOCK_INIT;     /* the published image: the worker writes it exclusive; only the draw thread ever takes it shared */
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static int g_inited, g_stop, g_off, g_started, g_clean = 1;
static volatile LONG g_shut;
static LONG g_gen;
static Worker g_w;
static double g_qpc_ms;
static LgWorkStats g_st;
static float g_times[LG_WORK_TIMES];
static long g_ntimes;

/* request (under g_wl) */
static int g_load_want, g_load_hdr;
static LONG g_load_gen;
static char g_load_path[MAX_PATH_BYTES];
static LgSnapshot g_snap;
static long g_snap_seq;

/* worker only */
static LgMap *g_map;
static LgGrid *g_grid;
static LONG g_grid_gen;
static long g_done_seq, g_build_seq;
static LgSnapshot g_wsnap;

/* published (under g_pub) */
static float *g_pimg;
static uint8_t g_pdirty[LG_TEX_H];
static float g_pc63[4];
static LONG g_pgen = -1;
static long g_pseq, g_pub_snapseq;     /* g_pub_snapseq: the snapshot the publish was built from */
static uint16_t *g_pfold;              /* P5b: the fold image (its dirty rows are g_pdirty's below LG_FOLD_ROWS) */
static LgFoldLight *g_pftab;           /* the fold table, FTAB_MAX entries of which g_pftab_n are this grid's */
static int g_pftab_n, g_pnstatic;
static long g_pftab_ver;               /* bumped with every table publish */
static long g_pub_fseq = -1;           /* worker only: the grid's table sequence last published */

/* P6 sun (analysis/22): the map's SunMap, loaded with the map when lg_work_want_sun(1) was called. The worker publishes a new one in g_psun
 * under g_pub (freeing one the draw thread never took); the draw thread takes it with a try-lock, owns it from then on (it alone reads it), and
 * frees it when the generation changes or at a clean shutdown. g_dsun_serial changes whenever the map lg_work_sun returns changes. */
static int g_sun_on;
static SunMap *g_psun;                 /* under g_pub */
static LONG g_psun_gen;
static volatile LONG g_psun_ready;
static SunMap *g_dsun;                 /* draw thread only */
static LONG g_dsun_gen = -1;
static long g_dsun_serial;

/* L1 lamps: built by the worker after each grid build, published under g_pub, copied by the draw thread */
static LgLamp g_plamps[LG_LAMPS_MAX];  /* under g_pub */
static int g_plamps_n;
static LONG g_plamps_gen = -1;
static long g_plamps_seq;
static LgLamp g_dlamps[LG_LAMPS_MAX];  /* draw thread only */
static int g_dlamps_n;
static long g_dlamps_seq, g_dlamps_taken = -1;
static LONG g_dlamps_gen = -1;

/* draw thread only */
static int g_key_want, g_key_hdr;
static char g_key_level[MAX_PATH_BYTES];
static LONG g_up_gen = -1;
static long g_up_seq = -1, g_up_snapseq = -1;
static float g_up_c63[4];
static int g_tex_lost;
static uint16_t *g_dfold;              /* P5b: the draw thread's fold copy */
static LgFoldLight *g_dftab;
static int g_dftab_n, g_dnstatic;
static long g_dftab_ver = -1, g_dver;  /* g_dver: bumped whenever the copy changes */
/* The fold cache: direct-mapped by origin. An entry made at copy version ver stands while its window, its cell's row and every light its cell lists
 * are no newer than ver, so one light changing (a flickering style, a moving dynamic light) re-evaluates only the draws that list it. */
enum { FOLD_CACHE = 256 };
typedef struct { long ver; int cell; float origin[3], c63[3], out[6][3]; } FoldCacheEntry;
static FoldCacheEntry g_fc[FOLD_CACHE];   /* ver -1: empty */
static long g_row_ver[LG_FOLD_ROWS], g_ent_ver[FTAB_MAX];   /* the copy version each fold row and table entry last changed at */
static volatile long g_st_fold_evals, g_st_fold_hits;

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g_qpc_ms;
}

/* --- worker ------------------------------------------------------------------------------------------------------- */
/* The same intended race as AmbPickParams.cancel (tests/ambient/worktest.supp matches the name) */
static __attribute__((noinline)) int worker_cancelled(const Worker *w) { return w->cancel; }

static int stopping(void) {
    AcquireSRWLockExclusive(&g_wl);
    int s = g_stop;
    ReleaseSRWLockExclusive(&g_wl);
    return s;
}

static void drop_grid(void) {
    lg_grid_free(g_grid);
    lg_map_free(g_map);
    g_grid = NULL;
    g_map = NULL;
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
    drop_grid();
    double t0 = now_ms();
    long long size = 0;
    void *pin = LG_WORK_MUTANT == 6 ? NULL : g_files.pin();
    int gma = 0;
    void *f = g_files.open(path, &gma, &w->cancel);
    int opened = f != NULL, rc = AMB_ERR_FORMAT;
    LgMap *m = NULL;
    LgGrid *g = NULL;
    SunMap *sm = NULL;
    int src = AMB_ERR_ARG;
    AmbReadStats rs = { 0, 0, 0 };
    LoadCtx lc = { f, 0.0 };
    if (f && !stopping()) {
        size = g_files.size(f);
        AmbReader r = { &lc, size, load_read, &w->cancel };
        rc = lg_map_load_reader(&r, hdr, &m, &rs);
        LgParams p = g_params;
        p.cancel = &w->cancel;   /* the split pass (P5b) stops within one bucket */
        if (rc == AMB_OK) rc = lg_grid_new(m, &p, &g);
        if (rc == AMB_OK && g_sun_on && !stopping()) src = sun_map_load_reader(&r, hdr, &sm, NULL);   /* the same reader: GMA maps too */
    }
    int leaked = 0;
    if (f) {
        if (LG_WORK_MUTANT != 5 && stopping()) leaked = 1;
        else g_files.close(f);
    }
    if (pin) g_files.unpin(pin);
    double t1 = now_ms();
    int cancelled = worker_cancelled(w);
    if (cancelled || rc != AMB_OK) { lg_grid_free(g); lg_map_free(m); g = NULL; m = NULL; }
    if (cancelled || rc != AMB_OK || src != AMB_OK) { sun_map_free(sm); sm = NULL; }
    AcquireSRWLockExclusive(&g_wl);
    int current = gen == g_gen && !g_stop;
    if (g && current) { g_map = m; g_grid = g; g_grid_gen = gen; m = NULL; g = NULL; }
    g_st.loads++;
    g_st.read_ms = lc.ms;
    g_st.parse_ms = t1 - t0 - lc.ms;
    g_st.handles_leaked += leaked;
    g_st.loads_cancelled += rc == AMB_ERR_CANCELLED;
    int refused = gma && !f && current && !cancelled;
    if (gma) { g_st.maps_in_gma++; g_st.gma_refused += refused; }
    if (current && rc != AMB_OK && !refused && !cancelled) g_st.load_failures++;
    if (current && g_grid) {
        LgMapInfo mi;
        lg_map_info(g_map, &mi);
        g_st.nlights = mi.nlights;
        g_st.nstatic = lg_grid_static_count(g_grid);
    }
    if (g_sun_on && current && !cancelled && rc == AMB_OK) { g_st.sun_loads++; g_st.sun_failures += src != AMB_OK; }
    int nl = g_st.nlights, ns = g_st.nstatic;
    ReleaseSRWLockExclusive(&g_wl);
    lg_grid_free(g);   /* nobody else ever saw them */
    lg_map_free(m);
#ifdef LG_WORK_TEST_HOOKS
    if (sm && lg_work_test_sun_delay_ms) Sleep(lg_work_test_sun_delay_ms);
#endif
    if (sm && current) {   /* hand the sun to the draw thread (a map change may still overtake it: lg_work_sun checks the generation) */
        SunInfo si;
        sun_map_info(sm, &si);
        AcquireSRWLockExclusive(&g_pub);
        sun_map_free(g_psun);   /* one the draw thread never took */
        g_psun = sm;
        g_psun_gen = gen;
        InterlockedExchange(&g_psun_ready, 1);
        ReleaseSRWLockExclusive(&g_pub);
        sm = NULL;
        if (si.has_sun) photon_log("lgwork: sun for P6: direction %.4f %.4f %.4f, intensity %.3f %.3f %.3f, style %d; %d brushes, %d displacements",
                                   si.sun_dir[0], si.sun_dir[1], si.sun_dir[2], si.sun_intensity[0], si.sun_intensity[1], si.sun_intensity[2], si.sun_style,
                                   si.nbrushes, si.ndisps);
        else photon_log("lgwork: this map has no emit_skylight: P6 draws get no sun");
    } else if (g_sun_on && src != AMB_OK && rc == AMB_OK && current && !cancelled) photon_log("lgwork: no sun map for P6 (error %d)", src);
    sun_map_free(sm);
    if (!current || cancelled) photon_log("lgwork: load of %s abandoned (a newer map or a stop)", path);
    else if (refused) photon_log("lgwork: %s is inside a GMA and has no checked handle of its own: no light grid for it", path);
    else if (rc != AMB_OK) photon_log("lgwork: %s: no light grid for this map: %s, %lld of %lld bytes read, error %d", path, opened ? "opened" : "cannot open",
                                      rs.bytes_read, size, rc);
    else photon_log("lgwork: %s loaded%s: %d worldlights (lump %d), %d grid lights, %lld of %lld bytes read, %.0f ms reading, %.0f ms parsing", path,
                    gma ? " (inside a GMA)" : "", nl, hdr ? 54 : 15, ns, rs.bytes_read, size, lc.ms, t1 - t0 - lc.ms);
}

/* worker: the lamps for this snapshot's camera (analysis/35 step 1). Nearest first; a lamp counts when its sphere of reach comes within
 * LG_LAMP_RANGE of the camera and the camera's cluster sees its cluster. */
static int build_lamps(LgLamp *out) {
    int nw = 0, n = 0;
    const LgWorldLight *wl = lg_map_lights(g_map, &nw);
    const double *cam = g_wsnap.cam;
    int cc = lg_point_cluster(g_map, cam);
    float dist[LG_LAMPS_MAX];
    for (int i = 0; i < nw; i++) {
        const LgWorldLight *w = &wl[i];
        if (w->type != LG_EMIT_POINT && w->type != LG_EMIT_SPOT) continue;
        float sc = lg_style_scale(g_wsnap.style, w->style);
        if (!(sc > 0)) continue;
        double reach = lg_window_radius(w, g_params.cutoff);
        if (!(reach > 0)) continue;
        double dx = w->origin[0] - cam[0], dy = w->origin[1] - cam[1], dz = w->origin[2] - cam[2];
        double d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d - reach > LG_LAMP_RANGE) continue;
        if (cc >= 0 && w->cluster >= 0 && !lg_cluster_sees(g_map, cc, w->cluster)) continue;
        int at = n < LG_LAMPS_MAX ? n : LG_LAMPS_MAX;   /* insertion by distance, the farthest falls off the end */
        while (at > 0 && dist[at - 1] > d) at--;
        if (at >= LG_LAMPS_MAX) continue;
        int last = n < LG_LAMPS_MAX ? n : LG_LAMPS_MAX - 1;
        memmove(&out[at + 1], &out[at], (size_t)(last - at) * sizeof *out);
        memmove(&dist[at + 1], &dist[at], (size_t)(last - at) * sizeof *dist);
        LgLamp *l = &out[at];
        for (int k = 0; k < 3; k++) { l->origin[k] = w->origin[k]; l->color[k] = w->intensity[k] * sc; l->dir[k] = w->normal[k]; }
        l->stopdot = w->stopdot; l->stopdot2 = w->stopdot2; l->exponent = w->exponent; l->reach = (float)reach;
        l->att[0] = w->constant_attn; l->att[1] = w->linear_attn; l->att[2] = w->quadratic_attn;
        if (l->att[0] == 0 && l->att[1] == 0 && l->att[2] == 0) l->att[0] = 1;
        l->type = w->type; l->index = i;
        dist[at] = (float)d;
        if (n < LG_LAMPS_MAX) n++;
    }
    return n;
}

static void build_and_publish(void) {
    double t0 = now_ms();
    LgStats st;
    int rc = lg_grid_frame(g_grid, &g_wsnap, &st);
    double t1 = now_ms();
    st.ms = t1 - t0;
    uint8_t rows[LG_TEX_H];
    int n = rc == AMB_OK ? lg_grid_take_dirty(g_grid, rows) : 0;
    if (rc == AMB_OK) {
        if (LG_WORK_MUTANT != 2) AcquireSRWLockExclusive(&g_pub);
        int fresh = g_pgen != g_grid_gen;
        if (fresh) { memset(g_pdirty, 1, sizeof g_pdirty); g_pgen = g_grid_gen; }
        const float *img = lg_grid_image(g_grid);
        const uint16_t *fold = lg_grid_fold_image(g_grid);
        for (int r = 0; r < LG_TEX_H; r++)
            if (rows[r] || fresh) {
                memcpy(g_pimg + (size_t)r * ROW_FLOATS, img + (size_t)r * ROW_FLOATS, ROW_FLOATS * sizeof(float));
                if (r < LG_FOLD_ROWS) memcpy(g_pfold + (size_t)r * LG_FOLD_ROW, fold + (size_t)r * LG_FOLD_ROW, LG_FOLD_ROW * sizeof *fold);
                g_pdirty[r] = 1;
            }
        int ftn;
        long fseq;
        const LgFoldLight *ft = lg_grid_fold_table(g_grid, &ftn, &fseq);
        if (ftn > FTAB_MAX) ftn = FTAB_MAX;
        if ((fresh || fseq != g_pub_fseq) && (LG_WORK_MUTANT != 8 || fresh)) {
            memcpy(g_pftab, ft, (size_t)ftn * sizeof *ft);
            g_pftab_n = ftn;
            g_pnstatic = lg_grid_static_count(g_grid);
            g_pftab_ver++;
            g_pub_fseq = fseq;
        }
        lg_grid_constants(g_grid, g_pc63);
        g_plamps_n = build_lamps(g_plamps);
        g_plamps_gen = g_grid_gen;
        g_plamps_seq++;
        g_pseq++;
        g_pub_snapseq = g_build_seq;
        if (LG_WORK_MUTANT != 2) ReleaseSRWLockExclusive(&g_pub);
    }
    AcquireSRWLockExclusive(&g_wl);
    if (rc == AMB_OK) {
        g_st.builds++;
        g_st.publishes++;
        g_st.build_ms_sum += st.ms;
        if (st.ms > g_st.build_ms_max) g_st.build_ms_max = st.ms;
        g_st.last = st;
        g_times[g_ntimes++ % LG_WORK_TIMES] = (float)st.ms;
    } else g_st.build_failures++;
    ReleaseSRWLockExclusive(&g_wl);
    (void)n;
}

static DWORD WINAPI worker_main(LPVOID arg) {
    Worker *w = arg;
    if (g_files.thread_start) g_files.thread_start(3);   /* 0 and 1 the ambient workers, 2 the pick table */
    AcquireSRWLockExclusive(&g_wl);
    for (;;) {
        w->cancel = g_stop;
        if (g_stop) break;
        if (g_load_want) {
            char path[MAX_PATH_BYTES];
            memcpy(path, g_load_path, sizeof path);
            LONG gen = g_load_gen;
            int hdr = g_load_hdr;
            g_load_want = 0;
            ReleaseSRWLockExclusive(&g_wl);
            if (path[0]) do_load(w, path, hdr, gen);
            else drop_grid();
            AcquireSRWLockExclusive(&g_wl);
            continue;
        }
        if (g_grid && g_grid_gen != g_gen) {   /* a generation with nothing to load: the grid goes */
            ReleaseSRWLockExclusive(&g_wl);
            drop_grid();
            AcquireSRWLockExclusive(&g_wl);
            continue;
        }
        if (g_grid && g_snap_seq != g_done_seq) {
            memcpy(&g_wsnap, &g_snap, sizeof g_wsnap);
            g_done_seq = g_snap_seq;
            g_build_seq = g_snap_seq;
            ReleaseSRWLockExclusive(&g_wl);
            build_and_publish();
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
        photon_log("lgwork: no module reference for the worker (%lu)", (unsigned long)GetLastError());
        return 0;
    }
    AcquireSRWLockExclusive(&g_wl);
    g_w.module = self;
    g_w.cancel = 0;
    ReleaseSRWLockExclusive(&g_wl);
    HANDLE h = CreateThread(NULL, 0, worker_main, &g_w, 0, NULL);
    if (!h) {
        photon_log("lgwork: CreateThread failed (%lu)", (unsigned long)GetLastError());
        FreeLibrary(self);
        return 0;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    g_w.h = h;
    g_started = 1;
    photon_log("lgwork: worker started");
    return 1;
}

/* --- draw thread ------------------------------------------------------------------------------------------------- */
int lg_work_init(const AmbWorkFiles *files, const LgParams *p) {
    if (!files || !p || (g_inited && !(g_shut && g_clean))) return AMB_ERR_ARG;
    if (!(p->cell >= 1) || p->k < 1 || p->k > LG_KMAX) return AMB_ERR_ARG;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1e3 / (double)f.QuadPart;
    g_files = *files;
    g_params = *p;
    g_stop = g_off = g_started = 0;
    g_shut = 0;
    g_key_want = 0;
    g_key_level[0] = 0;
    g_load_want = 0;
    g_snap_seq = g_done_seq = 0;
    g_pgen = g_up_gen = -1;
    g_pseq = 0;
    g_up_seq = -1;
    g_pftab_ver = 0;
    g_pub_fseq = -1;
    g_dftab_ver = -1;
    g_dver = 0;
    for (int i = 0; i < FOLD_CACHE; i++) g_fc[i].ver = -1;
    memset(g_row_ver, 0, sizeof g_row_ver);
    memset(g_ent_ver, 0, sizeof g_ent_ver);
    g_ntimes = 0;
    g_map = NULL;
    g_psun = NULL;
    g_psun_ready = 0;
    g_dsun = NULL;
    g_grid = NULL;
    memset(&g_st, 0, sizeof g_st);
    memset(&g_w, 0, sizeof g_w);
    g_inited = 1;
    return AMB_OK;
}

static void new_generation(int want, const char *level, int hdr) {
    AcquireSRWLockExclusive(&g_wl);
    g_gen++;
    g_st.generations++;
    g_load_want = 1;
    g_load_hdr = hdr;
    snprintf(g_load_path, sizeof g_load_path, "%s", want ? level : "");
    g_load_gen = g_gen;
    if (g_started && LG_WORK_MUTANT != 4) g_w.cancel = 1;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_wl);
}

void lg_work_frame(int want, const char *level, int hdr, const LgSnapshot *snap) {
    if (!g_inited || g_stop || g_off) return;
    want = want && level && level[0] && strlen(level) < MAX_PATH_BYTES;
    if (want != g_key_want || (want && (strcmp(level, g_key_level) != 0 || hdr != g_key_hdr))) {
        if (want && !g_pimg) {
            g_pimg = calloc(IMAGE_FLOATS, sizeof *g_pimg);
            g_pfold = calloc(FOLD_U16, sizeof *g_pfold);
            g_dfold = calloc(FOLD_U16, sizeof *g_dfold);
            g_pftab = calloc(FTAB_MAX, sizeof *g_pftab);
            g_dftab = calloc(FTAB_MAX, sizeof *g_dftab);
            if (!g_pimg || !g_pfold || !g_dfold || !g_pftab || !g_dftab) {
                free(g_pimg); free(g_pfold); free(g_dfold); free(g_pftab); free(g_dftab);
                g_pimg = NULL; g_pfold = g_dfold = NULL; g_pftab = g_dftab = NULL;
                g_off = 1;
                photon_log("lgwork: out of memory for the image: off");
                return;
            }
        }
        new_generation(want, level, hdr);
        if (g_dsun && LG_WORK_MUTANT != 11) { sun_map_free(g_dsun); g_dsun = NULL; g_dsun_serial++; }   /* only this thread ever read it */
        g_key_want = want;
        g_key_hdr = hdr;
        if (want) {
            snprintf(g_key_level, sizeof g_key_level, "%s", level);
            AcquireSRWLockExclusive(&g_wl);
            snprintf(g_st.level, sizeof g_st.level, "%.63s", level);
            ReleaseSRWLockExclusive(&g_wl);
            photon_log("lgwork: generation %ld: %s (%s lights)", (long)g_gen, level, hdr ? "HDR" : "LDR");
            if (!g_started && !start_worker()) { g_off = 1; photon_log("lgwork: no worker: off"); return; }
        } else {
            g_key_level[0] = 0;
            photon_log("lgwork: generation %ld: not in game", (long)g_gen);
        }
    }
    if (want && snap) {
        AcquireSRWLockExclusive(&g_wl);
        if (g_snap_seq != g_done_seq) g_st.snapshots_skipped++;
        memcpy(&g_snap, snap, sizeof g_snap);
        g_snap_seq++;
        g_st.snapshots++;
        WakeAllConditionVariable(&g_cv);
        ReleaseSRWLockExclusive(&g_wl);
    }
}

int lg_work_upload(LgWorkCopyFn copy, void *ctx, float c63[4]) {
    if (!g_inited || !g_pimg || !g_key_want) return 0;
    if (!TryAcquireSRWLockShared(&g_pub)) {
        AcquireSRWLockExclusive(&g_wl);
        g_st.upload_busy++;
        ReleaseSRWLockExclusive(&g_wl);
        return g_up_gen == g_gen && !g_tex_lost ? (memcpy(c63, g_up_c63, sizeof g_up_c63), 1) : 0;
    }
    LONG gen = g_gen;   /* written by this thread only */
    int ok = 0, did = 0, failed = 0, folds = 0;
    if ((g_pgen == gen || LG_WORK_MUTANT == 1) && (g_pseq != g_up_seq || g_tex_lost)) {
        uint8_t rows[LG_TEX_H];
        if (LG_WORK_MUTANT == 3) memset(g_pdirty, 0, sizeof g_pdirty);
        for (int r = 0; r < LG_TEX_H; r++) rows[r] = g_tex_lost ? 1 : g_pdirty[r];
        if (copy(ctx, g_pimg, rows)) {
            int fc = 0;   /* P5b: the fold rows and table go with the texture, so a draw's fold matches the grid it reads */
            long v = g_dver + 1;   /* the version this copy makes, if it changes anything */
            for (int r = 0; r < LG_FOLD_ROWS && LG_WORK_MUTANT != 7; r++)
                if (rows[r]) {
                    memcpy(g_dfold + (size_t)r * LG_FOLD_ROW, g_pfold + (size_t)r * LG_FOLD_ROW, LG_FOLD_ROW * sizeof *g_dfold);
                    g_row_ver[r] = v;
                    fc = 1;
                }
            if (g_dftab_ver != g_pftab_ver) {
                int all = g_pftab_n != g_dftab_n || g_pnstatic != g_dnstatic;
                for (int e = 0; e < g_pftab_n; e++)
                    if (all || memcmp(&g_dftab[e], &g_pftab[e], sizeof g_dftab[e]) != 0) { g_dftab[e] = g_pftab[e]; g_ent_ver[e] = v; }
                g_dftab_n = g_pftab_n;
                g_dnstatic = g_pnstatic;
                g_dftab_ver = g_pftab_ver;
                fc = 1;
            }
            g_dver += fc;
            folds += fc;
            memset(g_pdirty, 0, sizeof g_pdirty);   /* only this thread holds g_pub shared: the write cannot race */
            g_up_seq = g_pseq;
            g_up_gen = gen;
            g_up_snapseq = g_pub_snapseq;
            memcpy(g_up_c63, g_pc63, sizeof g_up_c63);
            g_tex_lost = 0;
            did = 1;
        } else failed = 1;
    }
    ok = g_up_gen == gen && !g_tex_lost;
    if (ok) memcpy(c63, g_up_c63, sizeof g_up_c63);
    ReleaseSRWLockShared(&g_pub);
    AcquireSRWLockExclusive(&g_wl);
    g_st.uploads += did;
    g_st.upload_failures += failed;
    g_st.fold_copies += folds;
    if (ok) { if (g_up_snapseq == g_snap_seq) g_st.frames_fresh++; else g_st.frames_stale++; }   /* g_snap_seq: written by this thread */
    ReleaseSRWLockExclusive(&g_wl);
    return ok;
}

void lg_work_texture_lost(void) { g_tex_lost = 1; }

int lg_work_lamps(const LgLamp **out, long *seq) {
    *out = g_dlamps;
    if (!g_inited || g_shut || !g_key_want) { g_dlamps_n = 0; if (seq) *seq = g_dlamps_seq; return 0; }
    if (TryAcquireSRWLockExclusive(&g_pub)) {
        if (g_plamps_seq != g_dlamps_taken) {
            if (g_plamps_gen == g_gen) { memcpy(g_dlamps, g_plamps, (size_t)g_plamps_n * sizeof *g_dlamps); g_dlamps_n = g_plamps_n; g_dlamps_gen = g_gen; }
            else g_dlamps_n = 0;   /* built for a map the generation has moved past */
            g_dlamps_taken = g_plamps_seq;
            g_dlamps_seq++;
        }
        ReleaseSRWLockExclusive(&g_pub);
    }
    if (g_dlamps_gen != g_gen) g_dlamps_n = 0;   /* the copy is of a map the generation has moved past */
    if (seq) *seq = g_dlamps_seq;
    return g_dlamps_n;
}

void lg_work_want_sun(int on) { g_sun_on = on != 0; }

const SunMap *lg_work_sun(long *serial) {
    if (g_inited && g_sun_on && !g_shut && InterlockedCompareExchange(&g_psun_ready, 0, 0) && TryAcquireSRWLockExclusive(&g_pub)) {
        if (g_psun) {
            if (g_psun_gen == g_gen || LG_WORK_MUTANT == 10) {
                sun_map_free(g_dsun);
                g_dsun = g_psun;
                g_dsun_gen = g_psun_gen;
                g_dsun_serial++;
            } else sun_map_free(g_psun);   /* a map the generation has moved past */
            g_psun = NULL;
        }
        InterlockedExchange(&g_psun_ready, 0);
        ReleaseSRWLockExclusive(&g_pub);
    }
    if (serial) *serial = g_dsun ? g_dsun_serial : 0;
    return g_dsun;
}

int lg_work_fold(const float origin[3], float out[6][3]) {
    memset(out, 0, 18 * sizeof(float));
    if (!g_inited || !g_dfold || !g_key_want || g_up_gen != g_gen || g_tex_lost || g_dftab_ver < 0) return 0;
    uint32_t h;
    {
        uint32_t u[3];
        memcpy(u, origin, sizeof u);
        h = (u[0] * 0x9e3779b1u ^ u[1] * 0x85ebca77u ^ u[2] * 0xc2b2ae3du) >> 24;
    }
    FoldCacheEntry *ce = &g_fc[h % FOLD_CACHE];
    if (ce->ver >= 0 && memcmp(origin, ce->origin, sizeof ce->origin) == 0 && memcmp(g_up_c63, ce->c63, sizeof ce->c63) == 0) {
        int valid = 1;
        if (ce->cell >= 0) {
            const uint16_t *cell = g_dfold + (size_t)(ce->cell / LG_GX) * LG_FOLD_ROW + (size_t)(ce->cell % LG_GX) * LG_FOLD_STRIDE;
            valid = g_row_ver[ce->cell / LG_GX] <= ce->ver;
            for (int j = 0; j < cell[0] && j < LG_FOLD_MAX && valid && LG_WORK_MUTANT != 9; j++) {
                unsigned e = cell[2 + j];
                int idx = (e & LG_FOLD_DYN) ? g_dnstatic + (int)(e & 0x7fff) : (int)(e & 0x3fff);
                valid = idx >= FTAB_MAX || g_ent_ver[idx] <= ce->ver;
            }
        }
        if (valid) {
            memcpy(out, ce->out, sizeof ce->out);
            g_st_fold_hits++;
            return 1;
        }
    }
    float w = g_up_c63[3];
    int ci = -1;
    int key[3], in = 1;
    for (int k = 0; k < 3 && in; k++) {
        float c = floorf(origin[k] * w);
        if (!(c > -1e7f && c < 1e7f)) { in = 0; break; }
        key[k] = (int)c;
        int rel = key[k] - (int)g_up_c63[k], dim = k == 2 ? LG_GZ : k == 1 ? LG_GY : LG_GX;
        in = rel >= 0 && rel < dim;
    }
    if (in) {
        int i = ((key[0] % LG_GX + LG_GX) % LG_GX) + LG_GX * ((key[1] % LG_GY + LG_GY) % LG_GY) + LG_GX * LG_GY * ((key[2] % LG_GZ + LG_GZ) % LG_GZ);
        const uint16_t *cell = g_dfold + (size_t)(i / LG_GX) * LG_FOLD_ROW + (size_t)(i % LG_GX) * LG_FOLD_STRIDE;
        lg_fold_eval(g_dftab, g_dftab_n, g_dnstatic, cell, origin, out);
        ci = i;
    }
    memcpy(ce->origin, origin, sizeof ce->origin);
    memcpy(ce->c63, g_up_c63, sizeof ce->c63);
    memcpy(ce->out, out, sizeof ce->out);
    ce->cell = ci;
    ce->ver = g_dver;
    g_st_fold_evals++;
    return 1;
}

/* --- shutdown and reporting -------------------------------------------------------------------------------------------- */
void lg_work_shutdown(const char *why) {
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
        drop_grid();
        sun_map_free(g_psun);
        g_psun = NULL;
        g_psun_ready = 0;
        if (g_dsun) { sun_map_free(g_dsun); g_dsun = NULL; g_dsun_serial++; }
        free(g_pimg); free(g_pfold); free(g_dfold); free(g_pftab); free(g_dftab);
        g_pimg = NULL; g_pfold = g_dfold = NULL; g_pftab = g_dftab = NULL;
    }
    photon_log("lgwork: worker stopped at %s: %s", why ? why : "?",
               !n ? "never started" : clean ? "exited, module reference released" : "did not exit in time and keeps its module reference");
}

void lg_work_stats(LgWorkStats *out) {
    AcquireSRWLockShared(&g_wl);
    *out = g_st;
    ReleaseSRWLockShared(&g_wl);
    out->fold_evals = g_st_fold_evals;   /* the draw thread's own counters (reporting only) */
    out->fold_hits = g_st_fold_hits;
}

int lg_work_times(float *out, int cap) {
    AcquireSRWLockShared(&g_wl);
    long n = g_ntimes < LG_WORK_TIMES ? g_ntimes : LG_WORK_TIMES;
    if (n > cap) n = cap;
    memcpy(out, g_times, (size_t)n * sizeof *out);
    ReleaseSRWLockShared(&g_wl);
    return (int)n;
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

void lg_work_summary(const char *why) {
    LgWorkStats s;
    lg_work_stats(&s);
    static float t[LG_WORK_TIMES];
    int n = lg_work_times(t, LG_WORK_TIMES);
    qsort(t, (size_t)n, sizeof *t, cmpf);
    photon_log("lgwork (%s): %s, %d worldlights, %d grid lights; generations %ld, loads %ld (failed %ld, read %.0f ms, parse %.0f ms); snapshots %ld"
               " (skipped %ld), builds %ld (failed %ld; last %d: p50 %.3f ms, p99 %.3f ms, max %.3f ms over all %.3f), publishes %ld, uploads %ld (busy %ld,"
               " failed %ld); frames with the latest snapshot's grid %ld, with an older one %ld (%.2f%%); last frame: %d dynamic, %d pending, overflow %d"
               " cells / %d lights, %d static records, record overflow %d, %d dirty rows; maps inside a GMA %ld (refused %ld), handles left open at a stop %ld;"
               " P5b: %d static lights split, fold %d cells / %d entries, lost %d, fold table seq %ld; fold copies %ld, evaluations %ld, cache hits %ld",
               why ? why : "?", s.level[0] ? s.level : "no map", s.nlights,
               s.nstatic, s.generations, s.loads, s.load_failures, s.read_ms, s.parse_ms, s.snapshots, s.snapshots_skipped, s.builds, s.build_failures, n,
               n ? t[n / 2] : 0.0f, n ? t[n * 99 / 100] : 0.0f, n ? t[n - 1] : 0.0f, s.build_ms_max, s.publishes, s.uploads, s.upload_busy, s.upload_failures,
               s.frames_fresh, s.frames_stale, s.frames_fresh + s.frames_stale ? 100.0 * (double)s.frames_stale / (double)(s.frames_fresh + s.frames_stale) : 0.0,
               s.last.dyn_lights, s.last.pending, s.last.overflow_cells, s.last.overflow_lights, s.last.records_static, s.last.record_overflow,
               s.last.dirty_rows, s.maps_in_gma, s.gma_refused, s.handles_leaked, s.last.split_lights, s.last.fold_cells, s.last.fold_entries,
               s.last.fold_lost, s.last.fold_seq, s.fold_copies, s.fold_evals, s.fold_hits);
}
