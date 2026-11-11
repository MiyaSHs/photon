/* bumppair: lighting plan v2 P2 pair test for the bumped VertexLitGeneric path, headless on DXVK/lavapipe.
 *
 * Per case it draws the same curved, tangent-framed grid (stock vertex layout, uncompressed) with:
 *   A  GMod's own vertexlit_and_unlit_generic_bump_vs20 + _bump_ps20b combo (from the user's VPK)
 *   C  our vs_3_0 (photon_bump_vs20.vcs) + the PHOTON_FORCE_STOCK ps_3_0 compile of the same PS combo
 *   D  our vs_3_0 + the shipped Photon ps_3_0 (photon_bump_ps20b.vcs) with the Photon flag off (c32.x = 0,
 *      c33..c55 random)
 * with a varied 4x4 normal map, under three PS fog setups (none: fog params 0; range: c12.x = 0; height: c12.x = 1 +
 * fog to dest alpha), and checks C == A and D == A within TOL per channel (RGBA, float target).
 *   FF   D3DRS_FOGENABLE = TRUE (r_pixelfog 0): A changes, D must not (DXVK applies FF fog to SM < 3 PS only).
 *   ON   flag on, c32.y = 4 lights of every type (point, spot, directional, point with a range window), a uniform
 *        1x1 normal map, debug view 1 (diffuse lighting only): compared per pixel with a CPU double-precision
 *        evaluation of the PHOTON block (photon_light_ps.h + the bump call site: Lambert or half-Lambert per the PS
 *        HALFLAMBERT static, the cube only with AMBIENT_LIGHT=1, c55 tuning) at the probed interpolants (TEXCOORD6
 *        world position, TEXCOORD3 normal, TEXCOORD4 tangent + sign) with the bumped normal built on the CPU.
 *   ON2  the same with debug view 2 (bumped world normal * 0.5 + 0.5) against the CPU normal.
 *   OCC  (P1 envmap occlusion, c56; cases with occ > 0) as in vlgpair.c: white base (alpha 0), white detail (alpha 0)
 *        with g_DetailBlendFactor c4.w = 0 (neutral in every DETAIL_BLEND_MODE), c1 = 1, no fog, so the
 *        black-cubemap image is the final diffuse lighting and image(cube) - image(black) the envmap term; per diffuse
 *        source (stock PixelShaderDoLighting: c32.x = 0; Photon per-pixel: c32.x = 1) and level (dark x 0.05; bright:
 *        ambient 1.0 grey, lights x 4) with c56 default and a second setting: the occluded term = f * stock term per
 *        pixel (photon_envocc_ps.h), dark <= 0.3 x stock, bright 0.8..1.2 x (bright only with AMBIENT_LIGHT=1, occ 2:
 *        lights alone leave pixels facing away unlit). The flag-off draws have c56.x = 0 with c56.yzw random.
 * usage: bumppair.exe <list>; list lines: "probe <t6.bin> <t3.bin> <t4.bin>",
 *        "case <name> <vs_lights> <halflambert> <ambient> <stock_vs> <stock_ps> <vs30> <ps30off> <ps30> <on> [occ]"
 *        (vs_lights: lights enabled for the VS through cLightInfo/b0..b3/i0; on = 0 skips ON/ON2; occ 0/1/2).
 * Built in CMake; scripts/run_vlgpixel.sh --bump feeds it the built photon_bump_vs20/_ps20b files
 * (tools/t34_bumppair_list.py). Prints BUMPPAIR OK <n> cases.
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
#define GN 4
#define TOL 1e-5

typedef struct { float pos[4], n[4], t0[4], t1[4], pf[4], nf[4], c0[4], c1[4], bw[4], tan[4]; DWORD bi; } Vtx;
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
    {0, 144, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TANGENT, 0},
    {0, 160, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_BLENDINDICES, 0},
    D3DDECL_END()};

static IDirect3DDevice9 *g_dev;
static IDirect3DSurface9 *g_rt, *g_sys;
static IDirect3DVertexDeclaration9 *g_vdecl;
static IDirect3DTexture9 *g_tex, *g_nrm4, *g_nrm1, *g_nrm, *g_white;
static IDirect3DCubeTexture9 *g_cube, *g_cubeocc, *g_cubeblk;
static IDirect3DBaseTexture9 *g_s0, *g_s1, *g_s2;   /* draw() binds these on s0/s1/s2 when set (OCC), else g_tex/g_cube/g_tex */
static Vtx g_v[GN * GN];
static WORD g_idx[(GN - 1) * (GN - 1) * 6];
static float g_vc[256][4], g_pc[64][4];
static BOOL g_vb[16];
static int g_vi[4][4];
static float A[H][W][4], Bm[H][W][4], P1[H][W][4], P2[H][W][4], P3[H][W][4];
static const BYTE g_n1[4] = {170, 100, 230, 200}; /* uniform normal texel R, G, B, A for ON/ON2 */

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

/* grid: x, y in [-40, 40], z = 8 + 0.004 (x^2 - y^2); normal (-0.008 x, 0.008 y, 1) and tangent (1, 0, 0.008 x),
 * both unnormalized on purpose; binormal sign alternates by column so the PS cross product sign is exercised */
static void make_grid(void)
{
    for (int j = 0; j < GN; j++) for (int i = 0; i < GN; i++) {
        Vtx *o = &g_v[j * GN + i];
        memset(o, 0, sizeof *o);
        float u = i / (float)(GN - 1), v = j / (float)(GN - 1), x = -40 + 80 * u, y = -40 + 80 * v;
        o->pos[0] = x; o->pos[1] = y; o->pos[2] = 8 + 0.004f * (x * x - y * y); o->pos[3] = 1;
        o->n[0] = -0.008f * x; o->n[1] = 0.008f * y; o->n[2] = 1; o->n[3] = 0;
        o->tan[0] = 1; o->tan[1] = 0.1f * v; o->tan[2] = 0.008f * x; o->tan[3] = (i & 2) ? -1.0f : 1.0f;
        o->t0[0] = u; o->t0[1] = v; o->t0[2] = 1; o->t0[3] = 1;
        o->t1[0] = 2 * u; o->t1[1] = 2 * v;
        o->c0[0] = 0.8f; o->c0[1] = 0.6f; o->c0[2] = 0.4f; o->c0[3] = 1;
        o->c1[0] = 0.2f + 0.3f * u; o->c1[1] = 0.25f; o->c1[2] = 0.3f * v; o->c1[3] = 1;
        o->bw[0] = 0.6f; o->bw[1] = 0.3f;
        o->bi = 0;
    }
    int k = 0;
    for (int j = 0; j < GN - 1; j++) for (int i = 0; i < GN - 1; i++) {
        WORD a = j * GN + i, b = a + 1, c = a + GN, d = c + 1;
        g_idx[k++] = a; g_idx[k++] = b; g_idx[k++] = c; g_idx[k++] = b; g_idx[k++] = d; g_idx[k++] = c;
    }
}

/* engine-style lights for the stock pair (all point lights: the VS attenuates, the PS does N.L) */
typedef struct { float pos[3], col[3], att[3]; } Light;
static Light g_l[4] = {{{-10, 12, 20}, {1.6f, 1.1f, 0.7f}, {0.0f, 0.02f, 0.003f}},
                       {{25, -20, 14}, {0.4f, 0.8f, 1.5f}, {0.2f, 0.0f, 0.002f}},
                       {{0, -30, 25}, {0.9f, 0.3f, 0.5f}, {0.5f, 0.01f, 0.001f}},
                       {{30, 25, 12}, {0.3f, 1.0f, 0.4f}, {1.0f, 0.0f, 0.0015f}}};
static float g_amb[6][3] = {{0.10f, 0.05f, 0.02f}, {0.02f, 0.06f, 0.10f}, {0.08f, 0.08f, 0.03f},
                            {0.03f, 0.02f, 0.07f}, {0.12f, 0.11f, 0.10f}, {0.01f, 0.01f, 0.02f}};

/* the Photon block's own lights for ON/ON2: one of every type */
typedef struct { float type, pos[3], col[3], dir[3], att[3], range, falloff, cos_in, cos_out; } PLight;
static PLight g_pl[4] = {
    {PHOTON_LB_TYPE_POINT, {-10, 12, 20}, {1.6f, 1.1f, 0.7f}, {0, 0, -1}, {0.0f, 0.02f, 0.003f}, 0, 0, 0, 0},
    {PHOTON_LB_TYPE_SPOT, {25, -20, 30}, {2.0f, 1.4f, 0.9f}, {-0.4211f, 0.3369f, -0.8422f}, {0.2f, 0.0f, 0.001f}, 80, 2.0f,
     0.95f, 0.75f},
    {PHOTON_LB_TYPE_DIRECTIONAL, {0, 0, 0}, {0.3f, 0.25f, 0.2f}, {0.2860f, 0.1907f, -0.9391f}, {1, 0, 0}, 0, 0, 0, 0},
    {PHOTON_LB_TYPE_POINT, {5, 30, 10}, {0.5f, 0.9f, 0.4f}, {0, 0, -1}, {1.0f, 0.0f, 0.0005f}, 45, 0, 0, 0}};

static void vs_consts(int nl)
{
    memset(g_vc, 0, sizeof g_vc);
    float c0[4] = {0, 1, 2, 0.5f}, c1[4] = {1 / 2.2f, 1, 1 / 3.0f, 0};
    memcpy(g_vc[0], c0, 16); memcpy(g_vc[1], c1, 16);
    g_vc[2][0] = 0; g_vc[2][1] = 0; g_vc[2][2] = 200; g_vc[2][3] = 9;     /* cEyePosWaterZ */
    g_vc[3][0] = 1;                                                         /* cFlexScale */
    g_vc[8][0] = 1 / 42.0f; g_vc[9][1] = 1 / 42.0f; g_vc[10][2] = 1 / 40.0f; g_vc[10][3] = 0.3f; g_vc[11][3] = 1;
    memcpy(g_vc[4], g_vc[8], 64);
    memcpy(g_vc[13], g_vc[10], 16); memcpy(g_vc[12], g_vc[10], 16);
    g_vc[16][0] = 1.2f; g_vc[16][1] = 1; g_vc[16][2] = 0; g_vc[16][3] = 1.5f;
    for (int f = 0; f < 6; f++) memcpy(g_vc[21 + f], g_amb[f], 12);
    for (int k = 0; k < 4; k++) {                                           /* cLightInfo c27 + 5k */
        float *L = g_vc[27 + 5 * k];
        memcpy(L + 0, g_l[k].col, 12); L[3] = 0;
        L[4] = 0; L[5] = 0; L[6] = -1; L[7] = 0;
        memcpy(L + 8, g_l[k].pos, 12); L[11] = 1;
        L[12] = 1; L[13] = 0; L[14] = 0; L[15] = 1;
        memcpy(L + 16, g_l[k].att, 12);
    }
    g_vc[37][0] = g_vc[37][1] = g_vc[37][2] = g_vc[37][3] = 1;
    g_vc[48][0] = 1; g_vc[49][1] = 1;                                       /* cBaseTexCoordTransform */
    g_vc[52][0] = 2; g_vc[53][1] = 2;                                       /* cDetailTexCoordTransform */
    for (int b = 0; b < 3; b++) { g_vc[58 + b][b] = 1; }                    /* cModel[0] = identity */
    for (int k = 0; k < 16; k++) g_vb[k] = k < nl;
    memset(g_vi, 0, sizeof g_vi);
    g_vi[0][0] = nl; g_vi[0][1] = 0; g_vi[0][2] = 1;
}

static void ps_consts(int fog, unsigned seed)
{
    g_seed = seed;
    for (int r = 0; r < 64; r++) for (int c = 0; c < 4; c++) g_pc[r][c] = frand(0.05f, 0.95f);
    g_pc[1][0] = 0.9f; g_pc[1][1] = 0.8f; g_pc[1][2] = 0.7f; g_pc[1][3] = 1;      /* g_DiffuseModulation */
    for (int f = 0; f < 6; f++) { memcpy(g_pc[5 + f], g_amb[f], 12); g_pc[5 + f][3] = 0; }   /* cAmbientCube */
    /* cLightInfo c13..c18 (PixelShaderLightInfo[3]: color, pos), light 3 packed in the .w components */
    for (int k = 0; k < 3; k++) { memcpy(g_pc[13 + 2 * k], g_l[k].col, 12); memcpy(g_pc[14 + 2 * k], g_l[k].pos, 12); }
    g_pc[13][3] = g_l[3].col[0]; g_pc[14][3] = g_l[3].col[1]; g_pc[15][3] = g_l[3].col[2];
    g_pc[16][3] = g_l[3].pos[0]; g_pc[17][3] = g_l[3].pos[1]; g_pc[18][3] = g_l[3].pos[2];
    g_pc[12][0] = fog == 2 ? 1.0f : 0.0f; g_pc[12][1] = 0; g_pc[12][2] = fog == 2 ? 1.0f : 0.0f; g_pc[12][3] = 1;
    g_pc[20][0] = 0; g_pc[20][1] = 0; g_pc[20][2] = 200; g_pc[20][3] = 0;
    if (fog == 0) { g_pc[21][0] = 0; g_pc[21][1] = 9; g_pc[21][2] = 0; g_pc[21][3] = 0; }
    else { g_pc[21][0] = 0.3f; g_pc[21][1] = 9; g_pc[21][2] = 0.85f; g_pc[21][3] = 1.6f; }
    g_pc[29][0] = 0.3f; g_pc[29][1] = 0.5f; g_pc[29][2] = 0.7f; g_pc[29][3] = 1;
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
    IDirect3DDevice9_SetTexture(d, 3, (IDirect3DBaseTexture9 *)g_nrm);
    /* detail (s2, at the detail UVs): the base pattern, so DETAILTEXTURE statics read real texels */
    IDirect3DDevice9_SetTexture(d, 2, g_s2 ? g_s2 : (IDirect3DBaseTexture9 *)g_tex);
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

static double sat(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* CPU bumped normal: normalize(tn.x T + tn.y (cross(N, T) * T.w) + tn.z N) at the interpolants */
static void ref_normal(const float t3[4], const float t4[4], double n[3])
{
    double N[3] = {t3[0], t3[1], t3[2]}, T[3] = {t4[0], t4[1], t4[2]}, B[3], tn[3];
    B[0] = (N[1] * T[2] - N[2] * T[1]) * t4[3];
    B[1] = (N[2] * T[0] - N[0] * T[2]) * t4[3];
    B[2] = (N[0] * T[1] - N[1] * T[0]) * t4[3];
    for (int i = 0; i < 3; i++) tn[i] = g_n1[i] / 255.0 * 2.0 - 1.0;
    for (int i = 0; i < 3; i++) n[i] = tn[0] * T[i] + tn[1] * B[i] + tn[2] * N[i];
    double l = sqrt(dot3(n, n));
    for (int i = 0; i < 3; i++) n[i] /= l;
}

/* CPU evaluation of the PHOTON block (photon_light_ps.h PhotonLightPS/PhotonDirectPS/PhotonAmbientPS and the call
 * site in photon_bump_ps2x.fxc) from the packed block itself */
static void ref_photon(const PhotonLightBlock *lb, const float wp[3], const double n[3], int hl, int amb, double out[3])
{
    double lit[3] = {0, 0, 0};
    for (int k = 0; k < PHOTON_LB_MAX_LIGHTS && k < lb->flags[1]; k++) {
        const float (*L)[4] = lb->lights[k];
        double t[3] = {L[0][0] - wp[0], L[0][1] - wp[1], L[0][2] - wp[2]};
        double d2 = fmax(dot3(t, t), 1e-6), ood = 1 / sqrt(d2), ld[3] = {t[0] * ood, t[1] * ood, t[2] * ood};
        double dist = 1 / fmax(L[3][0] + L[3][1] * d2 * ood + L[3][2] * d2, 1e-6);
        double sd = -(L[2][0] * ld[0] + L[2][1] * ld[1] + L[2][2] * ld[2]) - L[3][3];
        double spot = sat(pow(fmax(1e-4, sd), L[2][3]));
        double rr = d2 * L[0][3], win = sat(1 - rr * rr);
        double att = (L[1][3] > 0.5 ? dist * spot : dist) * win * win;
        if (L[1][3] > 1.5) { ld[0] = -L[2][0]; ld[1] = -L[2][1]; ld[2] = -L[2][2]; att = 1; }
        double ndl = dot3(n, ld), h = ndl * 0.5 + 0.5, term = hl ? h * h : fmax(0, ndl);
        for (int i = 0; i < 3; i++) lit[i] += L[1][i] * term * att;
    }
    for (int i = 0; i < 3; i++) {
        double a = 0;
        if (amb) {
            for (int ax = 0; ax < 3; ax++) a += n[ax] * n[ax] * lb->ambient[ax * 2 + (n[ax] >= 0 ? 0 : 1)][i];
            a = fmax(a * lb->tuning[1], lb->tuning[2]);
        }
        out[i] = lit[i] * lb->tuning[0] + a;
    }
}

/* --- OCC: the P1 envmap occlusion (photon_envocc_ps.h); same method as tests/vlgpixel/vlgpair.c ------------------ */
#define OCC_LOW 64 /* the cubemap's last mip, grey: Y = 64/255 */
static float R0[H][W][4], R1[H][W][4], R2[H][W][4], R3[H][W][4];
static double luma(const float *c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
static int occ_exact(float b[H][W][4], float s[H][W][4], float o[H][W][4], const float c56[4], double *maxerr, int *npx)
{
    int bad = 0;
    double yc = OCC_LOW / 255.0 + 1e-4;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double es = luma(s[y][x]) - luma(b[y][x]);
        if (!(fabs(es) > 1e-3)) continue;
        /* photon_envocc_ps.h: t = saturate(log2(x) c56.x + c56.y), factor = smoothstep01(t) c56.z + c56.w */
        double t = sat(log2(luma(b[y][x]) / yc) * c56[0] + c56[1]), f = t * t * (3 - 2 * t) * c56[2] + c56[3];
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
/* light sources at a level (0 dark: x 0.05; 1 bright: ambient 1.0 grey, lights x 4): stock PS cube c5..c10 and light
 * colours (c13/c15/c17 .xyz, light 3 in c13..c15 .w), and the Photon block */
static void occ_level(int bright, PhotonLightBlock *lb)
{
    float m = bright ? 4.0f : 0.05f;
    for (int f = 0; f < 6; f++) for (int i = 0; i < 3; i++) {
        float a = bright ? 1.0f : g_amb[f][i] * m;
        g_pc[5 + f][i] = a;
        lb->ambient[f][i] = a;
    }
    for (int k = 0; k < 3; k++) for (int i = 0; i < 3; i++) g_pc[13 + 2 * k][i] = g_l[k].col[i] * m;
    for (int i = 0; i < 3; i++) g_pc[13 + i][3] = g_l[3].col[i] * m;
    for (int k = 0; k < 4; k++) {
        float col[3] = {g_pl[k].col[0] * m, g_pl[k].col[1] * m, g_pl[k].col[2] * m};
        photon_lb_pack_light(lb->lights[k], g_pl[k].type, col, g_pl[k].pos, g_pl[k].dir, g_pl[k].att[0], g_pl[k].att[1],
                             g_pl[k].att[2], g_pl[k].range, g_pl[k].falloff, g_pl[k].cos_in, g_pl[k].cos_out);
    }
}
static int occ_test(IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps, int vnl, int occ, char *line, int *o, int cap)
{
    static const float c56_alt_in[4] = {0.6f, 0.15f, 1.3f, 0.8f};   /* s, kmin, kmax, k */
    float c56_def[4], c56_alt[4];
    photon_lb_envocc(c56_def, 1, 0, 1, 0.5f);                         /* the vlg.c defaults */
    photon_lb_envocc(c56_alt, c56_alt_in[0], c56_alt_in[1], c56_alt_in[2], c56_alt_in[3]);
    IDirect3DDevice9 *d = g_dev;
    IDirect3DDevice9_SetSamplerState(d, 1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    g_s0 = g_s2 = (IDirect3DBaseTexture9 *)g_white;   /* detail: white with alpha 0 and blend factor 0 (below) */
    g_nrm = g_nrm4;
    int fail = 0, nexact = 0, bad = 0;
    double maxerr = 0, rdark[2] = {0, 0}, rbright[2] = {0, 0};
    vs_consts(vnl);
    for (int mode = 0; mode < 2; mode++)
        for (int lvl = 0; lvl < 2; lvl++) {
            ps_consts(0, 4242u + lvl);
            g_pc[1][0] = g_pc[1][1] = g_pc[1][2] = g_pc[1][3] = 1;               /* g_DiffuseModulation = 1 */
            g_pc[4][3] = 0;   /* g_DetailBlendFactor 0: every DETAIL_BLEND_MODE leaves the base (and its alpha) alone */
            PhotonLightBlock lb; memset(&lb, 0, sizeof lb);
            occ_level(lvl, &lb);
            if (mode) {
                lb.flags[0] = 1; lb.flags[1] = 4; lb.flags[2] = 0; lb.flags[3] = 0;
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
    g_s0 = g_s1 = g_s2 = NULL;
    IDirect3DDevice9_SetSamplerState(d, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    int ok = bad == 0 && nexact > W * H / 2;
    for (int mode = 0; mode < 2; mode++) {
        ok = ok && rdark[mode] <= 0.3;
        if (occ == 2 || mode == 1) ok = ok && rbright[mode] >= 0.8 && rbright[mode] <= 1.2;
    }
    fail |= !ok;
    *o += snprintf(line + *o, cap - *o, " OCC[%s exact %d px max %.1e; dark %.3f bright %.3f%s; per-pixel on: dark %.3f "
                   "bright %.3f]", ok ? "ok" : "FAIL", nexact, maxerr, rdark[0], rbright[0],
                   occ == 2 ? "" : " (not checked: no ambient)", rdark[1], rbright[1]);
    return fail;
}

static IDirect3DPixelShader9 *g_p6, *g_p3, *g_p4;
static int run_case(char *name, int vnl, int hl, int amb, char *f[5], int on, int occ)
{
    IDirect3DVertexShader9 *vsA = load_vs(f[0]), *vs3 = load_vs(f[2]);
    IDirect3DPixelShader9 *psA = load_ps(f[1]), *ps3o = load_ps(f[3]), *ps3 = load_ps(f[4]);
    if (!vsA || !vs3 || !psA || !ps3o || !ps3) { printf("%s: shader create FAIL\n", name); return 1; }
    int fail = 0, meaningful = 0;
    char line[4096]; int o = snprintf(line, sizeof line, "%-22s", name);
    static const char *fn[3] = {"nofog", "range", "height"};
    vs_consts(vnl);
    g_nrm = g_nrm4;
    for (int fog = 0; fog < 3; fog++) {
        ps_consts(fog, 777u + fog);
        if (draw(vsA, psA, 0, A)) return 1;
        if (draw(vs3, ps3o, 0, Bm)) return 1;
        Cmp c = cmp(A, Bm, TOL);
        if (draw(vs3, ps3, 0, Bm)) return 1;
        Cmp dd = cmp(A, Bm, TOL);
        /* equality is required under every fog setup; the image must be non-trivial under at least one (stock's
         * zero-diffuse combos, NUM_LIGHTS=0 + AMBIENT_LIGHT=0 without a cubemap, are legitimately black unfogged) */
        int ok = c.bad == 0 && dd.bad == 0;
        meaningful |= c.covered > W * H * 3 / 4 && c.distinct >= 64;
        fail |= !ok;
        o += snprintf(line + o, sizeof line - o, " %s[%s C %.1e D %.1e cov %d col %d]", fn[fog], ok ? "ok" : "FAIL",
                      c.maxd, dd.maxd, c.covered, c.distinct);
    }
    if (!meaningful) { fail = 1; o += snprintf(line + o, sizeof line - o, " TRIVIAL[FAIL no fog setup covers 3/4 with 64 colours]"); }
    ps_consts(0, 777u);
    draw(vsA, psA, 0, A); draw(vsA, psA, 1, Bm);
    Cmp fa = cmp(A, Bm, TOL);
    draw(vs3, ps3, 0, A); draw(vs3, ps3, 1, Bm);
    Cmp fd = cmp(A, Bm, TOL);
    if (fd.bad) fail = 1;
    o += snprintf(line + o, sizeof line - o, " FF[stock %d%% sm3 %d px]", fa.bad * 25 / (W * H), fd.bad);
    if (on) {
        g_nrm = g_nrm1;
        ps_consts(0, 999u);
        PhotonLightBlock lb; memset(&lb, 0, sizeof lb);
        lb.flags[0] = 1; lb.flags[1] = 4; lb.flags[2] = hl ? 0.0f : 1.0f; /* c32.z deliberately opposite: ignored */
        for (int k = 0; k < 6; k++) memcpy(lb.ambient[k], g_amb[k], 12);
        for (int k = 0; k < 4; k++)
            photon_lb_pack_light(lb.lights[k], g_pl[k].type, g_pl[k].col, g_pl[k].pos, g_pl[k].dir, g_pl[k].att[0],
                                 g_pl[k].att[1], g_pl[k].att[2], g_pl[k].range, g_pl[k].falloff, g_pl[k].cos_in,
                                 g_pl[k].cos_out);
        lb.tuning[0] = 1.3f; lb.tuning[1] = 0.8f; lb.tuning[2] = 0.01f; lb.tuning[3] = 1;
        memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, sizeof lb);
        if (draw(vs3, ps3, 0, A)) return 1;
        if (draw(vs3, g_p6, 0, P1) || draw(vs3, g_p3, 0, P2) || draw(vs3, g_p4, 0, P3)) return 1;
        lb.tuning[3] = 2;
        memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, sizeof lb);
        if (draw(vs3, ps3, 0, Bm)) return 1;
        lb.tuning[3] = 1;
        double maxrel = 0, maxn = 0, peak = 0, spread_lo = 1e9, spread_hi = 0; int bad = 0, nbad = 0, n = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (P3[y][x][3] == 0) continue;            /* not covered (TEXCOORD4.w = +-1) */
            double nn[3], r[3]; ref_normal(P2[y][x], P3[y][x], nn); ref_photon(&lb, P1[y][x], nn, hl, amb, r); n++;
            for (int c = 0; c < 3; c++) {
                double rel = fabs(A[y][x][c] - r[c]) / fmax(fabs(r[c]), 1e-2);
                if (rel > maxrel) maxrel = rel;
                if (rel > 1e-3) bad++;
                double dn = fabs(Bm[y][x][c] - (nn[c] * 0.5 + 0.5));
                if (dn > maxn) maxn = dn;
                if (dn > 1e-4) nbad++;
            }
            double s = r[0] + r[1] + r[2];
            if (s > peak) peak = s;
            if (s < spread_lo) spread_lo = s;
            if (s > spread_hi) spread_hi = s;
        }
        int ok = bad == 0 && nbad == 0 && n > W * H * 3 / 4 && spread_hi > spread_lo + 0.1;
        fail |= !ok;
        o += snprintf(line + o, sizeof line - o, " ON[%s rel %.1e, %d px, rgb-sum %.3f..%.3f] ON2[normal |d| %.1e]",
                      ok ? "ok" : "FAIL", maxrel, n, spread_lo, spread_hi, maxn);
    }
    if (occ) fail |= occ_test(vs3, ps3, vnl, occ, line, &o, (int)sizeof line);
    printf("%s%s\n", line, fail ? "  <-- FAIL" : "");
    IDirect3DVertexShader9_Release(vsA); IDirect3DVertexShader9_Release(vs3);
    IDirect3DPixelShader9_Release(psA); IDirect3DPixelShader9_Release(ps3o); IDirect3DPixelShader9_Release(ps3);
    return fail;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
#define CHECK(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    printf("BUMPPAIR FAIL %s hr=0x%08lx\n", what, (unsigned long)hr_); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: bumppair.exe <list>\n"); return 2; }
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { printf("BUMPPAIR FAIL cannot read %s\n", argv[1]); return 1; }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "bumppair";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("bumppair", "bumppair", WS_OVERLAPPEDWINDOW, 0, 0, W + 16, H + 40, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!hwnd || !d3d) { printf("BUMPPAIR FAIL window/Direct3DCreate9\n"); return 1; }
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
    /* varied normal map (tilted normals, alpha = envmap mask) and the uniform 1x1 one */
    CHECK(IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_nrm4, NULL), "nrm4");
    IDirect3DTexture9_LockRect(g_nrm4, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = ((DWORD)(90 + 50 * y) << 24) | ((DWORD)(80 + 30 * x) << 16) |
                                                          ((DWORD)(170 - 25 * y) << 8) | (DWORD)(200 + 15 * ((x + y) & 3));
    IDirect3DTexture9_UnlockRect(g_nrm4, 0);
    CHECK(IDirect3DDevice9_CreateTexture(g_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_nrm1, NULL), "nrm1");
    IDirect3DTexture9_LockRect(g_nrm1, 0, &lr, NULL, 0);
    *(DWORD *)lr.pBits = ((DWORD)g_n1[3] << 24) | ((DWORD)g_n1[0] << 16) | ((DWORD)g_n1[1] << 8) | (DWORD)g_n1[2];
    IDirect3DTexture9_UnlockRect(g_nrm1, 0);
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cube, NULL), "cube");
    for (int fc = 0; fc < 6; fc++) {
        IDirect3DCubeTexture9_LockRect(g_cube, (D3DCUBEMAP_FACES)fc, 0, &lr, NULL, 0);
        *(DWORD *)lr.pBits = 0x80000000u | ((DWORD)(30 * fc) << 16) | ((DWORD)(200 - 25 * fc) << 8) | (DWORD)(60 + 20 * fc);
        IDirect3DCubeTexture9_UnlockRect(g_cube, (D3DCUBEMAP_FACES)fc, 0);
    }
    /* OCC: white base (alpha 0: no selfillum), the mipped cube (last mip uniform OCC_LOW grey) and a black cube */
    CHECK(IDirect3DDevice9_CreateTexture(g_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_white, NULL), "white");
    IDirect3DTexture9_LockRect(g_white, 0, &lr, NULL, 0);
    *(DWORD *)lr.pBits = 0x00ffffffu;
    IDirect3DTexture9_UnlockRect(g_white, 0);
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
    for (int s = 0; s < 4; s++) {
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    make_grid();
    char ln[4096]; int cases = 0, fails = 0, nocc = 0;
    while (fgets(ln, sizeof ln, lf)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char a[5][MAX_PATH], name[64]; int vnl, hl, amb, on, occ = 0;
        if (sscanf(ln, "probe %259s %259s %259s", a[0], a[1], a[2]) == 3) {
            g_p6 = load_ps(a[0]); g_p3 = load_ps(a[1]); g_p4 = load_ps(a[2]);
            if (!g_p6 || !g_p3 || !g_p4) return 1;
            continue;
        }
        int got = sscanf(ln, "case %63s %d %d %d %259s %259s %259s %259s %259s %d %d", name, &vnl, &hl, &amb, a[0], a[1], a[2],
                         a[3], a[4], &on, &occ);
        if (got == 10 || got == 11) {
            char *f[5]; for (int i = 0; i < 5; i++) f[i] = a[i];
            cases++; nocc += occ != 0; fails += run_case(name, vnl, hl, amb, f, on, occ);
        }
    }
    if (!g_p6) { printf("BUMPPAIR FAIL no probe line\n"); return 1; }
    if (fails) { printf("BUMPPAIR FAIL %d of %d cases\n", fails, cases); return 1; }
    printf("BUMPPAIR OK %d cases (%d with OCC)\n", cases, nocc);
    return 0;
}
