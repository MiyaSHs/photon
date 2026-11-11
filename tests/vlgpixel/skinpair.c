/* skinpair: the skin go/no-go probe's pixel check (lighting plan v2 P0 [rev C10], tools/p0_skin_probe.py),
 * headless on DXVK/lavapipe.
 *
 * Per case it draws the same curved, tangent-framed grid (stock skin vertex layout, uncompressed, no skinning) with:
 *   A  GMod's own skin_vs20 + skin_ps20b combo (from the user's VPK)
 *   B  GMod's skin_vs20 + our ps_2_b compile of src/shaderdll/hlsl/photon_skin_ps2x.fxc (the PS alone)
 *   C  our vs_3_0 (SDK 2015 skin_vs20.fxc, VS_2_0 define) + our ps_3_0 (photon_skin_ps2x.fxc, PS_2_B define):
 *      the SM3 pair Option A would substitute
 *   N  our vs_3_0 + the unmodified SDK skin source as ps_3_0 (negative control)
 * with varied 4x4 base / normal / spec-exponent textures and a cube map, phong constants that make every GMod change
 * visible (albedo tint on: c26.x < 0; albedo boost c0.w = 1.7; spec exponent from the map), under the case's fog
 * setup (0 none = range type with zero params; 1 range; 2 height + fog to dest alpha), and checks
 * B == A and C == A within 2/255 per channel (RGBA, float target, >= 3/4 of the target covered, >= 64 colours);
 * N must differ from A by more than NEG_DIFF (1e-3) iff the case's neg_expect is 1, and match within NEG_SAME (1e-5)
 * otherwise.
 * usage: skinpair.exe <list>; list lines (tools/p0_skinpair_list.py):
 *   "case <name> <fog> <lights> <stock_vs> <stock_ps> <vs30> <ps30> <ps2b> <neg30> <neg_expect>"
 *
 * Pair mode (lighting plan v2 P2: the BUILT photon_skin_vs20/_ps20b, tools/t34_build_skin20.py; list lines from
 * tools/t34_skinpair_list.py, run by scripts/run_skinpair.sh --pair):
 *   "pair <name> <fog> <lights> <stock_vs> <stock_ps> <vs30> <ps30off> <ps30> <ps30ref> <on> <occ>"
 *   A  GMod's skin_vs20 + skin_ps20b combo;
 *   C  our vs_3_0 (from photon_skin_vs20.vcs) + the PHOTON_FORCE_STOCK ps_3_0 compile: C == A within 2/255;
 *   D  our vs_3_0 + the shipped ps_3_0 (photon_skin_ps20b.vcs) with the Photon flag off (c32.x = 0, c33..c55
 *      random, c56.x = 0 with c56.yzw random): D == A within 2/255;
 *   FF D3DRS_FOGENABLE = TRUE (r_pixelfog 0) must not change D (DXVK applies FF fog to SM < 3 PS only);
 *   ON (on = 1) flag on: c32 = (1, lights), the c39.. block holds 4 DIRECTIONAL lights and c33..c38 the stock cube,
 *      c55 = (1, 1, 0, 0); reference: our vs_3_0 + <ps30ref> (the flag-off ps_3_0 without GMod's
 *      GMOD_FXC_UNSAT_NDOTL artefact) with the same lights as stock constants (VS cLightInfo directional, so the
 *      interpolated attenuation is exactly 1; PS cLightInfo position -dir * 1e8): within 2/255, so the Photon path
 *      keeps skin's per-light diffuse, phong, warps and rim (plan v2 rev C4); the image must be lit (>= 64 colours);
 *   OCC (occ = 1, CUBEMAP combos, no pixel fog) the P1 envmap occlusion (photon_envocc_ps.h) as in bumppair.c: white
 *      base with alpha 0 (also on the detail sampler s13, with the detail blend factor c0.w = 0: neutral in every
 *      DETAIL_BLEND_MODE), specular tint 0 (c26.xyz = 0), selfillum mask control 0, inverted envmap mask, c1 = 1, so
 *      the black-cubemap image is the final diffuse lighting and image(cube) - image(black) the envmap term; per
 *      diffuse source (stock lights: c32.x = 0; Photon lights: c32.x = 1) and level (dark x 0.01; bright: ambient 1.0
 *      grey, lights x 4), c56 default and a second setting: occluded term = f * stock term per pixel, dark <= 0.3 x
 *      stock, bright 0.8..1.2 x.
 * Built by scripts/run_skinpair.sh. Prints SKINPAIR OK <n> cases.
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
#define TOL (2.0 / 255.0)
#define NEG_DIFF 1e-3 /* the SDK math must be off by more than this where GMod changed it */
#define NEG_SAME 1e-5 /* and within this where it did not */

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
static IDirect3DTexture9 *g_base, *g_nrm, *g_spec;
static IDirect3DCubeTexture9 *g_cube, *g_cubeocc, *g_cubeblk;
static IDirect3DTexture9 *g_white;
static IDirect3DBaseTexture9 *g_s0, *g_s8;  /* draw() binds these on s0 (and s14) / s8 when set (OCC) */
static int g_ffog;                          /* draw(): D3DRS_FOGENABLE */
static Vtx g_v[GN * GN];
static WORD g_idx[(GN - 1) * (GN - 1) * 6];
static float g_vc[256][4], g_pc[64][4];
static BOOL g_vb[16];
static int g_vi[4][4];
static float A[H][W][4], Bm[H][W][4];

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

/* grid: x, y in [-40, 40], z = 8 + 0.004 (x^2 - y^2); normal (-0.008 x, 0.008 y, 1), tangent (1, 0.1 v, 0.008 x) with
 * the binormal sign alternating by column */
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
        o->c1[0] = 0.2f; o->c1[1] = 0.25f; o->c1[2] = 0.3f; o->c1[3] = 1;
        o->bw[0] = 1;
        o->pf[3] = 0.4f * u - 0.2f;                                         /* wrinkle weight (POSITION1.w) */
    }
    int k = 0;
    for (int j = 0; j < GN - 1; j++) for (int i = 0; i < GN - 1; i++) {
        WORD a = j * GN + i, b = a + 1, c = a + GN, d = c + 1;
        g_idx[k++] = a; g_idx[k++] = b; g_idx[k++] = c; g_idx[k++] = b; g_idx[k++] = d; g_idx[k++] = c;
    }
}

typedef struct { float pos[3], col[3], att[3]; } Light;
/* light 0 lights the grid from the front; lights 1..3 sit far away just below the grid's horizon on the -y side, so
 * on the +y half N.L < 0 while the grazing reflection of the view still points at them (R.L > 0): that is where
 * GMod's fxc artefact (the last light's specular/rim mask without saturate, GMOD_FXC_UNSAT_NDOTL) shows */
static Light g_l[4] = {{{-10, 12, 20}, {1.6f, 1.1f, 0.7f}, {0.0f, 0.02f, 0.003f}},
                       {{-60, 300, -150}, {0.4f, 0.8f, 1.5f}, {1.0f, 0.0f, 0.0f}},
                       {{0, 300, -150}, {0.9f, 0.3f, 0.5f}, {1.0f, 0.0f, 0.0f}},
                       {{60, 300, -150}, {0.3f, 1.0f, 0.4f}, {1.0f, 0.0f, 0.0f}}};
static float g_amb[6][3] = {{0.10f, 0.05f, 0.02f}, {0.02f, 0.06f, 0.10f}, {0.08f, 0.08f, 0.03f},
                            {0.03f, 0.02f, 0.07f}, {0.12f, 0.11f, 0.10f}, {0.01f, 0.01f, 0.02f}};

static void vs_consts(int nl)
{
    memset(g_vc, 0, sizeof g_vc);
    float c0[4] = {0, 1, 2, 0.5f}, c1[4] = {1 / 2.2f, 1, 1 / 3.0f, 0};
    memcpy(g_vc[0], c0, 16); memcpy(g_vc[1], c1, 16);
    g_vc[2][0] = 10; g_vc[2][1] = -5; g_vc[2][2] = 60; g_vc[2][3] = 9;    /* cEyePosWaterZ */
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
    g_vc[47][0] = g_vc[47][1] = g_vc[47][2] = g_vc[47][3] = 1;
    g_vc[48][0] = 1; g_vc[49][1] = 1;                                       /* cBaseTexCoordTransform */
    g_vc[52][0] = 2; g_vc[53][1] = 2;                                       /* cDetailTexCoordTransform */
    for (int b = 0; b < 3; b++) { g_vc[58 + b][b] = 1; }                    /* cModel[0] = identity */
    for (int k = 0; k < 16; k++) g_vb[k] = k < nl;
    memset(g_vi, 0, sizeof g_vi);
    g_vi[0][0] = nl; g_vi[0][1] = 0; g_vi[0][2] = 1;
}

/* skin_ps20b register map (shader_constant_register_map.h) */
static void ps_consts(int fog, unsigned seed)
{
    g_seed = seed;
    for (int r = 0; r < 64; r++) for (int c = 0; c < 4; c++) g_pc[r][c] = frand(0.05f, 0.95f);
    g_pc[PHOTON_LB_FLAGS][0] = 0;                                                    /* Photon flag off */
    g_pc[PHOTON_LB_ENVOCC][0] = 0;                                                   /* occlusion off (yzw random) */
    g_pc[0][0] = 0.9f; g_pc[0][1] = 0.6f; g_pc[0][2] = 0.3f; g_pc[0][3] = 1.7f;   /* selfillum tint; w albedo boost / detail */
    g_pc[1][0] = 0.9f; g_pc[1][1] = 0.8f; g_pc[1][2] = 0.7f; g_pc[1][3] = 1;      /* g_DiffuseModulation */
    g_pc[2][0] = 0.5f; g_pc[2][1] = 0.6f; g_pc[2][2] = 0.7f; g_pc[2][3] = 0.5f;   /* envmap tint, w spec-mask control */
    g_pc[3][0] = 1.0f; g_pc[3][1] = 0.1f; g_pc[3][2] = 2.0f; g_pc[3][3] = 1.2f;   /* selfillum fresnel scale/bias/exp/bright */
    for (int f = 0; f < 6; f++) { memcpy(g_pc[4 + f], g_amb[f], 12); g_pc[4 + f][3] = 0; }  /* cAmbientCube c4..c9 */
    g_pc[10][0] = 0.6f; g_pc[10][3] = 0.5f;                                        /* envmap fresnel, selfillum mask ctl */
    g_pc[11][0] = 10; g_pc[11][1] = -5; g_pc[11][2] = 60; g_pc[11][3] = -1;       /* eye pos; exponent < 0: from the map */
    if (fog == 0) { g_pc[12][0] = 0; g_pc[12][1] = 9; g_pc[12][2] = 0; g_pc[12][3] = 0; }
    else if (fog == 1) { g_pc[12][0] = 0.3f; g_pc[12][1] = 9; g_pc[12][2] = 0.85f; g_pc[12][3] = 1.6f; }
    else { g_pc[12][0] = 0.3f; g_pc[12][1] = 20; g_pc[12][2] = 0.85f; g_pc[12][3] = 0.05f; }   /* 2, 3: height */
    g_pc[13][0] = 0.5f;                                                             /* rim mask control */
    g_pc[14][3] = 1.5f;                                                             /* rim boost */
    g_pc[19][0] = 0.2f; g_pc[19][1] = 0.6f; g_pc[19][2] = 1.0f; g_pc[19][3] = 2.0f; /* fresnel ranges, spec boost */
    /* cLightInfo c20..c25 (PixelShaderLightInfo[3]: color, pos), light 3 packed in the .w components */
    for (int k = 0; k < 3; k++) { memcpy(g_pc[20 + 2 * k], g_l[k].col, 12); memcpy(g_pc[21 + 2 * k], g_l[k].pos, 12); }
    g_pc[20][3] = g_l[3].col[0]; g_pc[21][3] = g_l[3].col[1]; g_pc[22][3] = g_l[3].col[2];
    g_pc[23][3] = g_l[3].pos[0]; g_pc[24][3] = g_l[3].pos[1]; g_pc[25][3] = g_l[3].pos[2];
    g_pc[26][0] = -1; g_pc[26][1] = 0.5f; g_pc[26][2] = 0.5f; g_pc[26][3] = 4.0f;  /* spec tint < 0: albedo tint; rim power */
    g_pc[27][0] = 0; g_pc[27][1] = 1; g_pc[27][2] = 0.3f; g_pc[27][3] = 0;         /* basealpha phong mask 0, invert 0 */
    g_pc[29][0] = 0.3f; g_pc[29][1] = 0.5f; g_pc[29][2] = 0.7f; g_pc[29][3] = 1;
    g_pc[30][0] = 1; g_pc[30][1] = 1; g_pc[30][2] = 1; g_pc[30][3] = 1;           /* cLightScale */
}

static int draw(IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps, float out[H][W][4])
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
    IDirect3DDevice9_SetPixelShaderConstantF(d, 0, &g_pc[0][0], PHOTON_LB_ENVOCC + 1);   /* c0..c56 */
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, g_ffog);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGCOLOR, 0x00204080);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    for (int s = 0; s < 16; s++) {
        IDirect3DBaseTexture9 *t = (IDirect3DBaseTexture9 *)g_base;
        if (s == 3 || s == 11 || s == 12) t = (IDirect3DBaseTexture9 *)g_nrm;
        else if (s == 7) t = (IDirect3DBaseTexture9 *)g_spec;
        else if (s == 5 || s == 8) t = (IDirect3DBaseTexture9 *)g_cube;
        if (g_s0 && (s == 0 || s == 13 || s == 14)) t = g_s0;   /* OCC: white base, detail and selfillum mask */
        if (g_s8 && s == 8) t = g_s8;
        IDirect3DDevice9_SetTexture(d, s, t);
    }
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

typedef struct { double maxd; int bad, covered, distinct, nan; } Cmp;
static Cmp cmp(float a[H][W][4], float b[H][W][4])
{
    Cmp r = {0, 0, 0, 0, 0};
    float seen[512][3]; int ns = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int cov = a[y][x][0] != 0 || a[y][x][1] != 0 || a[y][x][2] != 0;
        r.covered += cov;
        for (int c = 0; c < 4; c++) {
            if (isnan(a[y][x][c])) r.nan++;
            double d = fabs((double)a[y][x][c] - b[y][x][c]);
            if (!(d <= TOL)) { r.bad++; d = isnan(d) ? 1e9 : d; }
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

static int run_case(char *name, int fog, int nl, char *f[6], int neg_expect)
{
    IDirect3DVertexShader9 *vsA = load_vs(f[0]), *vs3 = load_vs(f[2]);
    IDirect3DPixelShader9 *psA = load_ps(f[1]), *ps3 = load_ps(f[3]), *ps2b = load_ps(f[4]), *psN = load_ps(f[5]);
    if (!vsA || !vs3 || !psA || !ps3 || !ps2b || !psN) { printf("%s: shader create FAIL\n", name); return 1; }
    vs_consts(nl);
    ps_consts(fog, 777u + fog);
    Cmp b, c, n;
    if (draw(vsA, psA, A)) return 1;
    if (draw(vsA, ps2b, Bm)) return 1;
    b = cmp(A, Bm);
    if (draw(vs3, ps3, Bm)) return 1;
    c = cmp(A, Bm);
    if (draw(vs3, psN, Bm)) return 1;
    n = cmp(A, Bm);
    /* the negative control is judged on a finer scale than 2/255: the real pairs match to ~1e-7, and the albedo boost
     * through the rim's ambient term at NUM_LIGHTS=0 is only a few 1e-3 in this scene */
    int neg_ok = neg_expect ? n.maxd > NEG_DIFF : n.maxd <= NEG_SAME;
    int ok = b.bad == 0 && c.bad == 0 && c.covered > W * H * 3 / 4 && c.distinct >= 64 && c.nan == 0 && neg_ok;
    printf("%-22s %s B %.1e C %.1e (bad %d) cov %d col %d | neg %s %.1e (%d values > 2/255, expect %s)%s\n", name, ok ? "ok" : "FAIL",
           b.maxd, c.maxd, c.bad, c.covered, c.distinct, neg_ok ? "ok" : "FAIL", n.maxd, n.bad,
           neg_expect ? "differ" : "equal", ok ? "" : "  <-- FAIL");
    IDirect3DVertexShader9_Release(vsA); IDirect3DVertexShader9_Release(vs3);
    IDirect3DPixelShader9_Release(psA); IDirect3DPixelShader9_Release(ps3); IDirect3DPixelShader9_Release(ps2b);
    IDirect3DPixelShader9_Release(psN);
    return !ok;
}

/* --- pair mode (the built SM3 pair) ---------------------------------------------------------------------------- */
static float R0[H][W][4], R1[H][W][4], R2[H][W][4], R3[H][W][4];
/* ON: 4 directional lights (the direction light travels; N = +z faces the sky), colours as g_l */
static const float g_dd[4][3] = {{0.3f, -0.2f, -1.0f}, {-0.6f, 0.4f, -0.7f}, {0.2f, 0.9f, -0.3f}, {-0.1f, -0.8f, 0.2f}};
static void unit(const float in[3], float out[3])
{
    float l = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    for (int i = 0; i < 3; i++) out[i] = in[i] / l;
}
/* the stock PS light constants c20..c25 (PixelShaderLightInfo[3], light 3 in the .w's) */
static void ps_lights(const float col[4][3], const float pos[4][3])
{
    for (int k = 0; k < 3; k++) { memcpy(g_pc[20 + 2 * k], col[k], 12); memcpy(g_pc[21 + 2 * k], pos[k], 12); }
    g_pc[20][3] = col[3][0]; g_pc[21][3] = col[3][1]; g_pc[22][3] = col[3][2];
    g_pc[23][3] = pos[3][0]; g_pc[24][3] = pos[3][1]; g_pc[25][3] = pos[3][2];
}
/* ON setup: VS lights directional (atten 1), PS lights at -dir * 1e8, the Photon block with the same lights */
static void on_setup(int nl, float mul, PhotonLightBlock *lb)
{
    float col[4][3], pos[4][3], dir[4][3];
    for (int k = 0; k < 4; k++) {
        unit(g_dd[k], dir[k]);
        for (int i = 0; i < 3; i++) { col[k][i] = g_l[k].col[i] * mul; pos[k][i] = -dir[k][i] * 1e8f; }
        float *L = g_vc[27 + 5 * k];
        memcpy(L + 0, col[k], 12); L[3] = 1;                                  /* color.w 1: directional */
        memcpy(L + 4, dir[k], 12); L[7] = 0;
        memcpy(L + 8, pos[k], 12); L[11] = 1;
    }
    ps_lights(col, pos);
    memset(lb, 0, sizeof *lb);
    lb->flags[0] = 1; lb->flags[1] = (float)nl;
    for (int f = 0; f < 6; f++) memcpy(lb->ambient[f], g_pc[4 + f], 12);
    for (int k = 0; k < 4; k++)
        photon_lb_pack_light(lb->lights[k], PHOTON_LB_TYPE_DIRECTIONAL, col[k], pos[k], dir[k], 1, 0, 0, 0, 0, 0, 0);
    lb->tuning[0] = 1; lb->tuning[1] = 1; lb->tuning[2] = 0; lb->tuning[3] = 0;
    lb->envocc[0] = 0; lb->envocc[3] = 1;
}

#define OCC_LOW 64 /* the cubemap's last mip, grey: Y = 64/255 */
static double luma(const float *c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
static double sat(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
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
/* OCC light level (0 dark: x 0.01; 1 bright: ambient 1.0 grey, lights x 4), stock PS constants and the Photon block
 * (the stock point lights g_l: the VS attenuates them per vertex, the Photon block per pixel). The dark level is x 0.01,
 * not bumppair's x 0.05: skin always adds the ambient cube and, with PHONG_HALFLAMBERT=1 and 4 lights, the far lights
 * 1..3 light the grid even from below its horizon, so x 0.05 gives x = Y(diffuse) / Y(cube mip) up to ~0.6, above the
 * occlusion's knee (k = 0.5) and well above the dark-room x measured in game (0.03-0.12, photon_envocc_ps.h);
 * x 0.01 puts the scene in that range. */
static void occ_level(int bright, int nl, PhotonLightBlock *lb)
{
    float m = bright ? 4.0f : 0.01f, col[4][3], pos[4][3];
    for (int f = 0; f < 6; f++) for (int i = 0; i < 3; i++) {
        float a = bright ? 1.0f : g_amb[f][i] * m;
        g_pc[4 + f][i] = a; g_vc[21 + f][i] = a;
        lb->ambient[f][i] = a;
    }
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 3; i++) { col[k][i] = g_l[k].col[i] * m; pos[k][i] = g_l[k].pos[i]; }
        memcpy(g_vc[27 + 5 * k], col[k], 12);
        static const float dn[3] = {0, 0, -1};
        photon_lb_pack_light(lb->lights[k], PHOTON_LB_TYPE_POINT, col[k], pos[k], dn, g_l[k].att[0], g_l[k].att[1],
                             g_l[k].att[2], 0, 0, 0, 0);
    }
    ps_lights(col, pos);
    lb->flags[0] = 1; lb->flags[1] = (float)nl; lb->flags[2] = 0; lb->flags[3] = 0;
    lb->tuning[0] = 1; lb->tuning[1] = 1; lb->tuning[2] = 0; lb->tuning[3] = 0;
}
static int occ_test(IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps, int nl, char *line, int *o, int cap)
{
    static const float c56_alt_in[4] = {0.6f, 0.15f, 1.3f, 0.8f};   /* s, kmin, kmax, k */
    float c56_def[4], c56_alt[4];
    photon_lb_envocc(c56_def, 1, 0, 1, 0.5f);                         /* the vlg.c defaults */
    photon_lb_envocc(c56_alt, c56_alt_in[0], c56_alt_in[1], c56_alt_in[2], c56_alt_in[3]);
    IDirect3DDevice9 *d = g_dev;
    IDirect3DDevice9_SetSamplerState(d, 8, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    g_s0 = (IDirect3DBaseTexture9 *)g_white;
    int fail = 0, nexact = 0, bad = 0;
    double maxerr = 0, rdark[2] = {0, 0}, rbright[2] = {0, 0};
    for (int mode = 0; mode < 2; mode++)
        for (int lvl = 0; lvl < 2; lvl++) {
            vs_consts(nl);
            ps_consts(0, 4242u + lvl);
            g_pc[1][0] = g_pc[1][1] = g_pc[1][2] = g_pc[1][3] = 1;                /* g_DiffuseModulation = 1 */
            g_pc[26][0] = g_pc[26][1] = g_pc[26][2] = 0;                           /* specular tint 0: no phong/rim */
            g_pc[10][3] = 0;                                                       /* selfillum mask = base alpha (0) */
            g_pc[0][3] = 0;   /* DETAILTEXTURE: blend factor 0, every DETAIL_BLEND_MODE leaves the base alone (else the
                               * albedo boost, which only reaches the specular, zero here) */
            g_pc[27][0] = 0; g_pc[27][3] = 1;                                      /* inverted envmap mask */
            PhotonLightBlock lb; memset(&lb, 0, sizeof lb);
            occ_level(lvl, nl, &lb);
            if (mode) memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, (PHOTON_LB_TUNING - PHOTON_LB_FIRST + 1) * 16);   /* c32..c55 */
            g_pc[56][0] = 0;
            g_s8 = (IDirect3DBaseTexture9 *)g_cubeblk;
            if (draw(vs, ps, R0)) return 1;
            g_s8 = (IDirect3DBaseTexture9 *)g_cubeocc;
            if (draw(vs, ps, R1)) return 1;
            memcpy(g_pc[56], c56_def, 16);
            if (draw(vs, ps, R2)) return 1;
            memcpy(g_pc[56], c56_alt, 16);
            if (draw(vs, ps, R3)) return 1;
            bad += occ_exact(R0, R1, R2, c56_def, &maxerr, &nexact);
            bad += occ_exact(R0, R1, R3, c56_alt, &maxerr, &nexact);
            double es, r = occ_ratio(R0, R1, R2, &es);
            if (!(es > 1.0)) { fail = 1; *o += snprintf(line + *o, cap - *o, " OCC[FAIL no envmap signal (%.3g)]", es); }
            if (lvl) rbright[mode] = r; else rdark[mode] = r;
        }
    g_s0 = g_s8 = NULL;
    IDirect3DDevice9_SetSamplerState(d, 8, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    int ok = bad == 0 && nexact > W * H / 2;
    for (int mode = 0; mode < 2; mode++) ok = ok && rdark[mode] <= 0.3 && rbright[mode] >= 0.8 && rbright[mode] <= 1.2;
    fail |= !ok;
    *o += snprintf(line + *o, cap - *o, " OCC[%s exact %d px max %.1e; stock lights: dark %.3f bright %.3f; Photon "
                   "lights: dark %.3f bright %.3f]", ok ? "ok" : "FAIL", nexact, maxerr, rdark[0], rbright[0], rdark[1], rbright[1]);
    return fail;
}

static int run_pair(char *name, int fog, int nl, char *f[6], int on, int occ)
{
    IDirect3DVertexShader9 *vsA = load_vs(f[0]), *vs3 = load_vs(f[2]);
    IDirect3DPixelShader9 *psA = load_ps(f[1]), *ps3o = load_ps(f[3]), *ps3 = load_ps(f[4]), *ps3r = load_ps(f[5]);
    if (!vsA || !vs3 || !psA || !ps3o || !ps3 || !ps3r) { printf("%s: shader create FAIL\n", name); return 1; }
    char line[4096]; int o = snprintf(line, sizeof line, "%-26s", name), fail = 0;
    vs_consts(nl);
    ps_consts(fog, 777u + fog);
    if (draw(vsA, psA, A)) return 1;
    if (draw(vs3, ps3o, Bm)) return 1;
    Cmp c = cmp(A, Bm);
    if (draw(vs3, ps3, Bm)) return 1;
    Cmp dd = cmp(A, Bm);
    int ok = c.bad == 0 && dd.bad == 0 && c.covered > W * H * 3 / 4 && c.distinct >= 64 && c.nan == 0;
    fail |= !ok;
    o += snprintf(line + o, sizeof line - o, " pair[%s C %.1e D %.1e (bad %d/%d) cov %d col %d]", ok ? "ok" : "FAIL",
                  c.maxd, dd.maxd, c.bad, dd.bad, c.covered, c.distinct);
    g_ffog = TRUE;
    if (draw(vs3, ps3, R0)) return 1;
    g_ffog = FALSE;
    Cmp ff = cmp(Bm, R0);
    if (ff.maxd > 1e-6) fail = 1;
    o += snprintf(line + o, sizeof line - o, " FF[%s %.1e]", ff.maxd > 1e-6 ? "FAIL" : "ok", ff.maxd);
    if (on) {
        PhotonLightBlock lb;
        vs_consts(nl);
        ps_consts(fog, 999u);
        on_setup(nl, 1.0f, &lb);
        if (draw(vs3, ps3r, A)) return 1;                       /* the stock math with these lights */
        memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, sizeof lb);
        if (draw(vs3, ps3, Bm)) return 1;                       /* the Photon path */
        Cmp on_ = cmp(A, Bm);
        lb.flags[0] = 0;
        memcpy(&g_pc[PHOTON_LB_FIRST][0], &lb, sizeof lb);
        if (draw(vs3, ps3, R0)) return 1;                       /* flag off, same constants */
        Cmp fo = cmp(A, R0);
        int ok2 = on_.bad == 0 && on_.nan == 0 && on_.covered > W * H * 3 / 4 && on_.distinct >= 64 && fo.bad == 0;
        fail |= !ok2;
        o += snprintf(line + o, sizeof line - o, " ON[%s %.1e (bad %d) cov %d col %d; flag-off %.1e]", ok2 ? "ok" : "FAIL",
                      on_.maxd, on_.bad, on_.covered, on_.distinct, fo.maxd);
    }
    if (occ) fail |= occ_test(vs3, ps3, nl, line, &o, (int)sizeof line);
    printf("%s%s\n", line, fail ? "  <-- FAIL" : "");
    IDirect3DVertexShader9_Release(vsA); IDirect3DVertexShader9_Release(vs3);
    IDirect3DPixelShader9_Release(psA); IDirect3DPixelShader9_Release(ps3o); IDirect3DPixelShader9_Release(ps3);
    IDirect3DPixelShader9_Release(ps3r);
    return fail;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
#define CHECK(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    printf("SKINPAIR FAIL %s hr=0x%08lx\n", what, (unsigned long)hr_); return 1; } } while (0)

static IDirect3DTexture9 *tex4(DWORD (*px)(int x, int y))
{
    IDirect3DTexture9 *t = NULL;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL))) return NULL;
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = px(x, y);
    IDirect3DTexture9_UnlockRect(t, 0);
    return t;
}
static DWORD px_base(int x, int y) { return ((DWORD)(120 + 30 * x) << 24) | ((DWORD)(80 + 40 * y) << 16) | ((DWORD)(220 - 40 * x) << 8) | (DWORD)(100 + 30 * y); }
static DWORD px_nrm(int x, int y)   /* A = spec mask, RGB = tangent-space normal around +Z */
{
    return ((DWORD)(150 + 25 * y) << 24) | ((DWORD)(128 + 28 * (x - 1)) << 16) | ((DWORD)(128 + 24 * (y - 2)) << 8) | 235u;
}
static DWORD px_spec(int x, int y)  /* R = exponent (1 + 149 r), G = albedo tint mask, A = rim mask */
{
    return ((DWORD)(90 + 50 * y) << 24) | ((DWORD)(10 + 20 * x) << 16) | ((DWORD)(60 + 60 * y) << 8) | (DWORD)(40 + 50 * x);
}

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: skinpair.exe <list>\n"); return 2; }
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { printf("SKINPAIR FAIL cannot read %s\n", argv[1]); return 1; }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "skinpair";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("skinpair", "skinpair", WS_OVERLAPPEDWINDOW, 0, 0, W + 16, H + 40, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!hwnd || !d3d) { printf("SKINPAIR FAIL window/Direct3DCreate9\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id))) printf("adapter: %s\n", id.Description);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev), "CreateDevice");
    CHECK(IDirect3DDevice9_CreateRenderTarget(g_dev, W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, NULL), "RT");
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &g_sys, NULL), "sysmem");
    CHECK(IDirect3DDevice9_CreateVertexDeclaration(g_dev, g_decl, &g_vdecl), "decl");
    g_base = tex4(px_base); g_nrm = tex4(px_nrm); g_spec = tex4(px_spec);
    if (!g_base || !g_nrm || !g_spec) { printf("SKINPAIR FAIL textures\n"); return 1; }
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cube, NULL), "cube");
    for (int fc = 0; fc < 6; fc++) {
        IDirect3DCubeTexture9_LockRect(g_cube, (D3DCUBEMAP_FACES)fc, 0, &lr, NULL, 0);
        *(DWORD *)lr.pBits = 0x80000000u | ((DWORD)(30 * fc) << 16) | ((DWORD)(200 - 25 * fc) << 8) | (DWORD)(60 + 20 * fc);
        IDirect3DCubeTexture9_UnlockRect(g_cube, (D3DCUBEMAP_FACES)fc, 0);
    }
    /* OCC: white base (alpha 0), the mipped cube (last mip uniform OCC_LOW grey) and a black cube */
    {
        IDirect3DTexture9 *t = NULL;
        CHECK(IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL), "white");
        IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = 0x00ffffffu;
        IDirect3DTexture9_UnlockRect(t, 0);
        g_white = t;
    }
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 4, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cubeocc, NULL), "cubeocc");
    for (int fc = 0; fc < 6; fc++)
        for (int lv = 0; lv < 3; lv++) {
            IDirect3DCubeTexture9_LockRect(g_cubeocc, (D3DCUBEMAP_FACES)fc, lv, &lr, NULL, 0);
            int n = 4 >> lv;
            for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
                DWORD c = lv == 2 ? 0xff000000u | (OCC_LOW << 16) | (OCC_LOW << 8) | OCC_LOW
                                  : 0xff000000u | ((DWORD)(200 - 30 * fc) << 16) | ((DWORD)(90 + 40 * x) << 8) | (DWORD)(60 + 50 * y);
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
    for (int s = 0; s < 16; s++) {
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    }
    make_grid();
    char ln[4096]; int cases = 0, fails = 0, npair = 0, non = 0, nocc = 0;
    while (fgets(ln, sizeof ln, lf)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char a[6][MAX_PATH], name[64]; int fog, nl, ne, on, occ;
        if (sscanf(ln, "case %63s %d %d %259s %259s %259s %259s %259s %259s %d", name, &fog, &nl, a[0], a[1], a[2], a[3],
                   a[4], a[5], &ne) == 10) {
            char *f[6]; for (int i = 0; i < 6; i++) f[i] = a[i];
            cases++; fails += run_case(name, fog, nl, f, ne);
        } else if (sscanf(ln, "pair %63s %d %d %259s %259s %259s %259s %259s %259s %d %d", name, &fog, &nl, a[0], a[1], a[2],
                          a[3], a[4], a[5], &on, &occ) == 11) {
            char *f[6]; for (int i = 0; i < 6; i++) f[i] = a[i];
            cases++; npair++; non += on; nocc += occ; fails += run_pair(name, fog, nl, f, on, occ);
        }
    }
    if (!cases) { printf("SKINPAIR FAIL no cases\n"); return 1; }
    if (fails) { printf("SKINPAIR FAIL %d of %d cases\n", fails, cases); return 1; }
    if (npair) printf("SKINPAIR OK %d cases (%d pair cases: %d with ON, %d with OCC)\n", cases, npair, non, nocc);
    else printf("SKINPAIR OK %d cases\n", cases);
    return 0;
}
