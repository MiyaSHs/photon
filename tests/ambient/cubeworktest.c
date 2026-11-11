/* The pick table's worker (src/proxy_shaderapi/cubework.c) on the Win32 shim, with a real thread, real builds and a fake filesystem, built by
 * scripts/run_ambient_test.sh under ThreadSanitizer and under AddressSanitizer with leak checks, and once per CUBE_WORK_MUTANT, which must fail.
 * The test's main thread plays the draw thread. The rule checked on every frame: a lookup that answers holds exactly the current map's entry for that
 * voxel (amb_pick_voxel on the same map), never another map's and never a voxel the table has not finished.
 * Scenarios (--only <name>):
 *   run    a walk with steps on every axis, back and forth, and teleports, on map a, then b, then a; lookups past the window answer from the
 *          coarse level (P4c) at its own voxel size, and must equal amb_pick_voxel there
 *   maps   map changes in the middle of builds, every few frames, and frames out of game
 *   quit   20 shutdowns at random points of builds and loads (slow reads), each followed by a fresh init
 *   stuck  a read that never returns until released: the bounded shutdown gives up, the worker leaks the handle instead of closing it after the stop,
 *          and the filesystem's module stays referenced while the read is inside it */
#define _GNU_SOURCE
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../src/proxy_shaderapi/cubework.h"
#include "synth_pick.h"

long shim_module_refs;
volatile long shim_thread_calls, shim_thread_fail_from;
static long g_fails;
static int g_verbose;
#define CHECK(cond, ...) do { if (!(cond)) { if (g_fails++ < 12) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

void photon_log(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* --- maps and the fake filesystem ------------------------------------------------------------------------------------------ */
static uint8_t *g_bsp[2];
static size_t g_len[2];
static AmbMap *g_ref[2];
static AmbCubes g_cubes[2];
static AmbPickTargets g_tgt[2];
typedef struct { const uint8_t *data; long long size; int slow, stuck; } FakeFile;
static FakeFile g_pool[256];
static volatile long g_next, g_opens, g_closes, g_reads, g_bad;
static volatile int g_pins, g_stuck_inside, g_stuck_release, g_pins_while_stuck;

static void *fs_pin(void) { __atomic_add_fetch(&g_pins, 1, __ATOMIC_SEQ_CST); return (void *)&g_pins; }
static void fs_unpin(void *p) { (void)p; __atomic_sub_fetch(&g_pins, 1, __ATOMIC_SEQ_CST); }
static void *fs_open(const char *path, int *gma, const volatile int *cancel) {
    (void)cancel;
    *gma = 0;
    FakeFile *f = &g_pool[__atomic_fetch_add(&g_next, 1, __ATOMIC_SEQ_CST) % 256];
    memset(f, 0, sizeof *f);
    if (!strcmp(path, "maps/a.bsp")) { f->data = g_bsp[0]; f->size = (long long)g_len[0]; }
    else if (!strcmp(path, "maps/b.bsp")) { f->data = g_bsp[1]; f->size = (long long)g_len[1]; }
    else if (!strcmp(path, "maps/slow.bsp")) { f->data = g_bsp[0]; f->size = (long long)g_len[0]; f->slow = 1; }
    else if (!strcmp(path, "maps/stuck.bsp")) { f->data = g_bsp[0]; f->size = (long long)g_len[0]; f->stuck = 1; }
    else return NULL;
    __atomic_add_fetch(&g_opens, 1, __ATOMIC_SEQ_CST);
    return f;
}
static long long fs_size(void *file) { return ((FakeFile *)file)->size; }
static int fs_read_at(void *file, long long ofs, void *buf, int n) {
    FakeFile *f = file;
    __atomic_add_fetch(&g_reads, 1, __ATOMIC_SEQ_CST);
    if (ofs < 0 || n <= 0 || ofs > f->size || n > f->size - ofs) { __atomic_add_fetch(&g_bad, 1, __ATOMIC_SEQ_CST); return 0; }
    if (f->slow) usleep(20000);
    if (f->stuck) {
        __atomic_store_n(&g_pins_while_stuck, g_pins, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_stuck_inside, 1, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&g_stuck_release, __ATOMIC_SEQ_CST)) usleep(1000);
    }
    memcpy(buf, f->data + ofs, (size_t)n);
    return n;
}
static void fs_close(void *file) { (void)file; __atomic_add_fetch(&g_closes, 1, __ATOMIC_SEQ_CST); }
static const AmbWorkFiles k_files = { fs_pin, fs_unpin, fs_open, fs_size, fs_read_at, fs_close, NULL };

static unsigned g_rs = 777;
static int rnd(int n) { g_rs = g_rs * 1103515245u + 12345u; return (int)((g_rs >> 8) % (unsigned)n); }
static double rndf(double lo, double hi) { return lo + (hi - lo) * (double)rnd(1000000) / 1000000.0; }

/* every answer must be the current map's own entry */
static long g_checked, g_coarse;
static long check_lookups(int map, const double cam[3], int n) {
    long hits = 0;
    AmbPickParams p;
    amb_pick_params_default(&p);
    for (int i = 0; i < n; i++) {
        float o[3];
        for (int a = 0; a < 3; a++) o[a] = (float)(cam[a] + rndf(-1400, 1400) * (a == 2 ? 0.5 : 1.0));
        uint32_t e;
        int lv = cube_work_lookup(o, &e);
        if (!lv) continue;
        hits++;
        if (map < 0) { CHECK(0, "a lookup answered out of game"); continue; }
        double vox = 32.0;
        if (lv == 2) {   /* P4c: the coarse level, at its own voxel size */
            CubeWorkStats st;
            cube_work_stats(&st);
            vox = st.coarse_voxel;
            g_coarse++;
        }
        p.voxel = vox;
        int v[3] = { (int)floor(o[0] / vox), (int)floor(o[1] / vox), (int)floor(o[2] / vox) };
        uint32_t want = amb_pick_voxel(g_ref[map], &g_cubes[map], &g_tgt[map], &p, v);
        CHECK(e == want, "map %d voxel %d %d %d: 0x%08x, wants 0x%08x", map, v[0], v[1], v[2], e, want);
        g_checked++;
    }
    return hits;
}

static void frame(int map, const char *level, const double cam[3], int lookups) {
    cube_work_frame(map >= 0, level, cam);
    check_lookups(map, cam, lookups);
}

static int init(void) {
    int rc = cube_work_init(&k_files);
    CHECK(rc == AMB_OK, "init %d", rc);
    return rc == AMB_OK;
}

static void stop(const char *why) {
    cube_work_shutdown(why);
    CHECK(__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) == 0, "%s: module references left: %ld", why, shim_module_refs);
    CHECK(g_pins == 0, "%s: filesystem pins left: %d", why, g_pins);
}

static void scen_run(void) {
    if (!init()) return;
    static const char *lv[2] = { "maps/a.bsp", "maps/b.bsp" };
    double cam[3] = { -300, 40, 30 };
    long hits = 0;
    for (int phase = 0; phase < 3; phase++) {
        int map = phase == 1;
        for (int f = 0; f < 900; f++) {
            /* steps along every axis, reversals, and a teleport now and then */
            int t = f % 300;
            if (t < 100) cam[0] += 9;
            else if (t < 160) cam[1] -= 13;
            else if (t < 220) cam[0] -= 11;
            else if (t < 260) cam[2] += 7;
            else cam[1] += 17;
            if (f % 250 == 249) { cam[0] = rndf(-900, 900); cam[1] = rndf(-900, 900); cam[2] = rndf(-100, 200); }
            /* a diagonal stretch: X and Y slabs alternate in the queue, so consecutive jobs share texels at their corner */
            if (f >= 600 && f < 750) { cam[0] += 23; cam[1] += 29; }
            cube_work_frame(1, lv[map], cam);
            hits += check_lookups(map, cam, 128);
            /* now and then a slow frame, in which the worker gets ahead of the uploads */
            usleep(rnd(8) == 0 ? 40000 : 1500);
        }
    }
    CubeWorkStats s;
    cube_work_stats(&s);
    CHECK(s.jobs[AMB_JOB_SLAB] > 20 && s.jobs[AMB_JOB_WHOLE] >= 3, "run: whole %ld, slabs %ld", s.jobs[AMB_JOB_WHOLE], s.jobs[AMB_JOB_SLAB]);
    CHECK(hits > 3000, "run: only %ld answered lookups", hits);
    CHECK(g_coarse > 100 && s.coarse_voxels > 0, "run: the coarse level answered %ld lookups (%lld voxels)", g_coarse, s.coarse_voxels);
    printf("run: coarse level %lld voxels of %d units in %.0f ms, %ld of the lookups from it; ", s.coarse_voxels, s.coarse_voxel, s.coarse_ms, g_coarse);
    printf("run: %ld lookups answered and checked, whole windows %ld (mean %.1f ms), slabs %ld, uploads %ld, first window %.0f ms\n", hits,
           s.jobs[AMB_JOB_WHOLE], s.jobs[AMB_JOB_WHOLE] ? s.build_ms[AMB_JOB_WHOLE] / s.jobs[AMB_JOB_WHOLE] : 0, s.jobs[AMB_JOB_SLAB], s.uploads, s.first_ms);
    stop("run");
}

static void scen_maps(void) {
    if (!init()) return;
    static const char *lv[2] = { "maps/a.bsp", "maps/b.bsp" };
    double cam[3] = { 100, 100, 0 };
    int map = 0;
    long hits = 0;
    for (int f = 0; f < 1500; f++) {
        if (rnd(f < 600 ? 9 : 70) == 0) map = rnd(5) == 0 ? -1 : rnd(2);   /* -1: out of game; first in the middle of loads and builds, then also long enough to finish */
        cam[0] += rndf(-20, 20); cam[1] += rndf(-20, 20);
        cube_work_frame(map >= 0, map >= 0 ? lv[map] : "", cam);
        hits += check_lookups(map, cam, 16);
        usleep(1000 + rnd(4000));
    }
    CubeWorkStats s;
    cube_work_stats(&s);
    CHECK(s.generations > 20, "maps: generations %ld", s.generations);
    CHECK(hits > 200, "maps: only %ld lookups answered", hits);
    /* a build stopped by its cancel flag (a map change), not merely dropped at the write */
    CHECK(s.jobs_cancelled > 0, "maps: no build was cancelled by a map change (cancelled %ld, at the write %ld)", s.jobs_cancelled, s.write_cancelled);
    printf("maps: %ld generations, %ld maps freed, %ld loads, builds cancelled %ld (at the write %ld), %ld lookups answered\n", s.generations,
           s.maps_freed, s.loads, s.jobs_cancelled, s.write_cancelled, hits);
    stop("maps");
}

static void scen_quit(void) {
    for (int k = 0; k < 20; k++) {
        if (!init()) return;
        double cam[3] = { rndf(-500, 500), rndf(-500, 500), 0 };
        const char *lv = k % 3 == 0 ? "maps/slow.bsp" : "maps/a.bsp";
        int frames = rnd(40);
        for (int f = 0; f < frames; f++) {
            cam[0] += 40;
            frame(0, lv, cam, 8);
            usleep((useconds_t)rnd(3000));
        }
        stop("quit");
    }
    printf("quit: 20 shutdowns during loads and builds, references balanced\n");
}

static void scen_stuck(void) {
    cube_work_test_wait_ms = 200;
    if (!init()) return;
    double cam[3] = { 0, 0, 0 };
    cube_work_frame(1, "maps/stuck.bsp", cam);
    for (int i = 0; i < 2000 && !__atomic_load_n(&g_stuck_inside, __ATOMIC_SEQ_CST); i++) usleep(1000);
    CHECK(g_stuck_inside, "stuck: the read never started");
    CHECK(g_pins_while_stuck > 0, "stuck: no reference on the filesystem's module during the read");
    long closes = g_closes;
    cube_work_shutdown("stuck");   /* gives up after the bound */
    __atomic_store_n(&g_stuck_release, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && __atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST); i++) usleep(1000);
    CHECK(shim_module_refs == 0, "stuck: the worker kept its module reference after it finished");
    CHECK(g_closes == closes, "stuck: the file was closed after the stop flag");
    CubeWorkStats s;
    cube_work_stats(&s);
    CHECK(s.handles_leaked == 1, "stuck: handles leaked %ld", s.handles_leaked);
    printf("stuck: the shutdown gave up after 200 ms, the worker then left the file open and released its reference\n");
}

int main(int argc, char **argv) {
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }
    AmbPickParams p;
    amb_pick_params_default(&p);
    cube_work_test_coarse_voxel = 512;   /* 32^3 voxels over the synthetic map's +-8000: built in seconds even under ThreadSanitizer */
    for (int i = 0; i < 2; i++) {
        g_bsp[i] = synth_pick_bsp(i, 0, 0, 0, &g_len[i]);
        if (amb_map_load(g_bsp[i], g_len[i], 0, &g_ref[i]) != AMB_OK || amb_cubes_load(g_bsp[i], g_len[i], &g_cubes[i]) != AMB_OK) { printf("FAIL: maps\n"); return 1; }
        amb_pick_targets(g_ref[i], &g_cubes[i], &g_tgt[i]);
    }
    if (!only || !strcmp(only, "run")) scen_run();
    if (!only || !strcmp(only, "maps")) scen_maps();
    if (!only || !strcmp(only, "quit")) scen_quit();
    if (!only || !strcmp(only, "stuck")) scen_stuck();
    CHECK(g_bad == 0, "%ld reads outside a file", g_bad);
    if (!only || strcmp(only, "stuck"))
        for (int i = 0; i < 2; i++) { amb_map_free(g_ref[i]); free(g_bsp[i]); }
    if (g_fails) { printf("CUBEWORKTEST FAIL (%ld)\n", g_fails); return 1; }
    printf("CUBEWORKTEST OK (%ld answered lookups checked)\n", g_checked);
    return 0;
}
