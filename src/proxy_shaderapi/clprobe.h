/* analysis/30 step 1 (test only, PHOTON_TEST_CLPROBE=1): how often the client calls four IClientRenderable slots per drawn model per frame
 * (16 SetupBones, 20 GetRenderBounds, 21 GetRenderBoundsWorldspace, 45 the fx-blend / render-group update). Each renderable vtable seen at
 * DrawModelExecute (up to 16) gets those slots counted in place (the class table, so every object of the class); nothing is cached or
 * skipped. The summary prints per vtable: calls per frame per slot, draws per frame, and calls per draw. */
#ifndef PHOTON_CLPROBE_H
#define PHOTON_CLPROBE_H
#include <stdint.h>
void photon_clprobe_setup(void);
int photon_clprobe_on(void);
void photon_clprobe_draw(const uint8_t *info);   /* from the DrawModelExecute hook, any thread */
void photon_clprobe_frame(void);
void photon_clprobe_restore(void);
void photon_clprobe_dump(const char *why);
#endif
