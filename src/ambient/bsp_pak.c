/* A file inside a map's pakfile. See bsp_pak.h. Zip: the end-of-central-directory record (0x06054b50, searched backwards over the last
 * 64 KiB + 22) gives the central directory (entries 0x02014b50: method +10, compressed size +20, name length +28, extra +30, comment +32,
 * local header offset +42, name +46); the local header (0x04034b50: name length +26, extra +28) precedes the data. */
#include "bsp_pak.h"
#include <ctype.h>
#include <string.h>

static uint32_t r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t r16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static int same(const char *a, size_t al, const char *b) {
    size_t bl = strlen(b);
    if (al != bl) return 0;
    for (size_t i = 0; i < al; i++) {
        char x = a[i] == '\\' ? '/' : (char)tolower((unsigned char)a[i]), y = b[i] == '\\' ? '/' : (char)tolower((unsigned char)b[i]);
        if (x != y) return 0;
    }
    return 1;
}

const uint8_t *bsp_pak_find(const uint8_t *bsp, size_t size, const char *name, size_t *n) {
    *n = 0;
    if (size < 1036) return NULL;
    int32_t lo, ll; memcpy(&lo, bsp + 8 + 16 * 40, 4); memcpy(&ll, bsp + 8 + 16 * 40 + 4, 4);
    if (lo <= 0 || ll < 22 || (size_t)lo + (size_t)ll > size) return NULL;
    const uint8_t *z = bsp + lo;
    size_t zn = (size_t)ll;
    size_t e = zn - 22, stop = zn > 65536 + 22 ? zn - 65536 - 22 : 0;
    for (;; e--) {
        if (r32(z + e) == 0x06054b50) break;
        if (e == stop) return NULL;
    }
    uint32_t cd = r32(z + e + 16), cn = r16(z + e + 10);
    size_t p = cd;
    for (uint32_t i = 0; i < cn; i++) {
        if (p + 46 > zn || r32(z + p) != 0x02014b50) return NULL;
        uint16_t method = r16(z + p + 10), nl = r16(z + p + 28), xl = r16(z + p + 30), cl = r16(z + p + 32);
        uint32_t csize = r32(z + p + 20), lho = r32(z + p + 42);
        if (p + 46 + nl > zn) return NULL;
        if (same((const char *)z + p + 46, nl, name)) {
            if (method != 0 || (size_t)lho + 30 > zn || r32(z + lho) != 0x04034b50) return NULL;
            size_t d = lho + 30 + r16(z + lho + 26) + r16(z + lho + 28);
            if (d + csize > zn) return NULL;
            *n = csize;
            return z + d;
        }
        p += 46 + nl + xl + cl;
    }
    return NULL;
}
