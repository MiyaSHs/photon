/* P6 sun (analysis/22, analysis/24): the sun for draws whose engine light setup P6 skips. The engine traced its emit_skylight for every model
 * from the light cache entry's point and, if sunlit, sent it to the shader API as a directional LightDesc_t; P6 removes that, so for a
 * suppressed draw p6.c asks here, once per draw on the draw thread:
 *   - the map's SunMap comes from the light grid's worker (lgwork.c, lg_work_sun: loaded with the map through the same reader, GMA maps too);
 *   - visibility is src/ambient/sun_vis.h's engine rule (light cache point from the lighting origin, the leaf's sky flags, the MASK_OPAQUE
 *     world trace), through a SunCache keyed like LightcacheGet's entries, rebuilt whenever the map changes (lg_work_sun's serial);
 *   - the light is the engine's own conversion: lg_desc_from_worldlight (FUN_180107ea0) of the emit_skylight, colour times the live light
 *     style scale (d_lightstylevalue / 264, R_SetNonAmbientLightingState), the same LightDesc_t stock SetLight would have received.
 * Draw thread only (the thread P6 suppresses on). Pure apart from the two engine reads behind photon_lg_style_scale and lg_work_sun, so
 * tests/ambient/p6suntest.c runs it on the host with stubs for both. */
#ifndef PHOTON_P6SUN_H
#define PHOTON_P6SUN_H
void photon_p6sun_setup(int on);     /* from p6.c at load: on = P6 wanted; off: every call below returns 0 at once */
/* The sun for a suppressed draw lit at origin: 1 and an 88-byte LightDesc_t (directional) in desc if sunlit, else 0 (shadow, no map yet, no
 * sun on this map, P6 off) */
int photon_p6sun_light(const float origin[3], void *desc);
int photon_p6sun_state(void);   /* the last photon_p6sun_light call on this thread: -1 unknown (off, no map, no sun), 0 shade, 1 lit */
void photon_p6sun_dump(const char *why);
/* the two reads it needs from elsewhere */
float photon_lg_style_scale(int style);   /* lights.c: d_lightstylevalue[style] / 264 for style < 128, else 1; 1 before the engine reads */
#endif
