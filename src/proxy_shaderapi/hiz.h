/* T4.7b: occlusion culling of model draws against a Hi-Z of the world's depth (hiz.c) */
#ifndef PHOTON_HIZ_H
#define PHOTON_HIZ_H
int photon_hiz_on(void);    /* PHOTON_HIZ=1 */
void photon_hiz_init(void); /* registers the Vulkan pass */
/* device thread, the main view's end: the world-only depth (R32F texture, w x h) and that frame's view-projection rows and eye */
void photon_hiz_main_view_end(void *world_depth_tex, unsigned w, unsigned h, const float vp[4][4], const float cam[3]);
void photon_hiz_view(const float cam[3], const float fwd[3]);   /* the camera now (hiz.c takes it from photon_shadows_view) */
/* main thread at DrawModelExecute in the main view: 1 if the model's padded render bounds under m34 are hidden behind the world */
int photon_hiz_occluded(const float mins[3], const float maxs[3], const float m34[3][4]);
void photon_hiz_note_entity(int entity_index, int hidden);   /* p6.c: the result of an entity's main-view test */
int photon_hiz_hidden(int entity_index);     /* the CPU module (host version 6): hidden at its latest test, at most 2 Hi-Z frames ago */
int photon_hiz_view_moved(void);             /* the CPU module: the camera has moved past the culling's limit (nothing culled) */
#endif
