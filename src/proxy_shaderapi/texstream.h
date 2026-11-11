/* TEXSTREAM (TASKS): texture streaming, first version (PHOTON_TEXSTREAM=1). See texstream.c. */
#ifndef PHOTON_TEXSTREAM_H
#define PHOTON_TEXSTREAM_H
#include <windows.h>
#include <d3d9.h>
int photon_texstream_on(void);
void photon_texstream_created(void *tex, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool);   /* dmirror, after CreateTexture */
void photon_texstream_bound(void *tex);   /* dmirror's SetTexture hook */
void photon_texstream_tick(void);         /* dmirror's EndScene hook (the device thread) */
#endif
