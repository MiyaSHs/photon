/* SP layer 1: screen passes from shader packs (packs.c, analysis/47) */
#ifndef PHOTON_PACKS_H
#define PHOTON_PACKS_H
int photon_packs_active(void);   /* a pass is loaded */
void photon_packs_poll(void);    /* main thread, each frame */
int photon_packs_listed(void);   /* active.txt exists (or passes are loaded): the main view's end must run */
void photon_packs_main_view_end(void *backbuffer, void *scene_depth, unsigned w, unsigned h);   /* device thread, after Photon's own passes */
void photon_packs_device_lost(void);
#endif
