/* NANITE / PT-PROPS: static props. See bsp_props.h. Game lump directory at lump 35: count, then 16-byte entries (id, flags, version,
 * offset, length); "prps" (id 'sprp' little-endian) holds the model name dictionary (128 bytes each), the leaf list (uint16) and the
 * props, whose record size depends on the version (v4 56, v5 60, v6 64, v7+ more): taken as the remaining bytes / the count. Common
 * prefix: origin +0, angles +12, model index +24, first leaf +26, leaf count +28, solid +30, flags +31, skin +32, fade min +36, max +40. */
#include "bsp_props.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int32_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

int bsp_props_read(const uint8_t *bsp, size_t size, BspProp **out, int *n, int *version) {
    *out = NULL; *n = 0; *version = 0;
    if (size < 1036) return 1;
    int32_t go = rd32(bsp + 8 + 16 * 35), gl = rd32(bsp + 8 + 16 * 35 + 4);
    if (go < 0 || gl < 4 || (size_t)go + (size_t)gl > size) return 0;
    int32_t ng = rd32(bsp + go);
    for (int i = 0; i < ng && 4 + 16 * (i + 1) <= gl; i++) {
        const uint8_t *e = bsp + go + 4 + 16 * i;
        if (memcmp(e, "prps", 4) != 0) continue;
        uint16_t ver; memcpy(&ver, e + 6, 2);
        int32_t off = rd32(e + 8), len = rd32(e + 12);
        if (off < 0 || len < 12 || (size_t)off + (size_t)len > size) return 1;
        const uint8_t *p = bsp + off, *end = p + len;
        int32_t nd = rd32(p); p += 4;
        if (nd < 0 || p + 128 * (size_t)nd + 4 > end) return 1;
        const uint8_t *dict = p; p += 128 * (size_t)nd;
        int32_t nl = rd32(p); p += 4;
        if (nl < 0 || p + 2 * (size_t)nl + 4 > end) return 1;
        p += 2 * (size_t)nl;
        int32_t np = rd32(p); p += 4;
        if (np <= 0) return 0;
        size_t rec = (size_t)(end - p) / (size_t)np;
        if (rec < 44) return 1;
        BspProp *a = calloc((size_t)np, sizeof *a);
        if (!a) return 2;
        for (int k = 0; k < np; k++) {
            const uint8_t *r = p + rec * (size_t)k;
            BspProp *b = &a[k];
            memcpy(b->origin, r, 12); memcpy(b->angles, r + 12, 12);
            uint16_t mi; memcpy(&mi, r + 24, 2);
            b->solid = r[30]; b->flags = r[31]; b->skin = rd32(r + 32);
            memcpy(&b->fade_min, r + 36, 4); memcpy(&b->fade_max, r + 40, 4);
            if (mi < nd) {
                size_t j = 0;
                for (; j < sizeof b->model - 1 && dict[128 * (size_t)mi + j]; j++) {
                    char c = (char)dict[128 * (size_t)mi + j];
                    b->model[j] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                }
                b->model[j] = 0;
            }
        }
        *out = a; *n = np; *version = ver;
        return 0;
    }
    return 0;
}

void bsp_prop_matrix(const BspProp *p, double m[3][4]) {
    const double d = 3.14159265358979323846 / 180.0;
    double sp = sin(p->angles[0] * d), cp = cos(p->angles[0] * d), sy = sin(p->angles[1] * d), cy = cos(p->angles[1] * d);
    double sr = sin(p->angles[2] * d), cr = cos(p->angles[2] * d);
    m[0][0] = cp * cy; m[1][0] = cp * sy; m[2][0] = -sp;
    m[0][1] = sr * sp * cy - cr * sy; m[1][1] = sr * sp * sy + cr * cy; m[2][1] = sr * cp;
    m[0][2] = cr * sp * cy + sr * sy; m[1][2] = cr * sp * sy - sr * cy; m[2][2] = cr * cp;
    for (int i = 0; i < 3; i++) m[i][3] = p->origin[i];
}
