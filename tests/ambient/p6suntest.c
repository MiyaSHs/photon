/* P6 sun glue (src/proxy_shaderapi/p6sun.c) on the host, with stubs for its two outside reads (lg_work_sun, photon_lg_style_scale):
 *   off      P6 off: nothing is asked, nothing is lit;
 *   rule     every answer is src/ambient/sun_vis.h's sun_model_visible at the draw's origin (distinct light cache keys), and a sunlit draw's light
 *            is the engine's conversion of the emit_skylight (lg_desc_from_worldlight) times the live style scale;
 *   maps     a new map (and a new map at the old one's address: the serial) rebuilds the cache; no map, no sun;
 *   bench    the per-draw cost of a P6-like scene (400 props, 100 moving) on one thread.
 * Usage: p6suntest <construct.bsp> <flatgrass.bsp>. Prints P6SUNTEST OK. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/proxy_shaderapi/p6sun.h"
#include "../../src/ambient/sun_vis.h"
#include "../../src/ambient/lg_light.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails < 30) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } fails++; } } while (0)

/* --- stubs ------------------------------------------------------------------------------------------------------ */
static const SunMap *g_stub_map;
static long g_stub_serial, g_stub_calls;
static float g_stub_scale = 1.0f;
const SunMap *lg_work_sun(long *serial) { g_stub_calls++; if (serial) *serial = g_stub_map ? g_stub_serial : 0; return g_stub_map; }
float photon_lg_style_scale(int style) { (void)style; return g_stub_scale; }
void photon_log(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  log: "); vprintf(fmt, a); printf("\n"); va_end(a); }

static uint64_t g_rng = 88172645463325252ull;
static double rndu(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (double)(g_rng >> 11) / 9007199254740992.0; }
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

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

/* the expected light: the engine's conversion of the map's emit_skylight, colour times scale */
static void want_desc(const SunMap *m, float scale, LgDesc *d) {
    SunInfo si;
    sun_map_info(m, &si);
    LgWorldLight w;
    memset(&w, 0, sizeof w);
    w.type = LG_EMIT_SKYLIGHT;
    w.style = si.sun_style;
    for (int k = 0; k < 3; k++) { w.intensity[k] = si.sun_intensity[k]; w.normal[k] = si.sun_normal[k]; }
    lg_desc_from_worldlight(&w, d);
    for (int k = 0; k < 3; k++) d->color[k] *= scale;
}

/* random origins over the world box (far apart: distinct light cache keys), each answer against sun_model_visible */
static void rule(const char *name, const SunMap *m, int n, int *lit_out) {
    SunInfo si;
    sun_map_info(m, &si);
    LgDesc want;
    want_desc(m, g_stub_scale, &want);
    int bad = 0, badd = 0, lit = 0;
    for (int i = 0; i < n; i++) {
        float o[3];
        double od[3];
        for (int k = 0; k < 3; k++) { od[k] = o[k] = (float)(si.world_min[k] * 0.6 + rndu() * (si.world_max[k] - si.world_min[k]) * 0.6); }
        LgDesc got;
        memset(&got, 0x55, sizeof got);
        int v = photon_p6sun_light(o, &got);
        int w = sun_model_visible(m, NULL, od) == SUN_LIT;
        bad += v != w;
        if (v) { lit++; badd += memcmp(&got, &want, sizeof got) != 0; }
    }
    CHECK(bad == 0, "%s: %d of %d answers differ from sun_model_visible", name, bad, n);
    CHECK(badd == 0, "%s: %d sunlit draws got a light other than the engine's conversion times the style scale", name, badd);
    CHECK(lit > n / 20 && lit <= n, "%s: an implausible lit count %d of %d", name, lit, n);
    if (lit_out) *lit_out = lit;
    printf("  %s: %d origins equal sun_model_visible (%d sunlit, each with the engine's light: type %d, colour %.3f %.3f %.3f, direction %.3f %.3f %.3f)\n",
           name, n, lit, want.type, want.color[0], want.color[1], want.color[2], want.dir[0], want.dir[1], want.dir[2]);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: p6suntest <construct.bsp> <flatgrass.bsp>\n"); return 2; }
    size_t n0, n1;
    uint8_t *b0 = read_file(argv[1], &n0), *b1 = read_file(argv[2], &n1);
    SunMap *ma = NULL, *mb = NULL;
    if (!b0 || !b1 || sun_map_load(b0, n0, 1, &ma) != AMB_OK || sun_map_load(b1, n1, 1, &mb) != AMB_OK) { printf("FAIL: maps\n"); return 1; }
    float o[3] = { 0, 0, 0 };
    unsigned char d[88];

    /* off */
    photon_p6sun_setup(0);
    g_stub_map = ma; g_stub_serial = 1;
    CHECK(photon_p6sun_light(o, d) == 0 && g_stub_calls == 0, "off: asked %ld times", g_stub_calls);
    /* on, no map yet */
    photon_p6sun_setup(1);
    g_stub_map = NULL;
    CHECK(photon_p6sun_light(o, d) == 0, "no map: a sun");
    /* construct, style scale 0.75 */
    g_stub_map = ma; g_stub_serial = 1; g_stub_scale = 0.75f;
    rule("gm_construct", ma, 4000, NULL);
    /* the style scale is read live */
    g_stub_scale = 0.25f;
    rule("gm_construct at style scale 0.25", ma, 500, NULL);
    g_stub_scale = 1.0f;
    /* flatgrass */
    g_stub_map = mb; g_stub_serial = 2;
    rule("gm_flatgrass", mb, 2000, NULL);
    /* the same origins on two maps: an answer remembered for an exact origin (p6sun.c's memo) must not outlive its map */
    {
        SunInfo sx;
        sun_map_info(ma, &sx);
        static float oo[2000][3];
        for (int i = 0; i < 2000; i++)
            for (int k = 0; k < 3; k++) oo[i][k] = (float)(sx.world_min[k] * 0.6 + rndu() * (sx.world_max[k] - sx.world_min[k]) * 0.6);
        const SunMap *seq[2] = { ma, mb };
        int bad = 0;
        for (int r = 0; r < 2; r++) {
            g_stub_map = seq[r]; g_stub_serial = 10 + r;
            for (int i = 0; i < 2000; i++) {
                double od[3] = { oo[i][0], oo[i][1], oo[i][2] };
                bad += photon_p6sun_light(oo[i], d) != (sun_model_visible(seq[r], NULL, od) == SUN_LIT);
            }
        }
        CHECK(bad == 0, "the same origins on gm_construct then gm_flatgrass: %d answers differ from sun_model_visible", bad);
        printf("  the same 2000 origins on gm_construct then gm_flatgrass: each answer is its own map's\n");
    }
    /* a new map at the old one's address: only the serial says so */
    SunInfo sa;
    sun_map_info(ma, &sa);
    void *old = mb;
    sun_map_free(mb);
    SunMap *keep[64];
    int nk = 0, same = 0;
    SunMap *mc = NULL;
    while (nk < 64) {
        SunMap *t = NULL;
        if (sun_map_load(b0, n0, 1, &t) != AMB_OK) break;
        if ((void *)t == old) { mc = t; same = 1; break; }
        keep[nk++] = t;
    }
    if (same) {
        g_stub_map = mc; g_stub_serial = 3;
        rule("gm_construct reloaded at gm_flatgrass's address", mc, 1500, NULL);
    } else printf("  (the allocator gave no reused address: the serial case not run)\n");
    for (int i = 0; i < nk; i++) sun_map_free(keep[i]);
    if (!mc) { sun_map_load(b0, n0, 1, &mc); g_stub_map = mc; g_stub_serial = 3; }
    /* back to no map */
    g_stub_map = NULL;
    CHECK(photon_p6sun_light(o, d) == 0, "map gone: a sun");

    /* bench: 400 props on construct's floors, 100 moving 1-6 units a frame, 600 frames */
    g_stub_map = ma; g_stub_serial = 4;
    SunInfo si;
    sun_map_info(ma, &si);
    float pos[400][3], vel[400][3];
    for (int i = 0; i < 400; i++) {
        double p[3] = { -3000 + rndu() * 4000, -3000 + rndu() * 6000, 2000 };
        double down[3] = { p[0], p[1], -2000 };
        SunTrace t;
        sun_trace(ma, NULL, p, down, 1, &t);
        p[2] = p[2] + (down[2] - p[2]) * t.fraction + 24;
        for (int k = 0; k < 3; k++) pos[i][k] = (float)p[k];
        double sp = i < 300 ? 0 : 1 + 5 * rndu(), a = rndu() * 6.283185307;
        vel[i][0] = (float)(sp * cos(a)); vel[i][1] = (float)(sp * sin(a)); vel[i][2] = 0;
    }
    int lit = 0;
    double t0 = now_us();
    for (int f = 0; f < 600; f++)
        for (int i = 0; i < 400; i++) {
            for (int k = 0; k < 3; k++) pos[i][k] += vel[i][k];
            lit += photon_p6sun_light(pos[i], d);
        }
    double t1 = now_us();
    printf("  bench: 400 props x 600 frames on gm_construct: %.3f us per suppressed draw, %.3f ms per frame (%.0f%% sunlit)\n", (t1 - t0) / 240000.0,
           (t1 - t0) / 600.0 / 1000.0, 100.0 * lit / 240000.0);
    photon_p6sun_dump("test");
    sun_map_free(ma);
    sun_map_free(mc);
    free(b0);
    free(b1);
    if (fails) { printf("P6SUNTEST FAIL (%d)\n", fails); return 1; }
    printf("P6SUNTEST OK\n");
    return 0;
}
