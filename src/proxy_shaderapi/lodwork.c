/* F3 auto LOD in game. See lodwork.h. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include "lodwork.h"
#include "frames.h"
#include "log.h"
#include "wrap.h"
#include "../lod/lod_model.h"
#include "vlg.h"

/* IBaseFileSystem (public SDK 2013 filesystem.h): Read 0, Write 1, Open 2, Close 3, Seek 4, Tell 5, Size(handle) 6, Size(name, path) 7 */
typedef int (*FsReadFn)(void *self, void *out, int size, void *file);
typedef void *(*FsOpenFn)(void *self, const char *name, const char *options, const char *path);
typedef void (*FsCloseFn)(void *self, void *file);
static void *g_fs;
static void **g_fsvt;

static int fs_bind(void) {
    if (g_fs) return 1;
    HMODULE m = GetModuleHandleA("filesystem_stdio.dll");
    typedef void *(*CiFn)(const char *, int *);
    CiFn ci = m ? (CiFn)(void *)GetProcAddress(m, "CreateInterface") : NULL;
    void *fs = ci ? ci("VBaseFileSystem011", NULL) : NULL;
    if (!fs) { photon_log("lod: no VBaseFileSystem011 in filesystem_stdio.dll"); return 0; }
    void **vt = *(void ***)fs;
    uintptr_t lo, hi;
    if (!photon_module_text(vt[0], &lo, &hi)) { photon_log("lod: VBaseFileSystem011 vtable not in a module's code"); return 0; }
    for (int i = 0; i <= 7; i++)
        if ((uintptr_t)vt[i] < lo || (uintptr_t)vt[i] >= hi) { photon_log("lod: VBaseFileSystem011 slot %d outside the module's code", i); return 0; }
    g_fs = fs; g_fsvt = vt;
    photon_log("lod: VBaseFileSystem011 %p, vtable %p (module code %p-%p), thread %lu", fs, (void *)vt, (void *)lo, (void *)hi, GetCurrentThreadId());
    return 1;
}

/* the whole file through the engine's search paths (pathID "GAME"), malloc'd; NULL if missing. Read in chunks until a short read: slot 6
 * is not Size(handle) in this build (the probe crashed there), so only Open 2, Read 0 and Close 3 are used. */
static uint8_t *fs_slurp_impl(const char *name, unsigned *n);
uint8_t *photon_fs_slurp(const char *name, unsigned *n) { return fs_bind() ? fs_slurp_impl(name, n) : NULL; }
static uint8_t *fs_slurp_impl(const char *name, unsigned *n) {
    static int logged;
    void *f = ((FsOpenFn)g_fsvt[2])(g_fs, name, "rb", "GAME");
    if (!f) return NULL;
    size_t cap = 1u << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (!b) break;
        if (len == cap) {
            if (cap >= (256u << 20)) { free(b); b = NULL; break; }
            uint8_t *nb = realloc(b, cap * 2);
            if (!nb) { free(b); b = NULL; break; }
            b = nb; cap *= 2;
        }
        int want = (int)(cap - len);
        int got = ((FsReadFn)g_fsvt[0])(g_fs, b + len, want, f);
        if (!logged++) photon_log("lod probe: first Read of %s returned %d", name, got);
        if (got <= 0) break;
        len += (size_t)got;
        if (got < want) break;   /* a short read: end of file */
    }
    ((FsCloseFn)g_fsvt[3])(g_fs, f);
    if (!b || !len) { free(b); return NULL; }
    *n = (unsigned)len;
    return b;
}

void photon_lod_frame(void) {
    static int probe = -1, done;
    if (probe < 0) { char e[4]; probe = GetEnvironmentVariableA("PHOTON_LOD_PROBE", e, sizeof e) > 0 && e[0] == '1'; }
    if (!probe || done || photon_frame_count() < 120) return;
    done = 1;
    if (!fs_bind()) return;
    static const char *const names[] = { "models/props_c17/oildrum001.mdl", "models/props_c17/oildrum001.vvd", "models/props_c17/oildrum001.dx90.vtx",
                                         "models/no_such_model_photon.mdl" };
    for (int i = 0; i < 4; i++) {
        unsigned n = 0;
        uint8_t *b = fs_slurp_impl(names[i], &n);
        photon_log("lod probe: %s: %s, %u bytes, starts %02x %02x %02x %02x", names[i], b ? "read" : "missing", n, b ? b[0] : 0, b && n > 1 ? b[1] : 0,
                   b && n > 2 ? b[2] : 0, b && n > 3 ? b[3] : 0);
        free(b);
    }
}

/* --- the worker (PHOTON_LOD=1; VM copy only until the user's OK, NEEDS_USER 7) ------------------------------------------------------- */
/* p6.c's DrawModelExecute hook (main thread) passes each model pointer it has not passed before; the name comes from IVModelInfoClient
 * GetModelName (VModelInfoClient006 slot 3, after the virtual destructor: slot 7 is GetModelRenderBounds, cull.c), accepted only if it
 * looks like "models/....mdl". New names go to a ring the worker thread drains: read the three files through the engine's filesystem,
 * lod_model_build (levels 0.5 / 0.25 / 0.1, only models with one LOD, at least LOD_MIN_TRIS triangles), write them to
 * <game>/garrysmod/addons/!photon_lod/<path>. The folder is mounted at game start, so a model built now is drawn with its LODs from the
 * next start on. Each name is tried once per session. */
enum { LOD_SEEN = 8192, LOD_RING = 1024, LOD_MIN_TRIS = 1000 };
static int g_lod_on = -1;
static const void *g_seen[LOD_SEEN];
static char g_ring[LOD_RING][260];
static volatile LONG g_ring_w, g_ring_r;
static HANDLE g_lod_ev, g_lod_thread;
static void *g_mi;
typedef const char *(*NameFn)(void *self, const void *model);
static NameFn g_name;
static volatile LONG64 g_lod_built, g_lod_skipped, g_lod_small, g_lod_failed, g_lod_names;
static wchar_t g_lod_root[MAX_PATH];
/* index.txt in the folder: one line per model tried ("<code> <bytes> <name>": code 0 built, else lod_model_build's or -1 files missing,
 * -2 too small), loaded at start so later sessions skip names already handled; the bytes written count against the size cap */
enum { LOD_KNOWN = 16384 };
static uint64_t g_known[LOD_KNOWN];   /* FNV-1a 64 of the name, 0 = empty (open addressing) */
static long long g_lod_bytes, g_lod_cap = 512ll << 20;
static SRWLOCK g_idx_lock = SRWLOCK_INIT;
static uint64_t name_hash(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ull; } return h ? h : 1; }
static int known_test_set(uint64_t h, int set) {
    size_t i = (size_t)(h * 0x9E3779B97F4A7C15ull >> 50) & (LOD_KNOWN - 1);
    for (int k = 0; k < LOD_KNOWN; k++, i = (i + 1) & (LOD_KNOWN - 1)) {
        if (g_known[i] == h) return 1;
        if (!g_known[i]) { if (set) g_known[i] = h; return 0; }
    }
    return 1;   /* full: treat as known (no more work this session) */
}
static void index_append(int code, long long bytes, const char *name) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\index.txt", g_lod_root);
    AcquireSRWLockExclusive(&g_idx_lock);
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char line[400];
        int n = snprintf(line, sizeof line, "%d %lld %s\n", code, bytes, name);
        DWORD w;
        if (n > 0) WriteFile(h, line, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_idx_lock);
}
static void index_load(void) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\index.txt", g_lod_root);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    char *b = GetFileSizeEx(h, &sz) && sz.QuadPart < (64 << 20) ? malloc((size_t)sz.QuadPart + 1) : NULL;
    DWORD got = 0;
    if (b && ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL)) {
        b[got] = 0;
        long lines = 0;
        for (char *line = b, *nl; *line; line = nl ? nl + 1 : line + strlen(line)) {
            nl = strchr(line, '\n');
            if (nl) *nl = 0;
            int code; long long bytes; int off = 0;
            if (sscanf(line, "%d %lld %n", &code, &bytes, &off) >= 2 && off > 0 && line[off]) {
                known_test_set(name_hash(line + off), 1);
                if (code == 0) g_lod_bytes += bytes;
                lines++;
            }
            if (!nl) break;
        }
        photon_log("lod: index: %ld models known, %.1f MB written so far (cap %lld MB)", lines, g_lod_bytes / 1048576.0, g_lod_cap >> 20);
    }
    free(b);
    CloseHandle(h);
}

static int lod_on(void) {   /* the setting photon_auto_lod (vlg.c: env PHOTON_LOD, settings.txt, photon.cfg); read on each new model */
    (void)g_lod_on;
    return photon_vlg_auto_lod();
}

static int write_file(const char *rel, const char *ext, const uint8_t *b, size_t n) {
    wchar_t p[MAX_PATH * 2];
    int k = _snwprintf(p, MAX_PATH * 2, L"%ls\\%hs%hs", g_lod_root, rel, ext);
    if (k <= 0 || k >= MAX_PATH * 2) return 0;
    for (wchar_t *c = p; *c; c++) if (*c == L'/') *c = L'\\';
    for (wchar_t *c = p + wcslen(g_lod_root) + 1; *c; c++)   /* create each directory under the root */
        if (*c == L'\\') { *c = 0; CreateDirectoryW(p, NULL); *c = L'\\'; }
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, b, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(h);
    return ok;
}

static void build_one(const char *mdlname) {
    char base[260];
    size_t L = strlen(mdlname);
    if (L < 5 || L >= sizeof base) return;
    memcpy(base, mdlname, L - 4); base[L - 4] = 0;   /* without ".mdl" */
    char n1[300], n2[300], n3[300];
    snprintf(n1, sizeof n1, "%s.mdl", base); snprintf(n2, sizeof n2, "%s.vvd", base); snprintf(n3, sizeof n3, "%s.dx90.vtx", base);
    unsigned a = 0, b = 0, c = 0;
    uint8_t *m = fs_slurp_impl(n1, &a), *v = fs_slurp_impl(n2, &b), *x = fs_slurp_impl(n3, &c);
    LodModelOut o;
    double ratios[3] = { 0.5, 0.25, 0.1 };
    if (InterlockedAdd64(&g_lod_bytes, 0) >= g_lod_cap) {   /* the folder is full: stop building this session (not indexed: later sessions retry) */
        static int said;
        if (!said++) photon_log("lod: size cap %lld MB reached: no more builds", g_lod_cap >> 20);
        free(m); free(v); free(x);
        return;
    }
    int rc = m && v && x ? lod_model_build(m, a, v, b, x, c, ratios, 3, &o) : -1;
    if (rc == LOD_OK && o.tris[0] < LOD_MIN_TRIS) { lod_model_free(&o); rc = -2; }
    if (rc == LOD_OK) {
        int ok = write_file(base, ".dx90.vtx", o.vtx, o.vtx_n) && write_file(base, ".vvd", o.vvd, o.vvd_n) && write_file(base, ".mdl", o.mdl, o.mdl_n);
        if (ok) { InterlockedIncrement64(&g_lod_built); InterlockedAdd64(&g_lod_bytes, (LONG64)(o.vtx_n + o.vvd_n + o.mdl_n)); }
        else InterlockedIncrement64(&g_lod_failed);
        if (ok) index_append(0, (long long)(o.vtx_n + o.vvd_n + o.mdl_n), mdlname);
        if (InterlockedAdd64(&g_lod_built, 0) <= 40 || !ok)
            photon_log("lod: %s: %s, %u -> %u / %u / %u triangles, switch points %.0f / %.0f / %.0f", mdlname, ok ? "built" : "WRITE FAILED", o.tris[0], o.tris[1],
                       o.tris[2], o.tris[3], o.switch_point[1], o.switch_point[2], o.switch_point[3]);
        lod_model_free(&o);
    } else if (rc == -2) { InterlockedIncrement64(&g_lod_small); index_append(-2, 0, mdlname); }
    else {
        if (rc != -1) index_append(rc, 0, mdlname);   /* files missing: not indexed (maybe a mount appears later) */
        InterlockedIncrement64(&g_lod_skipped);
        if (InterlockedAdd64(&g_lod_skipped, 0) <= 20) photon_log("lod: %s: left alone (%s %d)", mdlname, rc == -1 ? "files missing" : "code", rc);
    }
    free(m); free(v); free(x);
}

static DWORD WINAPI lod_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        WaitForSingleObject(g_lod_ev, INFINITE);
        while (g_ring_r != g_ring_w) {
            char name[260];
            memcpy(name, g_ring[g_ring_r % LOD_RING], sizeof name);
            InterlockedIncrement(&g_ring_r);
            build_one(name);
        }
    }
    return 0;
}

static int lod_setup(void) {
    static int state;   /* 0 not yet, 1 on, 2 off */
    if (state) return state == 1;
    state = 2;
    HMODULE en = GetModuleHandleA("engine.dll");
    typedef void *(*CiFn)(const char *, int *);
    CiFn ci = en ? (CiFn)(void *)GetProcAddress(en, "CreateInterface") : NULL;
    g_mi = ci ? ci("VModelInfoClient006", NULL) : NULL;
    if (!g_mi || !fs_bind()) { photon_log("lod: no VModelInfoClient006 or filesystem: off"); return 0; }
    g_name = (NameFn)(*(void ***)g_mi)[3];
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *sl = n ? wcsrchr(exe, L'\\') : NULL;
    if (!sl) return 0;
    *sl = 0;
    size_t el = wcslen(exe);   /* gmod.exe is in <game>\\bin\\win64 (x64) or <game> itself */
    if (el > 10 && _wcsicmp(exe + el - 10, L"\\bin\\win64") == 0) exe[el - 10] = 0;
    _snwprintf(g_lod_root, MAX_PATH, L"%ls\\garrysmod\\addons\\!photon_lod", exe);
    CreateDirectoryW(g_lod_root, NULL);
    { char e[16]; if (GetEnvironmentVariableA("PHOTON_LOD_MAX_MB", e, sizeof e) > 0 && atoll(e) > 0) g_lod_cap = atoll(e) << 20; }
    index_load();
    g_lod_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_lod_thread = g_lod_ev ? CreateThread(NULL, 0, lod_thread, NULL, 0, NULL) : NULL;
    if (!g_lod_thread) return 0;
    photon_log("lod: worker on (photon_auto_lod), writing %ls", g_lod_root);
    state = 1;
    return 1;
}

void photon_lod_note_model(const void *model) {
    if (!model || !lod_on()) return;
    size_t h = (size_t)(((uintptr_t)model >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (LOD_SEEN - 1);
    for (int k = 0; k < 8; k++, h = (h + 1) & (LOD_SEEN - 1)) {
        if (g_seen[h] == model) return;
        if (!g_seen[h]) break;
    }
    g_seen[h] = model;   /* a full neighbourhood just means the model may be asked again; the worker tries a name once */
    if (!lod_setup()) return;
    const char *nm = g_name(g_mi, model);
    if (!nm || strncmp(nm, "models/", 7) != 0) return;
    size_t L = strlen(nm);
    if (L < 12 || L >= 260 || strcmp(nm + L - 4, ".mdl") != 0) return;
    if (strncmp(nm, "models/weapons/c_", 17) == 0) return;   /* view models and arms: always drawn close, a LOD would never switch */
    if (known_test_set(name_hash(nm), 1)) return;   /* tried this session, or in the index from an earlier one (main thread only) */
    if (g_ring_w - g_ring_r >= LOD_RING) return;
    snprintf(g_ring[g_ring_w % LOD_RING], 260, "%s", nm);
    InterlockedIncrement(&g_ring_w);
    InterlockedIncrement64(&g_lod_names);
    SetEvent(g_lod_ev);
}
