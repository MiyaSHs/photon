/* NANITE (analysis/44): a model material's base texture for Photon's own prop drawing: the .vmt's $basetexture (and whether it cuts out
 * by alpha), and that .vtf's image data from the first mip no larger than max_side down to 4 px, in its own block format (DXT1/3/5 kept
 * compressed: the GPU samples them as BC1/2/3) or as RGBA8 for the common uncompressed formats. Files come through the caller's reader.
 * Pure C. */
#ifndef PHOTON_MTL_TEX_H
#define PHOTON_MTL_TEX_H
#include <stddef.h>
#include <stdint.h>

enum { MTL_BC1 = 1, MTL_BC2 = 2, MTL_BC3 = 3, MTL_RGBA8 = 4 };
typedef struct MtlTex {
    int format, w, h, mips;     /* w x h: the first mip kept; mips: how many follow (each half the last, down to 4 px) */
    uint8_t *data; size_t size; /* the mips one after the other, largest first */
    size_t mip_ofs[16];
    int alphatest;              /* $alphatest or $translucent: cut out where alpha < 0.5 */
    char base[128];             /* the $basetexture path (for logs) */
} MtlTex;

typedef uint8_t *(*MtlReadFn)(void *ctx, const char *path, size_t *n);
/* material: e.g. "models/props_foliage/tree_deciduous_01a_leaves" (no "materials/", no ".vmt"). 0 on success. */
int mtl_tex_load(const char *material, MtlReadFn read, void *ctx, int max_side, MtlTex *out);
void mtl_tex_free(MtlTex *t);
#endif
