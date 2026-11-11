/* SP layer 1 (analysis/47; user, 2026-11-11: shader packs as Workshop addons that require Photon): screen passes from packs.
 *
 * A pack is ordinary Lua (Workshop-legal: no DLLs, no compiled shaders) that calls Photon.RegisterPass (lua/autorun/client/photon_packs.lua).
 * That writes the pass's HLSL source to garrysmod/data/photon/packs/<pack>/<pass>.txt and the list of passes to
 * garrysmod/data/photon/packs/active.txt, one line each:  <pack>/<pass> p0 p1 ... (up to 16 float parameters).
 * Here (device thread) the list is re-read when its time changes (checked every 30 frames), each pass is compiled with GMod's own
 * d3dcompiler_47.dll (ps_3_0, entry "main") when its source changes, errors go to <pass>.err.txt (the menu shows them), and at the main
 * view's end (after Photon's own passes and TAA, before the viewmodel and the HUD) the passes run in list order, each over a copy of the
 * frame so far. Native D3D9 and DXVK alike. No pack listed: nothing runs and nothing is read but one file time every 30 frames.
 *
 * A pass sees (the interface, documented in analysis/47):
 *   s0  the frame so far (sRGB-encoded colour; linear filtering)      s1  the scene depth (INTZ, 0 near .. 1 far; point)
 *   c0  1/width, 1/height, seconds since the pack loaded, frame       c1..c4  inverse view-projection rows (ndc -> world: see below)
 *   c5  eye position (xyz)                                             c6..c9  view-projection rows (world -> clip)
 *   c10..c13  the pass's 16 parameters                                c14  xyz: s2 / s3 / s4 valid this frame
 *   s2  the previous frame's final result (after the packs)          s3  moving models' motion (rg offset to last frame's uv, b 1)
 *   s4  the world's smooth normals (Path traced only; rgb*0.5+0.5)    c15..c18  last frame's view-projection rows
 * world position of a pixel: ndc = (uv.x * 2 - 1, 1 - uv.y * 2, depth, 1); w = (dot(ndc, c1), dot(ndc, c2), dot(ndc, c3), dot(ndc, c4));
 * pos = w.xyz / w.w. The pass writes the new colour (alpha ignored). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "packs.h"
#include "dmirror.h"
#include "d3dconst.h"
#include "frames.h"
#include "log.h"
#include "shadows.h"
#include "shaders/shpass_vs.h"

long photon_shadows_view(float iv[4][4], float vp[4][4], float cam[3], void **depth, unsigned *w, unsigned *h);   /* shadows.c */

enum { MAXPASS = 16, NPARAM = 16 };
typedef struct {
    char name[128];            /* <pack>/<pass> */
    float param[NPARAM];
    FILETIME src_time;         /* the source compiled last */
    DWORD *code; void *ps;     /* bytecode (malloc'd), the D3D9 shader made from it by dmirror's helper */
    int failed;
} Pass;
static Pass g_pass[MAXPASS]; static int g_npass;
static wchar_t g_dir[MAX_PATH]; static int g_dir_ok = -1;
static FILETIME g_list_time; static unsigned long long g_checked;
static void *g_vs;
static IDirect3DTexture9 *g_src; static IDirect3DSurface9 *g_src_s; static UINT g_sw, g_sh; static D3DFORMAT g_sf;
static IDirect3DTexture9 *g_prev; static IDirect3DSurface9 *g_prev_s; static int g_prev_ok; static float g_prev_vp[4][4];   /* last frame's result */
static LARGE_INTEGER g_t0, g_freq;
static long long g_runs, g_compiles, g_compile_fails;

typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, void **, void **);
static D3DCompileFn g_compile;
typedef struct { void **vt; } Blob;   /* ID3DBlob: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
static void *blob_ptr(void *b) { return ((void *(WINAPI *)(void *))((Blob *)b)->vt[3])(b); }
static SIZE_T blob_size(void *b) { return ((SIZE_T (WINAPI *)(void *))((Blob *)b)->vt[4])(b); }
static void blob_release(void *b) { if (b) ((ULONG (WINAPI *)(void *))((Blob *)b)->vt[2])(b); }

static int dir_ok(void) {   /* garrysmod\data\photon\packs (gmod.exe is in <game>\bin\win64) */
    if (g_dir_ok >= 0) return g_dir_ok;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *sl = n ? wcsrchr(exe, L'\\') : NULL;
    if (!sl) return g_dir_ok = 0;
    *sl = 0;
    size_t el = wcslen(exe);
    if (el > 10 && _wcsicmp(exe + el - 10, L"\\bin\\win64") == 0) exe[el - 10] = 0;
    _snwprintf(g_dir, MAX_PATH, L"%ls\\garrysmod\\data\\photon\\packs", exe);
    return g_dir_ok = 1;
}
static int file_time(const wchar_t *p, FILETIME *t) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) return 0;
    *t = a.ftLastWriteTime; return 1;
}
static char *slurp(const wchar_t *p, size_t *n) {
    FILE *f = _wfopen(p, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (l >= 0 && l < (4 << 20)) ? malloc((size_t)l + 1) : NULL;
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[l] = 0; *n = (size_t)l; }
    return b;
}
static int name_ok(const char *s) {   /* <pack>/<pass>: letters, digits, _ - . and one slash; no ".." */
    int slash = 0;
    if (!s[0] || strstr(s, "..")) return 0;
    for (const char *c = s; *c; c++) {
        if (*c == '/') { if (++slash > 1) return 0; continue; }
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) return 0;
    }
    return slash == 1;
}
static void pass_free(Pass *p) {
    free(p->code); p->code = NULL;
    if (p->ps) { IUnknown_Release((IUnknown *)p->ps); p->ps = NULL; }
}

static void compile(Pass *p) {
    wchar_t src[MAX_PATH], err[MAX_PATH], wn[128];
    MultiByteToWideChar(CP_UTF8, 0, p->name, -1, wn, 128);
    for (wchar_t *c = wn; *c; c++) if (*c == L'/') *c = L'\\';
    _snwprintf(src, MAX_PATH, L"%ls\\%ls.txt", g_dir, wn);
    _snwprintf(err, MAX_PATH, L"%ls\\%ls.err.txt", g_dir, wn);
    FILETIME t;
    if (!file_time(src, &t)) { if (!p->failed) photon_log("packs: %s: no source file", p->name); p->failed = 1; return; }
    if (p->code && CompareFileTime(&t, &p->src_time) == 0) return;   /* unchanged */
    p->src_time = t;
    if (!g_compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        g_compile = m ? (D3DCompileFn)(void *)GetProcAddress(m, "D3DCompile") : NULL;
        if (!g_compile) { photon_log("packs: no d3dcompiler_47.dll: packs off"); p->failed = 1; return; }
    }
    size_t n = 0; char *text = slurp(src, &n);
    if (!text) { p->failed = 1; return; }
    void *code = NULL, *errs = NULL;
    HRESULT hr = g_compile(text, n, p->name, NULL, NULL, "main", "ps_3_0", 1u << 15 /* OPTIMIZATION_LEVEL3 */, 0, &code, &errs);
    free(text);
    g_compiles++;
    FILE *f = _wfopen(err, L"wb");
    if (f) { if (errs) fwrite(blob_ptr(errs), 1, blob_size(errs), f); fclose(f); }   /* empty: compiled clean */
    if (FAILED(hr) || !code) {
        g_compile_fails++;
        photon_log("packs: %s: compile failed (0x%08lx): %.300s", p->name, (unsigned long)hr, errs ? (const char *)blob_ptr(errs) : "?");
        blob_release(code); blob_release(errs);
        p->failed = 1; return;   /* the old version (if any) keeps running */
    }
    pass_free(p);
    SIZE_T sz = blob_size(code);
    p->code = malloc(sz);
    if (p->code) memcpy(p->code, blob_ptr(code), sz);
    blob_release(code); blob_release(errs);
    p->failed = !p->code;
    photon_log("packs: %s: compiled (%u bytes)", p->name, (unsigned)sz);
}

static void reload(void) {   /* active.txt: the list and the parameters */
    wchar_t lp[MAX_PATH]; _snwprintf(lp, MAX_PATH, L"%ls\\active.txt", g_dir);
    FILETIME t;
    if (!file_time(lp, &t)) { if (g_npass) { for (int i = 0; i < g_npass; i++) pass_free(&g_pass[i]); g_npass = 0; photon_log("packs: no active.txt: none"); } return; }
    if (CompareFileTime(&t, &g_list_time) == 0) { for (int i = 0; i < g_npass; i++) compile(&g_pass[i]); return; }   /* sources may change */
    g_list_time = t;
    size_t n = 0; char *text = slurp(lp, &n);
    if (!text) return;
    Pass np[MAXPASS]; int nn = 0;
    memset(np, 0, sizeof np);
    for (char *line = strtok(text, "\r\n"); line && nn < MAXPASS; line = strtok(NULL, "\r\n")) {
        char nm[128]; int used = 0;
        if (sscanf(line, " %127s%n", nm, &used) != 1 || nm[0] == '#' || !name_ok(nm)) continue;
        Pass *p = &np[nn++];
        snprintf(p->name, sizeof p->name, "%s", nm);
        const char *q = line + used;
        for (int k = 0; k < NPARAM; k++) { int u = 0; if (sscanf(q, " %f%n", &p->param[k], &u) != 1) break; q += u; }
        for (int i = 0; i < g_npass; i++) if (!strcmp(g_pass[i].name, p->name)) {   /* keep the compiled shader */
            p->code = g_pass[i].code; p->ps = g_pass[i].ps; p->src_time = g_pass[i].src_time; g_pass[i].code = NULL; g_pass[i].ps = NULL;
        }
    }
    free(text);
    for (int i = 0; i < g_npass; i++) pass_free(&g_pass[i]);
    memcpy(g_pass, np, sizeof np); g_npass = nn;
    for (int i = 0; i < g_npass; i++) compile(&g_pass[i]);
    photon_log("packs: %d pass(es) active", g_npass);
}

int photon_packs_active(void) { return g_npass > 0; }
/* main thread, each frame's 3D end (shadows.c): whether active.txt exists (one file time every 30 frames), so the main view's end keeps
 * running for packs (it returns early when no Photon pass wants it) and finds a pack installed mid-game */
static volatile LONG g_listed;
void photon_packs_poll(void) {
    static unsigned long long n;
    if (n++ % 30 || !dir_ok()) return;
    wchar_t lp[MAX_PATH]; _snwprintf(lp, MAX_PATH, L"%ls\\active.txt", g_dir);
    FILETIME t;
    g_listed = file_time(lp, &t);
}
int photon_packs_listed(void) { return g_listed || g_npass > 0; }

/* device thread, the main view's end (shadows.c, after TAA) */
void photon_packs_main_view_end(void *bb, void *scene_depth, unsigned w, unsigned h) {
    if (!dir_ok()) return;
    unsigned long long fr = photon_frame_count();
    if (fr - g_checked >= 30 || !g_checked) { g_checked = fr ? fr : 1; reload(); }
    if (!g_npass || !bb || !w || !h) return;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return;
    D3DSURFACE_DESC bd;
    if (FAILED(IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)bb, &bd))) return;
    if (g_src && (g_sw != bd.Width || g_sh != bd.Height || g_sf != bd.Format)) {
        IDirect3DSurface9_Release(g_src_s); IDirect3DTexture9_Release(g_src); g_src = NULL; g_src_s = NULL;
        if (g_prev) { IDirect3DSurface9_Release(g_prev_s); IDirect3DTexture9_Release(g_prev); g_prev = NULL; g_prev_s = NULL; g_prev_ok = 0; }
    }
    if (!g_src) {
        if (FAILED(IDirect3DDevice9_CreateTexture(d, bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g_src, NULL))) { g_src = NULL; return; }
        IDirect3DTexture9_GetSurfaceLevel(g_src, 0, &g_src_s); g_sw = bd.Width; g_sh = bd.Height; g_sf = bd.Format;
    }
    if (!g_prev && SUCCEEDED(IDirect3DDevice9_CreateTexture(d, bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g_prev, NULL)))
        IDirect3DTexture9_GetSurfaceLevel(g_prev, 0, &g_prev_s);
    static unsigned long long last_fr;
    if (fr - last_fr > 2) g_prev_ok = 0;   /* packs were off: last frame's result is stale */
    last_fr = fr;
    void *mv = photon_shadows_motion_tex(), *wn = photon_shadows_world_nrm_tex();
    if (!g_freq.QuadPart) { QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0); }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned vw, vh;
    if (!photon_shadows_view(iv, vp, cam, &dep, &vw, &vh)) return;
    float c[19][4]; memset(c, 0, sizeof c);
    c[14][0] = g_prev_ok ? 1.0f : 0.0f; c[14][1] = mv ? 1.0f : 0.0f; c[14][2] = wn ? 1.0f : 0.0f;   /* s2 / s3 / s4 valid this frame */
    memcpy(c[15], g_prev_ok ? (const float *)g_prev_vp : (const float *)vp, sizeof(float) * 16);   /* last frame's view-projection rows */
    c[0][0] = 1.0f / (float)w; c[0][1] = 1.0f / (float)h; c[0][2] = (float)((double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart); c[0][3] = (float)(fr % 1000000);
    memcpy(c[1], iv, sizeof iv); c[5][0] = cam[0]; c[5][1] = cam[1]; c[5][2] = cam[2]; c[5][3] = 1; memcpy(c[6], vp, sizeof vp);
    RECT r = { 0, 0, (LONG)w, (LONG)h };
    for (int i = 0; i < g_npass; i++) {
        Pass *p = &g_pass[i];
        if (!p->code) continue;
        if (FAILED(IDirect3DDevice9_StretchRect(d, (IDirect3DSurface9 *)bb, &r, g_src_s, &r, D3DTEXF_NONE))) return;
        memcpy(c[10], p->param, sizeof p->param);
        photon_dmirror_fullscreen_texn(1, scene_depth, 0);
        photon_dmirror_fullscreen_texn(2, g_prev_ok ? (void *)g_prev : NULL, 1);
        photon_dmirror_fullscreen_texn(3, mv, 0);
        photon_dmirror_fullscreen_texn(4, wn, 0);
        photon_dmirror_fullscreen(k_shpass_vs, p->code, &g_vs, &p->ps, g_src, (const float (*)[4])c, 19, 0, (int)w, (int)h, bb);
        for (int s = 1; s <= 4; s++) photon_dmirror_fullscreen_texn(s, NULL, 0);
        g_runs++;
    }
    if (g_prev_s && SUCCEEDED(IDirect3DDevice9_StretchRect(d, (IDirect3DSurface9 *)bb, &r, g_prev_s, &r, D3DTEXF_NONE))) { g_prev_ok = 1; memcpy(g_prev_vp, vp, sizeof vp); }
    if (fr % 3000 == 1500) photon_log("packs: %d pass(es), %lld runs, %lld compiles (%lld failed)", g_npass, g_runs, g_compiles, g_compile_fails);
}

void photon_packs_device_lost(void) {
    if (g_src) { IDirect3DSurface9_Release(g_src_s); IDirect3DTexture9_Release(g_src); g_src = NULL; g_src_s = NULL; }
    if (g_prev) { IDirect3DSurface9_Release(g_prev_s); IDirect3DTexture9_Release(g_prev); g_prev = NULL; g_prev_s = NULL; g_prev_ok = 0; }
    for (int i = 0; i < g_npass; i++) if (g_pass[i].ps) { IUnknown_Release((IUnknown *)g_pass[i].ps); g_pass[i].ps = NULL; }
    if (g_vs) { IUnknown_Release((IUnknown *)g_vs); g_vs = NULL; }
}
