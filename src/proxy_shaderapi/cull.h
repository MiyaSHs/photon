/* T4.7 ceiling (test only, PHOTON_TEST_CULL; analysis/25): a D3D9 occlusion query around every model draw (CModelRender::DrawModelExecute,
 * so props, ragdolls, NPCs, vehicles and weapons), keyed by (entity index, model instance, flags, model). A draw whose last answered query
 * drew no sample counts as hidden. PHOTON_TEST_CULL=1 only measures; =N (N >= 2) skips hidden draws except every Nth frame, when they are
 * drawn and queried again: the frame time then shows (N-1)/N of what skipping them would save, with pop-in artefacts. The count is a lower
 * bound, as the engine does not draw front to back. Sync mode only (the draw thread issues the device calls); particles are not covered. */
#ifndef PHOTON_CULL_H
#define PHOTON_CULL_H
#include <stdint.h>
void photon_cull_setup(void);
int photon_cull_on(void);
int photon_cull_begin(const uint8_t *info);   /* -1: skip this draw; > 0: a query is open, pass it to photon_cull_end; 0: draw as usual */
void photon_cull_end(int h);
void photon_cull_device_lost(void);
void photon_cull_pre_present(void);          /* T4.7a, from the Present hook before the real Present: the frame's queued box queries */
void photon_cull_dump(const char *why);
int photon_cull_bounds(const void *model, float mins[3], float maxs[3]);   /* T4.7b: the engine's GetModelRenderBounds, cached */
#endif
