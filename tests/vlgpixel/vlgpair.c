/* vlgpair: T3.4c2 option A probe ("SM3-ify the vs20/ps20b pair"), headless on DXVK/lavapipe.
 *
 * Per case it draws the same curved grid (stock vertex layout, uncompressed) with:
 *   A  GMod's own vertexlit_and_unlit_generic_vs20 + _ps20b combo (from the user's VPK)
 *   B  our vs_2_0 + ps_2_b compile of the same combo (compiler sanity only)
 *   C  our vs_3_0 + ps_3_0 compile of the same combo, SM2 defines, PHOTON_FORCE_STOCK=1
 *   D  our vs_3_0 + Photon ps_3_0 (photon_vlg_ps2x.fxc) with the Photon flag off (c32.x = 0, c33..c55 random)
 * under three PS fog setups (shaderapi-style: none = fog params 0; range: c12.x = 0; height: c12.x = 1 + fog to
 * dest alpha) and checks C == A and D == A within 2/255 per channel (RGBA, float target).
 *   FF   D3DRS_FOGENABLE = TRUE (what shaderapi would set if r_pixelfog were 0): A changes, D does not
 *        (DXVK applies fixed-function fog only to SM < 3 pixel shaders) -- informational.
 *   ON   flag on, debug view 1 (diffuse lighting only), the VS's own lights + ambient cube in the Photon block:
 *        compared per pixel with a CPU reference of the stock VS light formula evaluated at the probed
 *        interpolants (TEXCOORD7 world position, TEXCOORD4 normal); plus the vertex-lit stock diffuse (debug probe
 *        of TEXCOORD2) for contrast.
 *   OCC  (P1 envmap occlusion, c56; cases with occ = 1) with a white base texture (alpha 0), an envmap mask, c1 = 1
 *        and no fog (DETAILTEXTURE statics: a white detail texture with alpha 0 and g_DetailBlendFactor c4.w = 0, which
 *        leaves the base alone in every DETAIL_BLEND_MODE), the image drawn with a black cubemap IS the final diffuse lighting, and image(cube) - image(black)
 *        is the envmap term. Cubemap: 4x4 faces with 3 mips, the last mip a uniform grey (Y = 64/255), s1 with linear
 *        mip filtering. Per diffuse source (stock: c32.x = 0; Photon per-pixel: c32.x = 1 when on) and lighting level
 *        (dark: every light source x 0.05; bright: ambient 1.0 grey, lights x 4, VHV 1.0, lightmap 0.5), with c56
 *        default (s 1, clamp 0..1, k 1) and a second setting (s 0.6, clamp 0.15..1.3, k 0.8): the occluded term must be
 *        f * stock term per pixel and channel, f = lerp(1, clamp(Y(diffuse) / (Y(lowest mip) + 1e-4) / k, kmin, kmax),
 *        s) (photon_envocc_ps.h), and with the defaults dark must drop it to <= 0.3 x stock (summed luminance) and
 *        bright keep it within 0.8..1.2 x. The flag-off draws (B, C, D, FF) have c56.x = 0 with c56.yzw random.
 * usage: vlgpair.exe <list>; list lines: "probe <wpos.bin> <nrm.bin> <col.bin>",
 *        "case <name> <numlights> <waterfog> <skin> <stock_vs> <stock_ps> <vs20> <ps20b> <vs30> <ps30off> <ps30> [on [occ]]"
 *        (optional on = 0 skips the flag-on check: a combo without the Photon block, e.g. LIGHTING_PREVIEW or
 *        STATIC_LIGHT_LIGHTMAP dynamics, or a VS without lighting; occ = 1 runs OCC: the PS has the occlusion).
 *        Built in CMake; scripts/run_vlgpixel.sh --pair feeds it the built photon_vlg_vs20/_ps20b files
 *        (tools/t34_vlgpair_list.py).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_lightblock.h"

#define W 64
#define H 64
#define GN 4 /* grid vertices per side */

typedef struct { float pos[4], n[4], t0[4], t1[4], pf[4], nf[4], c0[4], c1[4], bw[4]; DWORD bi; } Vtx;
static const D3DVERTEXELEMENT9 g_decl[] = {
    {0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0},
    {0, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_NORMAL, 0},
    {0, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 48, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 1},
    {0, 64, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 1},
    {0, 80, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_NORMAL, 1},
    {0, 96, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_COLOR, 0},
    {0, 112, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_COLOR, 1},
    {0, 128, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_BLENDWEIGHT, 0},
    {0, 144, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
    D3DDECL_END()};

static IDirect3DDevice9 *g_dev;
static IDirect3DSurface9 *g_rt, *g_sys;
static IDirect3DVertexDeclaration9 *g_vdecl;
static IDirect3DTexture9 *g_tex, *g_white, *g_mask, *g_black, *g_lmap;
static IDirect3DCubeTexture9 *g_cube, *g_cubeocc, *g_cubeblk;
static IDirect3DBaseTexture9 *g_s0, *g_s1, *g_s2;   /* draw() binds these on s0/s1/s2 when set (OCC), else g_tex/g_cube/g_tex */
static Vtx g_v[GN * GN];
static WORD g_idx[(GN - 1) * (GN - 1) * 6];
static float g_vc[256][4], g_pc[64][4];
static BOOL g_vb[16];
static int g_vi[4][4];
static float A[H][W][4], Bm[H][W][4], P1[H][W][4], P2[H][W][4], P3[H][W][4];

static void *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n);
    if (fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}
static IDirect3DVertexShader9 *load_vs(const char *p)
{
    long n; void *b = slurp(p, &n); IDirect3DVertexShader9 *s = NULL;
    if (!b) { printf("cannot read %s\n", p); return NULL; }
    HRESULT hr = IDirect3DDevice9_CreateVertexShader(g_dev, b, &s); free(b);
    if (FAILED(hr)) printf("CreateVertexShader(%s) hr=0x%08lx\n", p, (unsigned long)hr);
    return s;
}
static IDirect3DPixelShader9 *load_ps(const char *p)
{
    long n; void *b = slurp(p, &n); IDirect3DPixelShader9 *s = NULL;
    if (!b) { printf("cannot read %s\n", p); return NULL; }
    HRESULT hr = IDirect3DDevice9_CreatePixelShader(g_dev, b, &s); free(b);
    if (FAILED(hr)) printf("CreatePixelShader(%s) hr=0x%08lx\n", p, (unsigned long)hr);
    return s;
}
static unsigned g_seed;
static float frand(float lo, float hi) { g_seed = g_seed * 1664525u + 1013904223u; return lo + (hi - lo) * ((g_seed >> 8) / 16777216.0f); }

/* grid: x, y in [-40, 40], z = 8 + 0.004 (x^2 - y^2), normal = (-0.008 x, 0.008 y, 1) (unnormalized on purpose) */
static void make_grid(void)
{
    for (int j = 0; j < GN; j++) for (int i = 0; i < GN; i++) {
        Vtx *o = &g_v[j * GN + i];
        memset(o, 0, sizeof *o);
        float u = i / (float)(GN - 1), v = j / (float)(GN - 1), x = -40 + 80 * u, y = -40 + 80 * v;
        o->pos[0] = x; o->pos[1] = y; o->pos[2] = 8 + 0.004f * (x * x - y * y); o->pos[3] = 1;
        o->n[0] = -0.008f * x; o->n[1] = 0.008f * y; o->n[2] = 1; o->n[3] = 0;
        o->t0[0] = u; o->t0[1] = v; o->t0[2] = 1; o->t0[3] = 1;
        o->t1[0] = 2 * u; o->t1[1] = 2 * v;
        o->c0[0] = 0.8f; o->c0[1] = 0.6f; o->c0[2] = 0.4f; o->c0[3] = 1;
        o->c1[0] = 0.2f + 0.3f * u; o->c1[1] = 0.25f; o->c1[2] = 0.3f * v; o->c1[3] = 1;  /* VHV static light */
        o->bw[0] = 0.6f; o->bw[1] = 0.3f;
        o->bi = 0;
    }
    int k = 0;
    for (int j = 0; j < GN - 1; j++) for (int i = 0; i < GN - 1; i++) {
        WORD a = j * GN + i, b = a + 1, c = a + GN, d = c + 1;
        g_idx[k++] = a; g_idx[k++] = b; g_idx[k++] = c; g_idx[k++] = b; g_idx[k++] = d; g_idx[k++] = c;
    }
}

typedef struct { float pos[3], col[3], att[3]; } Light;
static Light g_l[2] = {{{-10, 12, 20}, {1.6f, 1.1f, 0.7f}, {0.0f, 0.02f, 0.003f}},
                       {{25, -20, 14}, {0.4f, 0.8f, 1.5f}, {0.2f, 0.0f, 0.002f}}};
static float g_amb[6][3] = {{0.10f, 0.05f, 0.02f}, {0.02f, 0.06f, 0.10f}, {0.08f, 0.08f, 0.03f},
                            {0.03f, 0.02f, 0.07f}, {0.12f, 0.11f, 0.10f}, {0.01f, 0.01f, 0.02f}};

static void vs_consts(int nl, int fogmode_unused)
{
    (void)fogmode_unused;
    memset(g_vc, 0, sizeof g_vc);
    float c0[4] = {0, 1, 2, 0.5f}, c1[4] = {1 / 2.2f, 1, 1 / 3.0f, 0};
    memcpy(g_vc[0], c0, 16); memcpy(g_vc[1], c1, 16);
    g_vc[2][0] = 0; g_vc[2][1] = 0; g_vc[2][2] = 200; g_vc[2][3] = 9;     /* cEyePosWaterZ */
    g_vc[3][0] = 1;                                                         /* cFlexScale */
    /* cViewProj (c8..c11), orthographic: clip = (x/42, y/42, z/40 + 0.3, 1); cViewProjZ c13 */
    g_vc[8][0] = 1 / 42.0f; g_vc[9][1] = 1 / 42.0f; g_vc[10][2] = 1 / 40.0f; g_vc[10][3] = 0.3f; g_vc[11][3] = 1;
    memcpy(g_vc[4], g_vc[8], 64);                                           /* cModelViewProj (unused by VLG) */
    memcpy(g_vc[13], g_vc[10], 16); memcpy(g_vc[12], g_vc[10], 16);
    g_vc[16][0] = 1.2f; g_vc[16][1] = 1; g_vc[16][2] = 0; g_vc[16][3] = 1.5f; /* cFogParams: VS oFog = 1.2 - 1.5 z */
    for (int f = 0; f < 6; f++) memcpy(g_vc[21 + f], g_amb[f], 12);       /* cAmbientCubeX/Y/Z [+,-] */
    for (int k = 0; k < 4; k++) {                                           /* cLightInfo c27 + 5k */
        float *L = g_vc[27 + 5 * k];
        if (k < 2) {
            memcpy(L + 0, g_l[k].col, 12); L[3] = 0;                        /* color, w = 0 (not directional) */
            L[4] = 0; L[5] = 0; L[6] = -1; L[7] = 0;                        /* dir, w = 0 (point) */
            memcpy(L + 8, g_l[k].pos, 12); L[11] = 1;
            L[12] = 1; L[13] = 0; L[14] = 0; L[15] = 1;                     /* spot params (ignored for a point) */
            memcpy(L + 16, g_l[k].att, 12);
        }
    }
    g_vc[37][0] = g_vc[37][1] = g_vc[37][2] = g_vc[37][3] = 1;             /* cModulationColor */
    g_vc[48][0] = 1; g_vc[49][1] = 1;                                       /* cBaseTexCoordTransform */
    g_vc[52][0] = 2; g_vc[53][1] = 2;                                       /* cDetailTexCoordTransform */
    for (int b = 0; b < 3; b++) { g_vc[58 + b][b] = 1; }                    /* cModel[0] = identity (bone 0) */
    for (int k = 0; k < 16; k++) g_vb[k] = k < nl;                         /* g_bLightEnabled b0..b3 */
    memset(g_vi, 0, sizeof g_vi);
    g_vi[0][0] = nl; g_vi[0][1] = 0; g_vi[0][2] = 1;                       /* i0 light count (loop count, start, step) */
}

/* fog: 0 none, 1 range, 2 height; seed: random fill of the remaining PS constants */
static void ps_consts(int fog, unsigned seed)
{
    g_seed = seed;
    for (int r = 0; r < 64; r++) for (int c = 0; c < 4; c++) g_pc[r][c] = frand(0.05f, 0.95f);
    g_pc[1][0] = 0.9f; g_pc[1][1] = 0.8f; g_pc[1][2] = 0.7f; g_pc[1][3] = 1;      /* g_DiffuseModulation */
    g_pc[12][0] = fog == 2 ? 1.0f : 0.0f; g_pc[12][1] = 0; g_pc[12][2] = fog == 2 ? 1.0f : 0.0f; g_pc[12][3] = 1;
    g_pc[20][0] = 0; g_pc[20][1] = 0; g_pc[20][2] = 200; g_pc[20][3] = 0;         /* g_EyePos */
    if (fog == 0) { g_pc[21][0] = 0; g_pc[21][1] = 9; g_pc[21][2] = 0; g_pc[21][3] = 0; }  /* shaderapi "no fog" */
    else { g_pc[21][0] = 0.3f; g_pc[21][1] = 9; g_pc[21][2] = 0.85f; g_pc[21][3] = 1.6f; }
    g_pc[29][0] = 0.3f; g_pc[29][1] = 0.5f; g_pc[29][2] = 0.7f; g_pc[29][3] = 1;  /* g_LinearFogColor */
    g_pc[30][0] = 1; g_pc[30][1] = 1; g_pc[30][2] = 1; g_pc[30][3] = 1;           /* cLightScale */
    g_pc[32][0] = 0;                                                               /* Photon flag off */
    g_pc[56][0] = 0;                                  /* P1 occlusion off (c56.yzw stay random: only x gates it) */
}

static int draw(IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps, int ffog, float out[H][W][4])
{
    IDirect3DDevice9 *d = g_dev;
    IDirect3DDevice9_SetRenderTarget(d, 0, g_rt);
    IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    IDirect3DDevice9_BeginScene(d);
    IDirect3DDevice9_SetVertexDeclaration(d, g_vdecl);
    IDirect3DDevice9_SetVertexShader(d, vs);
    IDirect3DDevice9_SetPixelShader(d, ps);
    IDirect3DDevice9_SetVertexShaderConstantF(d, 0, &g_vc[0][0], 256);
    IDirect3DDevice9_SetVertexShaderConstantB(d, 0, g_vb, 16);
    IDirect3DDevice9_SetVertexShaderConstantI(d, 0, &g_vi[0][0], 4);
    IDirect3DDevice9_SetPixelShaderConstantF(d, 0, &g_pc[0][0], 64);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, ffog);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGCOLOR, 0x00204080);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    IDirect3DDevice9_SetTexture(d, 0, g_s0 ? g_s0 : (IDirect3DBaseTexture9 *)g_tex);
    IDirect3DDevice9_SetTexture(d, 1, g_s1 ? g_s1 : (IDirect3DBaseTexture9 *)g_cube);
    /* detail (s2, at the detail UVs) and the depth-blend depth texture (s10): the base pattern, so DETAILTEXTURE and
     * DEPTHBLEND statics read real texels (unused by the other statics) */
    IDirect3DDevice9_SetTexture(d, 2, g_s2 ? g_s2 : (IDirect3DBaseTexture9 *)g_tex);
    IDirect3DDevice9_SetTexture(d, 10, (IDirect3DBaseTexture9 *)g_tex);
    /* the lightmap (s12, STATIC_LIGHT_LIGHTMAP): empty, every such case is black in stock and ours and compares nothing. OCC binds its own. */
    if (!g_s0) IDirect3DDevice9_SetTexture(d, 12, (IDirect3DBaseTexture9 *)g_tex);
    HRESULT hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(d, D3DPT_TRIANGLELIST, 0, GN * GN, (GN - 1) * (GN - 1) * 2,
                                                         g_idx, D3DFMT_INDEX16, g_v, sizeof(Vtx));
    IDirect3DDevice9_EndScene(d);
    if (FAILED(hr)) { printf("Draw hr=0x%08lx\n", (unsigned long)hr); return 1; }
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(d, g_rt, g_sys))) return 1;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(g_sys, &lr, NULL, D3DLOCK_READONLY))) return 1;
    for (int y = 0; y < H; y++) memcpy(out[y], (const BYTE *)lr.pBits + y * lr.Pitch, W * 16);
    IDirect3DSurface9_UnlockRect(g_sys);
    return 0;
}

typedef struct { double maxd; int bad, covered, distinct; } Cmp;
static Cmp cmp(float a[H][W][4], float b[H][W][4], double tol)
{
    Cmp r = {0, 0, 0, 0};
    float seen[512][3]; int ns = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int cov = a[y][x][0] != 0 || a[y][x][1] != 0 || a[y][x][2] != 0;
        r.covered += cov;
        for (int c = 0; c < 4; c++) {
            double d = fabs((double)a[y][x][c] - b[y][x][c]);
            if (!(d <= tol)) { r.bad++; d = isnan(d) ? 1e9 : d; }
            if (d > r.maxd) r.maxd = d;
        }
        if (ns < 512) {
            int k; for (k = 0; k < ns; k++) if (!memcmp(seen[k], a[y][x], 12)) break;
            if (k == ns) memcpy(seen[ns++], a[y][x], 12);
        }
    }
    r.distinct = ns;
    return r;
}

/* CPU reference of the stock VS light math (DoLightingUnrolled: lights + ambient, Lambert) at a pixel */
static void ref_light(const float wp[3], const float nn[3], int nl, double out[3])
{
    double n[3] = {nn[0], nn[1], nn[2]}, l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; i++) n[i] /= l;
    double lit[3] = {0, 0, 0};
    for (int k = 0; k < nl; k++) {
        double t[3], d2 = 0;
        for (int i = 0; i < 3; i++) { t[i] = g_l[k].pos[i] - wp[i]; d2 += t[i] * t[i]; }
        double d = sqrt(d2), ndl = (n[0] * t[0] + n[1] * t[1] + n[2] * t[2]) / d;
        double att = 1 / (g_l[k].att[0] + g_l[k].att[1] * d + g_l[k].att[2] * d2);
        if (ndl < 0) ndl = 0;
        for (int i = 0; i < 3; i++) lit[i] += g_l[k].col[i] * ndl * att;
    }
    for (int i = 0; i < 3; i++) {
        double a = 0;
        for (int ax = 0; ax < 3; ax++) a += n[ax] * n[ax] * g_amb[ax * 2 + (n[ax] >= 0 ? 0 : 1)][i];
        out[i] = lit[i] + a;
    }
}

/* --- OCC: the P1 envmap occlusion (photon_envocc_ps.h) ------------------------------------------------------- */
#define OCC_LOW 64 /* the cubemap's last mip, grey: Y = 64/255 */
static float R0[H][W][4], R1[H][W][4], R2[H][W][4], R3[H][W][4];
static double luma(const float *c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
static double sat1(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
/* per-pixel check of image o against the stock envmap term (s - b) scaled by the factor from the diffuse b */
static int occ_exact(float b[H][W][4], float s[H][W][4], float o[H][W][4], const float c56[4], double *maxerr, int *npx)
{
    int bad = 0;
    double yc = OCC_LOW / 255.0 + 1e-4;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double es = luma(s[y][x]) - luma(b[y][x]);
        if (!(fabs(es) > 1e-3)) continue;
        /* photon_envocc_ps.h: t = saturate(log2(x) c56.x + c56.y), factor = smoothstep01(t) c56.z + c56.w */
        double t = sat1(log2(luma(b[y][x]) / yc) * c56[0] + c56[1]), f = t * t * (3 - 2 * t) * c56[2] + c56[3];
        (*npx)++;
        for (int c = 0; c < 3; c++) {
            double st = (double)s[y][x][c] - b[y][x][c], oc = (double)o[y][x][c] - b[y][x][c];
            double e = fabs(oc - f * st);
            if (e > *maxerr) *maxerr = e;
            if (!(e <= 2e-4 + 2e-3 * fabs(st))) bad++;
        }
    }
    return bad;
}
static double occ_ratio(float b[H][W][4], float s[H][W][4], float o[H][W][4], double *estock)
{
    double so = 0, ss = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        ss += luma(s[y][x]) - luma(b[y][x]);
        so += luma(o[y][x]) - luma(b[y][x]);
    }
    *estock = ss;
    return ss > 0 ? so / ss : 0;
}
/* light sources at a level: 0 dark (x 0.05), 1 bright (ambient 1.0 grey, lights x 4, VHV 1.0, lightmap 0.5) */
static void occ_level(int bright, int nl, PhotonLightBlock *lb)
{
    float m = bright ? 4.0f : 0.05f;
    for (int f = 0; f < 6; f++) for (int i = 0; i < 3; i++) {
        float a = bright ? 1.0f : g_amb[f][i] * m;
        g_vc[21 + f][i] = a;
        lb->ambient[f][i] = a;
    }
    for (int k = 0; k < 2; k++) for (int i = 0; i < 3; i++) g_vc[27 + 5 * k][i] = g_l[k].col[i] * m;
    for (int v = 0; v < GN * GN; v++) for (int i = 0; i < 3; i++) g_v[v].c1[i] = bright ? 1.0f : 0.02f;
    float dir[3] = {0, 0, -1};
    for (int k = 0; k < nl && k < 2; k++) {
        float col[3] = {g_l[k].col[0] * m, g_l[k].col[1] * m, g_l[k].col[2] * m};
        photon_lb_pack_light(lb->lights[k], PHOTON_LB_TYPE_POINT, col, g_l[k].pos, dir, g_l[k].att[0], g_l[k].att[1],
                             g_l[k].att[2], 0, 0, 0, 0);
    }
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(g_lmap, 0, &lr, NULL, 0);
    DWORD v = bright ? 128 : 5;
    *(DWORD *)lr.pBits = 0xff000000u | (v << 16) | (v << 8) | v;
    IDirect3DTexture9_UnlockRect(g_lmap, 0);
}
/* returns 1 on failure; appends its summary to line */
static int occ_test(IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps, int nl, int on, char *line, int *o, int cap)
{
    static const float c56_alt_in[4] = {0.6f, 0.15f, 1.3f, 0.8f};   /* s, kmin, kmax, k */
    float c56_def[4], c56_alt[4];
    photon_lb_envocc(c56_def, 1, 0, 1, 0.5f);                         /* the vlg.c defaults */
    photon_lb_envocc(c56_alt, c56_alt_in[0], c56_alt_in[1], c56_alt_in[2], c56_alt_in[3]);
    Vtx saved[GN * GN];
    memcpy(saved, g_v, sizeof g_v);
    IDirect3DDevice9 *d = g_dev;
    IDirect3DDevice9_SetSamplerState(d, 1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetTexture(d, 4, (IDirect3DBaseTexture9 *)g_mask);
    IDirect3DDevice9_SetTexture(d, 11, (IDirect3DBaseTexture9 *)g_black);
    IDirect3DDevice9_SetTexture(d, 12, (IDirect3DBaseTexture9 *)g_lmap);
    g_s0 = g_s2 = (IDirect3DBaseTexture9 *)g_white;   /* detail: white with alpha 0 and blend factor 0 (below) */
    int fail = 0, nexact = 0, bad = 0;
    double maxerr = 0, rdark[2] = {0, 0}, rbright[2] = {0, 0};
    for (int mode = 0; mode < (on ? 2 : 1); mode++)
        for (int lvl = 0; lvl < 2; lvl++) {
            vs_consts(nl, 0);
            ps_consts(0, 4242u + lvl);
            g_pc[1][0] = g_pc[1][1] = g_pc[1][2] = g_pc[1][3] = 1;               /* g_DiffuseModulation = 1 */
            g_pc[4][3] = 0;   /* g_DetailBlendFactor 0: every DETAIL_BLEND_MODE leaves the base (and its alpha) alone */
            PhotonLightBlock lb; memset(&lb, 0, sizeof lb);
            occ_level(lvl, nl, &lb);
            if (mode) {
                lb.flags[0] = 1; lb.flags[1] = (float)nl; lb.flags[2] = 0; lb.flags[3] = 0;
                lb.tuning[0] = 1; lb.tuning[1] = 1; lb.tuning[2] = 0; lb.tuning[3] = 0;
                memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, (PHOTON_LB_TUNING - PHOTON_LB_FIRST + 1) * 16);   /* c32..c55 */
            }
            g_pc[56][0] = 0;
            g_s1 = (IDirect3DBaseTexture9 *)g_cubeblk;
            if (draw(vs, ps, 0, R0)) return 1;
            g_s1 = (IDirect3DBaseTexture9 *)g_cubeocc;
            if (draw(vs, ps, 0, R1)) return 1;
            memcpy(g_pc[56], c56_def, 16);
            if (draw(vs, ps, 0, R2)) return 1;
            memcpy(g_pc[56], c56_alt, 16);
            if (draw(vs, ps, 0, R3)) return 1;
            bad += occ_exact(R0, R1, R2, c56_def, &maxerr, &nexact);
            bad += occ_exact(R0, R1, R3, c56_alt, &maxerr, &nexact);
            double es, r = occ_ratio(R0, R1, R2, &es);
            if (!(es > 1.0)) { fail = 1; *o += snprintf(line + *o, cap - *o, " OCC[FAIL no envmap signal (%.3g)]", es); }
            if (lvl) rbright[mode] = r; else rdark[mode] = r;
        }
    memcpy(g_v, saved, sizeof g_v);
    g_s0 = g_s1 = g_s2 = NULL;
    IDirect3DDevice9_SetSamplerState(d, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    for (int s = 4; s <= 12; s += (s == 4 ? 7 : 1)) IDirect3DDevice9_SetTexture(d, s, NULL);
    int ok = bad == 0 && nexact > W * H / 2;
    for (int mode = 0; mode < (on ? 2 : 1); mode++)
        ok = ok && rdark[mode] <= 0.3 && rbright[mode] >= 0.8 && rbright[mode] <= 1.2;
    fail |= !ok;
    *o += snprintf(line + *o, cap - *o, " OCC[%s exact %d px max %.1e; dark %.3f bright %.3f", ok ? "ok" : "FAIL", nexact,
                   maxerr, rdark[0], rbright[0]);
    if (on) *o += snprintf(line + *o, cap - *o, "; per-pixel on: dark %.3f bright %.3f", rdark[1], rbright[1]);
    *o += snprintf(line + *o, cap - *o, "]");
    return fail;
}

static IDirect3DPixelShader9 *g_pw, *g_pn, *g_pcol;
static int run_case(char *name, int nl, int wf, int skin, char *f[7], int on, int occ)
{
    (void)wf; (void)skin;
    IDirect3DVertexShader9 *vsA = load_vs(f[0]), *vsB = load_vs(f[2]), *vs3 = load_vs(f[4]);
    IDirect3DPixelShader9 *psA = load_ps(f[1]), *psB = load_ps(f[3]), *ps3o = load_ps(f[5]), *ps3 = load_ps(f[6]);
    if (!vsA || !vsB || !vs3 || !psA || !psB || !ps3o || !ps3) { printf("%s: shader create FAIL\n", name); return 1; }
    int fail = 0;
    char line[4096]; int o = snprintf(line, sizeof line, "%-14s", name);
    const double tol = 2.0 / 255.0;
    static const char *fn[3] = {"nofog", "range", "height"};
    vs_consts(nl, 0);
    for (int fog = 0; fog < 3; fog++) {
        ps_consts(fog, 777u + fog);
        if (draw(vsA, psA, 0, A)) return 1;
        if (draw(vsB, psB, 0, Bm)) return 1;
        Cmp b = cmp(A, Bm, tol);
        if (draw(vs3, ps3o, 0, Bm)) return 1;
        Cmp c = cmp(A, Bm, tol);
        if (draw(vs3, ps3, 0, Bm)) return 1;
        Cmp dd = cmp(A, Bm, tol);
        int ok = c.bad == 0 && dd.bad == 0 && A[0][0][3] == A[0][0][3] && c.covered > W * H * 3 / 4 && c.distinct >= 64;
        fail |= !ok;
        o += snprintf(line + o, sizeof line - o, " %s[%s C %.1e D %.1e (B %.1e) cov %d col %d]", fn[fog], ok ? "ok" : "FAIL",
                      c.maxd, dd.maxd, b.maxd, c.covered, c.distinct);
    }
    /* fixed-function fog (r_pixelfog 0 world): SM2 pair changes, SM3 pair does not */
    ps_consts(0, 777u);
    draw(vsA, psA, 0, A); draw(vsA, psA, 1, Bm);
    Cmp fa = cmp(A, Bm, tol);
    draw(vs3, ps3, 0, A); draw(vs3, ps3, 1, Bm);
    Cmp fd = cmp(A, Bm, tol);
    o += snprintf(line + o, sizeof line - o, " FF[stock changes %d%% max %.2f, sm3 changes %d px]", fa.bad * 25 / (W * H),
                  fa.maxd, fd.bad);
    /* flag on: per-pixel lighting with the VS's own lights; debug view 1 = diffuse lighting only */
    if (nl > 0 && on) {
        ps_consts(0, 999u);
        PhotonLightBlock lb; memset(&lb, 0, sizeof lb);
        lb.flags[0] = 1; lb.flags[1] = (float)nl; lb.flags[2] = 0; lb.flags[3] = 0;
        for (int fa_ = 0; fa_ < 6; fa_++) memcpy(lb.ambient[fa_], g_amb[fa_], 12);
        float dir[3] = {0, 0, -1};
        for (int k = 0; k < nl; k++)
            photon_lb_pack_light(lb.lights[k], PHOTON_LB_TYPE_POINT, g_l[k].col, g_l[k].pos, dir, g_l[k].att[0],
                                 g_l[k].att[1], g_l[k].att[2], 0, 0, 0, 0);
        lb.tuning[0] = 1; lb.tuning[1] = 1; lb.tuning[2] = 0; lb.tuning[3] = 1;
        memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, sizeof lb);
        if (draw(vs3, ps3, 0, A)) return 1;
        if (draw(vs3, g_pw, 0, P1) || draw(vs3, g_pn, 0, P2) || draw(vs3, g_pcol, 0, P3)) return 1;
        double maxrel = 0, peak_pp = 0, peak_vx = 0, maxdiff_vx = 0; int bad = 0, n = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (P2[y][x][2] == 0) continue;            /* not covered */
            double r[3]; ref_light(P1[y][x], P2[y][x], nl, r); n++;
            for (int c = 0; c < 3; c++) {
                double rel = fabs(A[y][x][c] - r[c]) / fmax(fabs(r[c]), 1e-2);
                if (rel > maxrel) maxrel = rel;
                if (rel > 1e-3) bad++;
                double dv = fabs(A[y][x][c] - P3[y][x][c]); if (dv > maxdiff_vx) maxdiff_vx = dv;
            }
            double s = A[y][x][0] + A[y][x][1] + A[y][x][2], sv = P3[y][x][0] + P3[y][x][1] + P3[y][x][2];
            if (s > peak_pp) peak_pp = s;
            if (sv > peak_vx) peak_vx = sv;
        }
        int ok = bad == 0 && n > W * H * 3 / 4;
        fail |= !ok;
        o += snprintf(line + o, sizeof line - o, " ON[%s rel %.1e vs CPU per-pixel, %d px; peak rgb-sum per-pixel %.3f vs "
                      "vertex %.3f, max |pp-vertex| %.3f]", ok ? "ok" : "FAIL", maxrel, n, peak_pp, peak_vx, maxdiff_vx);
    }
    if (occ) fail |= occ_test(vs3, ps3, nl, on, line, &o, (int)sizeof line);
    printf("%s%s\n", line, fail ? "  <-- FAIL" : "");
    IDirect3DVertexShader9_Release(vsA); IDirect3DVertexShader9_Release(vsB); IDirect3DVertexShader9_Release(vs3);
    IDirect3DPixelShader9_Release(psA); IDirect3DPixelShader9_Release(psB); IDirect3DPixelShader9_Release(ps3o);
    IDirect3DPixelShader9_Release(ps3);
    return fail;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
#define CHECK(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    printf("VLGPAIR FAIL %s hr=0x%08lx\n", what, (unsigned long)hr_); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: vlgpair.exe <list>\n"); return 2; }
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { printf("VLGPAIR FAIL cannot read %s\n", argv[1]); return 1; }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "vlgpair";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("vlgpair", "vlgpair", WS_OVERLAPPEDWINDOW, 0, 0, W + 16, H + 40, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!hwnd || !d3d) { printf("VLGPAIR FAIL window/Direct3DCreate9\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id))) printf("adapter: %s\n", id.Description);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev), "CreateDevice");
    CHECK(IDirect3DDevice9_CreateRenderTarget(g_dev, W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, NULL), "RT");
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &g_sys, NULL), "sysmem");
    CHECK(IDirect3DDevice9_CreateVertexDeclaration(g_dev, g_decl, &g_vdecl), "decl");
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, NULL), "tex");
    IDirect3DTexture9_LockRect(g_tex, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = ((DWORD)(120 + 30 * x) << 24) | ((DWORD)(80 + 40 * y) << 16) |
                                                          ((DWORD)(220 - 40 * x) << 8) | (DWORD)(100 + 30 * y);
    IDirect3DTexture9_UnlockRect(g_tex, 0);
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cube, NULL), "cube");
    for (int fc = 0; fc < 6; fc++) {
        IDirect3DCubeTexture9_LockRect(g_cube, (D3DCUBEMAP_FACES)fc, 0, &lr, NULL, 0);
        *(DWORD *)lr.pBits = 0x80000000u | ((DWORD)(30 * fc) << 16) | ((DWORD)(200 - 25 * fc) << 8) | (DWORD)(60 + 20 * fc);
        IDirect3DCubeTexture9_UnlockRect(g_cube, (D3DCUBEMAP_FACES)fc, 0);
    }
    /* OCC textures: white base (alpha 0: no selfillum, base-alpha mask 1), envmap mask, black, 1x1 lightmap, the
     * mipped cube (last mip uniform OCC_LOW grey) and a black cube */
    IDirect3DTexture9 **t1[4] = {&g_white, &g_mask, &g_black, &g_lmap};
    const DWORD v1[4] = {0x00ffffffu, 0x00ccdde6u, 0xff000000u, 0xff808080u};
    for (int i = 0; i < 4; i++) {
        CHECK(IDirect3DDevice9_CreateTexture(g_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, t1[i], NULL), "occ tex");
        IDirect3DTexture9_LockRect(*t1[i], 0, &lr, NULL, 0);
        *(DWORD *)lr.pBits = v1[i];
        IDirect3DTexture9_UnlockRect(*t1[i], 0);
    }
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 4, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cubeocc, NULL), "cubeocc");
    for (int fc = 0; fc < 6; fc++)
        for (int lv = 0; lv < 3; lv++) {
            IDirect3DCubeTexture9_LockRect(g_cubeocc, (D3DCUBEMAP_FACES)fc, lv, &lr, NULL, 0);
            int n = 4 >> lv;
            for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
                DWORD c = lv == 2 ? 0xff000000u | (OCC_LOW << 16) | (OCC_LOW << 8) | OCC_LOW
                                  : 0xff000000u | ((DWORD)(40 + 35 * fc + 20 * x) << 16) | ((DWORD)(230 - 30 * fc - 10 * y) << 8) |
                                        (DWORD)(90 + 25 * fc + 15 * lv);
                ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = c;
            }
            IDirect3DCubeTexture9_UnlockRect(g_cubeocc, (D3DCUBEMAP_FACES)fc, lv);
        }
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cubeblk, NULL), "cubeblk");
    for (int fc = 0; fc < 6; fc++) {
        IDirect3DCubeTexture9_LockRect(g_cubeblk, (D3DCUBEMAP_FACES)fc, 0, &lr, NULL, 0);
        *(DWORD *)lr.pBits = 0xff000000u;
        IDirect3DCubeTexture9_UnlockRect(g_cubeblk, (D3DCUBEMAP_FACES)fc, 0);
    }
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    for (int s = 0; s < 13; s++) {
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    make_grid();
    char ln[4096]; int cases = 0, fails = 0, nocc = 0;
    while (fgets(ln, sizeof ln, lf)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char a[7][MAX_PATH], name[64]; int nl, wf, sk, on = 1, occ = 0;
        if (sscanf(ln, "probe %259s %259s %259s", a[0], a[1], a[2]) == 3) {
            g_pw = load_ps(a[0]); g_pn = load_ps(a[1]); g_pcol = load_ps(a[2]);
            if (!g_pw || !g_pn || !g_pcol) return 1;
            continue;
        }
        int got = sscanf(ln, "case %63s %d %d %d %259s %259s %259s %259s %259s %259s %259s %d %d", name, &nl, &wf, &sk, a[0],
                         a[1], a[2], a[3], a[4], a[5], a[6], &on, &occ);
        if (got >= 11 && got <= 13) {
            char *f[7]; for (int i = 0; i < 7; i++) f[i] = a[i];
            cases++; nocc += occ != 0; fails += run_case(name, nl, wf, sk, f, on, occ);
        }
    }
    if (fails) { printf("VLGPAIR FAIL %d of %d cases\n", fails, cases); return 1; }
    printf("VLGPAIR OK %d cases (%d with OCC)\n", cases, nocc);
    return 0;
}
