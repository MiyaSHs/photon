/* The worker pool (src/proxy_shaderapi/ambwork.c) on a shim of Win32, with real threads, real bakes and a fake texture, built twice by
 * scripts/run_ambient_test.sh: under ThreadSanitizer (the hand-off) and AddressSanitizer with leak checks (map lifetime). The test's main
 * thread plays the draw thread. Every pixel a draw with weight > 0 could fetch must hold the current map's voxels, bit for bit against a
 * larger bake of the same map. Scenarios (15d section 8.6): a run with walking, fast flight, teleports and device resets; map changes in
 * the middle of bakes and reads; 20 quits during a bake and 20 during a map read; maps that fail to load; a map inside a GMA, read and refused; a failed bake; the cancel rules on a window large
 * enough for a late cancel to show; a stale job between its two locks; a failed second worker; a read stuck past the shutdown bound.
 * scripts/run_ambient_test.sh builds this against each AMB_WORK_MUTANT, running the scenario that must catch it (--only name). */
#define _GNU_SOURCE
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../src/proxy_shaderapi/ambwork.h"
#include "synth_bsp.h"

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

static const int k_dim[3] = { 16, 16, 8 }, k_voxel[3] = { 32, 32, 32 };
enum { STEP = 4, ATLAS_W = 6 * 17, TEXELS = ATLAS_W * 16 * 8 };

/* --- the fake filesystem --------------------------------------------------------------------------------------------- */
typedef struct { const uint8_t *data; long long size; int slow, gma, stuck; } FakeFile;
static uint8_t *g_bsp[2];
static size_t g_bsp_len[2];
static uint8_t *g_lzma;
static size_t g_lzma_len;
static uint8_t g_slow_head[1032];   /* a header whose planes, nodes and leaves take 5.5 MB: the slow map is read in many chunks */
static volatile long g_reads, g_opens, g_closes, g_max_chunk, g_gma_reads, g_bad_reads;
static volatile int g_pins, g_stuck_inside, g_stuck_release, g_pins_while_stuck;

static void *fs_pin(void) {
    __atomic_add_fetch(&g_pins, 1, __ATOMIC_SEQ_CST);
    return (void *)&g_pins;
}
static void fs_unpin(void *p) { (void)p; __atomic_sub_fetch(&g_pins, 1, __ATOMIC_SEQ_CST); }
static long long fs_size(void *file) { return ((FakeFile *)file)->size; }
static FakeFile g_file_pool[2048];
static volatile long g_file_next;
static void *fs_open(const char *path, int *gma, const volatile int *cancel) {
    (void)cancel;
    *gma = !strncmp(path, "maps/gma", 8);
    if (!strcmp(path, "maps/gmabad.bsp")) return NULL;   /* inside a GMA, refused */
    long at = __atomic_fetch_add(&g_file_next, 1, __ATOMIC_SEQ_CST) % 2048;
    FakeFile *f = &g_file_pool[at];
    memset(f, 0, sizeof *f);
    if (!strcmp(path, "maps/a.bsp")) { f->data = g_bsp[0]; f->size = (long long)g_bsp_len[0]; }
    else if (!strcmp(path, "maps/b.bsp")) { f->data = g_bsp[1]; f->size = (long long)g_bsp_len[1]; }
    else if (!strcmp(path, "maps/lzma.bsp")) { f->data = g_lzma; f->size = (long long)g_lzma_len; }
    else if (!strcmp(path, "maps/short.bsp")) { f->data = g_bsp[0]; f->size = 40; }
    else if (!strcmp(path, "maps/slow.bsp")) { f->data = NULL; f->size = 12ll << 20; f->slow = 1; }
    else if (!strcmp(path, "maps/stuck.bsp")) { f->data = NULL; f->size = 2ll << 20; f->stuck = 1; }
    else if (!strcmp(path, "maps/gma.bsp")) { f->data = g_bsp[1]; f->size = (long long)g_bsp_len[1]; f->gma = 1; }
    else return NULL;
    __atomic_add_fetch(&g_opens, 1, __ATOMIC_SEQ_CST);
    return f;
}
static int fs_read_at(void *file, long long ofs, void *buf, int n) {
    FakeFile *f = file;
    long cur = __atomic_load_n(&g_max_chunk, __ATOMIC_SEQ_CST);
    if (n > cur) __atomic_store_n(&g_max_chunk, (long)n, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&g_reads, 1, __ATOMIC_SEQ_CST);
    if (ofs < 0 || n <= 0 || ofs > f->size || n > f->size - ofs) { __atomic_add_fetch(&g_bad_reads, 1, __ATOMIC_SEQ_CST); return 0; }
    if (f->gma) __atomic_add_fetch(&g_gma_reads, 1, __ATOMIC_SEQ_CST);
    if (f->slow) {
        if (n >= 64 << 10) usleep(15000);   /* the lumps are slow, the header and the LZMA tags are not */
        memset(buf, 0, (size_t)n);
        if (ofs < (long long)sizeof g_slow_head) memcpy(buf, g_slow_head + ofs, (size_t)(n < (int)sizeof g_slow_head - ofs ? n : (int)sizeof g_slow_head - ofs));
        return n;
    }
    if (f->stuck) {
        __atomic_store_n(&g_pins_while_stuck, g_pins, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_stuck_inside, 1, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&g_stuck_release, __ATOMIC_SEQ_CST)) usleep(1000);
        memset(buf, 0, (size_t)n);
        return n;
    }
    memcpy(buf, f->data + ofs, (size_t)n);
    return n;
}
static void fs_close(void *file) { (void)file; __atomic_add_fetch(&g_closes, 1, __ATOMIC_SEQ_CST); }
static const AmbWorkFiles k_files = { fs_pin, fs_unpin, fs_open, fs_size, fs_read_at, fs_close, NULL };
static void slow_head(void) {
    memcpy(g_slow_head, "VBSP", 4);
    synth_w32(g_slow_head + 4, 20);
    static const struct { int idx; uint32_t ofs, len, version; } l[] = { { 1, 1032, 65536 * 20, 0 }, { 5, 1032 + 65536 * 20, 65536 * 32, 0 },
                                                                       { 10, 1032 + 65536 * 52, 65536 * 32, 1 }, { 14, 1032 + 65536 * 84, 48, 0 } };
    for (int i = 0; i < 4; i++) {
        synth_w32(g_slow_head + 8 + 16 * l[i].idx, l[i].ofs);
        synth_w32(g_slow_head + 12 + 16 * l[i].idx, l[i].len);
        synth_w32(g_slow_head + 16 + 16 * l[i].idx, l[i].version);
    }
}

/* --- the reference: the same voxels from a much larger bake ------------------------------------------------------------ */
static const AmbBox k_ref = { { -40, -8, -20 }, { 80, 80, 40 } };
static float *g_refv[2];

static uint16_t ref_half(int map, int ax, int ay, int az, int face, int c) {
    int i = ax - k_ref.org[0], j = ay - k_ref.org[1], k = az - k_ref.org[2];
    if (i < 0 || j < 0 || k < 0 || i >= k_ref.dim[0] || j >= k_ref.dim[1] || k >= k_ref.dim[2]) {
        CHECK(0, "voxel %d %d %d is outside the reference", ax, ay, az);
        return 0;
    }
    size_t v = (((size_t)k * (size_t)k_ref.dim[1] + (size_t)j) * (size_t)k_ref.dim[0] + (size_t)i) * AMB_VOXEL_FLOATS + (size_t)face * 3 + (size_t)c;
    return amb_f32_to_f16(g_refv[map][v]);
}

/* --- the fake texture ---------------------------------------------------------------------------------------------------- */
static uint8_t g_gpu[TEXELS * 8];
static int g_fail_copies;
static long g_copies;
static unsigned rnd_state = 12345;
static int rnd(int n) { rnd_state = rnd_state * 1103515245u + 12345u; return (int)((rnd_state >> 8) % (unsigned)n); }
static double rndf(double lo, double hi) { return lo + (hi - lo) * (double)rnd(1000000) / 1000000.0; }

static int copy_box(void *ctx, const uint8_t *atlas, size_t rp, size_t sp, const AmbTexBox *b) {
    (void)ctx;
    if (g_fail_copies && rnd(100) == 0) return 0;   /* a failed LockBox */
    for (int w = b->w0; w < b->w1; w++)
        for (int v = b->v0; v < b->v1; v++) {
            size_t at = (size_t)w * sp + (size_t)v * rp + (size_t)b->u0 * 8;
            memcpy(g_gpu + at, atlas + at, (size_t)(b->u1 - b->u0) * 8);
        }
    g_copies++;
    return 1;
}

static const uint16_t *texel(int u, int v, int w) {
    return (const uint16_t *)(g_gpu + (size_t)w * ATLAS_W * 8 * 16 + (size_t)v * ATLAS_W * 8 + (size_t)u * 8);
}
static int wrapi(int a, int d) { int r = a % d; return r < 0 ? r + d : r; }

/* The shader's fetch, as in cliptest.c: every tap that has weight */
static long g_taps;
static void check_pixel(int map, const float c[4][4], const float pixel[3], int face) {
    float p[3], t[3];
    for (int i = 0; i < 3; i++) {
        p[i] = pixel[i] > c[2][i] ? pixel[i] : c[2][i];
        p[i] = p[i] < c[3][i] ? p[i] : c[3][i];
        t[i] = p[i] * c[0][i];
    }
    float shifted = t[0] - c[1][0], fr = shifted - floorf(shifted);
    int period = (int)floorf(shifted), band = 17, lo = face * band;
    float u = (float)face * c[1][3] + fr * c[1][1] + c[1][2];
    double U = (double)u * ATLAS_W - 0.5, V = (double)t[1] * 16 - 0.5, W = (double)t[2] * 8 - 0.5;
    int i0 = (int)floor(U), j0 = (int)floor(V), k0 = (int)floor(W);
    double fu = U - i0, fv = V - j0, fw = W - k0;
    for (int bits = 0; bits < 8; bits++) {
        static const double none = 1.0 / 4096.0;
        if ((bits & 1 ? fu : 1 - fu) < none || (bits & 2 ? fv : 1 - fv) < none || (bits & 4 ? fw : 1 - fw) < none) continue;
        int i = i0 + (bits & 1), j = j0 + (bits >> 1 & 1), k = k0 + (bits >> 2 & 1);
        CHECK(i >= lo && i <= lo + 16, "tap u %d left band %d", i, face);
        int want[3] = { period * 16 + (i - lo), j, k };
        const uint16_t *q = texel(i, wrapi(j, 16), wrapi(k, 8));
        for (int ch = 0; ch < 3; ch++)
            CHECK(q[ch] == ref_half(map, want[0], want[1], want[2], face, ch), "map %d: pixel (%.2f %.2f %.2f) face %d channel %d: voxel %d %d %d holds %04x, wants %04x", map, pixel[0],
                  pixel[1], pixel[2], face, ch, want[0], want[1], want[2], q[ch], ref_half(map, want[0], want[1], want[2], face, ch));
        g_taps++;
    }
}

static long check_draws(int map, const double cam[3], int n) {
    long seen = 0;
    float c[4][4];
    amb_work_constants(c);
    for (int d = 0; d < n; d++) {
        float o[3];
        for (int a = 0; a < 3; a++) o[a] = (float)(cam[a] + rndf(-400, 400));
        float f = amb_work_weight(o);
        if (!(f > 0)) continue;
        seen++;
        for (int q = 0; q < 3; q++) {
            float px[3];
            double spread = q == 0 ? 30 : q == 1 ? 200 : 900;
            for (int a = 0; a < 3; a++) px[a] = (float)((double)o[a] + rndf(-spread, spread));
            check_pixel(map, c, px, rnd(6));
        }
    }
    return seen;
}

/* What vlg.c does with the weight: A at the origin against the map itself, by the engine's rule. In a solid leaf the engine's leaf part is 0, so nothing
 * may be taken out of the cube: all of it is folded light. */
static long g_solid_folds;
static void check_fold(const AmbMap *ref, const double cam[3]) {
    float solid[3] = { 32.0f, 512.0f, (float)cam[2] };   /* x >= 0, y < 1024 */
    for (int t = 0; t < 2; t++) {
        float o[3] = { (float)cam[0], (float)cam[1], (float)cam[2] };
        if (t) memcpy(o, solid, sizeof o);
        float f = amb_work_weight(o);
        if (!(f > 0)) continue;
        float cube[6][4], orig[6][4], want[6][4], a[AMB_VOXEL_FLOATS];
        for (int i = 0; i < 6; i++) for (int k = 0; k < 4; k++) cube[i][k] = 0.2f + 0.05f * (float)(i + k);
        memcpy(orig, cube, sizeof orig);
        double p[3] = { o[0], o[1], o[2] };
        amb_eval_engine(ref, p, 1.0, a);
        amb_clip_fold((const float (*)[4])cube, a, f, want);
        amb_work_fold(o, f, cube);
        CHECK(memcmp(cube, want, sizeof want) == 0, "the fold at (%.1f %.1f %.1f)", o[0], o[1], o[2]);
        if (amb_point_solid(ref, p)) {
            g_solid_folds++;
            CHECK(memcmp(cube, orig, sizeof orig) == 0, "a solid origin (%.1f %.1f %.1f) took light out of the cube", o[0], o[1], o[2]);
        }
    }
}

/* Every voxel of the current window, every face, the gutter */
static void check_window(int map) {
    float c[4][4];
    amb_work_constants(c);
    int vmin[3];
    for (int a = 0; a < 3; a++) vmin[a] = (int)lround((double)c[2][a] / k_voxel[a] - 0.5625);
    long wrong = 0;
    for (int z = vmin[2]; z < vmin[2] + k_dim[2]; z++)
        for (int y = vmin[1]; y < vmin[1] + k_dim[1]; y++)
            for (int x = vmin[0]; x < vmin[0] + k_dim[0]; x++)
                for (int f = 0; f < 6; f++)
                    for (int ch = 0; ch < 3; ch++) {
                        uint16_t want = ref_half(map, x, y, z, f, ch);
                        wrong += texel(f * 17 + wrapi(x, 16), wrapi(y, 16), wrapi(z, 8))[ch] != want;
                        if (wrapi(x, 16) == 0) wrong += texel(f * 17 + 16, wrapi(y, 16), wrapi(z, 8))[ch] != want;
                    }
    CHECK(wrong == 0, "after convergence %ld texels are wrong (map %d)", wrong, map);
}


/* Frames until the window is complete around cam, then every voxel of it */
static void converge(int map, const char *level, const double cam[3], int hdr) {
    const float full0 = (float)((k_dim[0] - 2 * 0.5625) * k_voxel[0]);
    float c[4][4];
    int frames = 0;
    for (;; frames++) {
        amb_work_frame(1, level, hdr, cam);
        amb_work_upload(copy_box, NULL);
        check_draws(map, cam, 4);
        amb_work_constants(c);
        if (c[3][0] - c[2][0] == full0 && c[3][1] - c[2][1] == full0 && !amb_work_backlog()) {
            float o[3] = { (float)cam[0], (float)cam[1], (float)cam[2] };
            if (amb_work_weight(o) > 0) break;
        }
        if (frames > 40000) { CHECK(0, "no convergence"); return; }
        usleep(500);
    }
    /* drain what may still be queued for upload */
    for (int i = 0; i < 40; i++) { amb_work_frame(1, level, hdr, cam); amb_work_upload(copy_box, NULL); usleep(200); }
    check_window(map);
}

/* Sanitizers change bake speed a lot, so the test's clock is the bake's: speeds are fractions of what one worker keeps up with, measured */
static double g_slab_ms, g_whole_ms;
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void pool_up(void) {
    CHECK(amb_work_init(&k_files, k_dim, k_voxel, STEP) == AMB_OK, "init");
    amb_work_test_timing(1e-3 * g_whole_ms, 3e-3 * g_whole_ms);   /* this small window's hold, in step with its bakes */
}
static void pool_down(void) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    amb_work_shutdown("test");
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    CHECK(ms < 4000, "shutdown took %.0f ms", ms);
    CHECK(__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) == 0, "%ld module references left", shim_module_refs);
    amb_work_shutdown("again");   /* idempotent */
}

/* --- scenarios ---------------------------------------------------------------------------------------------------------- */
static volatile int g_hook_armed, g_hook_inside, g_hook_release;
static void clamp_cam(double cam[3]) {
    /* keeps the window inside the reference bake, across the wall at x = 0 and the wall at y = 1024 */
    static const double lo[3] = { -640, 300, -220 }, hi[3] = { 640, 1950, 220 };
    for (int a = 0; a < 3; a++) cam[a] = cam[a] < lo[a] ? lo[a] : cam[a] > hi[a] ? hi[a] : cam[a];
}

/* Two steps at once, half the small window: two jobs behind, which is what brings the second worker in. Frames until they are done. */
static void burst(double cam[3], double *dir, const char *level) {
    cam[0] += *dir * 2 * STEP * k_voxel[0];
    if (cam[0] > 500 || cam[0] < -500) *dir = -*dir;
    clamp_cam(cam);
    double t0 = now_ms();
    int frames = 0;
    do {
        amb_work_frame(1, level, 0, cam);
        amb_work_upload(copy_box, NULL);
        usleep(200);
    } while ((amb_work_backlog() || frames++ < 20) && !__atomic_load_n(&g_hook_inside, __ATOMIC_SEQ_CST) && now_ms() - t0 < 2000 + 10 * g_whole_ms);
}

static void scenario_run(const AmbMap *map0) {
    pool_up();
    double cam[3] = { -300, 700, 0 }, vel[3] = { 0, 0, 0 }, way = 1;   /* units per ms */
    converge(0, "maps/a.bsp", cam, 0);
    for (int i = 0; i < 3; i++) burst(cam, &way, "maps/a.bsp");
    double capacity = STEP * k_voxel[0] / g_slab_ms, t0 = now_ms(), t = t0, seg = 0;
    long seen = 0;
    int need_full = 0, resets = 0, teleports = 0, frames = 0;
    g_fail_copies = 1;
    for (;; frames++) {
        AmbWorkStats s;
        amb_work_stats(&s);
        double t1 = now_ms(), dt = t1 - t;
        t = t1;
        if ((s.jobs[AMB_JOB_SLAB] >= 60 && frames > 500) || t - t0 > 20000) break;
        if (t >= seg) {
            seg = t + rndf(2 * g_slab_ms, 8 * g_slab_ms);
            int mode = rnd(6);
            double speed = (mode == 0 ? 0.0 : mode <= 2 ? 0.3 : mode <= 4 ? 0.8 : 3.0) * capacity;
            for (int a = 0; a < 3; a++) vel[a] = rndf(-speed, speed);
            if (rnd(10) == 0) { for (int a = 0; a < 3; a++) cam[a] += rndf(-1800, 1800); teleports++; }
            if (rnd(8) == 0) { memset(g_gpu, 0xff, sizeof g_gpu); need_full = 1; resets++; }
        }
        for (int a = 0; a < 3; a++) cam[a] += vel[a] * dt;
        clamp_cam(cam);
        amb_work_frame(1, "maps/a.bsp", 0, frames % 17 ? cam : NULL);
        /* the proxy's order at the first draw of a frame */
        if (need_full) need_full = !amb_work_full_upload(copy_box, NULL);
        else amb_work_upload(copy_box, NULL);
        if (!need_full) { seen += check_draws(0, cam, 6); check_fold(map0, cam); }
        usleep(300);
    }
    g_fail_copies = 0;
    while (need_full) { need_full = !amb_work_full_upload(copy_box, NULL); usleep(200); }
    converge(0, "maps/a.bsp", cam, 0);
    AmbWorkStats s;
    amb_work_stats(&s);
    printf("  run: %ld draws with weight, %ld taps; %ld whole windows (max %.1f ms) + %ld slabs (max %.1f ms), %ld cancelled in a bake, %ld at the write, %ld uploads, %d resets, %d teleports,"
           " %ld workers, max backlog %ld, A evals %ld + %ld cached, atlas busy %ld, failed copies %ld\n", seen, g_taps, s.jobs[AMB_JOB_WHOLE], s.bake_ms_max[AMB_JOB_WHOLE], s.jobs[AMB_JOB_SLAB],
           s.bake_ms_max[AMB_JOB_SLAB], s.jobs_cancelled, s.write_cancelled, s.uploads, resets, teleports, s.workers_started, s.max_backlog, s.a_evals, s.a_hits, s.atlas_busy, s.copy_failures);
    CHECK(seen > 100, "only %ld draws had weight", seen);
    CHECK(s.jobs[AMB_JOB_SLAB] > 5 && s.jobs[AMB_JOB_WHOLE] >= 1, "too few jobs");
    CHECK(s.workers_started == 2, "%ld workers: the backlog must have brought up the second", s.workers_started);
    CHECK(g_solid_folds > 0, "no fold at a solid origin was tried");
    CHECK(s.loads == 1 && s.load_failures == 0, "map load");
    amb_work_summary("test");
    pool_down();
}

/* Device resets back to back while the workers write: the one moment the draw thread reads the atlas outside a job's turn, which only the atlas lock orders */
static void scenario_reset_storm(void) {
    pool_up();
    double cam[3] = { -300, 700, 0 }, capacity = STEP * k_voxel[0] / g_slab_ms, t = now_ms(), t0 = t;
    long fulls = 0, waits = 0;
    double vel = 0.7 * capacity;
    for (;;) {
        AmbWorkStats s;
        amb_work_stats(&s);
        double t1 = now_ms();
        if (s.jobs[AMB_JOB_SLAB] >= 30 || t1 - t0 > 15000) break;
        cam[0] += vel * (t1 - t);
        cam[1] += 0.5 * vel * (t1 - t);
        t = t1;
        if (cam[0] > 600 || cam[0] < -600) vel = -vel;
        clamp_cam(cam);
        amb_work_frame(1, "maps/a.bsp", 0, cam);
        memset(g_gpu, 0xff, sizeof g_gpu);
        while (!amb_work_full_upload(copy_box, NULL)) waits++;
        fulls++;
        check_draws(0, cam, 6);
    }
    converge(0, "maps/a.bsp", cam, 0);
    AmbWorkStats ss;
    amb_work_stats(&ss);
    printf("  reset storm: %ld re-uploads, %ld waits for a worker's write, %ld slabs moved\n", fulls, waits, ss.jobs[AMB_JOB_SLAB]);
    CHECK(fulls > 20, "only %ld re-uploads", fulls);
    /* only amb_work_full_upload runs here: the queue moves only if it takes a written head along */
    CHECK(ss.jobs[AMB_JOB_SLAB] >= 30, "only %ld slabs moved", ss.jobs[AMB_JOB_SLAB]);
    pool_down();
}

static void scenario_map_changes(void) {
    pool_up();
    double cam[3] = { -300, 700, 0 };
    static const char *const levels[2] = { "maps/a.bsp", "maps/b.bsp" };
    long seen = 0;
    int cur = 0;
    for (int change = 0; change < 24; change++) {
        double until = now_ms() + rndf(0, 3 * g_whole_ms);   /* often in the middle of a read, a parse or a bake */
        while (now_ms() < until) {
            for (int a = 0; a < 3; a++) cam[a] += rndf(-1, 1);
            clamp_cam(cam);
            amb_work_frame(1, levels[cur], cur, cam);
            amb_work_upload(copy_box, NULL);
            seen += check_draws(cur, cam, 4);
            usleep(200);
        }
        if (rnd(4) == 0) {   /* out to the menu for a while */
            amb_work_frame(0, "", 0, NULL);
            CHECK(check_draws(cur, cam, 4) == 0, "weight while not in game");
            usleep(1000);
        }
        cur = rnd(2);
    }
    converge(cur, levels[cur], cam, cur);
    /* every old map is freed once the workers moved on (a leak would also show in the ASan build) */
    for (int i = 0; i < 400; i++) { amb_work_frame(1, levels[cur], cur, cam); usleep(100); }
    AmbWorkStats s;
    amb_work_stats(&s);
    printf("  map changes: %ld generations, %ld maps freed, %ld draws with weight, %ld jobs cancelled in a bake\n", s.generations, s.maps_freed, seen, s.jobs_cancelled);
    CHECK(s.maps_freed > 5, "maps were not freed: %ld", s.maps_freed);
    CHECK(seen > 50, "only %ld draws had weight", seen);
    pool_down();
}

static void scenario_quit_in_bake(void) {
    long jobs_seen = 0;
    for (int i = 0; i < 20; i++) {
        pool_up();
        double cam[3] = { rndf(-400, 400), rndf(500, 1800), rndf(-100, 100) };
        double until = now_ms() + rndf(0, 2 * g_whole_ms);
        while (now_ms() < until) {
            cam[0] += rndf(-20, 20);
            clamp_cam(cam);
            amb_work_frame(1, "maps/a.bsp", 0, cam);
            amb_work_upload(copy_box, NULL);
            usleep(100);
        }
        AmbWorkStats s;
        amb_work_stats(&s);
        jobs_seen += s.jobs[0] + s.jobs[1];
        pool_down();
    }
    printf("  20 quits during bakes: clean, %ld jobs had finished at those moments\n", jobs_seen);
}

static void scenario_quit_in_read(void) {
    long reads0 = g_reads, opens0 = g_opens, closes0 = g_closes, leaked = 0;
    for (int i = 0; i < 20; i++) {
        pool_up();
        double cam[3] = { 0, 1200, 0 };
        amb_work_frame(1, "maps/slow.bsp", 0, cam);
        usleep((useconds_t)(1000 * (5 + rnd(60))));
        pool_down();
        AmbWorkStats s;
        amb_work_stats(&s);
        leaked += s.handles_leaked;
    }
    long reads = g_reads - reads0;
    printf("  20 quits during a map read: clean, %ld chunks read, the largest %ld bytes\n", reads, g_max_chunk);
    CHECK(g_max_chunk <= 1 << 20 && !g_bad_reads, "a read of %ld bytes, %ld outside the file", g_max_chunk, g_bad_reads);
    CHECK(reads > 0 && reads < 20 * 15, "the reads stopped early: %ld chunks", reads);
    CHECK(g_opens - opens0 == g_closes - closes0 + leaked, "%ld files opened, %ld closed, %ld left open after the stop flag", g_opens - opens0, g_closes - closes0, leaked);
    CHECK(leaked > 0, "no quit landed in a read");
    CHECK(!g_pins, "%d references on the filesystem's module left", g_pins);
}

static void scenario_bad_maps(void) {
    pool_up();
    long opens0 = g_opens, closes0 = g_closes;
    double cam[3] = { -100, 1200, 0 };
    static const char *const levels[4] = { "maps/missing.bsp", "maps/lzma.bsp", "maps/short.bsp", "maps/gmabad.bsp" };
    for (int i = 0; i < 4; i++) {
        for (int f = 0; f < 60; f++) {
            amb_work_frame(1, levels[i], 0, cam);
            amb_work_upload(copy_box, NULL);
            CHECK(check_draws(0, cam, 2) == 0, "%s: a draw had weight", levels[i]);
            CHECK(!amb_work_has_data(), "%s: data", levels[i]);
            usleep(500);
        }
    }
    AmbWorkStats s;
    amb_work_stats(&s);
    CHECK(s.load_failures == 3, "%ld load failures, wanted 3", s.load_failures);
    CHECK(s.maps_in_gma == 1 && s.gma_refused == 1 && !strcmp(s.last_gma_map, "maps/gmabad.bsp"), "GMA maps counted %ld, refused %ld, last \"%s\"", s.maps_in_gma,
          s.gma_refused, s.last_gma_map);
    CHECK(g_opens - opens0 == g_closes - closes0 && !g_pins, "%ld files opened, %ld closed, %d pins", g_opens - opens0, g_closes - closes0, g_pins);
    /* a map inside a GMA that ambvol.c gave a handle of its own: read like any other, and its voxels */
    long gma_reads0 = g_gma_reads;
    converge(1, "maps/gma.bsp", cam, 1);
    amb_work_stats(&s);
    CHECK(s.maps_in_gma == 2 && s.gma_refused == 1 && g_gma_reads > gma_reads0 && !g_bad_reads, "the GMA map: counted %ld, refused %ld, %ld reads", s.maps_in_gma,
          s.gma_refused, g_gma_reads - gma_reads0);
    CHECK(s.bytes_read > 0 && s.bytes_read <= (long long)g_bsp_len[1] && s.peak_bytes > 0, "the GMA map's stats: %lld bytes read, %lld held", s.bytes_read, s.peak_bytes);
    /* the next good map still works */
    converge(1, "maps/b.bsp", cam, 1);
    printf("  bad maps: missing, LZMA, truncated and a refused GMA map stay on the cube; a GMA map with its own handle loads (%lld bytes read), the next map loads\n", s.bytes_read);
    pool_down();
}

/* A failed bake: this map's volume goes off at once, and the next map works */
static void scenario_bake_failure(void) {
    pool_up();
    double cam[3] = { -300, 700, 0 };
    converge(0, "maps/a.bsp", cam, 0);
    CHECK(check_draws(0, cam, 400) > 0, "weight before the failure");
    __atomic_store_n(&amb_work_test_fail_bake, 1, __ATOMIC_SEQ_CST);
    cam[0] += 4 * STEP * k_voxel[0] / 3;   /* more than the hysteresis: a slab job */
    int frames = 0;
    for (; frames < 20000 && __atomic_load_n(&amb_work_test_fail_bake, __ATOMIC_SEQ_CST); frames++) { amb_work_frame(1, "maps/a.bsp", 0, cam); amb_work_upload(copy_box, NULL); usleep(200); }
    CHECK(!__atomic_load_n(&amb_work_test_fail_bake, __ATOMIC_SEQ_CST), "no bake ran");
    for (int i = 0; i < 200; i++) { amb_work_frame(1, "maps/a.bsp", 0, cam); amb_work_upload(copy_box, NULL); usleep(500); }
    long after = check_draws(0, cam, 50);
    CHECK(after == 0, "%ld draws still have weight after the failed bake", after);
    AmbWorkStats s;
    amb_work_stats(&s);
    long slabs = s.jobs[AMB_JOB_SLAB];
    for (int i = 0; i < 100; i++) { cam[1] += 2; amb_work_frame(1, "maps/a.bsp", 0, cam); amb_work_upload(copy_box, NULL); usleep(200); }
    amb_work_stats(&s);
    CHECK(s.jobs[AMB_JOB_SLAB] == slabs, "jobs went on after the failure");
    converge(1, "maps/b.bsp", cam, 1);
    printf("  bake failure: the box goes empty, no more jobs, the next map converges\n");
    pool_down();
}

/* The rules that only show on a bake long enough to interrupt: 64 x 64 x 32. Nothing is uploaded, so the fake texture's size does not matter. */
static const int k_big[3] = { 64, 64, 32 };
static double g_big_ms;
static void big_up(const double cam[3], const char *level, int hdr) {
    CHECK(amb_work_init(&k_files, k_big, k_voxel, 8) == AMB_OK, "init");
    /* until the worker has the whole window in hand */
    AmbWorkStats s;
    int frames = 0;
    do {
        amb_work_frame(1, level, hdr, cam);
        amb_work_stats(&s);
        usleep(200);
    } while (!s.loads && ++frames < 50000);
    amb_work_frame(1, level, hdr, cam);
    usleep((useconds_t)(g_big_ms * 0.25 * 1000));
    amb_work_stats(&s);
    CHECK(s.loads == 1 && !s.jobs[AMB_JOB_WHOLE], "the whole window should still be baking");
}
static void wait_cancelled(const double cam[3], const char *level, int hdr, const char *what) {
    double t0 = now_ms(), limit = g_big_ms * 0.3;
    AmbWorkStats s;
    amb_work_stats(&s);
    long c0 = s.jobs_cancelled;
    while (now_ms() - t0 < limit && s.jobs_cancelled == c0) {
        amb_work_frame(1, level, hdr, cam);
        usleep(200);
        amb_work_stats(&s);
    }
    CHECK(s.jobs_cancelled == c0 + 1, "%s did not stop the bake within %.0f ms", what, limit);
}
static void scenario_big_cancel(void) {
    double cam[3] = { -100, 1200, 0 }, far[3] = { -2000, 1200, 0 };
    /* a quit stops the bake within about a row: much less than the rest of it */
    double worst = 0;
    for (int i = 0; i < 3; i++) {
        big_up(cam, "maps/a.bsp", 0);
        double t0 = now_ms();
        amb_work_shutdown("test");
        double ms = now_ms() - t0;
        worst = ms > worst ? ms : worst;
        CHECK(ms < g_big_ms * 0.3, "the quit took %.1f ms, the whole bake %.1f ms: the bake was not cancelled", ms, g_big_ms);
        CHECK(__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) == 0, "module references left");
    }
    /* a map change, more than once: the old map must outlive the bake reading it, and a free under it only shows if the bake is in a stage that reads the map */
    for (int i = 0; i < 5; i++) {
        big_up(cam, "maps/a.bsp", 0);
        wait_cancelled(cam, "maps/b.bsp", 1, "a map change");
        amb_work_shutdown("test");
    }
    /* a jump */
    big_up(cam, "maps/a.bsp", 0);
    wait_cancelled(far, "maps/a.bsp", 0, "a jump");
    amb_work_shutdown("test");
    /* behind an unfinished whole window, walking piles up slabs but brings no second worker */
    big_up(cam, "maps/a.bsp", 0);
    double t0 = now_ms(), walk[3] = { cam[0], cam[1], cam[2] };
    AmbWorkStats s;
    int most = 0;
    while (now_ms() - t0 < g_big_ms * 0.3) {
        walk[1] += 16;
        amb_work_frame(1, "maps/a.bsp", 0, walk);
        int b = amb_work_backlog();
        most = b > most ? b : most;
        usleep(1000);
    }
    amb_work_stats(&s);
    CHECK(most >= 2, "walking made a backlog of only %d", most);
    CHECK(s.workers_started == 1 && !s.jobs[AMB_JOB_WHOLE], "%ld workers behind an unfinished whole window", s.workers_started);
    amb_work_shutdown("test");
    printf("  cancel rules on a 64 x 64 x 32 window (a bake takes %.0f ms): quit in %.1f ms at worst; map change and jump stop the bake; backlog %d behind a whole window brings no second worker\n",
           g_big_ms, worst, most);
}

/* A worker holding the head, already past its turn, is cancelled by a jump while it waits for the atlas. The whole window replacing it gets there first; the
 * stale voxels must not land on top: a later re-upload would send them to the texture. */
static void between_locks(int worker) {
    int armed = 1;
    if (worker != 1 || !__atomic_compare_exchange_n(&g_hook_armed, &armed, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&g_hook_inside, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_hook_release, __ATOMIC_SEQ_CST)) usleep(200);
}
static void scenario_stale_write(void) {
    pool_up();
    double cam[3] = { -300, 700, 0 }, t0 = now_ms(), way = 1;
    converge(0, "maps/a.bsp", cam, 0);
    amb_work_test_between_locks = between_locks;
    __atomic_store_n(&g_hook_armed, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_hook_inside, __ATOMIC_SEQ_CST) && now_ms() - t0 < 20000) burst(cam, &way, "maps/a.bsp");
    CHECK(g_hook_inside, "worker 1 never reached its write");
    double there[3] = { cam[0] < 0 ? 500 : -500, 1500, 100 };
    /* the jump, and the replacing window all the way into the texture, with worker 1 still in between its locks */
    for (int i = 0; i < 2; i++) { amb_work_frame(1, "maps/a.bsp", 0, there); usleep(200); }
    converge(0, "maps/a.bsp", there, 0);
    __atomic_store_n(&g_hook_release, 1, __ATOMIC_SEQ_CST);
    usleep(50000);
    /* what the worker did to the atlas shows in the next re-upload */
    memset(g_gpu, 0xff, sizeof g_gpu);
    for (int i = 0; i < 2000 && !amb_work_full_upload(copy_box, NULL); i++) usleep(200);
    check_window(0);
    AmbWorkStats s;
    amb_work_stats(&s);
    printf("  stale job between its locks: %ld writes dropped, the atlas stays right\n", s.write_cancelled);
    CHECK(s.write_cancelled >= 1, "the stale job was not seen at its write");
    amb_work_test_between_locks = NULL;
    __atomic_store_n(&g_hook_inside, 0, __ATOMIC_SEQ_CST);
    pool_down();
}

/* An X and a Y slab share the texels of a corner, for different voxels. The first job's worker is made slow to write, so the second job's would get to the atlas
 * first if it did not wait its turn. Every voxel of the window is checked after each pair. */
static void slow_worker0(int worker) {
    if (worker == 0) usleep((useconds_t)(g_slab_ms * 1000));
}
static void scenario_corners(void) {
    pool_up();
    double cam[3] = { -300, 700, 0 }, way = 1;
    converge(0, "maps/a.bsp", cam, 0);
    amb_work_test_between_locks = slow_worker0;
    for (int i = 0; i < 16; i++) {
        cam[0] += way * 1.5 * STEP * k_voxel[0];
        cam[1] += way * 1.5 * STEP * k_voxel[1];
        if (cam[0] > 500 || cam[0] < -500) way = -way;
        clamp_cam(cam);
        converge(0, "maps/a.bsp", cam, 0);
    }
    amb_work_test_between_locks = NULL;
    AmbWorkStats s;
    amb_work_stats(&s);
    printf("  X and Y slabs in pairs, the first writer slow: %ld slabs on %ld workers, every window right\n", s.jobs[AMB_JOB_SLAB], s.workers_started);
    CHECK(s.workers_started == 2 && s.jobs[AMB_JOB_SLAB] >= 16, "%ld workers, %ld slabs", s.workers_started, s.jobs[AMB_JOB_SLAB]);
    pool_down();
}

/* No second thread to be had: one attempt, one log line, and the first worker carries on */
static void scenario_second_worker_fails(void) {
    pool_up();
    double cam[3] = { -300, 700, 0 }, t0 = now_ms(), way = 1;
    __atomic_store_n(&shim_thread_calls, 0, __ATOMIC_SEQ_CST);
    shim_thread_fail_from = 2;
    AmbWorkStats s;
    long calls = 0;
    converge(0, "maps/a.bsp", cam, 0);
    do {
        burst(cam, &way, "maps/a.bsp");
        amb_work_stats(&s);
        calls = __atomic_load_n(&shim_thread_calls, __ATOMIC_SEQ_CST);
    } while ((s.jobs[AMB_JOB_SLAB] < 15 || calls < 2) && now_ms() - t0 < 15000);
    printf("  second worker unavailable: %ld thread attempts, %ld slabs on one worker\n", calls, s.jobs[AMB_JOB_SLAB]);
    CHECK(calls == 2 && s.workers_started == 1, "%ld attempts, %ld workers", calls, s.workers_started);
    CHECK(s.jobs[AMB_JOB_SLAB] >= 15, "only %ld slabs", s.jobs[AMB_JOB_SLAB]);
    CHECK(__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) == 1, "the failed attempt kept its module reference");
    shim_thread_fail_from = 0;
    pool_down();
}

/* A read that outlives the shutdown bound: the filesystem's module stays referenced under it, and after it returns the worker does not call the filesystem
 * again. Takes the full bound once. Last: the pool's memory stays allocated by design after this. */
static void scenario_stuck_read(void) {
    pool_up();
    double cam[3] = { -100, 1200, 0 };
    long closes0 = g_closes, opens0 = g_opens;
    amb_work_frame(1, "maps/stuck.bsp", 0, cam);
    double t0 = now_ms();
    while (!__atomic_load_n(&g_stuck_inside, __ATOMIC_SEQ_CST) && now_ms() - t0 < 5000) usleep(500);
    CHECK(g_stuck_inside, "the read did not start");
    CHECK(g_pins_while_stuck == 1, "%d references on the filesystem's module during the read", g_pins_while_stuck);
    t0 = now_ms();
    amb_work_shutdown("test");
    double ms = now_ms() - t0;
    CHECK(ms >= 4900 && ms < 7000, "the bounded wait took %.0f ms", ms);
    CHECK(__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) == 1, "the stuck worker must keep its own module reference");
    CHECK(__atomic_load_n(&g_pins, __ATOMIC_SEQ_CST) == 1, "the stuck worker must keep the filesystem's module");
    __atomic_store_n(&g_stuck_release, 1, __ATOMIC_SEQ_CST);
    t0 = now_ms();
    while (__atomic_load_n(&shim_module_refs, __ATOMIC_SEQ_CST) && now_ms() - t0 < 5000) usleep(500);
    CHECK(!shim_module_refs, "the worker did not exit after its read returned");
    CHECK(!g_pins, "the filesystem's module reference was not released");
    long closes = __atomic_load_n(&g_closes, __ATOMIC_SEQ_CST) - closes0;
    CHECK(closes == 0, "the filesystem was called after the stop flag: %ld closes", closes);
    CHECK(g_opens - opens0 == 1, "opens");
    printf("  read stuck past the bound: shutdown returned at %.0f ms, the worker left later, no call into the filesystem after the stop flag\n", ms);
}

static const struct { const char *name; void (*fn)(void); } k_scenarios[] = {
    { "storm", scenario_reset_storm }, { "maps", scenario_map_changes }, { "quitbake", scenario_quit_in_bake }, { "quitread", scenario_quit_in_read },
    { "bad", scenario_bad_maps }, { "bakefail", scenario_bake_failure }, { "big", scenario_big_cancel }, { "stale", scenario_stale_write },
    { "corners", scenario_corners }, { "worker1", scenario_second_worker_fails }, { "stuck", scenario_stuck_read } };

int main(int argc, char **argv) {
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_bsp[0] = synth_bsp_variant(0, 0, 0, &g_bsp_len[0]);
    g_bsp[1] = synth_bsp_variant(1, 0, 37, &g_bsp_len[1]);   /* L4D2 lump order, other colours */
    g_lzma = synth_bsp_variant(0, 1, 0, &g_lzma_len);
    slow_head();
    AmbMap *ref[2] = { NULL, NULL };
    AmbBakeParams p;
    amb_params_default(&p);
    for (int m = 0; m < 2; m++) {
        CHECK(amb_map_load(g_bsp[m], g_bsp_len[m], m, &ref[m]) == AMB_OK, "reference map %d", m);
        g_refv[m] = malloc((size_t)k_ref.dim[0] * (size_t)k_ref.dim[1] * (size_t)k_ref.dim[2] * AMB_VOXEL_FLOATS * sizeof(float));
        CHECK(g_refv[m] && amb_bake_region(ref[m], &p, &k_ref, g_refv[m], NULL) == AMB_OK, "reference bake %d", m);
    }
    if (!g_fails) {
        static float scratch[16 * 16 * 8 * AMB_VOXEL_FLOATS];
        const AmbBox whole = { { -8, 8, -4 }, { 16, 16, 8 } }, slab = { { -8, 8, -4 }, { 4, 16, 8 } }, big = { { -32, 16, -16 }, { 64, 64, 32 } };
        double t0 = now_ms();
        amb_bake_region(ref[0], &p, &whole, scratch, NULL);
        double t1 = now_ms();
        amb_bake_region(ref[0], &p, &slab, scratch, NULL);
        g_whole_ms = t1 - t0;
        g_slab_ms = now_ms() - t1;
        float *bv = malloc(64 * 64 * 32 * AMB_VOXEL_FLOATS * sizeof(float));
        t0 = now_ms();
        amb_bake_region(ref[0], &p, &big, bv, NULL);
        g_big_ms = now_ms() - t0;
        free(bv);
        printf("  one worker on this build: a whole window %.1f ms, a slab %.1f ms, a 64 x 64 x 32 window %.1f ms\n", g_whole_ms, g_slab_ms, g_big_ms);
    }
    if (!g_fails) {
        if (!only || !strcmp(only, "run")) scenario_run(ref[0]);
        for (size_t i = 0; i < sizeof k_scenarios / sizeof k_scenarios[0]; i++)
            if (!only || !strcmp(only, k_scenarios[i].name)) k_scenarios[i].fn();
    }
    for (int m = 0; m < 2; m++) { amb_map_free(ref[m]); free(g_refv[m]); free(g_bsp[m]); }
    free(g_lzma);
    printf(g_fails ? "WORKTEST FAIL: %ld\n" : "WORKTEST OK\n", g_fails);
    return g_fails ? 1 : 0;
}
