/* SP layer 3: volumetric fog (fog.c) */
#ifndef PHOTON_FOG_H
#define PHOTON_FOG_H
int photon_fog_on(void);
void photon_fog_main_view_end(void *backbuffer, void *scene_depth, unsigned w, unsigned h);
#endif
