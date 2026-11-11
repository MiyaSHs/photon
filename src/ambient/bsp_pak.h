/* A file inside a map's pakfile (lump 40, a zip): stored (uncompressed) entries only, as vbsp writes them. Pure C. */
#ifndef PHOTON_BSP_PAK_H
#define PHOTON_BSP_PAK_H
#include <stddef.h>
#include <stdint.h>
/* the entry's bytes inside the map (no copy), NULL if absent or compressed; name compared case-insensitively, '/' or '\\' */
const uint8_t *bsp_pak_find(const uint8_t *bsp, size_t size, const char *name, size_t *n);
#endif
