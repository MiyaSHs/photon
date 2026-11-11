/* P6 sun: see p6sun.h. */
#include <stdint.h>
#include <string.h>
#include "p6sun.h"
#include "lgwork.h"
#include "log.h"
#include "../ambient/lg_light.h"
#include "../ambient/sun_vis.h"

#ifndef P6SUN_MUTANT
#define P6SUN_MUTANT 0   /* test only (tests/ambient/p6suntest.c): 1 the cache kept across a map change, 2 no light style scale, 3 the trace from the origin, 4 the exact-origin memo kept across a map change */
#endif

enum { CACHE_LOG2 = 14 };   /* 16384 entries, 128 KB: a busy scene has a few thousand light cache cells */

static int g_on;
static const SunMap *g_map;   /* the map g_cache was built on */
static long g_serial;
static SunCache *g_cache;
static LgDesc g_desc;          /* the emit_skylight's LightDesc_t at style scale 1 */
static int g_style, g_has_sun;
static long long g_calls, g_lit, g_nomap, g_nosun, g_rebuilds, g_memo_hits;
/* In front of the cache: the answer for an exact origin (bit equal), so a prop at rest skips the cache key's leaf walk. Valid for the
 * map generation g_memo_gen (rebuild bumps it); 0 is never a generation. */
enum { MEMO_LOG2 = 12 };
typedef struct { float o[3]; unsigned gen; int v; } Memo;
static Memo g_memo[1u << MEMO_LOG2];
static unsigned g_memo_gen = 1;

void photon_p6sun_setup(int on) { g_on = on; }

static void rebuild(const SunMap *m, long serial) {
    sun_cache_free(g_cache);
    g_cache = NULL;
    g_map = m;
    g_serial = serial;
    g_has_sun = 0;
    g_rebuilds++;
    if (P6SUN_MUTANT != 4 && ++g_memo_gen == 0) { memset(g_memo, 0, sizeof g_memo); g_memo_gen = 1; }
    if (!m) return;
    SunInfo si;
    sun_map_info(m, &si);
    if (!si.has_sun) return;
    LgWorldLight w;
    memset(&w, 0, sizeof w);
    w.type = LG_EMIT_SKYLIGHT;
    w.style = si.sun_style;
    w.cluster = -1;
    for (int k = 0; k < 3; k++) { w.intensity[k] = si.sun_intensity[k]; w.normal[k] = si.sun_normal[k]; }
    if (!lg_desc_from_worldlight(&w, &g_desc)) return;
    g_style = si.sun_style;
    g_cache = sun_cache_new(m, CACHE_LOG2);
    g_has_sun = g_cache != NULL;
    if (!g_cache) photon_log("p6sun: no memory for the sun cache: no sun for P6 draws");
}

static int g_last_state = -1;   /* the last call's answer: -1 unknown (off, no map, no sun), 0 shade, 1 lit */
int photon_p6sun_state(void) { return g_last_state; }
int photon_p6sun_light(const float origin[3], void *desc) {
    g_last_state = -1;
    if (!g_on) return 0;
    g_calls++;
    long serial;
    const SunMap *m = lg_work_sun(&serial);
    if (m != g_map || (serial != g_serial && P6SUN_MUTANT != 1)) rebuild(m, serial);
    if (!m) { g_nomap++; return 0; }
    if (!g_has_sun) { g_nosun++; return 0; }
    uint32_t b[3];
    memcpy(b, origin, sizeof b);
    uint32_t h = (b[0] * 0x9E3779B1u ^ b[1] * 0x85EBCA77u ^ b[2] * 0xC2B2AE3Du);
    Memo *e = &g_memo[(h ^ h >> 15) & ((1u << MEMO_LOG2) - 1)];
    int v;
    if (e->gen == g_memo_gen && memcmp(e->o, origin, sizeof e->o) == 0) { v = e->v; g_memo_hits++; }
    else {
        double o[3] = { origin[0], origin[1], origin[2] };
        v = P6SUN_MUTANT == 3 ? sun_visible(m, NULL, o) : sun_cache_visible(g_cache, o);
        memcpy(e->o, origin, sizeof e->o); e->gen = g_memo_gen; e->v = v;
    }
    g_last_state = v == SUN_LIT;
    if (v != SUN_LIT) return 0;
    LgDesc d = g_desc;
    float sc = P6SUN_MUTANT == 2 ? 1.0f : photon_lg_style_scale(g_style);
    for (int k = 0; k < 3; k++) d.color[k] *= sc;   /* R_SetNonAmbientLightingState: the converted colour times the style's value / 264 */
    memcpy(desc, &d, sizeof d);
    g_lit++;
    return 1;
}

void photon_p6sun_dump(const char *why) {
    if (!g_on) return;
    SunCacheStats cs = { 0, 0, 0 };
    if (g_cache) sun_cache_stats(g_cache, &cs);
    photon_log("p6sun (%s): suppressed draws asked %lld, sunlit %lld, before a map %lld, on a map without a sun %lld; cache rebuilds %lld; exact-origin hits %lld; this map's"
               " cache: lookups %lld, hits %lld, traces %lld", why ? why : "?", g_calls, g_lit, g_nomap, g_nosun, g_rebuilds, g_memo_hits, cs.lookups, cs.hits, cs.traces);
}
