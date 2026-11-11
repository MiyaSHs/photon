/* L3c (analysis/37): see l3work.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "l3work.h"
#include "log.h"
#include "vlg.h"

static SRWLOCK g_l = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static const AmbWorkFiles *g_files;   /* under g_l */
static char g_want_level[MAX_PATH];   /* under g_l: the job */
static int g_want_voxel;
static LONG g_job;                    /* bumped per new job */
static volatile int g_cancel;
static int g_started;
static L3Ready *g_ready;              /* under g_l: published, not yet taken */
static char g_key_level[MAX_PATH];    /* draw thread */
static int g_key_voxel;
static volatile LONG64 g_bakes, g_cache_hits, g_fails;
static volatile double g_last_ms;

void l3_ready_free(L3Ready *r) {
    if (!r) return;
    l3_pack_free(r->pack);
    free(r->lights);
    free(r);
}

static int cache_path(const char *level, unsigned long long key, int voxel, wchar_t *out, size_t cap, int mkdirs) {
    wchar_t base[MAX_PATH];
    if (!photon_vlg_mod_dir(base, MAX_PATH)) return 0;
    const char *slash = strrchr(level, '/'), *bs = strrchr(level, '\\');
    const char *name = slash > bs ? slash + 1 : bs ? bs + 1 : level;
    char nm[96];
    snprintf(nm, sizeof nm, "%.64s", name);
    char *dot = strrchr(nm, '.');
    if (dot) *dot = 0;
    if (mkdirs) {
        wchar_t d[MAX_PATH];
        _snwprintf(d, MAX_PATH, L"%ls\\data\\photon", base); CreateDirectoryW(d, NULL);
        _snwprintf(d, MAX_PATH, L"%ls\\data\\photon\\bake", base); CreateDirectoryW(d, NULL);
    }
    _snwprintf(out, cap, L"%ls\\data\\photon\\bake\\%hs_%016llx_%d.l3", base, nm, key, voxel);
    out[cap - 1] = 0;
    return 1;
}

static L3Pack *cache_read(const wchar_t *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    L3Pack *p = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1LL << 30)) {
        uint8_t *b = malloc((size_t)sz.QuadPart);
        DWORD got = 0;
        if (b && ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart && l3_pack_load(b, (size_t)got, &p) != AMB_OK) p = NULL;
        free(b);
    }
    CloseHandle(h);
    return p;
}

static void cache_write(const wchar_t *path, const L3Pack *p) {
    uint8_t *b = NULL;
    size_t n = 0;
    if (l3_pack_save(p, &b, &n) != AMB_OK) return;
    wchar_t tmp[MAX_PATH + 8];
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
    tmp[MAX_PATH + 7] = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        BOOL ok = WriteFile(h, b, (DWORD)n, &put, NULL) && put == (DWORD)n;
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
    }
    free(b);
}

static void run_job(const AmbWorkFiles *files, const char *level, int voxel, LONG job) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    void *pin = files->pin ? files->pin() : NULL;
    int gma = 0;
    void *fh = files->open(level, &gma, &g_cancel);
    uint8_t *bsp = NULL;
    long long size = 0;
    if (fh) {
        size = files->size(fh);
        if (size > 1036 && size < (1LL << 30) && (bsp = malloc((size_t)size)) != NULL) {
            for (long long o = 0; o < size && !g_cancel;) {
                int n = (int)(size - o > (1 << 20) ? (1 << 20) : size - o);
                if (files->read_at(fh, o, bsp + o, n) != n) { free(bsp); bsp = NULL; break; }
                o += n;
            }
        }
        files->close(fh);
    }
    if (pin && files->unpin) files->unpin(pin);
    if (!bsp || g_cancel) { free(bsp); if (!g_cancel) { g_fails++; photon_log("l3: %s: cannot read the map (%s): no re-bake", level, fh ? "read" : "open"); } return; }
    unsigned long long key = 1469598103934665603ull;   /* the file's size and its lump directory */
    for (int i = 0; i < 1036; i++) { key ^= bsp[i]; key *= 1099511628211ull; }
    key ^= (unsigned long long)size; key *= 1099511628211ull;
    key ^= (unsigned long long)L3_BAKE_VERSION; key *= 1099511628211ull;   /* a new bake version is a new file */
    L3Ready *r = calloc(1, sizeof *r);
    LgMap *lm = NULL;
    if (!r || lg_map_load(bsp, (size_t)size, 1, &lm) != AMB_OK) { free(r); free(bsp); g_fails++; photon_log("l3: %s: no worldlights: no re-bake", level); return; }
    int nl = 0;
    const LgWorldLight *wl = lg_map_lights(lm, &nl);
    r->lights = malloc(sizeof *r->lights * (size_t)(nl ? nl : 1));
    if (r->lights) { memcpy(r->lights, wl, sizeof *wl * (size_t)nl); r->nlights = nl; }
    lg_map_free(lm);
    r->voxel = voxel;
    snprintf(r->level, sizeof r->level, "%.63s", level);
    wchar_t path[MAX_PATH];
    int have_path = cache_path(level, key, voxel, path, MAX_PATH, 0);
    const char *how = "cache";
    if (have_path) r->pack = cache_read(path);
    if (r->pack) g_cache_hits++;
    else {
        how = "baked";
        L3Params p = { (double)voxel, 0.01, &g_cancel, 0.2 };   /* min_share 0.2: a lamp under a fifth of the light at an edge is not re-baked (gm_sunsetgulch 86 -> 12 MB) */
        L3Bake *bk = NULL;
        L3Stats st;
        int rc = l3_bake(bsp, (size_t)size, &p, &bk, &st);
        if (rc == AMB_OK && bk->nbricks > 0) {
            if (l3_pack(bk, &r->pack) != AMB_OK) r->pack = NULL;
            else if (cache_path(level, key, voxel, path, MAX_PATH, 1)) cache_write(path, r->pack);
        }
        if (rc == AMB_OK) {
            g_bakes++;
            photon_log("l3: %s at %d units: %ld shell voxels, %ld edge bricks, %ld kept, %ld rays (%.0f / %.0f / %.0f ms shell / rays / sdf)", level, voxel,
                       st.shell_voxels, st.edge_bricks, st.bricks, st.rays, st.ms_shell, st.ms_rays, st.ms_sdf);
        } else if (rc != AMB_ERR_CANCELLED) { g_fails++; photon_log("l3: %s: bake failed (%d)", level, rc); }
        l3_bake_free(bk);
        if (rc == AMB_ERR_CANCELLED) { l3_ready_free(r); free(bsp); return; }
    }
    free(bsp);
    QueryPerformanceCounter(&t1);
    g_last_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    photon_log("l3: %s at %d units: %s in %.0f ms, %d bricks (%s), %d worldlights", level, voxel, how, g_last_ms, r->pack ? r->pack->nbricks : 0,
               r->pack ? "published" : "nothing to correct", r->nlights);
    AcquireSRWLockExclusive(&g_l);
    if (job == g_job) { l3_ready_free(g_ready); g_ready = r; r = NULL; }
    ReleaseSRWLockExclusive(&g_l);
    l3_ready_free(r);
}

static DWORD WINAPI worker(LPVOID mod) {
    LONG done = 0;
    AcquireSRWLockExclusive(&g_l);
    for (;;) {
        while (g_job == done) SleepConditionVariableSRW(&g_cv, &g_l, INFINITE, 0);
        done = g_job;
        char level[MAX_PATH];
        memcpy(level, g_want_level, sizeof level);
        int voxel = g_want_voxel;
        const AmbWorkFiles *files = g_files;
        g_cancel = 0;
        ReleaseSRWLockExclusive(&g_l);
        if (level[0] && voxel > 0 && files) run_job(files, level, voxel, done);
        AcquireSRWLockExclusive(&g_l);
    }
    ReleaseSRWLockExclusive(&g_l);
    FreeLibraryAndExitThread((HMODULE)mod, 0);
    return 0;
}

void l3_work_frame(const AmbWorkFiles *files, const char *level, int voxel) {
    if (!level) level = "";
    if (!files) voxel = 0;
    if (voxel == g_key_voxel && strcmp(level, g_key_level) == 0) return;
    snprintf(g_key_level, sizeof g_key_level, "%s", level);
    g_key_voxel = voxel;
    if (!g_started && voxel > 0 && level[0]) {
        HMODULE self = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)worker, &self)) return;
        HANDLE h = CreateThread(NULL, 0, worker, self, 0, NULL);
        if (!h) { FreeLibrary(self); photon_log("l3: no worker thread: no re-bake"); return; }
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
        g_started = 1;
    }
    AcquireSRWLockExclusive(&g_l);
    snprintf(g_want_level, sizeof g_want_level, "%s", voxel > 0 ? level : "");
    g_want_voxel = voxel;
    g_files = files;
    g_cancel = 1;   /* a bake of the previous job stops */
    g_job++;
    l3_ready_free(g_ready);
    g_ready = NULL;
    if (voxel <= 0 || !level[0]) {   /* nothing wanted: an empty result so the device side drops its textures */
        g_ready = calloc(1, sizeof *g_ready);
    }
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_l);
    if (g_started) photon_log("l3: job %ld: %s at %d units", (long)g_job, level[0] ? level : "(no map)", voxel);
}

L3Ready *l3_work_take(void) {
    L3Ready *r = NULL;
    if (!TryAcquireSRWLockExclusive(&g_l)) return NULL;
    r = g_ready;
    g_ready = NULL;
    ReleaseSRWLockExclusive(&g_l);
    return r;
}

void l3_work_summary(const char *why) {
    photon_log("l3 (%s): bakes %lld, cache hits %lld, failures %lld, last %.0f ms", why, (long long)g_bakes, (long long)g_cache_hits, (long long)g_fails,
               g_last_ms);
}
