/* SP layer 1b: the texture stage (texfilter.c, analysis/47) */
#ifndef PHOTON_TEXFILTER_H
#define PHOTON_TEXFILTER_H
#include <windows.h>
void *photon_texf_bind(DWORD stage, void *tex);   /* dmirror SetTexture: the texture to bind (a pack's filtered copy, or tex) */
void photon_texf_tick(void);                      /* dmirror EndScene: reload, conversions */
void photon_texf_level_change(void);
void photon_texf_device_lost(void);
#endif
