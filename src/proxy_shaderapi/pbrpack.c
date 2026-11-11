/* PBR packs (tools/make_pbr_pack.py; TASKS PBR-PACK): the installed pack's per-material maps for the map's world materials, gathered
 * into two DXT1 atlases for PT-RT's world-normals pass (dmirror.c), so roughness, metalness and surface detail vary per texel instead of
 * per material (mtl_pbr.c's constants stay the fallback for materials no pack covers):
 *   rmh atlas  2048 x 2048, 32 x 32 tiles of 64 px: R roughness, G metalness, B height (materials/photon_pbr/<material>.vtf)
 *   nrm atlas  4096 x 4096, 32 x 32 tiles of 128 px: a generated normal map (materials/photon_pbr/<material>_n.vtf; x along +u, y along -v)
 * A tile holds one repetition of the texture (the pack's mip of exactly that size: 64 / 128 px), so the shader wraps inside it. Only
 * the pack's own files are read (VTF 7.2, DXT1, no thumbnail, mips smallest first, as make_pbr_pack.py writes them); a material whose map
 * is smaller than its tile, or that the pack lacks, gets no tile (-1). Read through the engine's filesystem, so a pack installed as an
 * addon (garrysmod/addons/<pack>/materials/photon_pbr/...) is found as the game's own files are. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>
#include "pbrpack.h"
#include "lodwork.h"

enum { TILES = 32 };

/* Packs as GMA files (tools/make_pbr_gma.py), read here directly: current GMod no longer mounts loose .gma files from addons/, and a
 * Lua mount comes after the map (and so this atlas) has loaded. Looked for in garrysmod/data/photon/pbr/<name>.gma (a manual install, or the
 * menu's download) and at the paths listed one per line in garrysmod/data/photon/pbr_packs.txt (relative to garrysmod/, or absolute:
 * the Workshop's cache). Indexes are read once per atlas build; each file's bytes are read on demand. */
typedef struct { char *path; long long ofs, size; } GmaEnt;
typedef struct { FILE *f; GmaEnt *e; int n; } Gma;
static Gma g_gma[16]; static int g_ngma;

static int rd_cstr(FILE *f, char *buf, int cap) {   /* a c-string, at most cap - 1 chars kept; -1 on EOF or over 4096 */
    int c, k = 0, n = 0;
    while ((c = fgetc(f)) != EOF && c) { if (k + 1 < cap) buf[k++] = (char)c; if (++n > 4096) return -1; }
    buf[k] = 0;
    return c == EOF ? -1 : k;
}
static void gma_open(const wchar_t *path) {
    if (g_ngma >= (int)(sizeof g_gma / sizeof g_gma[0])) return;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    char magic[5] = { 0 }, s[1024]; unsigned char ver;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "GMAD", 4) || fread(&ver, 1, 1, f) != 1 || fseek(f, 16, SEEK_CUR)) { fclose(f); return; }
    if (ver > 1) while (rd_cstr(f, s, sizeof s) > 0) {}   /* required content, up to an empty string */
    for (int k = 0; k < 3; k++) if (rd_cstr(f, s, sizeof s) < 0) { fclose(f); return; }   /* name, description, author */
    int32_t av; if (fread(&av, 4, 1, f) != 1) { fclose(f); return; }
    Gma g = { f, NULL, 0 }; int cap = 0;
    long long at = 0;
    for (;;) {
        uint32_t num; if (fread(&num, 4, 1, f) != 1) goto bad;
        if (!num) break;
        long long sz; uint32_t crc;
        if (rd_cstr(f, s, sizeof s) < 0 || fread(&sz, 8, 1, f) != 1 || fread(&crc, 4, 1, f) != 1 || sz < 0 || g.n >= (1 << 20)) goto bad;
        if (g.n == cap) { cap = cap ? cap * 2 : 1024; GmaEnt *ne = realloc(g.e, sizeof *ne * (size_t)cap); if (!ne) goto bad; g.e = ne; }
        for (char *c = s; *c; c++) *c = *c == '\\' ? '/' : (*c >= 'A' && *c <= 'Z') ? (char)(*c + 32) : *c;
        g.e[g.n].path = _strdup(s); g.e[g.n].ofs = at; g.e[g.n].size = sz; g.n++;
        at += sz;
    }
    long long base = _ftelli64(f);
    for (int i = 0; i < g.n; i++) g.e[i].ofs += base;
    g_gma[g_ngma++] = g;
    return;
bad:
    for (int i = 0; i < g.n; i++) free(g.e[i].path);
    free(g.e); fclose(f);
}
static void gmas_close(void) {
    for (int k = 0; k < g_ngma; k++) { for (int i = 0; i < g_gma[k].n; i++) free(g_gma[k].e[i].path); free(g_gma[k].e); fclose(g_gma[k].f); }
    g_ngma = 0;
}
static void gmas_open(void) {
    gmas_close();
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *sl = n ? wcsrchr(dir, L'\\') : NULL;
    if (!sl) return;
    *sl = 0;
    size_t el = wcslen(dir);
    if (el > 10 && _wcsicmp(dir + el - 10, L"\\bin\\win64") == 0) dir[el - 10] = 0;
    _snwprintf(p, MAX_PATH, L"%ls\\garrysmod\\data\\photon\\pbr\\*.gma", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { _snwprintf(p, MAX_PATH, L"%ls\\garrysmod\\data\\photon\\pbr\\%ls", dir, fd.cFileName); gma_open(p); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    _snwprintf(p, MAX_PATH, L"%ls\\garrysmod\\data\\photon\\pbr_packs.txt", dir);
    FILE *lst = _wfopen(p, L"r");
    if (lst) {
        char line[MAX_PATH];
        while (fgets(line, sizeof line, lst)) {
            char *hash = strchr(line, '#'); if (hash) *hash = 0;   /* "<path> #<pack id>" (the menu's tag) */
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
            if (!l) continue;
            wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, line, -1, w, MAX_PATH);
            if (w[0] && w[1] == L':') gma_open(w);
            else { _snwprintf(p, MAX_PATH, L"%ls\\garrysmod\\%ls", dir, w); for (wchar_t *c = p; *c; c++) if (*c == L'/') *c = L'\\'; gma_open(p); }
        }
        fclose(lst);
    }
}
static uint8_t *pack_read(const char *rel, unsigned *n) {   /* the engine's filesystem (loose addon folders) first, then the pack GMAs */
    uint8_t *b = photon_fs_slurp(rel, n);
    if (b) return b;
    for (int k = 0; k < g_ngma; k++)
        for (int i = 0; i < g_gma[k].n; i++)
            if (!strcmp(g_gma[k].e[i].path, rel)) {
                long long sz = g_gma[k].e[i].size;
                if (sz <= 0 || sz > (64 << 20) || _fseeki64(g_gma[k].f, g_gma[k].e[i].ofs, SEEK_SET)) return NULL;
                b = malloc((size_t)sz);
                if (b && fread(b, 1, (size_t)sz, g_gma[k].f) != (size_t)sz) { free(b); b = NULL; }
                if (b) *n = (unsigned)sz;
                return b;
            }
    return NULL;
}

/* the DXT1 blocks of the mip whose size is `side` (square), or NULL */
static const uint8_t *vtf_mip(const uint8_t *f, unsigned n, unsigned side, int *have_side) {
    *have_side = 0;
    if (n < 80 || memcmp(f, "VTF\0", 4)) return NULL;
    uint32_t hsz; uint16_t w, h; int32_t fmt; uint8_t mips; int32_t lowfmt;
    memcpy(&hsz, f + 12, 4); memcpy(&w, f + 16, 2); memcpy(&h, f + 18, 2); memcpy(&fmt, f + 52, 4); mips = f[56]; memcpy(&lowfmt, f + 57, 4);
    if (fmt != 13 || lowfmt != -1 || w != h || hsz > n) return NULL;
    *have_side = w;
    size_t off = hsz;
    for (int m = mips - 1; m >= 0; m--) {   /* smallest first */
        unsigned s = w >> m; if (!s) s = 1;
        size_t bytes = (size_t)((s + 3) / 4) * ((s + 3) / 4) * 8;
        if (s == side) return off + bytes <= n ? f + off : NULL;
        off += bytes;
    }
    return NULL;
}

/* copies a side x side DXT1 tile into tile t of an atlas TILES x side wide */
static void put_tile(uint8_t *atlas, unsigned side, int t, const uint8_t *src) {
    unsigned bpr = side / 4, abpr = TILES * bpr;   /* blocks per tile row, per atlas row */
    unsigned tx = (unsigned)t % TILES, ty = (unsigned)t / TILES;
    for (unsigned r = 0; r < bpr; r++)
        memcpy(atlas + ((size_t)(ty * bpr + r) * abpr + tx * bpr) * 8, src + (size_t)r * bpr * 8, (size_t)bpr * 8);
}

/* a cubemap patch material (VBSP: "maps/<map>/<material>_<x>_<y>_<z>", including the original with an $envmap) back to the original's
 * name, which is what the pack holds; gm_construct: 171 of its 204 world materials are patches */
static void unpatch(char *n) {
    if (strncmp(n, "maps/", 5)) return;
    char *s = strchr(n + 5, '/');
    if (!s) return;
    char *e = n + strlen(n);
    for (int k = 0; k < 3; k++) {   /* three "_<int>" suffixes */
        char *u = e;
        while (u > s && ((u[-1] >= '0' && u[-1] <= '9') || u[-1] == '-')) u--;
        if (u == e || u <= s + 1 || u[-1] != '_') return;
        e = u - 1;
    }
    memmove(n, s + 1, (size_t)(e - (s + 1)));
    n[e - (s + 1)] = 0;
}

PbrAtlas *pbr_atlas_build(const uint8_t *bsp, size_t size) {
    if (size < 8 + 16 * 64) return NULL;
    { char e[4]; if (GetEnvironmentVariableA("PHOTON_PBR_PACK", e, sizeof e) > 0 && e[0] == '0') return NULL; }   /* A/B: the packs ignored */
    const int32_t *lump = (const int32_t *)(bsp + 8);
    int32_t o2 = lump[4 * 2], l2 = lump[4 * 2 + 1], o43 = lump[4 * 43], l43 = lump[4 * 43 + 1], o44 = lump[4 * 44], l44 = lump[4 * 44 + 1];
    if (o2 < 0 || l2 <= 0 || (size_t)o2 + (size_t)l2 > size || o43 < 0 || (size_t)o43 + (size_t)l43 > size || o44 < 0 || (size_t)o44 + (size_t)l44 > size) return NULL;
    int n = l2 / 32;
    gmas_open();
    PbrAtlas *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->ntd = n;
    a->tile = malloc(sizeof(int) * (size_t)(n ? n : 1));
    a->rmh = calloc(1, PBR_RMH_BYTES); a->nrm = calloc(1, PBR_NRM_BYTES);
    if (!a->tile || !a->rmh || !a->nrm) { pbr_atlas_free(a); return NULL; }
    /* texdata entries naming the same material share a tile */
    char (*names)[128] = calloc((size_t)(n ? n : 1), 128);
    if (!names) { pbr_atlas_free(a); return NULL; }
    for (int d = 0; d < n; d++) {
        a->tile[d] = -1;
        int32_t si; memcpy(&si, bsp + o2 + 32 * d + 12, 4);
        if (si < 0 || si >= l44 / 4) continue;
        int32_t so; memcpy(&so, bsp + o44 + 4 * si, 4);
        if (so < 0 || so >= l43) continue;
        size_t k = 0;
        while (so + (int32_t)k < l43 && bsp[o43 + so + k] && k + 1 < sizeof names[0]) {
            char c = (char)bsp[o43 + so + k];
            names[d][k++] = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        names[d][k] = 0;
        unpatch(names[d]);
        int same = -1;
        for (int e = 0; e < d && same < 0; e++) if (a->tile[e] >= 0 && !strcmp(names[e], names[d])) same = e;
        if (same >= 0) { a->tile[d] = a->tile[same]; continue; }
        if (a->ntiles >= TILES * TILES || !names[d][0]) continue;
        char path[192]; unsigned fn = 0, nn = 0;
        snprintf(path, sizeof path, "materials/photon_pbr/%s.vtf", names[d]);
        uint8_t *f = pack_read(path, &fn);
        if (!f) { a->missing++; continue; }
        snprintf(path, sizeof path, "materials/photon_pbr/%s_n.vtf", names[d]);
        uint8_t *g = pack_read(path, &nn);   /* none: the material has its own normal map (the pack does not repeat it) */
        int hs = 0, ns = 0;
        const uint8_t *r = vtf_mip(f, fn, PBR_RMH_TILE, &hs), *q = g ? vtf_mip(g, nn, PBR_NRM_TILE, &ns) : NULL;
        if (r) {
            int t = a->ntiles++;
            put_tile(a->rmh, PBR_RMH_TILE, t, r);
            if (q) put_tile(a->nrm, PBR_NRM_TILE, t, q);
            a->tile[d] = t | (q ? PBR_TILE_HAS_NORMAL : 0);
            a->with_normal += q != NULL;
        } else a->small++;
        free(f); free(g);
    }
    free(names);
    a->gmas = g_ngma;
    gmas_close();
    return a;
}

void pbr_atlas_free(PbrAtlas *a) {
    if (!a) return;
    free(a->tile); free(a->rmh); free(a->nrm); free(a);
}
