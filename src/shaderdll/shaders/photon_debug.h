/* photon_debug.h: shared between Photon_Debug (photon_debug.c) and Photon_Debug30 (photon_debug30.c). */
#ifndef PHOTON_SHADERS_DEBUG_H
#define PHOTON_SHADERS_DEBUG_H
#include "../baseshader.h"

enum { P_PHOTONCOLOR = P_NUM_STANDARD };   /* $photoncolor, the only shader-specific param of both */

/* One fillrate_vs20 + fillrate_ps20(b) pass with c0 = 2 * ($photoncolor * $color, $alpha) (fillrate_ps20b outputs
 * 0.5 * c0). Snapshot when sh != NULL, else dynamic. Calls ps_draw() exactly once. */
void photon_fillrate_draw(IMaterialVar **p, IShaderShadow *sh, IShaderDynamicAPI *api);
/* The same pass with an explicit colour in place of $photoncolor (Photon_Debug32's no-SM3 fallback draws magenta). */
void photon_fillrate_draw_rgb(IMaterialVar **p, IShaderShadow *sh, IShaderDynamicAPI *api, const float *rgb);

#endif
