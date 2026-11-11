/* PT-RT (TASKS, analysis/43): the live path-traced mode on the Vulkan bridge. See ptrt.c. */
#ifndef PHOTON_PTRT_H
#define PHOTON_PTRT_H
#include "ambwork.h"
int photon_ptrt_on(void);
void photon_ptrt_set_dyn_lamps(const float *rec, int n);   /* SP layer 2: this frame's dynamic lamps (VOX_LIGHT_WORDS floats each) */
int photon_ptrt_models(void);
void *photon_ptrt_motion_tex(void);      /* the motion-vector texture its pass reads (shadows.c's, wrapped at bind; NULL: none) */
void photon_ptrt_motion_drawn(int on);
void photon_ptrt_model_nrm_drawn(int on);
void photon_ptrt_pbr_drawn(int on);   /* shadows.c: this frame's world PBR channels are drawn (PBR step 3: glossy reflections) */
void *photon_ptrt_model_nrm_tex(void);       /* the model-normal texture its pass wrapped (NULL: none) */   /* shadows.c: this frame's model normals are drawn */   /* shadows.c: this frame's motion vectors are in it (1) or not (0) */   /* models lit by PT-RT's composite (their shaders draw the surface colour) */                                           /* PHOTON_PTRT=1 */
int photon_ptrt_world_copy(float **out, unsigned *n, long *seq);
float *photon_ptrt_world_ex_copy(unsigned n);   /* PBR packs: per triangle 14 floats (atlas tile or -1, u / v axes, corner uvs), NULL: no pack */
#include <stdint.h>
int photon_ptrt_atlas_with(long *seq, void (*fn)(void *ctx, const uint8_t *rmh, const uint8_t *nrm), void *ctx);   /* the PBR atlases when newer */
float *photon_ptrt_world_pbr_copy(unsigned n);   /* PBR: per triangle roughness, metalness of that copy (NULL: none); caller frees */    /* the world's triangles (18 floats: position, smooth normal per corner) when newer; caller frees */
void photon_ptrt_init(void);                                        /* registers the bridge pass */
void photon_ptrt_frame(const AmbWorkFiles *files, const char *level);   /* lights.c, each frame: the map for the worker */
void photon_ptrt_main_view(void);                                   /* shadows.c at the main view's end (device thread): runs the pass */
#endif
