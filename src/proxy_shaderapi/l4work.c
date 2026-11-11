/* L4 in game (analysis/39): see l4work.h. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "l4work.h"
#include "log.h"
#include "vlg.h"
#include "../ambient/l4_bake.h"

enum { L4_VERSION = 2,   /* 2: no double flip of side-1 faces */
        L4_PASSES = 4, PREFIX = 4096 };   /* paths and bounces: photon_pt_rays / photon_pt_bounces */
enum { RVA_WORLDBRUSH = 0x8afd18, OFF_LIGHTDATA = 0x120, RVA_REDOWNLOAD = 0xe5260, RVA_MARK_REDOWNLOAD = 0xdde30 };
/* R_RedownloadAllLightmaps returns at once unless its "needed" flag (engine DAT_1806ddda1) is set: FUN_1800dde30 sets it, the redownload clears it */
static void redownload(const uint8_t *en) { ((void (*)(void))(en + RVA_MARK_REDOWNLOAD))(); ((void (*)(void))(en + RVA_REDOWNLOAD))(); }
static const DWORD k_engine_ts = 0x6adfbd6c;

typedef struct {
    uint8_t *lump, *orig_prefix;   /* the bake, and the first PREFIX bytes of the map's own lump (to find and check the engine's copy) */
    uint8_t *orig;                 /* the map's own lump, to restore when the setting goes off */
    size_t n;
    int toon;                      /* the lump is the toon light steps, not a bake */
    char level[64];
} L4Ready;

static SRWLOCK g_l = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static const AmbWorkFiles *g_files;
static char g_want_level[MAX_PATH];
static int g_want;
static LONG g_job;
static volatile int g_cancel;
static int g_started;
static L4Ready *g_ready;              /* under g_l: published, not yet applied */
static char g_key_level[MAX_PATH];
static int g_key_want = -1;
static volatile LONG64 g_bakes, g_cache_hits, g_fails, g_applied;

static int g_mode;   /* the worker's job: 1 path-traced lightmaps, 2 toon light steps (TOON, TASKS), 3 flat lightmaps (PT-RT) */

/* TOON: every luxel's light snapped to one of a few flat bands (relative to the map's median luminance), its colour kept: light in steps
 * instead of gradients on the world, with the stock world shaders (only the lightmap data changes). ColorRGBExp32 in and out. */
static double dec_l(const uint8_t *c, double rgb[3]) { double s = ldexp(1.0, (int8_t)c[3]) / 255.0; for (int k = 0; k < 3; k++) rgb[k] = c[k] * s; return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]; }
static void enc(const double v[3], uint8_t o[4]) {
    double mx = v[0] > v[1] ? v[0] : v[1];
    if (v[2] > mx) mx = v[2];
    if (!(mx > 1e-9)) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    int e = (int)ceil(log2(mx));
    if (e < -128) e = -128;
    if (e > 127) e = 127;
    double sc = 255.0 / ldexp(1.0, e);
    for (int k = 0; k < 3; k++) { double c = v[k] * sc + 0.5; o[k] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c); }
    o[3] = (uint8_t)(int8_t)e;
}
/* PT-RT (analysis/43): every face's first light style set to one flat value (the others to 0), so the engine draws the world as its
 * surface colour times a known constant; the Vulkan pass then supplies all the light (ptrt.c). ColorRGBExp32 128, exponent 0 = 0.502:
 * the frame then holds about the plain surface colour, leaving the composite's 2x blend room to brighten. */
enum { FLAT_C = 128, FLAT_E = 0 };
static void flat_lump(uint8_t *out, const uint8_t *bsp, size_t bsp_n, size_t n);
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
/* per face: each luxel's light summed over the face's styles (what the engine adds up), snapped to a band, every style scaled by the same
 * factor (hue, the style ratio and switchable lights kept). Bands: one per stop, anchored at the map's median summed luminance. */
#pragma pack(push, 1)
typedef struct {
    uint16_t plane; uint8_t side, on_node; int32_t first_edge; int16_t nedges, texinfo, dispinfo, fog;
    uint8_t styles[4]; int32_t lightofs; float area; int32_t lm_mins[2], lm_size[2]; int32_t orig; uint16_t nprims, first_prim; uint32_t smoothing;
} TFace;
#pragma pack(pop)
typedef struct { float tv[2][4], lv[2][4]; int flags, texdata; } TTexInfo;
static void flat_lump(uint8_t *out, const uint8_t *bsp, size_t bsp_n, size_t n) {
    memset(out, 0, n);
    int hdr = ((const int32_t *)(bsp + 8 + 16 * 53))[1] > 0;
    const int32_t *Lf = (const int32_t *)(bsp + 8 + 16 * (hdr ? 58 : 7)), *Lt = (const int32_t *)(bsp + 8 + 16 * 6);
    if (Lf[1] <= 0) Lf = (const int32_t *)(bsp + 8 + 16 * 7);
    if ((size_t)Lf[0] + (size_t)Lf[1] > bsp_n || (size_t)Lt[0] + (size_t)Lt[1] > bsp_n) return;
    int nf = Lf[1] / (int)sizeof(TFace), nt = Lt[1] / (int)sizeof(TTexInfo);
    const TFace *faces = (const TFace *)(bsp + Lf[0]);
    const TTexInfo *ti = (const TTexInfo *)(bsp + Lt[0]);
    for (int fi = 0; fi < nf; fi++) {
        const TFace *fc = &faces[fi];
        if (fc->lightofs < 0 || fc->texinfo < 0 || fc->texinfo >= nt || fc->styles[0] == 255) continue;
        int nm = (ti[fc->texinfo].flags & 0x800) ? 4 : 1;
        long nl = (long)(fc->lm_size[0] + 1) * (fc->lm_size[1] + 1);
        if ((size_t)fc->lightofs + 4ul * (size_t)(nl * nm) > n) continue;
        for (long i = 0; i < nl * nm; i++) {   /* style 0's block: the flat lightmap and, on bump faces, the three basis lightmaps */
            uint8_t *c = out + fc->lightofs + 4 * i;
            c[0] = c[1] = c[2] = FLAT_C; c[3] = (uint8_t)(int8_t)FLAT_E;
        }
    }
}
static void toon_steps(uint8_t *out, const uint8_t *bsp, size_t bsp_n, const uint8_t *in, size_t n) {
    memcpy(out, in, n);
    int hdr = ((const int32_t *)(bsp + 8 + 16 * 53))[1] > 0;
    const int32_t *Lf = (const int32_t *)(bsp + 8 + 16 * (hdr ? 58 : 7)), *Lt = (const int32_t *)(bsp + 8 + 16 * 6);
    if (Lf[1] <= 0) Lf = (const int32_t *)(bsp + 8 + 16 * 7);
    if ((size_t)Lf[0] + (size_t)Lf[1] > bsp_n || (size_t)Lt[0] + (size_t)Lt[1] > bsp_n) return;
    int nf = Lf[1] / (int)sizeof(TFace), nt = Lt[1] / (int)sizeof(TTexInfo);
    const TFace *faces = (const TFace *)(bsp + Lf[0]);
    const TTexInfo *ti = (const TTexInfo *)(bsp + Lt[0]);
    /* the median summed luminance, over a sample of luxels */
    enum { NS = 65536 };
    float *smp = malloc(sizeof *smp * NS);
    long ns = 0, seen = 0;
    for (int pass = 0; pass < 2; pass++) {
        double med = 0.1;
        if (pass == 1 && smp && ns) { qsort(smp, (size_t)ns, sizeof *smp, cmpf); med = smp[ns / 2] > 1e-4 ? smp[ns / 2] : 0.1; }
        for (int fi = 0; fi < nf; fi++) {
            const TFace *fc = &faces[fi];
            if (fc->lightofs < 0 || fc->texinfo < 0 || fc->texinfo >= nt) continue;
            int ns2 = 0;
            while (ns2 < 4 && fc->styles[ns2] != 255) ns2++;
            int nm = (ti[fc->texinfo].flags & 0x800) ? 4 : 1;
            long nl = (long)(fc->lm_size[0] + 1) * (fc->lm_size[1] + 1);
            if (!ns2 || (size_t)fc->lightofs + 4ul * (size_t)(nl * nm * ns2) > n) continue;
            for (long i = 0; i < nl; i++) {
                double tot = 0, c[3];
                for (int st = 0; st < ns2; st++) tot += dec_l(in + fc->lightofs + 4 * ((long)(st * nm) * nl + i), c);
                if (pass == 0) { if (smp && (seen++ % 7) == 0 && ns < NS) smp[ns++] = (float)tot; continue; }
                if (tot <= 1e-6) continue;
                /* one band per stop (x2), anchored at the median: flat steps at every brightness, no ceiling (a fixed top band darkened
                 * sunlit ground far above the median) */
                double t = pow(2.0, floor(log2(tot / med) + 0.5));
                double k = t * med / tot;
                for (int st = 0; st < ns2; st++)
                    for (int bv = 0; bv < nm; bv++) {
                        long o = fc->lightofs + 4 * ((long)(st * nm + bv) * nl + i);
                        double v[3];
                        dec_l(in + o, v);
                        for (int m = 0; m < 3; m++) v[m] *= k;
                        enc(v, out + o);
                    }
            }
        }
    }
    free(smp);
}

static void ready_free(L4Ready *r) { if (r) { free(r->lump); free(r->orig_prefix); free(r->orig); free(r); } }

static int bake_dir(wchar_t *out, size_t cap) {
    wchar_t base[MAX_PATH];
    if (!photon_vlg_mod_dir(base, MAX_PATH)) return 0;
    wchar_t d[MAX_PATH];
    _snwprintf(d, MAX_PATH, L"%ls\\data\\photon", base); CreateDirectoryW(d, NULL);
    _snwprintf(out, cap, L"%ls\\data\\photon\\bake", base); CreateDirectoryW(out, NULL);
    return 1;
}
static void progress(const char *level, int pass, int passes, double secs) {
    wchar_t d[MAX_PATH], p[MAX_PATH];
    if (!bake_dir(d, MAX_PATH)) return;
    _snwprintf(p, MAX_PATH, L"%ls\\l4_progress.txt", d);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[256];
    int k = snprintf(line, sizeof line, "%s %d %d %.0f\n", level, pass, passes, secs);   /* map, passes done, passes, seconds so far */
    DWORD w;
    WriteFile(h, line, (DWORD)k, &w, NULL);
    CloseHandle(h);
}

typedef struct { L4Bake *b; int t, nt, pass, rays; } Sub;
static DWORD WINAPI sub_thread(LPVOID arg) {
    Sub *s = arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    int nf = l4_bake_faces(s->b), chunk = 64;
    for (int f0 = s->t * chunk; f0 < nf && !g_cancel; f0 += s->nt * chunk)
        l4_bake_pass(s->b, f0, f0 + chunk, s->rays, 0x9E3779B97F4A7C15ull * (uint64_t)(s->pass * 1009 + f0 + 1));
    return 0;
}

static void run_job(const AmbWorkFiles *files, const char *level, LONG job) {
    void *pin = files->pin ? files->pin() : NULL;
    int gma = 0;
    void *fh = files->open(level, &gma, &g_cancel);
    uint8_t *bsp = NULL;
    long long size = 0;
    if (fh) {
        size = files->size(fh);
        if (size > 1036 && size < (1LL << 30) && (bsp = malloc((size_t)size)) != NULL)
            for (long long o = 0; o < size && !g_cancel;) {
                int n = (int)(size - o > (1 << 20) ? (1 << 20) : size - o);
                if (files->read_at(fh, o, bsp + o, n) != n) { free(bsp); bsp = NULL; break; }
                o += n;
            }
        files->close(fh);
    }
    if (pin && files->unpin) files->unpin(pin);
    if (!bsp || g_cancel) { free(bsp); return; }
    unsigned long long key = 1469598103934665603ull;
    for (int i = 0; i < 1036; i++) { key ^= bsp[i]; key *= 1099511628211ull; }
    key ^= (unsigned long long)size; key *= 1099511628211ull;
    key ^= (unsigned long long)L4_VERSION; key *= 1099511628211ull;
    int rays, bounces;   /* the path tracer's settings (the settings page) are part of the cache key */
    photon_vlg_pt_params(&rays, &bounces);
    key ^= (unsigned long long)(rays * 16 + bounces); key *= 1099511628211ull;
    const char *slash = strrchr(level, '/'), *bs = strrchr(level, '\\');
    const char *name = slash > bs ? slash + 1 : bs ? bs + 1 : level;
    char nm[96];
    snprintf(nm, sizeof nm, "%.64s", name);
    char *dot = strrchr(nm, '.');
    if (dot) *dot = 0;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!bake_dir(dir, MAX_PATH)) { free(bsp); return; }
    _snwprintf(path, MAX_PATH, L"%ls\\%hs_%016llx_l4.lump", dir, nm, key);
    int hdr = ((const int32_t *)(bsp + 8 + 16 * 53))[1] > 0, li = hdr ? 53 : 8;
    int32_t lo = ((const int32_t *)(bsp + 8 + 16 * li))[0], ll = ((const int32_t *)(bsp + 8 + 16 * li))[1];
    if (lo < 0 || ll <= 0 || (long long)lo + ll > size) { free(bsp); g_fails++; return; }
    L4Ready *r = calloc(1, sizeof *r);
    if (!r) { free(bsp); return; }
    snprintf(r->level, sizeof r->level, "%.63s", level);
    r->n = (size_t)ll;
    r->orig = malloc((size_t)ll);
    r->orig_prefix = malloc(PREFIX);
    if (!r->orig || !r->orig_prefix) { ready_free(r); free(bsp); return; }
    memcpy(r->orig, bsp + lo, (size_t)ll);
    memcpy(r->orig_prefix, bsp + lo, ll < PREFIX ? (size_t)ll : PREFIX);
    if (g_mode == 2 || g_mode == 3) {   /* TOON: the map's own lump snapped to flat bands; PT-RT: one flat value (no bake either way) */
        r->lump = malloc((size_t)ll);
        r->toon = 1;
        if (r->lump) { if (g_mode == 2) toon_steps(r->lump, bsp, (size_t)size, bsp + lo, (size_t)ll); else flat_lump(r->lump, bsp, (size_t)size, (size_t)ll); }
        free(bsp);
        AcquireSRWLockExclusive(&g_l);
        if (job == g_job && r->lump) { ready_free(g_ready); g_ready = r; r = NULL; }
        ReleaseSRWLockExclusive(&g_l);
        if (!r) photon_log("l4: %s: %s ready", level, g_mode == 2 ? "toon light steps" : "flat lightmaps (PT-RT)");
        ready_free(r);
        return;
    }
    /* the cache */
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(h, &sz) && sz.QuadPart == ll && (r->lump = malloc((size_t)ll)) != NULL) {
            DWORD got = 0;
            if (!ReadFile(h, r->lump, (DWORD)ll, &got, NULL) || got != (DWORD)ll) { free(r->lump); r->lump = NULL; }
        }
        CloseHandle(h);
        if (r->lump) { g_cache_hits++; photon_log("l4: %s: path-traced lightmaps from the cache", level); }
    }
    if (!r->lump) {   /* bake: passes on cores - 2 threads, lowest priority */
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        L4Bake *b = NULL;
        if (l4_bake_new(bsp, (size_t)size, bounces, &b)) { ready_free(r); free(bsp); g_fails++; photon_log("l4: %s: the bake could not start", level); return; }
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int nt = (int)si.dwNumberOfProcessors - 2;
        if (nt < 1) nt = 1;
        if (nt > 32) nt = 32;
        photon_log("l4: %s: baking %ld luxel-styles, %d passes of %d paths (%d bounces), %d threads", level, l4_bake_luxels(b), L4_PASSES, rays, bounces, nt);
        for (int p = 0; p < L4_PASSES && !g_cancel; p++) {
            HANDLE th[32];
            Sub sub[32];
            int started = 0;
            for (int t = 0; t < nt; t++) {
                sub[t] = (Sub){ b, t, nt, p, rays };
                th[started] = CreateThread(NULL, 0, sub_thread, &sub[t], 0, NULL);
                if (th[started]) started++;
            }
            WaitForMultipleObjects((DWORD)started, th, TRUE, INFINITE);
            for (int t = 0; t < started; t++) CloseHandle(th[t]);
            QueryPerformanceCounter(&t1);
            progress(level, p + 1, L4_PASSES, (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
        }
        if (!g_cancel) {
            size_t n = 0;
            int idx = 0;
            r->lump = l4_bake_lump(b, &n, &idx);
            if (r->lump && n == (size_t)ll && idx == li) {
                wchar_t tmp[MAX_PATH + 8];
                _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
                HANDLE w = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
                if (w != INVALID_HANDLE_VALUE) {
                    DWORD put = 0;
                    BOOL ok = WriteFile(w, r->lump, (DWORD)n, &put, NULL) && put == (DWORD)n;
                    CloseHandle(w);
                    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
                }
                g_bakes++;
                QueryPerformanceCounter(&t1);
                photon_log("l4: %s: baked in %.1f s", level, (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
            } else { free(r->lump); r->lump = NULL; g_fails++; }
        }
        l4_bake_free(b);
    }
    free(bsp);
    if (!r->lump) { ready_free(r); return; }
    AcquireSRWLockExclusive(&g_l);
    if (job == g_job) { ready_free(g_ready); g_ready = r; r = NULL; }
    ReleaseSRWLockExclusive(&g_l);
    ready_free(r);
}

static DWORD WINAPI worker(LPVOID mod) {
    LONG done = 0;
    AcquireSRWLockExclusive(&g_l);
    for (;;) {
        while (g_job == done) SleepConditionVariableSRW(&g_cv, &g_l, INFINITE, 0);
        done = g_job;
        char level[MAX_PATH];
        memcpy(level, g_want_level, sizeof level);
        int want = g_want;
        g_mode = want;
        const AmbWorkFiles *files = g_files;
        g_cancel = 0;
        ReleaseSRWLockExclusive(&g_l);
        if (level[0] && want && files) run_job(files, level, done);
        AcquireSRWLockExclusive(&g_l);
    }
    ReleaseSRWLockExclusive(&g_l);
    FreeLibraryAndExitThread((HMODULE)mod, 0);
    return 0;
}

void l4_work_frame(const AmbWorkFiles *files, const char *level, int want) {
    if (!level) level = "";
    if (!files) want = 0;
    if (want == g_key_want && strcmp(level, g_key_level) == 0) return;
    snprintf(g_key_level, sizeof g_key_level, "%s", level);
    g_key_want = want;
    if (!g_started && want && level[0]) {
        HMODULE self = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)worker, &self)) return;
        HANDLE h = CreateThread(NULL, 0, worker, self, 0, NULL);
        if (!h) { FreeLibrary(self); photon_log("l4: no worker thread"); return; }
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
        g_started = 1;
    }
    AcquireSRWLockExclusive(&g_l);
    snprintf(g_want_level, sizeof g_want_level, "%s", want ? level : "");
    g_want = want;
    g_files = files;
    g_cancel = 1;
    g_job++;
    ready_free(g_ready);
    g_ready = NULL;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_l);
    if (g_started) photon_log("l4: job %ld: %s (%s)", (long)g_job, level[0] ? level : "(no map)", want == 1 ? "path-traced lightmaps" : want == 2 ? "toon light steps" : want == 3 ? "flat lightmaps (PT-RT)" : "off");
}

/* main thread: apply a published bake (or put the map's own lump back) and rebuild the lightmaps */
static uint8_t *g_applied_orig;   /* the map's own lump while Photon's is in the engine (to restore it) */
static volatile int g_applied_mode;   /* what is in the engine now: 0 the map's own, 1 path-traced, 2 toon, 3 flat (PT-RT) */
int photon_l4_applied_mode(void) { return g_applied_mode; }
static uint8_t *g_applied_at;
static size_t g_applied_n;
void photon_l4_main_tick(void) {
    static int bad;
    if (bad) return;
    HMODULE en = GetModuleHandleA("engine.dll");
    if (!en) return;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const uint8_t *)en + ((const IMAGE_DOS_HEADER *)en)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != k_engine_ts) { bad = 1; photon_log("l4: engine build 0x%lx is not the checked one: off", nt->FileHeader.TimeDateStamp); return; }
    const uint8_t *wb = *(const uint8_t *const *)((const uint8_t *)en + RVA_WORLDBRUSH);
    uint8_t *ld = wb ? *(uint8_t *const *)(wb + OFF_LIGHTDATA) : NULL;
    /* setting off (or another map) while Photon's lump is in: put the map's own back */
    if (g_applied_orig && (!g_key_want || ld != g_applied_at)) {
        if (ld == g_applied_at) {
            memcpy(ld, g_applied_orig, g_applied_n);
            redownload((const uint8_t *)en);
            photon_log("l4: the map's own lightmaps restored");
        }
        free(g_applied_orig); g_applied_orig = NULL; g_applied_at = NULL; g_applied_mode = 0;
    }
    L4Ready *r = NULL;
    if (!TryAcquireSRWLockExclusive(&g_l)) return;
    r = g_ready; g_ready = NULL;
    ReleaseSRWLockExclusive(&g_l);
    if (!r) return;
    if (!ld || memcmp(ld, r->orig_prefix, r->n < PREFIX ? r->n : PREFIX) != 0) {
        photon_log("l4: %s: the engine's lighting data does not match the map's lump: not applied", r->level);
        ready_free(r);
        return;
    }
    memcpy(ld, r->lump, r->n);
    redownload((const uint8_t *)en);   /* R_RedownloadAllLightmaps, flagged as needed */
    g_applied++;
    g_applied_orig = r->orig; r->orig = NULL;
    g_applied_at = ld; g_applied_n = r->n;
    g_applied_mode = r->toon ? g_mode : 1;
    photon_log("l4: %s: %s applied (%zu bytes)", r->level, g_applied_mode == 3 ? "flat lightmaps (PT-RT)" : r->toon ? "toon light steps" : "path-traced lightmaps", r->n);
    ready_free(r);
}

void l4_work_summary(const char *why) {
    photon_log("l4 (%s): bakes %lld, cache hits %lld, failures %lld, applied %lld", why, (long long)g_bakes, (long long)g_cache_hits, (long long)g_fails,
               (long long)g_applied);
}
