/* F3 auto LOD in game (analysis/38). Step 1: PHOTON_LOD_PROBE=1 reads one stock model through the engine's filesystem (VBaseFileSystem011,
 * filesystem_stdio.dll) once, to check the interface and slots this worker will use to read model files from any mount (folders, GMAs). */
#ifndef PHOTON_LODWORK_H
#define PHOTON_LODWORK_H
#include <stdint.h>
void photon_lod_frame(void);   /* Present thread, once a frame */
void photon_lod_note_model(const void *model);   /* p6.c DrawModelExecute hook, main thread: the model_t being drawn (PHOTON_LOD=1) */
uint8_t *photon_fs_slurp(const char *name, unsigned *n);   /* a file through the engine's search paths (GAME), malloc'd; NULL if missing */
#endif
