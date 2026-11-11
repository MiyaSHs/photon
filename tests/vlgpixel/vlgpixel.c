/* vlgpixel: pixel test of photon_vlg_ps30 against GMod's own vertexlit_and_unlit_generic_ps30 (T3.4b).
 *
 * Runs each (stock, photon) dynamic-combo-0 pair from tools/t34_build_vlg.py on a real D3D9 device (DXVK on
 * lavapipe, scripts/run_vlgpixel.sh) into a 64x64 A32B32G32R32F target, with a test VS (vlgpixel_vs30.fxc) that
 * feeds affine per-vertex data into the stock interpolator layout. A probe PS first reads back every pixel's exact
 * interpolated uv, so the CPU reference evaluates the same sample points.
 *
 * Per pair:
 *   E1  Photon flag off (c32.x = 0.25, c56.x = -0.5), c32..c56 otherwise random, random c0..c31  == stock, bit for bit
 *   E2  Photon block all zero                                                     == stock, bit for bit
 *   E3  flag on, no lights, no ambient, vertex-colour weight 1 (diffuse = VS colour) == stock (|d| <= 1e-6)
 *   S   sanity: the stock image is finite with >= 256 distinct colours (the equalities are not trivial), and the
 *       Photon flag-on image with the R1 lights (full output) differs from stock in >= 90% of the pixels
 *   R1  flag on, debug view 1 (diffuse only): 4 lights (point, spot, directional, ranged point), Lambert
 *   R2  2 lights + NaN in the unused light slots (must be skipped), half-Lambert, scales, floor, vertex colour
 *   R3  no lights: ambient cube with a floor
 *   R4  debug view 2: world normal * 0.5 + 0.5
 *   R1..R4 are compared with a CPU reference of the stock VS light math (SDK common_vs_fxc.h VertexAttenInternal,
 *   CosineTermInternal, PixelShaderAmbientLight) driven by physical light parameters packed with
 *   photon_lb_pack_light() (photon_lightblock.h), rel err <= 1e-3.
 *   F1  (plain static 4 only) flag on, full output: albedo * modulation * diffuse vs the CPU reference.
 * usage: vlgpixel.exe <list>   list lines: "vs <file>", "probe <file>", "pair <static id> <stock.bin> <photon.bin>"
 * Prints one line per pair and "VLGPIXEL OK <n> pairs" (exit 0) or "VLGPIXEL FAIL ..." (exit 1).
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
#define NC 57   /* c0..c56 (c56: the P1 envmap occlusion, 0 = off) */

typedef struct { float pos[4], n[3], col[4], tex[4], wpos[3], eye[3]; } Vtx;
static const D3DVERTEXELEMENT9 g_decl[] = {
    {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
    {0, 28, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    {0, 44, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    {0, 60, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
    {0, 72, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
    D3DDECL_END()};

/* the affine attribute fields; the CPU reference evaluates them at the probed (u, v) */
static void attr_normal(double u, double v, double n[3]) { n[0] = -0.8 + 1.6 * u; n[1] = 0.3 - 0.9 * v; n[2] = 0.6 + 0.2 * u - 0.4 * v; }
static void attr_wpos(double u, double v, double p[3]) { p[0] = -40 + 80 * u; p[1] = -40 + 80 * v; p[2] = 10 + 20 * u - 10 * v; }
static void attr_col(double u, double v, double c[3]) { c[0] = 0.2 + 0.5 * u; c[1] = 0.3 + 0.4 * v; c[2] = 0.6 - 0.3 * u; }

static void make_vtx(Vtx *o, float u, float v)
{
    double t[3];
    memset(o, 0, sizeof *o);
    o->pos[0] = -1 + 2 * u; o->pos[1] = 1 - 2 * v; o->pos[2] = 0.5f; o->pos[3] = 1;
    attr_normal(u, v, t); for (int i = 0; i < 3; i++) o->n[i] = (float)t[i];
    attr_col(u, v, t); for (int i = 0; i < 3; i++) o->col[i] = (float)t[i];
    o->col[3] = 1;
    o->tex[0] = u; o->tex[1] = v; o->tex[2] = 2 * u; o->tex[3] = 2 * v;
    attr_wpos(u, v, t); for (int i = 0; i < 3; i++) o->wpos[i] = (float)t[i];
    o->eye[0] = 1 - u; o->eye[1] = 0.5f; o->eye[2] = v + 0.2f;
}

static IDirect3DDevice9 *g_dev;
static IDirect3DSurface9 *g_rt, *g_sys;
static IDirect3DVertexDeclaration9 *g_vdecl;
static IDirect3DVertexShader9 *g_vs;
static IDirect3DTexture9 *g_tex4, *g_tex1;
static IDirect3DCubeTexture9 *g_cube;
static Vtx g_quad[4];
static float g_uv[H][W][4];

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

static IDirect3DPixelShader9 *load_ps(const char *path)
{
    long n; void *b = slurp(path, &n);
    IDirect3DPixelShader9 *ps = NULL;
    if (!b) { printf("cannot read %s\n", path); return NULL; }
    HRESULT hr = IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)b, &ps);
    free(b);
    if (FAILED(hr)) { printf("CreatePixelShader(%s) hr=0x%08lx\n", path, (unsigned long)hr); return NULL; }
    return ps;
}

static unsigned g_seed = 12345;
static float frand(float lo, float hi) { g_seed = g_seed * 1664525u + 1013904223u; return lo + (hi - lo) * ((g_seed >> 8) / 16777216.0f); }

/* draw a full-target quad with ps and constants c0..c55, read back into out */
static int draw(IDirect3DPixelShader9 *ps, const float c[NC][4], IDirect3DTexture9 *t2d, float out[H][W][4])
{
    IDirect3DDevice9 *d = g_dev;
    HRESULT hr;
    IDirect3DDevice9_SetRenderTarget(d, 0, g_rt);
    IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    IDirect3DDevice9_BeginScene(d);
    IDirect3DDevice9_SetVertexDeclaration(d, g_vdecl);
    IDirect3DDevice9_SetVertexShader(d, g_vs);
    IDirect3DDevice9_SetPixelShader(d, ps);
    IDirect3DDevice9_SetPixelShaderConstantF(d, 0, &c[0][0], NC);
    static const int s2d[] = {0, 2, 4, 6, 7, 8, 10, 11, 12};
    for (unsigned i = 0; i < sizeof s2d / sizeof s2d[0]; i++)
        IDirect3DDevice9_SetTexture(d, s2d[i], (IDirect3DBaseTexture9 *)t2d);
    IDirect3DDevice9_SetTexture(d, 1, (IDirect3DBaseTexture9 *)g_cube);
    hr = IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, g_quad, sizeof(Vtx));
    IDirect3DDevice9_EndScene(d);
    if (FAILED(hr)) { printf("DrawPrimitiveUP hr=0x%08lx\n", (unsigned long)hr); return 1; }
    if (FAILED(hr = IDirect3DDevice9_GetRenderTargetData(d, g_rt, g_sys))) { printf("GetRenderTargetData hr=0x%08lx\n", (unsigned long)hr); return 1; }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(g_sys, &lr, NULL, D3DLOCK_READONLY))) return 1;
    for (int y = 0; y < H; y++) memcpy(out[y], (const BYTE *)lr.pBits + y * lr.Pitch, W * 16);
    IDirect3DSurface9_UnlockRect(g_sys);
    return 0;
}

/* ---------------------------------------------------------------- CPU reference (stock VS formulas) */
typedef struct {
    int type;                      /* 0 point, 1 spot, 2 directional */
    double color[3], pos[3], dir[3], att[3], range, falloff, theta_dot, phi_dot;
} RefLight;
typedef struct {
    int n, half_lambert, debug;
    RefLight l[4];
    double amb[6][3], light_scale, amb_scale, floor_, vcol_w;
    int nan_unused;
} RefCfg;

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double sat(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static void ref_diffuse(const RefCfg *cf, double u, double v, double out[3])
{
    double N[3], P[3], C[3], n[3];
    attr_normal(u, v, N); attr_wpos(u, v, P); attr_col(u, v, C);
    double len = sqrt(dot3(N, N));
    for (int i = 0; i < 3; i++) n[i] = N[i] / len;
    if (cf->debug == 2) { for (int i = 0; i < 3; i++) out[i] = n[i] * 0.5 + 0.5; return; }
    double lit[3] = {0, 0, 0};
    for (int k = 0; k < cf->n; k++) {
        const RefLight *L = &cf->l[k];
        double toL[3], ld[3], atten, ndl;
        for (int i = 0; i < 3; i++) toL[i] = L->pos[i] - P[i];
        double d2 = dot3(toL, toL), ood = 1 / sqrt(d2);
        for (int i = 0; i < 3; i++) ld[i] = toL[i] * ood;
        /* VertexAttenInternal: 1/dot(atten, dst(d2, 1/d)) and the spot cone */
        double dist_atten = 1 / (L->att[0] + L->att[1] * d2 * ood + L->att[2] * d2);
        double nl[3] = {-ld[0], -ld[1], -ld[2]};
        double spot = (dot3(L->dir, nl) - L->phi_dot) * (1 / (L->theta_dot - L->phi_dot));
        spot = sat(pow(spot > 0.0001 ? spot : 0.0001, L->falloff));
        atten = L->type == 1 ? dist_atten * spot : dist_atten;
        if (L->range > 0) { double x = d2 / (L->range * L->range); double w = sat(1 - x * x); atten *= w * w; }
        if (L->type == 2) { for (int i = 0; i < 3; i++) ld[i] = -L->dir[i]; atten = 1; }
        ndl = dot3(n, ld);                           /* CosineTermInternal */
        ndl = cf->half_lambert ? (ndl * 0.5 + 0.5) * (ndl * 0.5 + 0.5) : (ndl > 0 ? ndl : 0);
        for (int i = 0; i < 3; i++) lit[i] += L->color[i] * ndl * atten;
    }
    for (int i = 0; i < 3; i++) {                    /* PixelShaderAmbientLight */
        double a = 0;
        for (int ax = 0; ax < 3; ax++) a += n[ax] * n[ax] * cf->amb[ax * 2 + (n[ax] >= 0 ? 0 : 1)][i];
        a *= cf->amb_scale;
        if (a < cf->floor_) a = cf->floor_;
        out[i] = lit[i] * cf->light_scale + a + C[i] * cf->vcol_w;
    }
}

static void pack_cfg(const RefCfg *cf, float c[NC][4])
{
    PhotonLightBlock lb;
    memset(&lb, 0, sizeof lb);
    lb.flags[0] = 1; lb.flags[1] = (float)cf->n; lb.flags[2] = (float)cf->half_lambert; lb.flags[3] = (float)cf->vcol_w;
    for (int f = 0; f < 6; f++) for (int i = 0; i < 3; i++) lb.ambient[f][i] = (float)cf->amb[f][i];
    for (int k = 0; k < 4; k++) {
        const RefLight *L = &cf->l[k];
        if (k >= cf->n && cf->nan_unused) { memset(lb.lights[k], 0xff, sizeof lb.lights[k]); continue; } /* NaN */
        float col[3], pos[3], dir[3];
        for (int i = 0; i < 3; i++) { col[i] = (float)L->color[i]; pos[i] = (float)L->pos[i]; dir[i] = (float)L->dir[i]; }
        photon_lb_pack_light(lb.lights[k], (float)L->type, col, pos, dir, (float)L->att[0], (float)L->att[1],
                             (float)L->att[2], (float)L->range, (float)L->falloff, (float)L->theta_dot, (float)L->phi_dot);
    }
    lb.tuning[0] = (float)cf->light_scale; lb.tuning[1] = (float)cf->amb_scale; lb.tuning[2] = (float)cf->floor_;
    lb.tuning[3] = (float)cf->debug;
    memcpy(&c[PHOTON_LB_FIRST][0], &lb, sizeof lb);
}

static void norm3(double *d) { double l = sqrt(dot3(d, d)); for (int i = 0; i < 3; i++) d[i] /= l; }

static void make_cfgs(RefCfg cf[4])
{
    memset(cf, 0, 4 * sizeof *cf);
    RefLight *l = cf[0].l;
    /* point, 1/d^2 */
    l[0] = (RefLight){0, {2000, 1500, 1000}, {0, 0, 60}, {0, 0, 1}, {0, 0, 1}, 0, 0, 1, 0};
    /* spot */
    l[1] = (RefLight){1, {3, 2, 4}, {30, -20, 40}, {-0.5, 0.3, -1}, {1, 0.01, 0.0005}, 0, 2, cos(20 * M_PI / 180), cos(45 * M_PI / 180)};
    norm3(l[1].dir);
    /* directional */
    l[2] = (RefLight){2, {0.4, 0.5, 0.6}, {0, 0, 0}, {0.3, -0.6, -0.7}, {0, 0, 0}, 0, 0, 1, 0};
    norm3(l[2].dir);
    /* ranged point, 1/d */
    l[3] = (RefLight){0, {40, 30, 20}, {-30, 30, 30}, {0, 0, 1}, {0, 1, 0}, 70, 0, 1, 0};
    cf[0].n = 4;
    for (int f = 0; f < 6; f++) for (int i = 0; i < 3; i++) cf[0].amb[f][i] = 0.02 + 0.05 * f + 0.03 * i;
    cf[0].light_scale = 1; cf[0].amb_scale = 1; cf[0].debug = 1;
    cf[1] = cf[0];
    cf[1].n = 2; cf[1].half_lambert = 1; cf[1].nan_unused = 1; cf[1].light_scale = 1.5; cf[1].amb_scale = 0.5;
    cf[1].floor_ = 0.05; cf[1].vcol_w = 1;
    cf[2] = cf[0];
    cf[2].n = 0; cf[2].floor_ = 0.3;
    cf[3] = cf[0];
    cf[3].debug = 2;
}

/* ---------------------------------------------------------------- comparisons */
typedef struct { int bad; double maxd; } Cmp;

static Cmp cmp_img(const float a[H][W][4], const float b[H][W][4], double tol)
{
    Cmp r = {0, 0};
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int i = 0; i < 4; i++) {
        double d = fabs((double)a[y][x][i] - b[y][x][i]);
        if (!(d <= tol) || (tol == 0 && memcmp(&a[y][x][i], &b[y][x][i], 4))) r.bad++;
        if (d > r.maxd || d != d) r.maxd = d != d ? INFINITY : d;
    }
    return r;
}

static Cmp cmp_ref(const RefCfg *cf, const float img[H][W][4], const double albedo_mod[3])
{
    Cmp r = {0, 0};
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double e[3];
        ref_diffuse(cf, g_uv[y][x][0], g_uv[y][x][1], e);
        for (int i = 0; i < 3; i++) {
            if (albedo_mod) e[i] *= albedo_mod[i];
            double d = fabs(img[y][x][i] - e[i]) / (fabs(e[i]) > 1 ? fabs(e[i]) : 1);
            if (!(d <= 1e-3)) r.bad++;
            if (d > r.maxd || d != d) r.maxd = d != d ? INFINITY : d;
        }
    }
    return r;
}

static float g_a[H][W][4], g_b[H][W][4];

static int run_pair(int sid, const char *stock_path, const char *photon_path)
{
    IDirect3DPixelShader9 *ps_s = load_ps(stock_path), *ps_p = load_ps(photon_path);
    if (!ps_s || !ps_p) return 1;
    float c[NC][4], cz[NC][4];
    int fail = 0;
    char line[1024];
    int o = snprintf(line, sizeof line, "static %-9d", sid);
    /* E1/E2/E3: equivalence with random stock constants */
    g_seed = 777u + (unsigned)sid;
    for (int r = 0; r < 32; r++) for (int i = 0; i < 4; i++) c[r][i] = frand(0.05f, 0.95f);
    for (int r = 32; r < NC; r++) for (int i = 0; i < 4; i++) c[r][i] = frand(-2.0f, 2.0f);
    c[PHOTON_LB_FLAGS][0] = 0.25f;
    c[PHOTON_LB_ENVOCC][0] = -0.5f;     /* P1 envmap occlusion off (c56.x <= 0; c56.yzw stay random) */
    memcpy(cz, c, sizeof c);
    memset(&cz[32][0], 0, (NC - 32) * 16);
    if (draw(ps_s, cz, g_tex4, g_a)) return 1;
    if (draw(ps_p, c, g_tex4, g_b)) return 1;
    Cmp e1 = cmp_img(g_a, g_b, 0);
    if (draw(ps_p, cz, g_tex4, g_b)) return 1;
    Cmp e2 = cmp_img(g_a, g_b, 0);
    float c3[NC][4];
    memcpy(c3, cz, sizeof c3);
    c3[32][0] = 1; c3[32][1] = 0; c3[32][2] = 0; c3[32][3] = 1;           /* diffuse = VS colour */
    c3[55][0] = 1; c3[55][1] = 1; c3[55][2] = 0; c3[55][3] = 0;
    if (draw(ps_p, c3, g_tex4, g_b)) return 1;
    Cmp e3 = cmp_img(g_a, g_b, 1e-6);
    fail |= e1.bad || e2.bad || e3.bad;
    int distinct = 0, finite = 1;
    {
        static float seen[H * W][3];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            const float *px = g_a[y][x];
            for (int i = 0; i < 4; i++) finite &= isfinite(px[i]) != 0;
            int k = 0;
            while (k < distinct && memcmp(seen[k], px, 12)) k++;
            if (k == distinct) memcpy(seen[distinct++], px, 12);
        }
    }
    o += snprintf(line + o, sizeof line - o, " E1 %s E2 %s E3 %s(max %.1e)", e1.bad ? "FAIL" : "eq", e2.bad ? "FAIL" : "eq",
                  e3.bad ? "FAIL" : "ok", e3.maxd);
    if (e1.bad || e2.bad) o += snprintf(line + o, sizeof line - o, " [E1 %d px max %.2e, E2 %d px max %.2e]", e1.bad, e1.maxd, e2.bad, e2.maxd);
    /* R1..R4: lighting against the CPU reference (neutral stock constants: no fog, light scale 1) */
    RefCfg cf[4];
    make_cfgs(cf);
    {
        float cs[NC][4];
        memcpy(cs, cz, sizeof cs);
        RefCfg f = cf[0];
        f.debug = 0;
        pack_cfg(&f, cs);
        if (draw(ps_p, cs, g_tex4, g_b)) return 1;
        int diff = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) diff += memcmp(g_a[y][x], g_b[y][x], 12) != 0;
        int sok = finite && distinct >= 256 && diff * 10 >= H * W * 9;
        fail |= !sok;
        o += snprintf(line + o, sizeof line - o, " S %s(%d colours, on differs %d%%)", sok ? "ok" : "FAIL", distinct,
                      diff * 100 / (H * W));
    }
    for (int k = 0; k < 4; k++) {
        float cr[NC][4];
        memset(cr, 0, sizeof cr);
        cr[1][0] = cr[1][1] = cr[1][2] = cr[1][3] = 1;    /* g_DiffuseModulation */
        cr[30][0] = 1;                                     /* cLightScale.x (LINEAR_LIGHT_SCALE) */
        pack_cfg(&cf[k], cr);
        if (draw(ps_p, cr, g_tex4, g_b)) return 1;
        Cmp rk = cmp_ref(&cf[k], g_b, NULL);
        fail |= rk.bad != 0;
        o += snprintf(line + o, sizeof line - o, " R%d %s(%.1e)", k + 1, rk.bad ? "FAIL" : "ok", rk.maxd);
    }
    if (sid == 4) {                                        /* F1: the full output of the plain combo */
        float cr[NC][4];
        memset(cr, 0, sizeof cr);
        cr[1][0] = 0.9f; cr[1][1] = 0.8f; cr[1][2] = 0.7f; cr[1][3] = 1;
        cr[30][0] = 1;
        RefCfg f = cf[0];
        f.debug = 0;
        pack_cfg(&f, cr);
        if (draw(ps_p, cr, g_tex1, g_b)) return 1;
        double am[3] = {0.9 * 128 / 255.0, 0.8 * 192 / 255.0, 0.7 * 255 / 255.0};
        Cmp f1 = cmp_ref(&f, g_b, am);
        fail |= f1.bad != 0;
        o += snprintf(line + o, sizeof line - o, " F1 %s(%.1e)", f1.bad ? "FAIL" : "ok", f1.maxd);
    }
    printf("%s%s\n", line, fail ? "  <-- FAIL" : "");
    IDirect3DPixelShader9_Release(ps_s);
    IDirect3DPixelShader9_Release(ps_p);
    return fail;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

#define CHECK(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    printf("VLGPIXEL FAIL %s hr=0x%08lx\n", what, (unsigned long)hr_); return 1; } } while (0)

static int make_textures(void)
{
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DDevice9_CreateTexture(g_dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex4, NULL), "CreateTexture 4x4");
    CHECK(IDirect3DTexture9_LockRect(g_tex4, 0, &lr, NULL, 0), "Lock 4x4");
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        ((DWORD *)((BYTE *)lr.pBits + y * lr.Pitch))[x] = ((DWORD)(4 + 7 * x + 2 * y) << 24) /* alpha < 1/8: SELFILLUM_ENVMAPMASK_ALPHA keeps some diffuse */ | ((DWORD)(30 + 60 * y) << 16) |
                                                          ((DWORD)(200 - 40 * x) << 8) | (DWORD)(90 + 30 * y);
    IDirect3DTexture9_UnlockRect(g_tex4, 0);
    CHECK(IDirect3DDevice9_CreateTexture(g_dev, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex1, NULL), "CreateTexture 1x1");
    CHECK(IDirect3DTexture9_LockRect(g_tex1, 0, &lr, NULL, 0), "Lock 1x1");
    *(DWORD *)lr.pBits = 0xFF80C0FFu;                      /* A=255 R=128 G=192 B=255 */
    IDirect3DTexture9_UnlockRect(g_tex1, 0);
    CHECK(IDirect3DDevice9_CreateCubeTexture(g_dev, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_cube, NULL), "CreateCubeTexture");
    for (int f = 0; f < 6; f++) {
        CHECK(IDirect3DCubeTexture9_LockRect(g_cube, (D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0), "Lock cube");
        *(DWORD *)lr.pBits = 0x80000000u | ((DWORD)(30 * f) << 16) | ((DWORD)(200 - 25 * f) << 8) | (DWORD)(60 + 20 * f);
        IDirect3DCubeTexture9_UnlockRect(g_cube, (D3DCUBEMAP_FACES)f, 0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: vlgpixel.exe <list>\n"); return 2; }
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { printf("VLGPIXEL FAIL cannot read %s\n", argv[1]); return 1; }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "vlgpixel";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("vlgpixel", "vlgpixel", WS_OVERLAPPEDWINDOW, 0, 0, W + 16, H + 40, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!hwnd || !d3d) { printf("VLGPIXEL FAIL window/Direct3DCreate9\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id))) printf("adapter: %s\n", id.Description);
    CHECK(IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
                                       D3DRTYPE_SURFACE, D3DFMT_A32B32G32R32F), "A32B32G32R32F render target unsupported");
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev), "CreateDevice");
    D3DCAPS9 caps;
    IDirect3DDevice9_GetDeviceCaps(g_dev, &caps);
    printf("caps: PS %lu.%lu, PS30 instruction slots %lu\n", (caps.PixelShaderVersion >> 8) & 0xff, caps.PixelShaderVersion & 0xff,
           (unsigned long)caps.MaxPixelShader30InstructionSlots);
    CHECK(IDirect3DDevice9_CreateRenderTarget(g_dev, W, H, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, NULL), "CreateRenderTarget");
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, W, H, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &g_sys, NULL), "sysmem surface");
    CHECK(IDirect3DDevice9_CreateVertexDeclaration(g_dev, g_decl, &g_vdecl), "CreateVertexDeclaration");
    if (make_textures()) return 1;
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHATESTENABLE, FALSE);
    D3DVIEWPORT9 vp = {0, 0, W, H, 0, 1};
    make_vtx(&g_quad[0], 0, 0); make_vtx(&g_quad[1], 1, 0); make_vtx(&g_quad[2], 0, 1); make_vtx(&g_quad[3], 1, 1);

    char ln[2048];
    int pairs = 0, fails = 0;
    IDirect3DPixelShader9 *probe = NULL;
    while (fgets(ln, sizeof ln, lf)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char kind[16], a[MAX_PATH], b[MAX_PATH];
        int sid;
        if (sscanf(ln, "vs %259s", a) == 1) {
            long n; void *bc = slurp(a, &n);
            if (!bc) { printf("VLGPIXEL FAIL cannot read %s\n", a); return 1; }
            CHECK(IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)bc, &g_vs), "CreateVertexShader");
            free(bc);
        } else if (sscanf(ln, "probe %259s", a) == 1) {
            if (!(probe = load_ps(a))) return 1;
            IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_rt);
            IDirect3DDevice9_SetViewport(g_dev, &vp);
            float c[NC][4];
            memset(c, 0, sizeof c);
            if (draw(probe, c, g_tex4, g_uv)) return 1;
            printf("probe: pixel (0,0) uv (%.6f, %.6f), (%d,%d) uv (%.6f, %.6f)\n", g_uv[0][0][0], g_uv[0][0][1], W - 1, H - 1,
                   g_uv[H - 1][W - 1][0], g_uv[H - 1][W - 1][1]);
        } else if (sscanf(ln, "%15s %d %259s %259s", kind, &sid, a, b) == 4 && !strcmp(kind, "pair")) {
            if (!g_vs || !probe) { printf("VLGPIXEL FAIL list needs vs and probe first\n"); return 1; }
            pairs++;
            fails += run_pair(sid, a, b);
        }
    }
    fclose(lf);
    if (!pairs || fails) { printf("VLGPIXEL FAIL %d of %d pairs\n", fails, pairs); return 1; }
    printf("VLGPIXEL OK %d pairs\n", pairs);
    return 0;
}
