/* tests/ambient/tracktest.c: src/ambient/amb_track.c against a reference that has what the proxy lacks, the engine's instance identity, and applies the
 * engine's averaging (FUN_180105dc0, FUN_180105bc0) per instance. Scenarios: a walk across steps with random frame times, several draws per model per frame,
 * two props crossing, a teleport, a frame-time spike, r_lightaverage switched off and on, a full table, a reset. Exit 1 on any failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ambient/amb_track.h"

enum { N = AMB_TRACK_FLOATS };
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL tracktest: " __VA_ARGS__); printf("\n"); g_fail = 1; } } while (0)

/* A leaf-like field: constant inside 96-unit cells along x, a different level per cell and per face; bright for x < 0, dark beyond */
static void field(const float p[3], float a[N]) {
    int cell = (int)floorf(p[0] / 96.0f);
    float base = p[0] < 0 ? 0.40f : 0.02f;
    for (int i = 0; i < N; i++) a[i] = base * (1.0f + 0.3f * (float)(((cell * 7 + i * 3) % 5)) / 4.0f) + 0.001f * (float)i;
}

typedef struct { int init; float a[N]; double stamp; } Ref;
static void ref_step(Ref *r, const float cur[N], double now, float rate, int average, float out[N]) {
    if (!average || rate == 0.0f) { memcpy(out, cur, sizeof(float) * N); return; }
    if (!r->init) { r->init = 1; memcpy(r->a, cur, sizeof r->a); r->stamp = now; memcpy(out, cur, sizeof(float) * N); return; }
    double dt = now - r->stamp;
    if (dt > 0) r->stamp = now; else dt = 0;
    float k = expf((float)(-dt * rate));
    if (k > 1) k = 1;
    for (int i = 0; i < N; i++) {
        float v = cur[i] - (cur[i] - r->a[i]) * k;
        r->a[i] = v < 0 ? 0 : v > 8 ? 8 : v;
    }
    memcpy(out, r->a, sizeof(float) * N);
}
static float maxdiff(const float *x, const float *y) {
    float d = 0;
    for (int i = 0; i < N; i++) d = fmaxf(d, fabsf(x[i] - y[i]));
    return d;
}
static unsigned g_seed = 1;
static double frand(void) { g_seed = g_seed * 1103515245u + 12345u; return (double)((g_seed >> 8) & 0xffff) / 65536.0; }

static AmbTrack g_t;
static AmbTrackStats g_last;   /* the stats of the last run() */

/* m models along their own paths; each frame every model draws `draws` times; frame times from dt(). Returns the largest error over frames not excused. */
typedef void (*PathFn)(int model, int frame, float p[3]);
typedef double (*DtFn)(int frame);
static float run(const char *name, int m, int frames, int draws, PathFn path, DtFn dtf, float rate, int (*excused)(int model, int frame), int (*avg)(int frame)) {
    static Ref ref[64];
    memset(ref, 0, sizeof ref);
    amb_track_reset(&g_t);
    double now = 100.0;
    float worst = 0;
    int wf = -1, wm = -1;
    for (int f = 0; f < frames; f++) {
        now += dtf(f);
        int av = avg ? avg(f) : 1;
        amb_track_frame(&g_t, (unsigned long long)(1000 + f), now, rate, av);
        for (int mi = 0; mi < m; mi++) {
            float p[3], cur[N], want[N], got[N];
            path(mi, f, p);
            field(p, cur);
            ref_step(&ref[mi], cur, now, rate, av, want);
            for (int d = 0; d < draws; d++) {
                amb_track_draw(&g_t, p, cur, got);
                float e = maxdiff(got, want);
                if ((!excused || !excused(mi, f)) && e > worst) { worst = e; wf = f; wm = mi; }
            }
        }
    }
    printf("  %-28s worst %.6f (frame %d, model %d); matched %lld, reused %lld, created %lld, ambiguous %lld, joined %lld, dropped %lld, overflow %lld\n", name, worst,
           wf, wm, g_t.st.matched, g_t.st.reused, g_t.st.created, g_t.st.ambiguous, g_t.st.joined, g_t.st.dropped, g_t.st.overflow);
    g_last = g_t.st;
    memset(&g_t.st, 0, sizeof g_t.st);
    return worst;
}

static void walk(int m, int f, float p[3]) { p[0] = -400.0f + 8.0f * (float)f; p[1] = 200.0f * (float)m; p[2] = 0; }
static double dt_random(int f) { (void)f; return 1.0 / 240.0 + frand() * (1.0 / 4.0 - 1.0 / 240.0); }
static double dt_240(int f) { (void)f; return 1.0 / 240.0; }
static double dt_spike(int f) { return f == 60 ? 2.0 : 1.0 / 30.0; }
/* two props crossing at 20 u a frame, 10 u apart sideways: one from the bright side, one from the dark */
static void cross(int m, int f, float p[3]) {
    float x = -310.0f + 20.0f * (float)f;
    p[0] = m == 0 ? x : -x; p[1] = m == 0 ? 0.0f : 10.0f; p[2] = 0;
}
/* a teleport at frame 40, from the bright side to the dark one */
static void tele(int m, int f, float p[3]) { (void)m; p[0] = f < 40 ? -500.0f + 2.0f * (float)f : 2500.0f + 2.0f * (float)f; p[1] = 0; p[2] = 0; }
static int tele_excused(int m, int f) { (void)m; return f == 40; }
static int avg_toggle(int f) { return f < 30 || f >= 60; }
static void many(int m, int f, float p[3]) { p[0] = -2000.0f + 64.0f * (float)m + 4.0f * (float)f; p[1] = 0; p[2] = 0; }

int main(void) {
    printf("tracktest: the tracker against an engine with instance identity\n");
    CHECK(run("walk, random frame times", 1, 200, 1, walk, dt_random, 5.0f, NULL, NULL) < 1e-5f, "walk with random frame times");
    CHECK(run("walk at 240 fps", 1, 400, 1, walk, dt_240, 5.0f, NULL, NULL) < 1e-5f, "walk at 240 fps");
    CHECK(run("walk, r_lightinterp 0.5", 1, 200, 1, walk, dt_random, 0.5f, NULL, NULL) < 1e-5f, "walk at rate 0.5");
    CHECK(run("three draws per model", 3, 120, 3, walk, dt_random, 5.0f, NULL, NULL) < 1e-5f, "several draws per model");
    CHECK(g_last.created == 3 && g_last.reused == 3 * 120 * 2 && g_last.ambiguous < 10, "several draws per model: %lld tracks for 3 models, %lld reused, %lld ambiguous",
          g_last.created, g_last.reused, g_last.ambiguous);
    CHECK(run("two props crossing", 2, 30, 1, cross, dt_240, 5.0f, NULL, NULL) < 1e-5f, "crossing props swapped");
    CHECK(run("teleport (its frame excused)", 1, 80, 1, tele, dt_240, 5.0f, tele_excused, NULL) < 1e-5f, "teleport: the history was not joined");
    CHECK(run("frame-time spike of 2 s", 1, 120, 1, walk, dt_spike, 5.0f, NULL, NULL) < 1e-5f, "frame-time spike");
    CHECK(run("r_lightaverage off, then on", 1, 100, 1, walk, dt_random, 5.0f, NULL, avg_toggle) < 1e-5f, "r_lightaverage toggle");
    /* a full table: the overflow is counted and gets the current value */
    {
        amb_track_reset(&g_t);
        amb_track_frame(&g_t, 1, 1.0, 5.0f, 1);
        float p[3], cur[N], got[N];
        for (int mi = 0; mi < AMB_TRACK_MAX + 20; mi++) { many(mi, 0, p); field(p, cur); amb_track_draw(&g_t, p, cur, got); }
        CHECK(g_t.st.overflow == 20 && g_t.st.created == AMB_TRACK_MAX, "overflow: %lld overflows, %lld created", g_t.st.overflow, g_t.st.created);
        amb_track_reset(&g_t);
        CHECK(g_t.st.overflow == 20, "reset keeps the stats");
        int any = 0;
        for (int i = 0; i < AMB_TRACK_MAX; i++) any |= g_t.e[i].used;
        CHECK(!any && !g_t.have_frame, "reset leaves tracks");
        printf("  full table: overflow %lld of %d draws\n", g_t.st.overflow, AMB_TRACK_MAX + 20);
    }
    if (g_fail) { printf("TRACKTEST FAIL\n"); return 1; }
    printf("TRACKTEST OK\n");
    return 0;
}
