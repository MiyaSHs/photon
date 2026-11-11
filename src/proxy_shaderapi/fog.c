/* SP layer 3, first version (analysis/47): volumetric fog (shaders/fog_ps.hlsl) over the main view, at its end after Photon's own passes
 * and TAA, before the shader packs' screen passes. A height fog lit by the sun (Photon's sun shadow map, so light shafts where sun shadows
 * are on) and an ambient colour, plus up to 8 volumes (spheres and boxes): from the "volumes" channel (host push, per frame: 8 floats each:
 * x y z radius density type(0 sphere, 1 box) half-height 0) when it was pushed in the last 10 frames, else from
 * data/photon/packs/volumes.txt (same 8 numbers per line; a pack writes it). Settings photon_fog* (vlg.c): packs set them with
 * Photon.SetFeature. Off by default: nothing runs. Native D3D9 and DXVK. */
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fog.h"
#include "dmirror.h"
#include "d3dconst.h"
#include "frames.h"
#include "vlg.h"
#include "channels.h"
#include "shadows.h"
#include "log.h"
#include "shaders/shpass_vs.h"
#include "shaders/fog_ps.h"
long photon_shadows_view(float iv[4][4], float vp[4][4], float cam[3], void **depth, unsigned *w, unsigned *h);   /* shadows.c */

static void *g_vs, *g_ps;
static float g_file_vol[8][8]; static int g_file_n; static FILETIME g_file_time; static wchar_t g_path[MAX_PATH]; static int g_path_ok = -1;
static long long g_runs;

int photon_fog_on(void) { int f = 0; return photon_vlg_setting("photon_fog", &f) > 0.5f && f; }
static float S(const char *k) { int f = 0; return photon_vlg_setting(k, &f); }

static void read_file(void) {
    if (g_path_ok < 0) {
        wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *sl = n ? wcsrchr(exe, L'\\') : NULL;
        g_path_ok = 0;
        if (sl) {
            *sl = 0; size_t el = wcslen(exe);
            if (el > 10 && _wcsicmp(exe + el - 10, L"\\bin\\win64") == 0) exe[el - 10] = 0;
            _snwprintf(g_path, MAX_PATH, L"%ls\\garrysmod\\data\\photon\\packs\\volumes.txt", exe); g_path_ok = 1;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!g_path_ok || !GetFileAttributesExW(g_path, GetFileExInfoStandard, &a)) { g_file_n = 0; return; }
    if (CompareFileTime(&a.ftLastWriteTime, &g_file_time) == 0) return;
    g_file_time = a.ftLastWriteTime; g_file_n = 0;
    FILE *f = _wfopen(g_path, L"r");
    if (!f) return;
    char line[512];
    while (g_file_n < 8 && fgets(line, sizeof line, f)) {
        float *v = g_file_vol[g_file_n];
        if (sscanf(line, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) >= 5) { if (v[3] > 0) g_file_n++; }
    }
    fclose(f);
}

void photon_fog_main_view_end(void *bb, void *scene_depth, unsigned w, unsigned h) {
    if (!photon_fog_on() || !bb || !scene_depth) return;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return;
    float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned vw, vh;
    if (!photon_shadows_view(iv, vp, cam, &dep, &vw, &vh)) return;
    unsigned long long fr = photon_frame_count();
    if (fr % 30 == 0) read_file();
    float c[30][4]; memset(c, 0, sizeof c);
    memcpy(c[0], iv, sizeof iv);
    float steps = S("photon_fog_steps"); if (steps < 4) steps = 24;
    c[4][0] = cam[0]; c[4][1] = cam[1]; c[4][2] = cam[2]; c[4][3] = steps;
    c[5][0] = S("photon_fog_density"); c[5][1] = S("photon_fog_height"); c[5][2] = S("photon_fog_falloff"); c[5][3] = S("photon_fog_dist");
    c[6][0] = S("photon_fog_r"); c[6][1] = S("photon_fog_g"); c[6][2] = S("photon_fog_b"); c[6][3] = S("photon_fog_sun");
    float rows[12][4]; void *sm = NULL;
    int sun = photon_shadows_sun_state(rows, &sm);
    if (sun) {
        float l = sqrtf(rows[7][0] * rows[7][0] + rows[7][1] * rows[7][1] + rows[7][2] * rows[7][2]);   /* the map's depth row runs away from the sun */
        c[7][0] = l > 0 ? -rows[7][0] / l : 0; c[7][1] = l > 0 ? -rows[7][1] / l : 0; c[7][2] = l > 0 ? -rows[7][2] / l : 1; c[7][3] = 1;
        memcpy(c[9], rows[5], sizeof(float) * 12);   /* the light rows (shadow_ps c5..c7) */
        memcpy(c[12], rows[4], sizeof(float) * 4);   /* bias, strength, 1/size, used/size */
    } else { c[7][2] = 1; }
    c[8][0] = 1.0f; c[8][1] = 0.95f; c[8][2] = 0.85f; c[8][3] = S("photon_fog_aniso");
    c[13][0] = (float)(fr % 4096);
    float vol[64]; unsigned long long vf = 0;
    int nv = photon_chan_get("volumes", vol, 64, &vf);
    const float *src = vol; int n = 0;
    if (nv > 0 && fr - vf <= 10) n = nv / 8; else { src = &g_file_vol[0][0]; n = g_file_n; }
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        const float *v = src + 8 * i;
        c[14 + i][0] = v[0]; c[14 + i][1] = v[1]; c[14 + i][2] = v[2]; c[14 + i][3] = v[3];
        c[22 + i][0] = v[4]; c[22 + i][1] = v[5]; c[22 + i][2] = v[6] > 0 ? v[6] : v[3];
    }
    c[13][3] = (float)n;
    IDirect3DSurface9 *bs = bb;
    static IDirect3DTexture9 *src_t; static IDirect3DSurface9 *src_s; static UINT sw, sh; static D3DFORMAT sf;
    D3DSURFACE_DESC bd; if (FAILED(IDirect3DSurface9_GetDesc(bs, &bd))) return;
    if (src_t && (sw != bd.Width || sh != bd.Height || sf != bd.Format)) { IDirect3DSurface9_Release(src_s); IDirect3DTexture9_Release(src_t); src_t = NULL; }
    if (!src_t) {
        if (FAILED(IDirect3DDevice9_CreateTexture(d, bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &src_t, NULL))) { src_t = NULL; return; }
        IDirect3DTexture9_GetSurfaceLevel(src_t, 0, &src_s); sw = bd.Width; sh = bd.Height; sf = bd.Format;
    }
    RECT r = { 0, 0, (LONG)w, (LONG)h };
    if (FAILED(IDirect3DDevice9_StretchRect(d, bs, &r, src_s, &r, D3DTEXF_NONE))) return;
    photon_dmirror_fullscreen_texn(1, scene_depth, 0);
    if (sun && sm) photon_dmirror_fullscreen_texn(4, sm, 0);
    photon_dmirror_fullscreen(k_shpass_vs, k_fog_ps, &g_vs, &g_ps, src_t, (const float (*)[4])c, 30, 0, (int)w, (int)h, bb);
    photon_dmirror_fullscreen_texn(1, NULL, 0); photon_dmirror_fullscreen_texn(4, NULL, 0);
    if (++g_runs % 3000 == 1) photon_log("fog: on (density %.2f, height %.0f, sun %s, %d volume(s))", c[5][0], c[5][1], sun ? "shafts" : "off (no sun map)", n);
}
