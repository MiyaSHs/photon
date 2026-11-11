/* NANITE: a material's base texture. See mtl_tex.h. VMT: the first "$basetexture" value (keys are case-insensitive, quotes optional),
 * "$alphatest" / "$translucent" set to non-zero; "patch" materials followed to their "include" (once). VTF 7.0-7.5: format +52, width +16,
 * height +18, frames +24, mips +56; 7.3+ find the image data through the resource table (tag 0x30), older ones take the end of the file;
 * the image data holds the mips smallest first, each with every frame (frame 0 is used). */
#include "mtl_tex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mtl_tex_free(MtlTex *t) { if (t) { free(t->data); memset(t, 0, sizeof *t); } }

/* the value of `key` in a VMT text (case-insensitive key, quoted or bare value); 1 if found */
static int vmt_value(const char *txt, size_t n, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl < n; i++) {
        if (strncasecmp(txt + i, key, kl) != 0) continue;
        char after = txt[i + kl];
        if (!(after == '"' || after == ' ' || after == '\t')) continue;
        if (i > 0 && !(txt[i - 1] == '"' || isspace((unsigned char)txt[i - 1]))) continue;
        size_t j = i + kl + (after == '"');
        while (j < n && (txt[j] == ' ' || txt[j] == '\t')) j++;
        int q = j < n && txt[j] == '"';
        j += q;
        size_t k = 0;
        while (j < n && k + 1 < cap && txt[j] != '\n' && txt[j] != '\r' && (q ? txt[j] != '"' : !isspace((unsigned char)txt[j]))) out[k++] = txt[j++];
        out[k] = 0;
        return k > 0;
    }
    return 0;
}

static size_t mip_bytes(int fmt, int w, int h) {
    w = w < 1 ? 1 : w; h = h < 1 ? 1 : h;
    switch (fmt) {
    case 13: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8;                 /* DXT1 */
    case 14: case 15: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;       /* DXT3, DXT5 */
    case 0: case 12: case 16: return (size_t)w * h * 4;                        /* RGBA8888, BGRA8888, BGRX8888 */
    case 2: case 3: return (size_t)w * h * 3;                                  /* RGB888, BGR888 */
    default: return 0;
    }
}

int mtl_tex_load(const char *material, MtlReadFn read, void *ctx, int max_side, MtlTex *out) {
    memset(out, 0, sizeof *out);
    char path[300], base[160] = "", tmp[160];
    snprintf(path, sizeof path, "materials/%s.vmt", material);
    size_t n = 0;
    char *vmt = (char *)read(ctx, path, &n);
    if (!vmt) return 1;
    if (!vmt_value(vmt, n, "$basetexture", base, sizeof base) && vmt_value(vmt, n, "include", tmp, sizeof tmp)) {   /* a patch material */
        free(vmt);
        snprintf(path, sizeof path, "%s", tmp);
        vmt = (char *)read(ctx, path, &n);
        if (!vmt) return 1;
        vmt_value(vmt, n, "$basetexture", base, sizeof base);
    }
    if (vmt_value(vmt, n, "$alphatest", tmp, sizeof tmp) && atof(tmp) != 0) out->alphatest = 1;
    if (vmt_value(vmt, n, "$translucent", tmp, sizeof tmp) && atof(tmp) != 0) out->alphatest = 1;
    free(vmt);
    if (!base[0]) return 2;
    for (char *c = base; *c; c++) { if (*c == '\\') *c = '/'; *c = (char)tolower((unsigned char)*c); }
    size_t bl = strlen(base);
    if (bl > 4 && !strcmp(base + bl - 4, ".vtf")) base[bl - 4] = 0;
    snprintf(out->base, sizeof out->base, "%s", base);
    snprintf(path, sizeof path, "materials/%s.vtf", base);
    uint8_t *v = read(ctx, path, &n);
    if (!v) return 3;
    int rc = 4;
    if (n < 80 || memcmp(v, "VTF", 4) != 0) goto done;
    uint32_t vmaj, vmin; memcpy(&vmaj, v + 4, 4); memcpy(&vmin, v + 8, 4);
    uint16_t w, h, frames; memcpy(&w, v + 16, 2); memcpy(&h, v + 18, 2); memcpy(&frames, v + 24, 2);
    int32_t fmt; memcpy(&fmt, v + 52, 4);
    int mips = v[56];
    uint32_t flags; memcpy(&flags, v + 20, 4);
    int faces = (flags & 0x4000) ? 6 : 1, depth = 1;
    if (vmaj == 7 && vmin >= 2) { uint16_t d16; memcpy(&d16, v + 63, 2); depth = d16 ? d16 : 1; }
    if (!mip_bytes(fmt, 4, 4) || mips < 1 || frames < 1 || depth != 1) goto done;
    size_t total = 0, msize[16];
    for (int m = 0; m < mips && m < 16; m++) { msize[m] = mip_bytes(fmt, w >> m, h >> m) * frames * faces; total += msize[m]; }
    size_t img;
    if (vmaj == 7 && vmin >= 3) {
        uint32_t nres; memcpy(&nres, v + 68, 4);
        img = 0;
        for (uint32_t r = 0; r < nres && 80 + 8 * (r + 1) <= n; r++) {
            if (v[80 + 8 * r] == 0x30 && v[81 + 8 * r] == 0 && v[82 + 8 * r] == 0) { uint32_t o; memcpy(&o, v + 84 + 8 * r, 4); img = o; }
        }
        if (!img) goto done;
    } else img = n - total;
    if (img + total > n) goto done;
    /* the first mip no larger than max_side, then the rest down to 4 px (block formats need whole blocks) */
    int first = 0;
    while (first < mips - 1 && ((w >> first) > max_side || (h >> first) > max_side)) first++;
    int last = first;
    while (last + 1 < mips && (w >> (last + 1)) >= 4 && (h >> (last + 1)) >= 4) last++;
    out->w = w >> first; out->h = h >> first; out->mips = last - first + 1;
    out->format = fmt == 13 ? MTL_BC1 : fmt == 14 ? MTL_BC2 : fmt == 15 ? MTL_BC3 : MTL_RGBA8;
    size_t keep = 0;
    for (int m = first; m <= last; m++) keep += out->format == MTL_RGBA8 ? (size_t)(w >> m) * (h >> m) * 4 : mip_bytes(fmt, w >> m, h >> m);
    out->data = malloc(keep);
    if (!out->data) { rc = 5; goto done; }
    size_t at = 0;
    for (int m = first; m <= last; m++) {
        size_t off = img;   /* the data is smallest mip first: mip m starts after mips m+1 .. mips-1 */
        for (int k = mips - 1; k > m; k--) off += msize[k];
        const uint8_t *src = v + off;   /* frame 0, face 0 */
        int mw = w >> m, mh = h >> m;
        out->mip_ofs[m - first] = at;
        if (out->format != MTL_RGBA8) { memcpy(out->data + at, src, mip_bytes(fmt, mw, mh)); at += mip_bytes(fmt, mw, mh); continue; }
        for (int p = 0; p < mw * mh; p++) {   /* to RGBA8 */
            uint8_t *d = out->data + at + 4 * (size_t)p;
            switch (fmt) {
            case 0: memcpy(d, src + 4 * p, 4); break;
            case 12: d[0] = src[4 * p + 2]; d[1] = src[4 * p + 1]; d[2] = src[4 * p]; d[3] = src[4 * p + 3]; break;
            case 16: d[0] = src[4 * p + 2]; d[1] = src[4 * p + 1]; d[2] = src[4 * p]; d[3] = 255; break;
            case 2: d[0] = src[3 * p]; d[1] = src[3 * p + 1]; d[2] = src[3 * p + 2]; d[3] = 255; break;
            case 3: d[0] = src[3 * p + 2]; d[1] = src[3 * p + 1]; d[2] = src[3 * p]; d[3] = 255; break;
            }
        }
        at += (size_t)mw * mh * 4;
    }
    out->size = at;
    rc = 0;
done:
    free(v);
    if (rc) { free(out->data); out->data = NULL; }
    return rc;
}
