/* P5: the light grid's worker (src/proxy_shaderapi/lgwork.c) on the Win32 shim, with a real thread, real maps and a fake filesystem, built by
 * scripts/run_ambient_test.sh under ThreadSanitizer and under AddressSanitizer, and once per LG_WORK_MUTANT, which must fail. The test's main thread
 * plays the draw thread and keeps its own "texture", written only through lg_work_upload's copy callback.
 * Rules checked:
 *   every frame an upload answers, its window (c63) is one this generation's snapshots asked for (never another map's or an old one);
 *   after a hold (the same snapshot for a while, the window's static part in), every window cell of the texture holds exactly the records, in
 *   order, that a fresh grid built from that snapshot gives (records compared by content: slot numbers may differ), and lg_work_fold (P5b) gives,
 *   at 300 points of the window, exactly the fold a fresh grid's own fold image and table give there, each asked twice; then the hold's style 32
 *   alone changes (the window and the fold lists stay, the table's entries of those lights change) and the same points must give the new fold:
 *   lg_work_fold's cache must not hand back a fold whose lights changed;
 * Scenarios (--only <name>): run (walks and holds on map a, b, a), maps (map changes in the middle of loads, slow loads, out of game: a load is
 * cancelled), quit (20 shutdowns during loads and builds), stuck (a read that never returns: bounded shutdown, handle leaked, module kept).
 * Usage: lgworktest <construct.bsp> <flatgrass.bsp> [--only name] [-v]. Prints LGWORKTEST OK. */
#define _GNU_SOURCE
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/proxy_shaderapi/lgwork.h"

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

static uint8_t *g_bsp[2];
static size_t g_len[2];
static LgMap *g_ref[2];
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
    int m = strstr(path, "b.bsp") ? 1 : 0;
    f->data = g_bsp[m];
    f->size = (long long)g_len[m];
    f->slow = strstr(path, "slow") != NULL;
    f->stuck = strstr(path, "stuck") != NULL;
    __atomic_add_fetch(&g_opens, 1, __ATOMIC_SEQ_CST);
    return f;
}
static long long fs_size(void *file) { return ((FakeFile *)file)->size; }
static int fs_read_at(void *file, long long ofs, void *buf, int n) {
    FakeFile *f = file;
    __atomic_add_fetch(&g_reads, 1, __ATOMIC_SEQ_CST);
    if (ofs < 0 || n <= 0 || ofs > f->size || n > f->size - ofs) { __atomic_add_fetch(&g_bad, 1, __ATOMIC_SEQ_CST); return 0; }
    if (f->slow) usleep(30000);
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

/* --- the draw thread's texture and the checks ---------------------------------------------------------------------------------- */
static float *g_tex;
static long g_rows_copied;
static int copy_cb(void *ctx, const float *image, const uint8_t rows[LG_TEX_H]) {
    (void)ctx;
    for (int r = 0; r < LG_TEX_H; r++)
        if (rows[r]) { memcpy(g_tex + (size_t)r * LG_TEX_W * 4, image + (size_t)r * LG_TEX_W * 4, LG_TEX_W * 4 * sizeof(float)); g_rows_copied++; }
    return 1;
}

enum { MAXCAM = 4096 };
static int g_cams[MAXCAM][3], g_ncams;   /* the windows asked for in this generation */
static void note_cam(const double cam[3]) {
    int lo[3] = { (int)floor(cam[0] / 128) - 16, (int)floor(cam[1] / 128) - 16, (int)floor(cam[2] / 128) - 8 };
    if (g_ncams < MAXCAM) memcpy(g_cams[g_ncams++], lo, sizeof lo);
}
static int imod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

static void snapshot(LgSnapshot *s, const double cam[3], int ndyn) {
    memset(s, 0, sizeof *s);
    memcpy(s->cam, cam, sizeof s->cam);
    for (int i = 0; i < 128; i++) s->style[i] = 264;
    s->style[32] = 100 + rnd(300);   /* the ColourRoom's switch style: the fold table's static entries change with it */
    s->el_on = 1;
    for (int i = 0; i < ndyn && i < LG_NDL; i++) {
        LgDlight *d = &s->dl[i];
        for (int k = 0; k < 3; k++) d->origin[k] = (float)(cam[k] + rndf(-600, 600) * (k == 2 ? 0.3 : 1));
        d->radius = (float)rndf(80, 400);
        d->r = (uint8_t)rnd(256); d->g = (uint8_t)rnd(256); d->b = (uint8_t)rnd(256);
        s->dl_mask |= 1u << i;
    }
}

static long g_fold_nonzero;
/* every window cell of `tex` (window c63) against a fresh grid on `snap`: the records, in order, by content. reuse: the fold's points are the last call's */
static void compare_fresh(const float *tex, const float c63[4], const LgSnapshot *snap, int map, const char *tag, int reuse) {
    LgParams p;
    lg_params_default(&p);
    p.budget = 0;
    LgGrid *g = NULL;
    CHECK(lg_grid_new(g_ref[map], &p, &g) == AMB_OK, "%s: fresh grid", tag);
    if (!g) return;
    LgStats st;
    lg_grid_frame(g, snap, &st);
    const float *ref = lg_grid_image(g);
    float rc[4];
    lg_grid_constants(g, rc);
    CHECK(memcmp(rc, c63, sizeof rc) == 0, "%s: window %g %g %g, the fresh grid's %g %g %g", tag, c63[0], c63[1], c63[2], rc[0], rc[1], rc[2]);
    int bad = 0;
    for (int z = 0; z < LG_GZ && !bad; z++)
        for (int y = 0; y < LG_GY && !bad; y++)
            for (int x = 0; x < LG_GX && !bad; x++) {
                int key[3] = { (int)rc[0] + x, (int)rc[1] + y, (int)rc[2] + z };
                int i = imod(key[0], LG_GX) + LG_GX * imod(key[1], LG_GY) + LG_GX * LG_GY * imod(key[2], LG_GZ);
                const float *a = tex + ((size_t)(i / LG_GX) * LG_TEX_W + 4 * (size_t)(i % LG_GX)) * 4;
                const float *b = ref + ((size_t)(i / LG_GX) * LG_TEX_W + 4 * (size_t)(i % LG_GX)) * 4;
                if (a[0] != b[0]) { bad = 1; CHECK(0, "%s: cell %d %d %d holds %g lights, the fresh grid %g", tag, key[0], key[1], key[2], a[0], b[0]); break; }
                for (int j = 0; j < (int)a[0]; j++) {
                    int ra = (int)a[1 + j], rb = (int)b[1 + j];
                    const float *qa = tex + ((size_t)(LG_REC_ROW + ra / 32) * LG_TEX_W + 4 * (size_t)(ra % 32)) * 4;
                    const float *qb = ref + ((size_t)(LG_REC_ROW + rb / 32) * LG_TEX_W + 4 * (size_t)(rb % 32)) * 4;
                    if (memcmp(qa, qb, 16 * sizeof(float))) { bad = 1; CHECK(0, "%s: cell %d %d %d light %d differs from the fresh grid's", tag, key[0], key[1], key[2], j); break; }
                }
            }
    /* P5b: the draw thread's fold against the fresh grid's, at points of the window (half of them near the window's centre, where cells fold most) */
    int ntab, nonzero = 0;
    const LgFoldLight *tab = lg_grid_fold_table(g, &ntab, NULL);
    static float pts[300][3];
    for (int q = 0; q < 300 && !bad; q++) {
        float pt[3];
        if (reuse) memcpy(pt, pts[q], sizeof pt);
        else
            for (int k = 0; k < 3; k++) {
                int dim = k == 2 ? LG_GZ : LG_GX;
                double lo = q % 2 ? rc[k] + dim / 2 - 2 : rc[k], span = q % 2 ? 4 : dim;
                pt[k] = (float)((lo + rndf(0, span)) * 128.0);
            }
        memcpy(pts[q], pt, sizeof pt);
        int key[3];
        for (int k = 0; k < 3; k++) key[k] = (int)floorf(pt[k] / 128.0f);
        int i = imod(key[0], LG_GX) + LG_GX * imod(key[1], LG_GY) + LG_GX * LG_GY * imod(key[2], LG_GZ);
        float got[6][3], want[6][3], again[6][3];
        int ok = lg_work_fold(pt, got) && lg_work_fold(pt, again) && memcmp(got, again, sizeof got) == 0;
        lg_fold_eval(tab, ntab, lg_grid_static_count(g), lg_grid_fold_image(g) + (size_t)(i / LG_GX) * LG_FOLD_ROW + (size_t)(i % LG_GX) * LG_FOLD_STRIDE, pt, want);
        for (int f = 0; f < 6; f++) for (int c = 0; c < 3; c++) nonzero += want[f][c] != 0.0f;
        if (!ok || memcmp(got, want, sizeof got) != 0) {
            bad = 1;
            CHECK(0, "%s: the draw thread's fold at %.0f %.0f %.0f differs from the fresh grid's (ok %d, +X r %g / %g)", tag, pt[0], pt[1], pt[2], ok, got[0][0], want[0][0]);
        }
    }
    g_fold_nonzero += nonzero;
    lg_grid_free(g);
}

static int g_ok_frames;
static void frame(int want, int map, const char *level, const LgSnapshot *s, int check_window) {
    lg_work_frame(want, level, 1, s);
    if (want && s) note_cam(s->cam);
    float c63[4];
    if (lg_work_upload(copy_cb, NULL, c63)) {
        g_ok_frames++;
        int found = 0;
        for (int i = 0; i < g_ncams && !found; i++) found = g_cams[i][0] == (int)c63[0] && g_cams[i][1] == (int)c63[1] && g_cams[i][2] == (int)c63[2];
        if (check_window) CHECK(found && want, "an upload with window %g %g %g that this generation never asked for (map %d)", c63[0], c63[1], c63[2], map);
    }
}

static int init(void) {
    LgParams p;
    lg_params_default(&p);
    int rc = lg_work_init(&k_files, &p);
    CHECK(rc == AMB_OK, "init %d", rc);
    return rc == AMB_OK;
}
static void stop(const char *why) {
    lg_work_shutdown(why);
    CHECK(__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) == 0, "%s: module references left: %ld", why, shim_module_refs);
    CHECK(g_pins == 0, "%s: filesystem pins left: %d", why, g_pins);
}

static void scen_run(void) {
    if (!init()) return;
    static const char *lv[2] = { "maps/a.bsp", "maps/b.bsp" };
    double cam[3] = { -1500, -1000, -40 };
    LgSnapshot s;
    int holds = 0, styles = 0;
    for (int phase = 0; phase < 3; phase++) {
        int map = phase == 1;
        g_ncams = 0;
        if (map) { cam[0] = -16; cam[1] = 12; cam[2] = -12300; }
        else { cam[0] = -1500; cam[1] = -1000; cam[2] = -40; }
        for (int f = 0; f < 600; f++) {
            if (f % 150 < 110) { cam[0] += rndf(-30, 40); cam[1] += rndf(-30, 30); snapshot(&s, cam, rnd(33)); frame(1, map, lv[map], &s, 1); usleep(1000); continue; }
            if (f % 150 == 110) snapshot(&s, cam, rnd(33));
            frame(1, map, lv[map], &s, 1);   /* the hold: the same snapshot */
            usleep(f % 150 == 149 ? 0 : 20000);
            if (f % 150 == 149) {
                LgWorkStats st0;
                lg_work_stats(&st0);
                long b_done = -1;
                for (int w = 0; w < 2000; w++) {   /* until a build of this hold left nothing pending, and an upload after it */
                    LgWorkStats st;
                    lg_work_stats(&st);
                    if (b_done < 0 && st.builds > st0.builds + 1 && st.last.pending == 0) b_done = st.uploads;
                    if (b_done >= 0 && st.uploads > b_done + 1) break;
                    frame(1, map, lv[map], &s, 1);
                    usleep(5000);
                }
                float c63[4];
                if (lg_work_upload(copy_cb, NULL, c63)) {
                    compare_fresh(g_tex, c63, &s, map, "run hold", 0);
                    holds++;
                    /* the same camera and lights, style 32 alone changed (still on): wait for a build of it and its upload, then the same points */
                    LgSnapshot s2 = s;
                    s2.style[32] = s.style[32] > 250 ? 120 : 380;
                    LgWorkStats sa;
                    lg_work_stats(&sa);
                    long u_done = -1;
                    for (int w = 0; w < 2000; w++) {
                        LgWorkStats st;
                        lg_work_stats(&st);
                        if (u_done < 0 && st.builds > sa.builds + 1) u_done = st.uploads;
                        if (u_done >= 0 && st.uploads > u_done + 1) break;
                        frame(1, map, lv[map], &s2, 1);
                        usleep(5000);
                    }
                    if (lg_work_upload(copy_cb, NULL, c63)) { compare_fresh(g_tex, c63, &s2, map, "run style", 1); styles++; }
                    memcpy(&s, &s2, sizeof s);
                }
            }
        }
    }
    LgWorkStats st;
    lg_work_stats(&st);
    CHECK(holds >= 9, "run: only %d holds compared", holds);
    CHECK(styles >= 9, "run: only %d style changes compared", styles);
    CHECK(g_fold_nonzero > 1000, "run: the fold compared was almost always zero (%ld non-zero values)", g_fold_nonzero);
    CHECK(st.builds > 300 && st.uploads > 100, "run: builds %ld, uploads %ld", st.builds, st.uploads);
    printf("run: %d holds equal to a fresh grid (the fold too: %ld non-zero values compared), and %d style changes; %ld builds (%ld snapshots skipped), %ld uploads (%ld busy),"
           " %ld rows copied, %d frames answered; fold copies %ld, evaluations %ld, cache hits %ld\n", holds, g_fold_nonzero, styles, st.builds, st.snapshots_skipped, st.uploads,
           st.upload_busy, g_rows_copied, g_ok_frames, st.fold_copies, st.fold_evals, st.fold_hits);
    stop("run");
}

static void scen_maps(void) {
    if (!init()) return;
    static const char *lv[3] = { "maps/a.bsp", "maps/b.bsp", "maps/slow_a.bsp" };
    double cam[3] = { -1500, -1000, -40 };
    LgSnapshot s;
    int map = 0, prev = -2;
    for (int f = 0; f < 900; f++) {
        if (rnd(f < 400 ? 8 : 40) == 0) map = rnd(6) == 0 ? -1 : rnd(3);
        if (map != prev) { g_ncams = 0; prev = map; }
        cam[0] += rndf(-20, 20);
        snapshot(&s, cam, rnd(8));
        frame(map >= 0, map, map >= 0 ? lv[map] : "", map >= 0 ? &s : NULL, 1);
        usleep(1000 + rnd(3000));
    }
    LgWorkStats st;
    lg_work_stats(&st);
    CHECK(st.generations > 20, "maps: generations %ld", st.generations);
    CHECK(st.loads_cancelled > 0, "maps: no load was cancelled by a map change (loads %ld)", st.loads);
    CHECK(lg_work_sun(NULL) == NULL && st.sun_loads == 0, "maps: a sun map without lg_work_want_sun");
    printf("maps: %ld generations, %ld loads (%ld cancelled), %ld builds, %d frames answered\n", st.generations, st.loads, st.loads_cancelled, st.builds,
           g_ok_frames);
    stop("maps");
}

/* P6 sun (lg_work_want_sun): the draw thread only ever gets the current map's SunMap (map a and b have different suns), never an old one after a
 * map change, the serial changes with every new map, a trace on it runs while the worker loads the next (TSan), and a shutdown frees it */
static SunInfo g_sun_ref[2];
static void scen_sun(void) {
    lg_work_want_sun(1);
    lg_work_test_sun_delay_ms = 300;   /* map changes overtake publishes: those must never reach the draw thread */
    if (!init()) { lg_work_want_sun(0); return; }
    static const char *lv[3] = { "maps/a.bsp", "maps/b.bsp", "maps/slow_a.bsp" };
    double cam[3] = { -1500, -1000, -40 };
    LgSnapshot s;
    int map = 0, got = 0, wrong = 0, changes = 0, stale_serial = 0, held = 0;
    long last_serial = 0;
    const SunMap *last = NULL;
    /* 16 phases: most hold their map until its sun arrived and 30 frames more; every third switches after a few frames (cancelled loads and
     * publishes overtaken by the next map) */
    /* then 6 scripted phases, so a publish overtaken by a map with another sun happens on every run: b, and back to a after 100 frames (b's
     * load done, its publish held by the delay), then the same from a to b. Each entry: the map, its frames (-1: until its sun arrived + 30) */
    static const int script[6][2] = { { 0, -1 }, { 1, 100 }, { 0, -1 }, { 1, -1 }, { 0, 100 }, { 1, -1 } };
    int phase = 0, pf = 0, arrived = 0;
    map = -2;
    for (int f = 0; f < 60000 && phase < 16 + 6; f++) {
        int nm = map, cur = phase - 1;
        int done = cur >= 16 ? (script[cur - 16][1] < 0 ? arrived >= 30 || pf > 6000 : pf >= script[cur - 16][1])
                             : phase % 3 == 2 ? pf >= 3 + rnd(400) : (arrived >= 30 || pf > 6000);
        if (map == -2 || done) {
            nm = phase >= 16 ? script[phase - 16][0] : phase % 5 == 4 ? -1 : rnd(3);
            phase++; pf = 0; arrived = 0;
        }
        pf++;
        cam[0] += rndf(-20, 20);
        snapshot(&s, cam, rnd(8));
        frame(nm >= 0, nm < 0 ? 0 : nm, nm >= 0 ? lv[nm] : "", nm >= 0 ? &s : NULL, 0);
        if (nm != map) { map = nm; held = 0; }
        long serial = -1;
        const SunMap *m = lg_work_sun(&serial);
        if (m) {
            SunInfo si;
            sun_map_info(m, &si);
            int want = map == 1;
            if (map < 0 || memcmp(si.sun_dir, g_sun_ref[want].sun_dir, sizeof si.sun_dir) != 0) wrong++;
            else got++;
            held++;
            arrived++;
            double pt[3] = { cam[0], cam[1], 512 };
            (void)sun_visible(m, NULL, pt);
            if (m != last && serial == last_serial) stale_serial++;
        } else CHECK(serial == 0, "sun: no map but serial %ld", serial);
        if (m != last || serial != last_serial) changes++;
        if (map < 0) arrived++;
        last = m;
        last_serial = serial;
        usleep(1000 + rnd(1000));
    }
    CHECK(wrong == 0, "sun: %d frames got a sun map that is not the current map's (of %d with one)", wrong, wrong + got);
    CHECK(got > 40 && changes >= 4, "sun: only %d frames had a sun map (%d changes)", got, changes);
    CHECK(stale_serial == 0, "sun: the map changed %d times with the same serial", stale_serial);
    LgWorkStats st;
    lg_work_stats(&st);
    CHECK(st.sun_loads > 3 && st.sun_failures == 0, "sun: %ld sun loads, %ld failed", st.sun_loads, st.sun_failures);
    printf("sun: %d frames with the current map's sun, %d changes of map or serial, %ld sun loads over %ld generations\n", got, changes, st.sun_loads,
           st.generations);
    stop("sun");
    long serial = -1;
    CHECK(lg_work_sun(&serial) == NULL && serial == 0, "sun: a map after the shutdown");
    lg_work_want_sun(0);
    lg_work_test_sun_delay_ms = 0;
    (void)held;
}

static void scen_quit(void) {
    for (int k = 0; k < 20; k++) {
        if (!init()) return;
        double cam[3] = { rndf(-500, 500), rndf(-500, 500), 0 };
        const char *lv = k % 3 == 0 ? "maps/slow_a.bsp" : "maps/a.bsp";
        LgSnapshot s;
        int frames = rnd(40);
        for (int f = 0; f < frames; f++) {
            cam[0] += 40;
            snapshot(&s, cam, rnd(30));
            frame(1, 0, lv, &s, 0);
            usleep((useconds_t)rnd(3000));
        }
        stop("quit");
    }
    printf("quit: 20 shutdowns during loads and builds, references balanced\n");
}

static void scen_stuck(void) {
    lg_work_test_wait_ms = 200;
    if (!init()) return;
    LgSnapshot s;
    double cam[3] = { 0, 0, 0 };
    snapshot(&s, cam, 0);
    lg_work_frame(1, "maps/stuck_a.bsp", 1, &s);
    for (int i = 0; i < 2000 && !__atomic_load_n(&g_stuck_inside, __ATOMIC_SEQ_CST); i++) usleep(1000);
    CHECK(g_stuck_inside, "stuck: the read never started");
    CHECK(g_pins_while_stuck > 0, "stuck: no reference on the filesystem's module during the read");
    long closes = g_closes;
    lg_work_shutdown("stuck");
    __atomic_store_n(&g_stuck_release, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 2000 && __atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST); i++) usleep(1000);
    CHECK(shim_module_refs == 0, "stuck: the worker kept its module reference after it finished");
    CHECK(g_closes == closes, "stuck: the file was closed after the stop flag");
    LgWorkStats st;
    lg_work_stats(&st);
    CHECK(st.handles_leaked == 1, "stuck: handles leaked %ld", st.handles_leaked);
    printf("stuck: the shutdown gave up after 200 ms, the worker then left the file open and released its reference\n");
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv) {
    const char *only = NULL, *maps[2] = { NULL, NULL };
    int nm = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (nm < 2) maps[nm++] = argv[i];
    }
    if (nm < 2) { printf("usage: lgworktest <construct.bsp> <flatgrass.bsp> [--only name]\n"); return 2; }
    for (int i = 0; i < 2; i++) {
        g_bsp[i] = read_file(maps[i], &g_len[i]);
        if (!g_bsp[i] || lg_map_load(g_bsp[i], g_len[i], 1, &g_ref[i]) != AMB_OK) { printf("FAIL: maps\n"); return 1; }
        SunMap *sm = NULL;
        if (sun_map_load(g_bsp[i], g_len[i], 1, &sm) != AMB_OK) { printf("FAIL: sun maps\n"); return 1; }
        sun_map_info(sm, &g_sun_ref[i]);
        sun_map_free(sm);
    }
    g_tex = calloc((size_t)LG_TEX_W * LG_TEX_H * 4, sizeof *g_tex);
    if (!only || !strcmp(only, "run")) scen_run();
    if (!only || !strcmp(only, "maps")) scen_maps();
    if (!only || !strcmp(only, "sun")) scen_sun();
    if (!only || !strcmp(only, "quit")) scen_quit();
    if (!only || !strcmp(only, "stuck")) scen_stuck();
    CHECK(g_bad == 0, "%ld reads outside a file", g_bad);
    if (!only || strcmp(only, "stuck")) {
        for (int i = 0; i < 2; i++) { lg_map_free(g_ref[i]); free(g_bsp[i]); }
        free(g_tex);
    }
    if (g_fails) { printf("LGWORKTEST FAIL (%ld)\n", g_fails); return 1; }
    printf("LGWORKTEST OK\n");
    return 0;
}
