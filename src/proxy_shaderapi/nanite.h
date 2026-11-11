/* NANITE (TASKS, analysis/44): static props drawn by Photon on the Vulkan bridge. See nanite.c. */
#ifndef PHOTON_NANITE_H
#define PHOTON_NANITE_H
#include "ambwork.h"
int photon_nanite_on(void);                                             /* PHOTON_NANITE=1 */
void photon_nanite_init(void);                                          /* registers the bridge pass */
void photon_nanite_frame(const AmbWorkFiles *files, const char *level);   /* lights.c, each frame: the map for the worker */
#include "dmirror.h"
const float *photon_nanite_casters(unsigned *n, const PhotonCastRange **r, int *nr, long *seq);   /* PT-RT's sun map: the placements' lowest LODs (15 floats a triangle) and their ranges */
void photon_nanite_main_view(void);
void photon_nanite_set_albedo(int on);                                  /* PT-RT: props with their surface colour alone (lit by its composite) */
void *photon_nanite_depth_tex(void);                                    /* PT-RT's composite: Nanite's D3D depth this frame (NULL: none) */                                     /* shadows.c at the main view's end (device thread) */
#endif
