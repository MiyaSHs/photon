/* A stand-in for engine.dll in the mock host's PHOTON_TEST_AMB runs (tests/proxyhost/vlgtest.c), so that ambvol.c's photon_ambient 1 layer runs under Wine without the game.
 * It has the code bytes ambvol.c's engine_bind validates, assembled at load inside one RWX array of this image: real code for the two calls the proxy makes (slots 26 and
 * 51), the rest only read. It also serves the VFileSystem022 the proxy reads maps through, from memory, with the file handle layout ambvol.c relies on (+0x50, set for a file
 * inside a GMA). Built under the name engine.dll, in its own directory: the game's real one is linked next to the proxy and must not be picked up by a plain load. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fake_engine.h"

enum { IMG_BYTES = 0x130000, RVA_MAPLOAD = 0x112805, FS_STAMP = 0x6adfbca3, RVA_FS_GMA_LOCK = 0xe2088, RVA_FS_WIN32_VTBL = 0xafd08, RVA_LA_READ = 0x105de9,
       RVA_LI_READ = 0x105e2c, RVA_TIME_CALL = 0x105ed9 };
static uint8_t g_img[IMG_BYTES] __attribute__((aligned(4096)));

static volatile int g_signon, g_dedicated;
static char g_level[260];
static const char g_empty[1] = "", g_dedicated_name[] = "Dedicated Server";
static volatile float g_origin[3];
static void *g_renderer_ptr;
static void *g_fs_global;
/* The light averaging inputs as GMod lays them out. ConVars: name +0x18, parent +0x38, float bits +0x54 and int +0x58, both XORed with the object's low 32
 * address bits. The client state: tick count +0x1c4, paused byte +0x20b70, remainder (double) +0x20b78; the tick interval is a global double. */
static const char g_la_name[] = "r_lightaverage", g_li_name[] = "r_lightinterp";
static uint8_t g_la_cv[0x60] __attribute__((aligned(16))), g_li_cv[0x60] __attribute__((aligned(16)));
static uint8_t g_cl[0x20b80] __attribute__((aligned(16)));
static double g_interval = 0.015;
static void cv_set(uint8_t *cv, float f, int n) {
    uint32_t key = (uint32_t)(uintptr_t)cv, fb;
    memcpy(&fb, &f, 4);
    *(volatile uint32_t *)(cv + 0x54) = fb ^ key;
    *(volatile uint32_t *)(cv + 0x58) = (uint32_t)n ^ key;
}
static void cv_init(uint8_t *cv, const char *name) {
    memcpy(cv + 0x18, &name, sizeof name);
    void *self = cv;
    memcpy(cv + 0x38, &self, sizeof self);
}
static void set_time(double s) { *(volatile int *)(g_cl + 0x1c4) = 0; *(volatile double *)(g_cl + 0x20b78) = s; }
static void set_lightavg(float rate, int average) { cv_set(g_li_cv, rate, (int)rate); cv_set(g_la_cv, (float)average, average); }

/* --- the code ------------------------------------------------------------------------------------------------------ */
static int hexv(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static uint8_t *emit(uint8_t *p, const char *pat) {
    for (; *pat; pat += pat[2] ? 3 : 2)
        *p++ = pat[0] == '?' ? 0 : (uint8_t)(hexv(pat[0]) * 16 + hexv(pat[1]));
    return p;
}
static void rel(uint8_t *code, int off_rel, int off_next, const volatile void *target) {
    int32_t r = (int32_t)((const volatile uint8_t *)target - (code + off_next));
    memcpy(code + off_rel, &r, sizeof r);
}

static struct { void **vt; } g_client, g_renderview, g_renderer;
static void *g_client_vt[64], *g_renderview_vt[64], *g_renderer_vt[64];

/* --- the filesystem ------------------------------------------------------------------------------------------------ */
typedef struct FakeFile {
    char pad[0x50];
    void *pack;            /* +0x50: the GMA file object of a file inside a GMA, as in filesystem_stdio's CFileHandle */
    const uint8_t *data;
    long long size, at;
    uint8_t gobj[0x20];    /* that object: +0 offset in the .gma, +8 length, +0x10 position, +0x18 the .gma's own CFileHandle */
} FakeFile;
/* A .gma as the mount leaves it: its CFileHandle (+0x50 NULL, +0x68 the Win32 file object, +0x78 the magic) and that object (vtable RVA 0xafd08, +0x20 the HANDLE) */
typedef struct { uint8_t fh[0x80], w32[0x38]; long long base, len; int mode; } FakeGma;
static FakeGma g_gmas[4];
static int g_ngmas;
static uint8_t *g_base;
static struct { char name[64]; const uint8_t *data; size_t size; int gma; FakeGma *g; } g_files[8];
static int g_nfiles;
static FakeFile g_open[64];
static volatile LONG g_nopen;
static FakeEngineCtl g_ctl;

static void *fs_openex(void *self, const char *name, const char *opts, unsigned flags, const char *path_id, char **resolved) {
    (void)self; (void)flags;
    if (strcmp(opts, "rb") != 0 || path_id || resolved) return NULL;
    for (int i = 0; i < g_nfiles; i++)
        if (strcmp(name, g_files[i].name) == 0) {
            FakeFile *f = &g_open[InterlockedIncrement(&g_nopen) % 64];
            memset(f, 0, sizeof *f);
            f->data = g_files[i].data;
            f->size = (long long)g_files[i].size;
            FakeGma *g = g_files[i].g;
            if (g) {
                f->pack = f->gobj;
                memcpy(f->gobj, &g->base, 8);
                memcpy(f->gobj + 8, &g->len, 8);
                void *inner = g->mode == 2 ? NULL : g->fh;
                memcpy(f->gobj + 0x18, &inner, 8);
            } else if (g_files[i].gma) f->pack = f;
            InterlockedIncrement(&g_ctl.opens);
            return f;
        }
    return NULL;
}
static int bfs_read(void *self, void *buf, int n, void *file) {
    (void)self;
    FakeFile *f = file;
    if (f->pack) InterlockedIncrement(&g_ctl.pack_used);
    if (n > g_ctl.largest_read) g_ctl.largest_read = n;
    long long left = f->size - f->at;
    int m = n < left ? n : (int)left;
    if (m <= 0) return 0;
    memcpy(buf, f->data + f->at, (size_t)m);
    f->at += m;
    return m;
}
static void bfs_close(void *self, void *file) { (void)self; (void)file; InterlockedIncrement(&g_ctl.closes); }
static void bfs_seek(void *self, void *file, long long pos, int mode) {
    (void)self;
    FakeFile *f = file;
    if (f->pack) InterlockedIncrement(&g_ctl.pack_used);
    if (mode == 0 && pos >= 0) f->at = pos;
}
static unsigned bfs_size(void *self, void *file) {
    (void)self;
    FakeFile *f = file;
    if (f->pack) InterlockedIncrement(&g_ctl.pack_used);
    return (unsigned)f->size;
}
static void *g_fs_vt[80], *g_bfs_vt[16];
static struct { void **vt, **bvt; } g_fs = { g_fs_vt, g_bfs_vt };

static void add_file(const char *name, const uint8_t *data, size_t size, int gma) {
    snprintf(g_files[g_nfiles].name, sizeof g_files[0].name, "%s", name);
    g_files[g_nfiles].data = data;
    g_files[g_nfiles].size = size;
    g_files[g_nfiles++].gma = gma;
}
static void put_le(uint8_t **p, unsigned long long v, int n) { for (int i = 0; i < n; i++) *(*p)++ = (uint8_t)(v >> (8 * i)); }
static void put_s(uint8_t **p, const char *s) { size_t n = strlen(s) + 1; memcpy(*p, s, n); *p += n; }
static int add_gma(const char *name, const uint8_t *data, size_t size, const uint8_t *other, size_t other_size, int mode) {
    if (g_ngmas == 4 || g_nfiles == 8 || !g_base) return -1;
    FakeGma *g = &g_gmas[g_ngmas];
    char dir[MAX_PATH], path[MAX_PATH + 32], other_name[64];
    if (!GetTempPathA(sizeof dir, dir)) return -2;
    snprintf(path, sizeof path, "%sphoton_fake_%d.gma", dir, g_ngmas);
    snprintf(other_name, sizeof other_name, "maps/other_%d.bsp", g_ngmas);
    static const uint8_t pad[777] = { 'p' };
    size_t cap = 4096 + sizeof pad + size + other_size;
    uint8_t *b = malloc(cap), *p = b;
    if (!b) return -3;
    memcpy(p, "GMADx", 5); p[4] = 3; p += 5;
    put_le(&p, 0, 8);
    put_le(&p, 1462129767ull, 8);
    put_s(&p, "");
    put_s(&p, "fake");
    put_s(&p, "{}");
    put_s(&p, "Author Name");
    put_le(&p, 1, 4);
    const char *names[3] = { "materials/pad.vmt", other_name, name };
    const size_t sizes[3] = { sizeof pad, other_size, size };
    for (int i = 0; i < 3; i++) { put_le(&p, (unsigned)(i + 1), 4); put_s(&p, names[i]); put_le(&p, sizes[i], 8); put_le(&p, 0, 4); }
    put_le(&p, 0, 4);
    long long other_at = (long long)(p - b) + (long long)sizeof pad, at = other_at + (long long)other_size;
    memcpy(p, pad, sizeof pad); p += sizeof pad;
    memcpy(p, other, other_size); p += other_size;
    memcpy(p, data, size); p += size;
    put_le(&p, 0, 4);
    HANDLE w = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    BOOL ok = w != INVALID_HANDLE_VALUE && WriteFile(w, b, (DWORD)(p - b), &wrote, NULL) && wrote == (DWORD)(p - b);
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    free(b);
    if (!ok) return -4;
    /* as FS_fopen's Win32 class opens it (filesystem_stdio 0x180028e70) */
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -5;
    memset(g, 0, sizeof *g);
    void *vt = g_base + RVA_FS_WIN32_VTBL, *w32 = g->w32;
    DWORD magic = 0x43464861;
    memcpy(g->w32, &vt, 8);
    memcpy(g->w32 + 0x20, &h, 8);
    memcpy(g->fh + 0x68, &w32, 8);
    memcpy(g->fh + 0x78, &magic, 4);
    g->base = mode == 1 ? other_at : at;
    g->len = (long long)size;
    g->mode = mode;
    g_ngmas++;
    snprintf(g_files[g_nfiles].name, sizeof g_files[0].name, "%s", name);
    g_files[g_nfiles].data = data;
    g_files[g_nfiles].size = size;
    g_files[g_nfiles].gma = 1;
    g_files[g_nfiles++].g = g;
    return 0;
}
static void set_map(int signon, const char *level) {
    snprintf(g_level, sizeof g_level, "%s", level);
    g_signon = signon;
}
static void set_origin(float x, float y, float z) { g_origin[0] = x; g_origin[1] = y; g_origin[2] = z; }

/* break_what: 1 one byte differs in slot 26's code, 2 filesystem_stdio's stamp differs, 3 one byte differs in the r_lightinterp reader */
__declspec(dllexport) FakeEngineCtl *FakeEngineControl(int break_what) {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)FakeEngineControl, &self);
    uint8_t *base = (uint8_t *)self;
    /* the array must span the fixed RVAs: the map loader's code, the filesystem's Win32 vtable and GMA lock,
     * the light averaging reads; the time function sits at +0x400 */
    if (g_img + IMG_BYTES <= base + RVA_MAPLOAD + 0x100 || g_img > base + RVA_FS_WIN32_VTBL || g_img + 0x500 > base + RVA_FS_WIN32_VTBL ||
        g_img > base + RVA_LA_READ) return NULL;
    g_base = base;
    DWORD old;
    if (!VirtualProtect(g_img, IMG_BYTES, PAGE_EXECUTE_READWRITE, &old)) return NULL;

    uint8_t *p = g_img;
    uint8_t *ingame = p;
    p = emit(p, "83 3d ?? ?? ?? ?? 06 0f 94 c0 c3");   /* cmp dword [signon], 6; sete al; ret */
    rel(ingame, 2, 7, &g_signon);
    if (break_what == 1) ingame[6] = 7;
    uint8_t *levelname = p = g_img + 0x100;
    p = emit(p, "80 3d ?? ?? ?? ?? 00 74 08 48 8d 05 ?? ?? ?? ?? c3 83 3d ?? ?? ?? ?? 02 48 8d 05 ?? ?? ?? ?? 7c 07 48 8d 05 ?? ?? ?? ?? c3");
    rel(levelname, 2, 7, &g_dedicated);
    rel(levelname, 12, 16, g_dedicated_name);
    rel(levelname, 19, 24, &g_signon);
    rel(levelname, 27, 31, g_empty);
    rel(levelname, 36, 40, g_level);
    uint8_t *setmainview = p = g_img + 0x200;
    p = emit(p, "48 8b 0d ?? ?? ?? ?? 48 8b 01 48 ff a0 d8 00 00 00");
    rel(setmainview, 3, 7, &g_renderer_ptr);
    uint8_t *setter = p = g_img + 0x300;
    p = emit(p, "f3 0f 10 02 4c 8d 0d ?? ?? ?? ?? f3 0f 11 05 ?? ?? ?? ?? 49 8b c8 f3 0f 10 4a 04 4c 8d 05 ?? ?? ?? ?? f3 0f 11 0d ?? ?? ?? ?? "
                "f3 0f 10 42 08 48 8d 15 ?? ?? ?? ?? f3 0f 11 05 ?? ?? ?? ??");
    rel(setter, 0x0f, 0x13, &g_origin[0]);
    rel(setter, 0x26, 0x2a, &g_origin[1]);
    rel(setter, 0x3a, 0x3e, &g_origin[2]);
    uint8_t *mapload = base + RVA_MAPLOAD;
    emit(mapload, "48 8b 0d ?? ?? ?? ?? 4c 8d 05 ?? ?? ?? ?? 48 89 5c 24 28 48 8d 54 24 50 45 33 c9 48 89 5c 24 20 48 8b 01 ff 90 48 02 00 00 "
                  "48 89 05 ?? ?? ?? ?? 4c 8b c8 48 85 c0 75 11 48 8d 54 24 50 48 8d 0d ?? ?? ?? ?? e9 ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? "
                  "48 8d 15 ?? ?? ?? ?? 48 83 c1 08 41 b8 0c 04 00 00 48 8b 01 ff 10");
    rel(mapload, 3, 7, &g_fs_global);
    g_fs_global = &g_fs;
    /* FUN_180105dc0's reads: r_lightaverage, r_lightinterp, then the client time through FUN_1800aaea0 (real code here, the proxy calls it) */
    uint8_t *la = base + RVA_LA_READ;
    emit(la, "48 8b 0d ?? ?? ?? ?? 49 8b e8 0f b7 fa 48 8d 15 ?? ?? ?? ?? 48 89 74 24 38 48 3b ca 75 0a 8b 05 ?? ?? ?? ?? 33 c2 eb 06 48 8b 01 ff 50 68");
    rel(la, 3, 7, g_la_cv + 0x38); rel(la, 16, 20, g_la_cv); rel(la, 32, 36, g_la_cv + 0x58);
    uint8_t *li = base + RVA_LI_READ;
    emit(li, "48 8b 0d ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 3b ca 75 1b f3 0f 10 05 ?? ?? ?? ?? f3 0f 11 44 24 30 31 54 24 30 f3 44 0f 10 44 24 30 eb 0a 48 8b 01 ff 50 60");
    rel(li, 3, 7, g_li_cv + 0x38); rel(li, 10, 14, g_li_cv); rel(li, 23, 27, g_li_cv + 0x54);
    if (break_what == 3) li[17] = 0x74;   /* one byte differs: the proxy must fall back to the default rate, the volume stays on */
    uint8_t *timefn = g_img + 0x400;
    emit(timefn, "80 b9 70 0b 02 00 00 66 0f 6e 81 c4 01 00 00 f3 0f e6 c0 f2 0f 59 05 ?? ?? ?? ?? 75 08 f2 0f 58 81 78 0b 02 00 c3");
    rel(timefn, 23, 27, &g_interval);
    uint8_t *tc = base + RVA_TIME_CALL;
    emit(tc, "48 8d 0d ?? ?? ?? ?? 4c 89 ac 24 e0 00 00 00 4c 89 b4 24 d8 00 00 00 4c 89 bc 24 d0 00 00 00 0f 29 b4 24 c0 00 00 00 e8 ?? ?? ?? ?? f3 0f 10 8f 28 01 00 00");
    rel(tc, 3, 7, g_cl); rel(tc, 40, 44, timefn);
    cv_init(g_la_cv, g_la_name); cv_init(g_li_cv, g_li_name);
    set_lightavg(5.0f, 1);
    set_time(100.0);
    FlushInstructionCache(GetCurrentProcess(), g_img, IMG_BYTES);
    InitializeCriticalSection((CRITICAL_SECTION *)(base + RVA_FS_GMA_LOCK));

    g_client.vt = g_client_vt;
    g_client_vt[26] = ingame;
    g_client_vt[51] = levelname;
    g_renderview.vt = g_renderview_vt;
    g_renderview_vt[41] = setmainview;
    g_renderer.vt = g_renderer_vt;
    g_renderer_vt[27] = setter;
    g_renderer_ptr = &g_renderer;

    g_fs_vt[73] = (void *)fs_openex;
    g_bfs_vt[0] = (void *)bfs_read;
    g_bfs_vt[3] = (void *)bfs_close;
    g_bfs_vt[4] = (void *)bfs_seek;
    g_bfs_vt[7] = (void *)bfs_size;

    /* the proxy pins filesystem_stdio's stamp on the module holding the filesystem's vtable: here, this one */
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (!VirtualProtect(nt, sizeof *nt, PAGE_READWRITE, &old)) return NULL;
    nt->FileHeader.TimeDateStamp = break_what == 2 ? FS_STAMP + 1 : FS_STAMP;
    VirtualProtect(nt, sizeof *nt, old, &old);

    g_ctl.fs = &g_fs;
    g_ctl.add_file = add_file;
    g_ctl.add_gma = add_gma;
    g_ctl.set_map = set_map;
    g_ctl.set_origin = set_origin;
    g_ctl.set_time = set_time;
    g_ctl.set_lightavg = set_lightavg;
    return &g_ctl;
}

__declspec(dllexport) void *CreateInterface(const char *name, int *ret) {
    void *o = strcmp(name, "VEngineClient015") == 0 ? (void *)&g_client : strcmp(name, "VEngineRenderView014") == 0 ? (void *)&g_renderview : NULL;
    if (ret) *ret = o ? 0 : 1;
    return o;
}
