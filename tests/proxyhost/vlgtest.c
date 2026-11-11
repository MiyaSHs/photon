/* T3.4c/T3.4f mock-host tests of the proxy's lighting v0 module, Option A (src/proxy_shaderapi/vlg.h). Run by
 * scripts/run_proxyhost.sh with PHOTON_TEST_VLG=on|off on Wine + DXVK + lavapipe in a private Xvfb, after the base
 * proxyhost checks.
 *
 * Harness:
 *   - a real IDirect3DDevice9 (DXVK) is created and planted in stock's g_pD3DDevice (stock+0xa6b18), so the proxy's
 *     d3dconst validation passes for real; after validation its SetPixelShaderConstantF slot (109) is patched to a
 *     recorder that keeps every call (start, count, data) and forwards it, so the test sees exactly what the device
 *     receives; GetPixelShaderConstantF reads the device state back;
 *   - the real ShaderShadow010 and ShaderApi030 objects get a vtable copy in which the slots that cannot run in this
 *     host are fakes: shadow 0/23/24 model stock (per thread: the current VS/PS name and index, a handle per name and
 *     index at +0x270/+0x278 and the index at +0x280/+0x284, SetDefaultState = both NULL; an invalid handle for our
 *     name on request = a load failure), api 109 TakeSnapshot (checks the PAIRING INVARIANT on what the fake stock
 *     shadow holds: our vs_3_0 iff our ps_3_0; returns a test-chosen id with garbage upper bits), 125 RenderPass
 *     (reads c32..c55 back from the device, i.e. what a draw would see), 170/260 (counted). SetLight,
 *     SetAmbientLightCube, SetLightingOrigin, DisableAllLocalLights, BeginPass, SetVertexShaderIndex and GetMaxLights
 *     stay the real stock functions (stock SetLight builds the D3DLIGHT9 records the proxy's layout check uses);
 *   - the r_pixelfog gate is driven through stock's own ConVar object (stock+0xa2a90: m_nValue +0x58 xor this, read
 *     back through its own GetInt), dxlevel through the caps object (+0x4f8), FF fog through ShaderApi030 +0x132d.
 * on:  G  the fog gate (r_pixelfog 0, dxlevel 85): zero renames, zero marks, zero device writes;
 *      R  renames: rejection rules, every id of both installed .ids files (and its unlisted neighbour);
 *      P  the exhaustive pairing matrix: VS kinds x PS kinds x call sequences: the fake stock never holds our VS
 *         with a stock PS or the reverse, stock receives the caller's own pointer and index for whatever is not
 *         renamed, both are renamed exactly when the rule says, the stock VS is re-sent BEFORE a non-renamed PS;
 *      N  the TakeSnapshot net (a SetVertexShader that bypasses the wrapper, as stock-internal calls do);
 *      D  the light block bytes (vs20 dynamic index bits), FOG (draws with FF fog on are counted), live config,
 *         snapshot clears, T a threaded snapshot/draw run, F a load failure (PHOTON_TEST_VLG_FAIL=vs|ps, default
 *         ps), then the matrix again: zero renames.
 * off: no vlg hook on any of the 13 slots, stock receives the caller's pointers for the whole matrix, the device
 *      receives nothing.
 * Plan v2 P1/P2 (version 3): the bump pair (PHOTON_TEST_BUMP_VS_IDS / _PS_IDS; PHOTON_TEST_VLG_PAIRS = "vlg,bump"
 * (default) or "vlg" when the run sets photon_bump 0) is in the fake stock, the matrix (bump VS/PS kinds, cross-pair
 * sequences: our VS of one pair never with our PS of the other), R (bump renames and every bump id), D13 (a bump draw:
 * the block, any VS index), D14 (c56: the occlusion-only writes and their cache, the config keys folded independently
 * of the proxy), D15 (slot 260 invalidates the c56 cache).
 * Plan v2 P2 (version 4): the skin pair (PHOTON_TEST_SKIN_VS_IDS / _PS_IDS; "skin" in PHOTON_TEST_VLG_PAIRS, default
 * "vlg,bump,skin") the same way: matrix kinds (skin VS statics 0/1, an out-of-range static, idx +1; skin PS listed,
 * unlisted, idx +1) and cross-pair sequences with vlg and bump, R (skin renames, every skin id), D16 (a skin draw: the
 * block at any VS index without LIGHTING_PREVIEW (x8) and < 48, stock look otherwise, the shader-manager index too).
 * The mutant build (PHOTON_VLG_MUTANT=1, no reverts) must FAIL here with fake-stock pairing violations.
 * PHOTON_AMBIENT=1 with PHOTON_TEST_AMB: only amb_tests, below; every other check above needs the volume off, and D21 proves that it is by default. */
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/proxy_shaderapi/photon_stats.h"
#include "../../src/proxy_shaderapi/photon_lightblock.h"
#include "../../src/ambient/amb_bake.h"
#include "../../src/ambient/amb_clip.h"
#include "../ambient/synth_bsp.h"
#include "fake_engine.h"

static const char VS_STOCK[] = "vertexlit_and_unlit_generic_vs20", PS_STOCK[] = "vertexlit_and_unlit_generic_ps20b";
static const char BVS_STOCK[] = "vertexlit_and_unlit_generic_bump_vs20", BPS_STOCK[] = "vertexlit_and_unlit_generic_bump_ps20b";
#define VS_OURS "photon_vlg_vs20"
#define PS_OURS "photon_vlg_ps20b"
#define BVS_OURS "photon_bump_vs20"
#define BPS_OURS "photon_bump_ps20b"
static const char SVS_STOCK[] = "skin_vs20", SPS_STOCK[] = "skin_ps20b";
#define SVS_OURS "photon_skin_vs20"
#define SPS_OURS "photon_skin_ps20b"
enum { BVSN = 24, BPSN = 20, SVSN = 48, SPSN = 80, NB = 25 /* c32..c56 */ };
static const char VS30[] = "vertexlit_and_unlit_generic_vs30", VS_OTHER[] = "lightmappedgeneric_vs20",
                  PS_DECAL[] = "decalmodulate_ps20b", PS20[] = "vertexlit_and_unlit_generic_ps20",
                  PS30[] = "vertexlit_and_unlit_generic_ps30", PS_OTHER[] = "lightmappedgeneric_ps20b";
enum { VSN = 384, PSN = 12 };

static int g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL vlg: " __VA_ARGS__); printf("\n"); g_fails++; } } while (0)

typedef struct TLightDesc {   /* public mathlib/lightdesc.h */
    int type; float color[3], pos[3], dir[3]; float range, falloff, att0, att1, att2, theta, phi, thetadot, phidot;
    unsigned flags; float oo, rsq;
} TLightDesc;

/* --- device recorder ------------------------------------------------------------------------------------------ */
static IDirect3DDevice9 *g_dev;
static void *g_orig_setpsf;
typedef struct { UINT start, count; float v[NB][4]; } RecCall;
static RecCall g_rec[64];
static volatile LONG g_nrec, g_rec_total, g_rec_above_block;
static volatile int g_rec_store = 1;
static HRESULT WINAPI rec_setpsf(IDirect3DDevice9 *d, UINT start, const float *v, UINT count) {
    InterlockedIncrement(&g_rec_total);
    if (start + count > PHOTON_LB_FIRST + PHOTON_LB_COUNT) InterlockedIncrement(&g_rec_above_block);
    if (g_rec_store && g_nrec < 64) {
        RecCall *c = &g_rec[g_nrec++];
        c->start = start; c->count = count;
        memcpy(c->v, v, (count > NB ? NB : count) * 16);
    }
    return ((HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, const float *, UINT))g_orig_setpsf)(d, start, v, count);
}
static void rec_reset(void) { g_nrec = 0; }

/* --- fake stock shadow (per thread, like a snapshot under construction) ----------------------------------------- */
static int is(const char *n, const char *s) { return n && strcmp(n, s) == 0; }
static uint64_t fk_handle(const char *n, int idx) {
    uint64_t id = 0;
    if (n) {
        if (is(n, VS_OURS)) id = 1;
        else if (is(n, PS_OURS)) id = 2;
        else if (is(n, BVS_OURS)) id = 3;
        else if (is(n, BPS_OURS)) id = 4;
        else if (is(n, SVS_OURS)) id = 5;
        else if (is(n, SPS_OURS)) id = 6;
        else { uint32_t h = 5381; for (const char *p = n; *p; p++) h = h * 33 + (uint8_t)*p; id = 7 + h % 1000003u; }
    }
    return (id << 32) | (uint32_t)idx;
}
typedef struct { int ps; const char *name; int idx; } FkCall;
static __thread const char *t_fv, *t_fp;
static __thread int t_fvi, t_fpi;
static __thread FkCall t_log[16];
static __thread int t_nlog;
static __thread const char *t_snap_vs, *t_snap_ps;
static __thread int t_snap_vsi, t_snap_psi;
static volatile int g_fail_vs, g_fail_ps;
static volatile uint64_t g_fk_force_ps_h;   /* one-shot: the next PS gets this handle (a reused handle of ours) */
static volatile LONG g_fk_viol, g_fk_takes, g_def_calls, g_c170, g_c260;
static void fk_log(int ps, const char *n, int i) {
    if (t_nlog < 16) { t_log[t_nlog].ps = ps; t_log[t_nlog].name = n; t_log[t_nlog].idx = i; }
    t_nlog++;
}
static void fake_sh_default(void *self) {
    InterlockedIncrement(&g_def_calls);
    t_fv = t_fp = NULL; t_fvi = t_fpi = 0;
    *(volatile uint64_t *)((char *)self + 0x270) = fk_handle(NULL, 0);
    *(volatile uint64_t *)((char *)self + 0x278) = fk_handle(NULL, 0);
}
static void fake_sh_vs(void *self, const char *n, int i) {
    fk_log(0, n, i);
    t_fv = n; t_fvi = i;
    *(volatile uint64_t *)((char *)self + 0x270) = g_fail_vs && is(n, VS_OURS) ? 0xffffffffull : fk_handle(n, i);
    *(volatile int *)((char *)self + 0x280) = i;
}
static void fake_sh_ps(void *self, const char *n, int i) {
    fk_log(1, n, i);
    t_fp = n; t_fpi = i;
    *(volatile uint64_t *)((char *)self + 0x278) = g_fail_ps && is(n, PS_OURS) ? 0xffffffffull : fk_handle(n, i);
    if (g_fk_force_ps_h) { *(volatile uint64_t *)((char *)self + 0x278) = g_fk_force_ps_h; g_fk_force_ps_h = 0; }
    *(volatile int *)((char *)self + 0x284) = i;
}
static __thread int t_next_snap;
/* which pair's shader of ours a name is (0: a stock or other shader) */
static int ours_vs_kind(const char *n) { return is(n, VS_OURS) ? 1 : is(n, BVS_OURS) ? 2 : is(n, SVS_OURS) ? 3 : 0; }
static int ours_ps_kind(const char *n) { return is(n, PS_OURS) ? 1 : is(n, BPS_OURS) ? 2 : is(n, SPS_OURS) ? 3 : 0; }
static const char *ours_vs_name(int k) { return k == 2 ? BVS_OURS : k == 3 ? SVS_OURS : VS_OURS; }
static const char *ours_ps_name(int k) { return k == 2 ? BPS_OURS : k == 3 ? SPS_OURS : PS_OURS; }
/* the snapshot mark of a pair of kind k (1 vlg, 2 bump, 3 skin), without the half-Lambert bit */
static int pair_mark(int k) { return 1 | (k == 2 ? 4 : 0) | (k == 3 ? 8 : 0); }
static uintptr_t fake_take(void *self) {
    (void)self;
    InterlockedIncrement(&g_fk_takes);
    /* the invariant: our vs_3_0 iff our ps_3_0, and of the same pair */
    if (ours_vs_kind(t_fv) != ours_ps_kind(t_fp)) InterlockedIncrement(&g_fk_viol);
    t_snap_vs = t_fv; t_snap_vsi = t_fvi; t_snap_ps = t_fp; t_snap_psi = t_fpi;
    return 0xdead0000ull | (uint16_t)t_next_snap;
}
static __thread float t_rp_c[NB][4];
static __thread int t_rp_calls;
static void fake_rp(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)self; (void)a; (void)b; (void)c;
    t_rp_calls++;
    if (g_dev) g_dev->lpVtbl->GetPixelShaderConstantF(g_dev, 32, &t_rp_c[0][0], NB);
}
static void fake_vsidx(void *self, int i) { (void)self; (void)i; }
static void fake_170(void *self) { (void)self; InterlockedIncrement(&g_c170); }
static void fake_260(void *self) { (void)self; InterlockedIncrement(&g_c260); }

/* --- call helpers through the proxy's wrappers -------------------------------------------------------------- */
static void *g_sh, *g_api;
#define VT(o) (*(void ***)(o))
static void sh_default(void) { ((void (*)(void *))VT(g_sh)[0])(g_sh); }
static void sh_vs(const char *n, int i) { ((void (*)(void *, const char *, int))VT(g_sh)[23])(g_sh, n, i); }
static void sh_ps(const char *n, int i) { ((void (*)(void *, const char *, int))VT(g_sh)[24])(g_sh, n, i); }
static uintptr_t take(int id) { t_next_snap = id; return ((uintptr_t (*)(void *))VT(g_api)[109])(g_api); }
static void beginpass(int id) { ((void (*)(void *, uintptr_t))VT(g_api)[124])(g_api, (uintptr_t)(uint16_t)id); }
static void vsidx(int i) { ((void (*)(void *, int))VT(g_api)[40])(g_api, i); }
static void renderpass(void) { ((void (*)(void *, int, int))VT(g_api)[125])(g_api, 0, 1); }
static void setlight(int i, const TLightDesc *d) { ((void (*)(void *, int, const TLightDesc *))VT(g_api)[127])(g_api, i, d); }
static void setorigin(const float *o) { ((void (*)(void *, const float *))VT(g_api)[128])(g_api, o); }
static void setcube(const float c[6][4]) { ((void (*)(void *, const float *))VT(g_api)[130])(g_api, &c[0][0]); }
static void setambient(float r, float g, float b) { ((void (*)(void *, float, float, float))VT(g_api)[129])(g_api, r, g, b); }
static void disablelights(void) { ((void (*)(void *))VT(g_api)[207])(g_api); }
static void clearsnapshots(void) { ((void (*)(void *))VT(g_api)[170])(g_api); }
static void slot260(void) { ((void (*)(void *))VT(g_api)[260])(g_api); }

/* one snapshot: SetDefaultState, the steps (VS = slot 23, PS = slot 24), TakeSnapshot */
typedef struct { int ps; const char *name; int idx; } Step;
static void snap(const Step *st, int n, int id) {
    sh_default();
    t_nlog = 0;
    for (int i = 0; i < n; i++) { if (st[i].ps) sh_ps(st[i].name, st[i].idx); else sh_vs(st[i].name, st[i].idx); }
    uintptr_t r = take(id);
    CHECK(r == (0xdead0000ull | (uint16_t)id), "TakeSnapshot returned 0x%llx, stock gave 0x%llx", (unsigned long long)r,
          (unsigned long long)(0xdead0000ull | (uint16_t)id));
}
static void snap2(const char *vs, int vsi, const char *ps, int psi, int id) {
    Step s[2] = { { 0, vs, vsi }, { 1, ps, psi } };
    snap(s, 2, id);
}
/* one draw: BeginPass, optional VS index, RenderPass; returns the number of device writes it caused */
static int draw(int id, int vs_index, int set_vs) {
    LONG before = g_rec_total;
    beginpass(id);
    if (set_vs) vsidx(vs_index);
    renderpass();
    return (int)(g_rec_total - before);
}

static void **swap_vtbl(void *obj, size_t n, void ***saved) {
    void **vt = VT(obj);
    void **copy = malloc((n + 1) * sizeof(void *));
    copy[0] = vt[-1];
    memcpy(copy + 1, vt, n * sizeof(void *));
    *saved = vt;
    VT(obj) = copy + 1;
    return copy + 1;
}

static int owned_by(HMODULE hp, void *fn) {
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m);
    return m == hp;
}

/* --- the installed .ids (the test's own parse) ----------------------------------------------------------------- */
typedef struct { uint32_t id[16384]; int n; } Ids;
static void read_ids(const char *path, Ids *o) {
    o->n = 0;
    FILE *f = fopen(path, "r");
    char line[1024];   /* the generated header comment lines are long */
    while (f && fgets(line, sizeof line, f) && o->n < 16384)
        if (line[0] >= '0' && line[0] <= '9') o->id[o->n++] = (uint32_t)strtoul(line, NULL, 10);
    if (f) fclose(f);
}
static int in_ids(const Ids *s, long v) { for (int i = 0; i < s->n; i++) if ((long)s->id[i] == v) return 1; return 0; }
static Ids g_vids, g_pids, g_bvids, g_bpids, g_svids, g_spids;
static int g_ps_unlisted = 5;   /* a VLG PS static the .ids do not list; which ones are built changes with tools/p3_statics.json */
static int g_bump_on, g_skin_on;              /* the bump / skin pair is expected in use (PHOTON_TEST_VLG_PAIRS) */
/* The rule's inputs, computed from the files (not from the proxy): a listed VS / a pilot PS request. */
static int vs_listed(const char *n, int i) { return n == VS_STOCK && i >= 0 && i % VSN == 0 && in_ids(&g_vids, i / VSN); }
static int ps_pilot(const char *n, int i) { return n == PS_STOCK && i >= 0 && i % PSN == 0 && in_ids(&g_pids, i / PSN); }
static int bvs_listed(const char *n, int i) {
    return g_bump_on && n == BVS_STOCK && i >= 0 && i % BVSN == 0 && in_ids(&g_bvids, i / BVSN);
}
static int bps_pilot(const char *n, int i) {
    return g_bump_on && n == BPS_STOCK && i >= 0 && i % BPSN == 0 && in_ids(&g_bpids, i / BPSN);
}
static int svs_listed(const char *n, int i) {
    return g_skin_on && n == SVS_STOCK && i >= 0 && i % SVSN == 0 && in_ids(&g_svids, i / SVSN);
}
static int sps_pilot(const char *n, int i) {
    return g_skin_on && n == SPS_STOCK && i >= 0 && i % SPSN == 0 && in_ids(&g_spids, i / SPSN);
}
/* the pair a request would be renamed for (1 vlg, 2 bump, 3 skin), 0 none */
static int vs_pair(const char *n, int i) { return vs_listed(n, i) ? 1 : bvs_listed(n, i) ? 2 : svs_listed(n, i) ? 3 : 0; }
static int ps_pair(const char *n, int i) { return ps_pilot(n, i) ? 1 : bps_pilot(n, i) ? 2 : sps_pilot(n, i) ? 3 : 0; }
/* expected c56 from the config, written from the shader formula (photon_envocc_ps.h), not from photon_lightblock.h:
 * t = saturate(log2(x) c.x + c.y), factor = t^2 (3 - 2t) c.z + c.w must equal
 * lerp(1, kmin + (kmax - kmin) smoothstep(log2(k / 4), log2(k), log2(x)), s) */
static void exp_occ(float o[4], double s, double kmin, double kmax, double k) {
    if (s <= 0) { o[0] = 0; o[1] = 0; o[2] = 0; o[3] = 1; return; }
    double lo = 1 + s * (kmin - 1), hi = 1 + s * (kmax - 1), w = 2;   /* knees x = k / 2^w and x = k */
    o[0] = (float)(1 / w); o[1] = (float)((w - log2(k)) / w); o[2] = (float)(hi - lo); o[3] = (float)lo;
}

/* --- expected packing (written from the stock CommitVertexShaderLighting rules, not from the proxy code) ------ */
static void exp_light(float o[4][4], const TLightDesc *d) {
    memset(o, 0, 64);
    float tw = d->type == 1 ? 0.0f : d->type == 3 ? 1.0f : 2.0f, s = 1.0f, w2 = 0, w3 = 0;
    if (d->type == 3) {
        float ph = (double)d->phi > 3.141592653589793 ? 3.14159274f : d->phi, th = d->theta;
        if ((double)(th - ph) > -0.001) th = (float)((double)ph - 0.001);
        float sd = cosf(th * 0.5f), sd2 = cosf(ph * 0.5f);
        s = 1.0f / (sd - sd2);
        w2 = d->falloff;
        w3 = sd2 * s;
    }
    for (int i = 0; i < 3; i++) {
        o[0][i] = d->pos[i]; o[1][i] = d->color[i]; o[2][i] = d->dir[i] * s;
    }
    o[1][3] = tw; o[2][3] = w2;
    o[3][0] = d->att0; o[3][1] = d->att1; o[3][2] = d->att2; o[3][3] = w3;
}
static int approx_eq(float a, float b) { return fabsf(a - b) <= 1e-6f * (1.0f + fabsf(b)); }
static int cmp_block(const float *got, const float *want, int nregs, const char *what) {
    int bad = 0;
    for (int i = 0; i < nregs * 4; i++)
        if (!approx_eq(got[i], want[i])) {
            if (bad < 6) printf("  %s: c%d.%c = %.9g, want %.9g\n", what, 32 + i / 4, "xyzw"[i % 4], got[i], want[i]);
            bad++;
        }
    return bad;
}

static void write_cfg(const char *dir, const char *text) {
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\cfg", dir);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\cfg\\photon.cfg", dir);
    FILE *f = fopen(p, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* --- the pairing matrix ------------------------------------------------------------------------------------------ */
typedef struct { int present; const char *name; int idx; const char *what; } Kind;
static Kind g_vk[24], g_pk[24];
static int g_nvk, g_npk;
static void kinds(void) {
    int last_vs = g_vids.n ? (int)g_vids.id[g_vids.n - 1] : 0, last_ps = g_pids.n ? (int)g_pids.id[g_pids.n - 1] : 4;
    static char wl[48], wp[48], wbv[48], wbu[48], wbp[48], wbpu[48];
    snprintf(wl, sizeof wl, "vs20 static %d (last listed)", last_vs);
    snprintf(wp, sizeof wp, "ps20b static %d (last listed)", last_ps);
    int bvs0 = g_bvids.n ? (int)g_bvids.id[0] : 2, bps0 = g_bpids.n ? (int)g_bpids.id[g_bpids.n - 1] : 3, bvs_un = 0, bps_un = 0;
    while (in_ids(&g_bvids, bvs_un)) bvs_un++;
    while (in_ids(&g_bpids, bps_un)) bps_un++;
    snprintf(wbv, sizeof wbv, "bump vs20 static %d (listed)", bvs0);
    snprintf(wbu, sizeof wbu, "bump vs20 static %d (unlisted)", bvs_un);
    snprintf(wbp, sizeof wbp, "bump ps20b static %d (last listed)", bps0);
    snprintf(wbpu, sizeof wbpu, "bump ps20b static %d (unlisted)", bps_un);
    static char wsp[48], wspu[48];
    int sps0 = g_spids.n ? (int)g_spids.id[g_spids.n - 1] : 6, sps_un = 0;
    while (in_ids(&g_spids, sps_un)) sps_un++;
    snprintf(wsp, sizeof wsp, "skin ps20b static %d (last listed)", sps0);
    snprintf(wspu, sizeof wspu, "skin ps20b static %d (unlisted)", sps_un);
    const Kind vk[] = {
        { 0, NULL, 0, "no VS" }, { 1, VS_STOCK, 0, "vs20 static 0" }, { 1, VS_STOCK, 4 * VSN, "vs20 static 4 (HL)" },
        { 1, VS_STOCK, last_vs * VSN, wl }, { 1, VS_STOCK, 1 * VSN, "vs20 static 1 (VERTEXCOLOR)" },
        { 1, VS_STOCK, 8 * VSN, "vs20 static 8 (FLASHLIGHT)" }, { 1, VS_STOCK, 512 * VSN, "vs20 static 512 (TREESWAY)" },
        { 1, VS_STOCK, 3, "vs20 idx 3" }, { 1, VS_STOCK, -VSN, "vs20 idx -384" }, { 1, VS30, 0, "vs30" },
        { 1, VS_OTHER, 0, "lightmappedgeneric_vs20" }, { 1, NULL, 0, "NULL VS name" },
        { 1, BVS_STOCK, bvs0 * BVSN, wbv }, { 1, BVS_STOCK, bvs_un * BVSN, wbu }, { 1, BVS_STOCK, bvs0 * BVSN + 1, "bump vs20 idx +1" },
        { 1, SVS_STOCK, 0, "skin vs20 static 0" }, { 1, SVS_STOCK, SVSN, "skin vs20 static 1 (static control flow)" },
        { 1, SVS_STOCK, 2 * SVSN, "skin vs20 static 2 (out of range)" }, { 1, SVS_STOCK, 1, "skin vs20 idx 1" } };
    const Kind pk[] = {
        { 0, NULL, 0, "no PS" }, { 1, PS_STOCK, 4 * PSN, "ps20b static 4 (pilot)" }, { 1, PS_STOCK, last_ps * PSN, wp },
        { 1, PS_STOCK, g_ps_unlisted * PSN, "ps20b unlisted static" }, { 1, PS_STOCK, 0, "ps20b static 0 (UnlitGeneric)" },
        { 1, PS_STOCK, 4 * PSN + 1, "ps20b idx 49" }, { 1, PS_DECAL, 0, "decalmodulate_ps20b 0" },
        { 1, PS_DECAL, 4 * PSN, "decalmodulate_ps20b 48" }, { 1, PS20, 4 * PSN, "vlg ps20" }, { 1, PS30, 4 * PSN, "vlg ps30" },
        { 1, PS_OTHER, 4 * PSN, "lightmappedgeneric_ps20b" }, { 1, NULL, 0, "NULL PS name" },
        { 1, BPS_STOCK, bps0 * BPSN, wbp }, { 1, BPS_STOCK, bps_un * BPSN, wbpu }, { 1, BPS_STOCK, bps0 * BPSN + 1, "bump ps20b idx +1" },
        { 1, SPS_STOCK, sps0 * SPSN, wsp }, { 1, SPS_STOCK, sps_un * SPSN, wspu }, { 1, SPS_STOCK, sps0 * SPSN + 1, "skin ps20b idx +1" } };
    g_nvk = (int)(sizeof vk / sizeof vk[0]); g_npk = (int)(sizeof pk / sizeof pk[0]);
    _Static_assert(sizeof vk <= sizeof g_vk && sizeof pk <= sizeof g_pk, "kind tables");
    memcpy(g_vk, vk, sizeof vk); memcpy(g_pk, pk, sizeof pk);
}
enum { SEQ_VP, SEQ_PV, SEQ_VP_VOTHER, SEQ_VP_PDECAL, SEQ_VP_VLISTED, SEQ_VP_P, SEQ_VV_P, NSEQ };
static const char *const k_seq[NSEQ] = { "VS,PS", "PS,VS", "VS,PS,VS(other)", "VS,PS,PS(decal)", "VS,PS,VS(vs20 0)",
                                         "VS,PS,PS", "VS,VS,PS" };
typedef struct { int cases, renamed, renamed_bump, renamed_skin, bad, order_bad, fk_viol; } MatrixResult;
/* allow: renames possible at all (gate open, lighting active). Checks every case; returns the totals. */
static MatrixResult matrix(const PhotonStats *st, int allow, int seqs, int id0, const char *tag) {
    MatrixResult m = { 0, 0, 0, 0, 0, 0, 0 };
    LONG viol0 = g_fk_viol;
    long long pv0 = st->vlg->pair_violations;
    for (int s = 0; s < seqs; s++)
        for (int a = 0; a < g_nvk; a++)
            for (int b = 0; b < g_npk; b++) {
                const Kind *V = &g_vk[a], *P = &g_pk[b];
                Step stp[4];
                int n = 0;
                if (s == SEQ_PV) { if (P->present) stp[n++] = (Step){ 1, P->name, P->idx }; if (V->present) stp[n++] = (Step){ 0, V->name, V->idx }; }
                else {
                    if (V->present) stp[n++] = (Step){ 0, V->name, V->idx };
                    if (s == SEQ_VV_P && V->present) stp[n++] = (Step){ 0, V->name, V->idx };
                    if (P->present) stp[n++] = (Step){ 1, P->name, P->idx };
                    if (s == SEQ_VP_VOTHER) stp[n++] = (Step){ 0, VS_OTHER, 0 };
                    if (s == SEQ_VP_PDECAL) stp[n++] = (Step){ 1, PS_DECAL, 0 };
                    if (s == SEQ_VP_VLISTED) stp[n++] = (Step){ 0, VS_STOCK, 0 };
                    if (s == SEQ_VP_P && P->present) stp[n++] = (Step){ 1, P->name, P->idx };
                }
                /* what the caller last sent for each stage (NULL after SetDefaultState) */
                const char *lv = NULL, *lp = NULL;
                int lvi = 0, lpi = 0;
                for (int i = 0; i < n; i++) { if (stp[i].ps) { lp = stp[i].name; lpi = stp[i].idx; } else { lv = stp[i].name; lvi = stp[i].idx; } }
                /* the rule: renamed iff the VS before the last PS was listed, that PS a pilot of the SAME pair, nothing
                 * after it broke the pair (a VS after the PS that is not the same pair's listed VS, or a VS/PS order
                 * with no PS after the VS) */
                int want = 0, kv = V->present ? vs_pair(V->name, V->idx) : 0;
                if (allow) {
                    int pairable = kv && P->present && ps_pair(P->name, P->idx) == kv;
                    if (s == SEQ_VP || s == SEQ_VP_P || s == SEQ_VV_P) want = pairable;
                    else if (s == SEQ_VP_VLISTED) want = pairable && kv == 1 && vs_listed(VS_STOCK, 0);
                }
                int hl = want && kv == 1 && ((lvi / VSN) & 4) != 0;   /* the half-Lambert bit of the vlg VS it ends with */
                LONG v0 = g_fk_viol;
                int id = id0 + m.cases;
                snap(stp, n, id);
                int ok = g_fk_viol == v0;
                if (want) ok = ok && is(t_snap_vs, ours_vs_name(kv)) && t_snap_vsi == lvi && is(t_snap_ps, ours_ps_name(kv)) &&
                              t_snap_psi == lpi;
                else ok = ok && t_snap_vs == lv && t_snap_vsi == lvi && t_snap_ps == lp && t_snap_psi == lpi;
                ok = ok && st->vlg_snapshot_flags(id) == (want ? pair_mark(kv) | (hl ? 2 : 0) : 0);
                /* deferred VS rename (vlg.c t_vs_pending): a listed VS reaches stock as sent, and ours only follows once its PS pairs, so a
                 * PS that is not paired sees exactly the stock VS then the PS, with no rename and no revert in between */
                int order_ok = 1;
                if (allow && s == SEQ_VP && kv && P->present && !want)
                    order_ok = t_nlog == 2 && !t_log[0].ps && t_log[0].name == V->name && t_log[0].idx == V->idx && t_log[1].ps &&
                               t_log[1].name == P->name;
                if (!ok || !order_ok) {
                    if (m.bad + m.order_bad < 8)
                        printf("  P%s %s | %s | %s: stock holds %s/%d + %s/%d, mark %d, want %s%s\n", tag, k_seq[s], V->what,
                               P->what, t_snap_vs ? t_snap_vs : "NULL", t_snap_vsi, t_snap_ps ? t_snap_ps : "NULL", t_snap_psi,
                               st->vlg_snapshot_flags(id), want ? "both renamed" : "both as sent", order_ok ? "" : " (ORDER WRONG)");
                    m.bad += !ok; m.order_bad += !order_ok;
                }
                m.renamed += want;
                m.renamed_bump += want && kv == 2;
                m.renamed_skin += want && kv == 3;
                m.cases++;
            }
    m.fk_viol = (int)(g_fk_viol - viol0);
    CHECK(st->vlg->pair_violations == pv0, "%s: the proxy's TakeSnapshot net fired %lld times", tag,
          st->vlg->pair_violations - pv0);
    return m;
}

/* stock's r_pixelfog ConVar (stock+0xa2a90): m_nValue (+0x58) is stored xor the low 32 bits of this; returns whether
 * its own GetInt (vtable slot 13, the call stock and the proxy make) now reads x */
static int set_pixelfog(char *cv, int x) {
    *(volatile uint32_t *)(cv + 0x58) = (uint32_t)x ^ (uint32_t)(uintptr_t)cv;
    return ((int (*)(void *))(*(void ***)cv)[13])(cv) == x;
}

/* --- threaded run: one thread snapshots (as materials load on the main thread in queued mode) while another draws */
enum { TH_N = 10000, TH_DRAWS = 20000, TH_BASE = 2000 };
static volatile LONG g_published, g_th_mismatch, g_th_draws, g_th_lit;
static float g_th_nlights;
static DWORD WINAPI th_snap(void *p) {
    (void)p;
    for (int i = 0; i < TH_N; i++) {
        sh_default();
        sh_vs(VS_STOCK, 0);
        sh_ps(PS_STOCK, (i % 3) ? 4 * PSN : g_ps_unlisted * PSN);
        take(TH_BASE + i);
        InterlockedExchange(&g_published, i + 1);
    }
    return 0;
}
static DWORD WINAPI th_draw(void *p) {
    (void)p;
    for (int k = 0; k < TH_DRAWS; k++) {
        LONG n = g_published;
        if (n == 0) { Sleep(0); k--; continue; }
        int i = (int)(((unsigned)k * 7919u) % (unsigned)n);
        beginpass(TH_BASE + i);
        vsidx(2);
        renderpass();
        int pilot = (i % 3) != 0;
        float want = pilot ? 1.0f : 0.0f;
        if (t_rp_c[0][0] != want || (pilot && t_rp_c[0][1] != g_th_nlights)) InterlockedIncrement(&g_th_mismatch);
        InterlockedIncrement(&g_th_draws);
        if (pilot) InterlockedIncrement(&g_th_lit);
    }
    return 0;
}

/* --- photon_ambient 1 under the mock (PHOTON_TEST_AMB): ambvol.c's glue with a fake engine.dll and a real DXVK device --------------------------------------------------------
 * PHOTON_TEST_AMB=on: engine checks, map load through the fake filesystem, the first upload, slabs on all axes (the texture read back and compared voxel by voxel with a
 * bake of the same map), the draw path's constants and residual, every gate, a real device Reset, maps inside a GMA (read through a handle of the proxy's own, an
 * offset that disagrees with the GMA's index, an unmounted GMA), a map change, then main's Shutdown and FreeLibrary.
 * =pattern, =stamp: a byte of the engine or filesystem_stdio's stamp differs: the volume must stay off, with no device write and no thread. */
extern void *g_fake_fs;   /* proxyhost.c: what its factory answers for VFileSystem022 */
static const float k_amb_cube[6][4] = { { 2.0f, 2.1f, 2.2f, 0 }, { 2.3f, 2.4f, 2.5f, 0 }, { 2.6f, 2.7f, 2.8f, 0 }, { 2.9f, 3.0f, 3.1f, 0 }, { 3.2f, 3.3f, 3.4f, 0 }, { 3.5f, 3.6f, 3.7f, 0 } };
static void *g_amb_dev_w;
static void fake_present(void *self) { (void)self; }
static void amb_present(void) { ((void (*)(void *))VT(g_amb_dev_w)[9])(g_amb_dev_w); }
/* the engine's call order for a lit VLG model: origin, cube, 129(0,0,0) */
static void amb_draw(const float o[3], int engine) {
    setorigin(o);
    setcube(k_amb_cube);
    if (engine) setambient(0, 0, 0);
    draw(200, 2, 1);
}
static float amb_c64(void) {
    float v[4];
    g_dev->lpVtbl->GetPixelShaderConstantF(g_dev, PHOTON_LB_DRAW, v, 1);
    return v[1];
}

/* Until c64.y satisfies the test, or the time is up; one Present and one lit draw per loop */
static int amb_wait(const float o[3], float at_least, int ms) {
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < ms) {
        amb_present();
        amb_draw(o, 1);
        if (amb_c64() >= at_least) return 1;
        Sleep(1);
    }
    return 0;
}
/* until the valid box's corner (c61) leaves `from` */
static int amb_wait_moved(const float o[3], const float from[4], int ms) {
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < ms) {
        amb_present();
        amb_draw(o, 1);
        float m[4];
        g_dev->lpVtbl->GetPixelShaderConstantF(g_dev, PHOTON_LB_VOL_MIN, m, 1);
        if (memcmp(m, from, 12) != 0) return 1;
        Sleep(1);
    }
    return 0;
}

/* The texture on s15, read back, against a bake of the window c61 says is valid */
static long amb_verify(const AmbMap *map, const char *what) {
    IDirect3DBaseTexture9 *t = NULL;
    g_dev->lpVtbl->GetTexture(g_dev, 15, &t);
    if (!t) { CHECK(0, "A %s: nothing bound on s15", what); return -1; }
    IDirect3DVolumeTexture9 *v = (IDirect3DVolumeTexture9 *)t;
    float mn[4];
    g_dev->lpVtbl->GetPixelShaderConstantF(g_dev, PHOTON_LB_VOL_MIN, mn, 1);
    AmbBox box;
    for (int a = 0; a < 3; a++) box.org[a] = (int)lround((double)mn[a] / 32.0 - 0.5625);
    box.dim[0] = 64; box.dim[1] = 64; box.dim[2] = 32;
    AmbBakeParams p;
    amb_params_default(&p);
    float *ref = malloc(64 * 64 * 32 * AMB_VOXEL_FLOATS * sizeof(float));
    int rc = ref ? amb_bake_region(map, &p, &box, ref, NULL) : AMB_ERR_NOMEM;
    D3DLOCKED_BOX lb;
    HRESULT hr = v->lpVtbl->LockBox(v, 0, &lb, NULL, D3DLOCK_READONLY);
    long wrong = 0, checked = 0;
    if (rc == AMB_OK && SUCCEEDED(hr)) {
        for (int z = 0; z < 32; z++)
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    int ax = box.org[0] + x, ay = box.org[1] + y, az = box.org[2] + z;
                    int tx = ((ax % 64) + 64) % 64, ty = ((ay % 64) + 64) % 64, tz = ((az % 32) + 32) % 32;
                    const float *o = ref + (((size_t)z * 64 + (size_t)y) * 64 + (size_t)x) * AMB_VOXEL_FLOATS;
                    const uint8_t *row = (const uint8_t *)lb.pBits + (size_t)tz * lb.SlicePitch + (size_t)ty * lb.RowPitch;
                    for (int f = 0; f < 6; f++) {
                        const uint16_t *q = (const uint16_t *)(row + (size_t)(f * 65 + tx) * 8);
                        for (int c = 0; c < 3; c++) wrong += q[c] != amb_f32_to_f16(o[f * 3 + c]);
                        if (tx == 0) {
                            q = (const uint16_t *)(row + (size_t)(f * 65 + 64) * 8);
                            for (int c = 0; c < 3; c++) wrong += q[c] != amb_f32_to_f16(o[f * 3 + c]);
                        }
                        checked += 3;
                    }
                }
        v->lpVtbl->UnlockBox(v, 0);
    } else CHECK(0, "A %s: bake %d, LockBox hr 0x%lx", what, rc, (unsigned long)hr);
    t->lpVtbl->Release(t);
    free(ref);
    printf("  A %s: window %d %d %d: %ld of %ld texel channels wrong\n", what, box.org[0], box.org[1], box.org[2], wrong, checked);
    CHECK(wrong == 0, "A %s: %ld wrong texel channels", what, wrong);
    return wrong;
}

/* The block a volume draw sent: c33..c38 = lerp(C, max(0, C - 1.5 A), f), A by the engine's rule at the origin (or the given A, low-passed) */
static void amb_check_block_a(const AmbMap *map, const float o[3], const float *a_given, const char *what) {
    float f = amb_c64(), a[AMB_VOXEL_FLOATS], want[6][4];
    double p[3] = { o[0], o[1], o[2] };
    if (a_given) memcpy(a, a_given, sizeof a);
    else amb_eval_engine(map, p, 1.0, a);
    amb_clip_fold(k_amb_cube, a, f, want);
    int bad = cmp_block(&t_rp_c[1][0], &want[0][0], 6, what);
    float d = 0;
    for (int i = 0; i < 18; i++) d = fabsf(a[i]) > d ? fabsf(a[i]) : d;
    printf("  A %s: f %.3f, largest A %.3f, block %s\n", what, f, d, bad ? "WRONG" : "as computed here");
    CHECK(!bad && d > 0 && memcmp(&t_rp_c[1][0], &k_amb_cube[0][0], sizeof k_amb_cube) != 0, "A %s block", what);
}
static void amb_check_block(const AmbMap *map, const float o[3], const char *what) { amb_check_block_a(map, o, NULL, what); }

/* A low-passed per model as the engine low-passes C (amb_track.h): a model moving 0.1 s of client time from o to a nearby point whose leaf ambient differs */
static void amb_check_smoothing(FakeEngineCtl *ctl, const AmbMap *map, const float o[3]) {
    double p1[3] = { o[0], o[1], o[2] }, p2[3];
    float a1[AMB_VOXEL_FLOATS], a2[AMB_VOXEL_FLOATS], o2[3] = { o[0], o[1], o[2] };
    amb_eval_engine(map, p1, 1.0, a1);
    int found = 0;
    /* within the tracker's reach after 0.1 s (64 u + 400 u) */
    for (int dx = 8; dx <= 400 && !found; dx += 8)
        for (int ax = 0; ax < 2 && !found; ax++)
            for (int sgn = -1; sgn <= 1 && !found; sgn += 2) {
                p2[0] = o[0]; p2[1] = o[1]; p2[2] = o[2];
                p2[ax] += sgn * dx;
                amb_eval_engine(map, p2, 1.0, a2);
                float d = 0;
                for (int i = 0; i < AMB_VOXEL_FLOATS; i++) d = fmaxf(d, fabsf(a2[i] - a1[i]) / fmaxf(fmaxf(a1[i], a2[i]), 1e-6f));
                if (d > 0.1f) { found = 1; o2[0] = (float)p2[0]; o2[1] = (float)p2[1]; }
            }
    printf("  A smoothing: from (%.0f %.0f %.0f) to (%.0f %.0f %.0f)\n", o[0], o[1], o[2], o2[0], o2[1], o2[2]);
    CHECK(found, "A smoothing: no point within 400 u of the origin with another leaf ambient");
    if (!found) return;
    double t = 200.0;
    ctl->set_time(t);
    amb_present(); amb_draw(o, 1);                  /* the model at o: its track's value is A(o) */
    amb_present(); amb_draw(o, 1);
    ctl->set_time(t + 0.1);
    amb_present(); amb_draw(o2, 1);                 /* 0.1 s later at o2: one averaging step */
    float k = expf(-0.1f * 5.0f), want[AMB_VOXEL_FLOATS];
    for (int i = 0; i < AMB_VOXEL_FLOATS; i++) { float v = a2[i] - (a2[i] - a1[i]) * k; want[i] = v < 0 ? 0 : v > 8 ? 8 : v; }
    amb_check_block_a(map, o2, want, "smoothing, one step of 0.1 s at rate 5");
    /* r_lightaverage 0: the current value, as the engine's cube */
    ctl->set_lightavg(5.0f, 0);
    ctl->set_time(t + 0.2);
    amb_present(); amb_draw(o, 1);
    amb_check_block(map, o, "r_lightaverage 0: no averaging");
    ctl->set_lightavg(5.0f, 1);
    ctl->set_time(t + 100.0);                       /* long after: the average has forgotten (the engine's k is about 0) */
    amb_present(); amb_draw(o, 1);
    amb_check_block(map, o, "after 100 s");
}

static void amb_tests(PhotonStats *st, HMODULE hs, HMODULE hp, void *api_r, const char *how, D3DPRESENT_PARAMETERS *pp) {
    const PhotonVlgStats *v = st->vlg;
    char epath[MAX_PATH] = "";
    GetEnvironmentVariableA("PHOTON_TEST_AMB_ENGINE", epath, sizeof epath);
    HMODULE ms = LoadLibraryA("materialsystem.dll");
    HMODULE en = LoadLibraryA(epath);
    if (!ms || !en) { CHECK(0, "A could not load materialsystem.dll (%p) or the fake engine %s (%p): %lu", (void *)ms, epath, (void *)en, GetLastError()); return; }
    CHECK(GetModuleHandleA("engine.dll") == en, "A engine.dll is not the fake");
    int break_what = strcmp(how, "pattern") == 0 ? 1 : strcmp(how, "stamp") == 0 ? 2 : strcmp(how, "avgpattern") == 0 ? 3 : 0;
    FakeEngineCtl *ctl = ((FakeEngineControlFn)(void *)GetProcAddress(en, "FakeEngineControl"))(break_what);
    if (!ctl) { CHECK(0, "A the fake engine did not set up"); return; }
    g_fake_fs = ctl->fs;
    size_t len = 0, len_b = 0;
    uint8_t *bsp = synth_bsp_variant(0, 0, 0, &len), *bsp_b = synth_bsp_variant(1, 0, 37, &len_b);
    ctl->add_file("maps/a.bsp", bsp, len, 0);
    ctl->add_file("maps/b.bsp", bsp_b, len_b, 0);
    int ga = ctl->add_gma("maps/gma.bsp", bsp_b, len_b, bsp, len, 0), gl = ctl->add_gma("maps/gmalie.bsp", bsp_b, len_b, bsp, len, 1),
        gg = ctl->add_gma("maps/gmagone.bsp", bsp_b, len_b, bsp, len, 2);
    CHECK(!ga && !gl && !gg, "A the fake GMAs: %d %d %d", ga, gl, gg);
    AmbMap *map = NULL, *map_b = NULL;
    amb_map_load(bsp, len, 0, &map);
    amb_map_load(bsp_b, len_b, 0, &map_b);
    CHECK(map && map_b, "A the reference maps");

    /* the frame hook without stock's Present, which has no real swap chain behind it here */
    void *(*ci)(const char *, int *) = (void *(*)(const char *, int *))(void *)GetProcAddress(hp, "CreateInterface");
    g_amb_dev_w = ci("ShaderDevice001", NULL);
    PhotonWrapClass *cd = NULL;
    for (size_t i = 0; i < st->nclasses; i++) if (strcmp(st->classes[i]->name, "ShaderDevice001") == 0) cd = st->classes[i];
    void **saved_dev_vt = cd->real_vtbl;
    void **dev_copy = malloc(cd->nslots * sizeof(void *));
    memcpy(dev_copy, saved_dev_vt, cd->nslots * sizeof(void *));
    dev_copy[9] = (void *)fake_present;
    cd->real_vtbl = dev_copy;

    /* stock's s15 cache at rest, as in game after the first draws */
    char *r = (char *)api_r;
    char saved_en = r[0x1874];
    uint64_t saved_h = *(uint64_t *)(r + 0x1850);
    r[0x1874] = 1;
    *(uint64_t *)(r + 0x1850) = *(uint64_t *)(r + 0x158) = 0x1234;

    snap2(VS_STOCK, (int)g_vids.id[0] * VSN, PS_STOCK, (int)g_pids.id[0] * PSN, 200);
    CHECK(st->vlg_snapshot_flags(200) & 1, "A the draw's snapshot is ours");

    float o[3] = { -64, 1200, 0 };   /* 2 voxels off the first window's centre on X and Y: 8 voxels more is past the hysteresis */
    ctl->set_origin(o[0], o[1], o[2]);
    IDirect3DBaseTexture9 *t = NULL;

    if (break_what == 3) {
        /* the averaging reads differ: the volume stays on, with the default rate and the process clock */
        ctl->set_map(6, "maps/a.bsp");
        CHECK(amb_wait(o, 1.0f, 30000), "A %s differs: the volume did not come up (c64.y %g)", how, amb_c64());
        ctl->set_origin(600, 1800, 0);
        amb_present();
    } else if (break_what) {
        LONG above0 = g_rec_above_block;
        ctl->set_map(6, "maps/a.bsp");
        for (int i = 0; i < 30; i++) { amb_present(); amb_draw(o, 1); Sleep(2); }
        g_dev->lpVtbl->GetTexture(g_dev, 15, &t);
        printf("  A %s differs: opens %ld, draws with weight %lld, writes above c56 %ld, s15 %p\n", how, (long)ctl->opens, (long long)v->vol_draws, (long)(g_rec_above_block - above0), (void *)t);
        CHECK(!ctl->opens && !v->vol_draws && g_rec_above_block == above0 && !t, "A %s differs: the volume stayed on", how);
        if (t) t->lpVtbl->Release(t);
    } else {
        /* not in game: nothing loads */
        ctl->set_map(0, "");
        for (int i = 0; i < 10; i++) { amb_present(); amb_draw(o, 1); }
        CHECK(!ctl->opens && amb_c64() == 0.0f, "A before the map: %ld files, c64.y %g", (long)ctl->opens, amb_c64());
        g_dev->lpVtbl->GetTexture(g_dev, 15, &t);
        CHECK(!t, "A before the map: a texture on s15");
        if (t) { t->lpVtbl->Release(t); t = NULL; }

        /* the map: load, whole window, 8 upload parts, the weight coming up */
        ctl->set_map(6, "maps/a.bsp");
        int up = amb_wait(o, 1.0f, 30000);
        CHECK(up, "A the volume did not come up: c64.y %g, reason counts range %lld rest %lld gate %lld", amb_c64(), (long long)v->vol_off[3], (long long)v->vol_off[4], (long long)v->vol_off[2]);
        CHECK(ctl->opens == 1 && ctl->closes == 1 && ctl->largest_read > 0 && ctl->largest_read <= 1 << 20, "A the file: %ld opens, %ld closes, largest read %d", (long)ctl->opens, (long)ctl->closes, ctl->largest_read);
        AmbClip clip;
        amb_clip_init(&clip);
        float wa[4], wb[4], got[6][4];
        amb_clip_constants(&clip, wa, wb);
        g_dev->lpVtbl->GetPixelShaderConstantF(g_dev, PHOTON_LB_VOL_A, &got[0][0], 4);
        CHECK(memcmp(got[0], wa, 16) == 0 && memcmp(got[1], wb, 16) == 0, "A c59, c60");
        CHECK(got[2][0] < o[0] && got[3][0] > o[0] && got[2][1] < o[1] && got[3][1] > o[1] && got[2][2] < o[2] && got[3][2] > o[2], "A c61, c62 hold the origin");
        amb_verify(map, "first upload");
        amb_check_block(map, o, "volume draw");
        amb_check_smoothing(ctl, map, o);
        /* the static-prop array signature (cube, then origin, after a latched engine cube): the cube, counted */
        {
            LONG64 arr0 = v->vol_off_array;
            setcube(k_amb_cube); setambient(0, 0, 0);
            setcube(k_amb_cube); setorigin(o); draw(200, 2, 1);
            CHECK(amb_c64() == 0.0f && memcmp(&t_rp_c[1][0], &k_amb_cube[0][0], sizeof k_amb_cube) == 0 && v->vol_off_array == arr0 + 1,
                  "A a static-prop array draw: c64.y %g, array count %lld", amb_c64(), (long long)(v->vol_off_array - arr0));
            amb_draw(o, 1);
            CHECK(amb_c64() > 0.0f, "A the engine draw after an array draw: c64.y %g", amb_c64());
        }

        /* slabs on every axis, and X far enough to reach the slab with the gutter */
        static const int axes[10] = { 0, 1, 2, 0, 0, 0, 0, 0, 0, 0 };
        for (int s = 0; s < 10; s++) {
            float from[4];
            g_dev->lpVtbl->GetPixelShaderConstantF(g_dev, PHOTON_LB_VOL_MIN, from, 1);
            o[axes[s]] += 256.0f;
            ctl->set_origin(o[0], o[1], o[2]);
            char what[48];
            snprintf(what, sizeof what, "slab %d on axis %d", s, axes[s]);
            CHECK(amb_wait_moved(o, from, 15000), "A %s: the window did not move", what);
            amb_verify(map, what);
        }
        amb_wait(o, 1.0f, 3000);

        /* the gates, one per frame */
        char *cvb = (char *)ms + 0x20f3c50, *cvs = (char *)hs + 0xa22e0;
        uint32_t saved_b = *(volatile uint32_t *)(cvb + 0x58), saved_s = *(volatile uint32_t *)(cvs + 0x58);
        *(volatile uint32_t *)(cvb + 0x58) = 1u ^ (uint32_t)(uintptr_t)cvb;
        amb_present(); amb_draw(o, 1);
        CHECK(amb_c64() == 0.0f, "A mat_fullbright: c64.y %g", amb_c64());
        *(volatile uint32_t *)(cvb + 0x58) = saved_b;
        CHECK(amb_wait(o, 0.5f, 2000), "A back from mat_fullbright");
        *(volatile uint32_t *)(cvs + 0x58) = 1u ^ (uint32_t)(uintptr_t)cvs;
        amb_present(); amb_draw(o, 1);
        CHECK(amb_c64() == 0.0f, "A r_shader_srgb: c64.y %g", amb_c64());
        *(volatile uint32_t *)(cvs + 0x58) = saved_s;
        CHECK(amb_wait(o, 0.5f, 2000), "A back from r_shader_srgb");
        amb_draw(o, 0);
        CHECK(amb_c64() == 0.0f && memcmp(&t_rp_c[1][0], &k_amb_cube[0][0], sizeof k_amb_cube) == 0, "A an override draw: c64.y %g, or the block differs from its cube", amb_c64());
        r[0x1874] = 0;
        amb_draw(o, 1);
        CHECK(amb_c64() == 0.0f && memcmp(&t_rp_c[1][0], &k_amb_cube[0][0], sizeof k_amb_cube) == 0, "A s15 not at rest: c64.y %g", amb_c64());
        r[0x1874] = 1;
        CHECK(amb_wait(o, 0.5f, 2000), "A back at rest");
        long long off_gate = v->vol_off[2], off_route = v->vol_off[1], off_rest = v->vol_off[4];

        /* a real Reset: the non-Ex device refuses while a DEFAULT resource lives. Then everything is back with no help. */
        void *util = *(void **)((char *)hs + 0xa6ad8);
        ((void (*)(void *))VT(util)[15])(util);
        g_dev->lpVtbl->GetTexture(g_dev, 15, &t);
        CHECK(!t, "A after the release: a texture on s15");
        if (t) { t->lpVtbl->Release(t); t = NULL; }
        HRESULT hr = g_dev->lpVtbl->Reset(g_dev, pp);
        printf("  A device Reset -> hr 0x%lx\n", (unsigned long)hr);
        CHECK(SUCCEEDED(hr), "A device Reset failed: hr 0x%lx", (unsigned long)hr);
        ((void (*)(void *, void *, int))VT(util)[16])(util, (char *)hs + 0x23f80, 0);
        CHECK(amb_wait(o, 1.0f, 5000), "A not back after the Reset: c64.y %g", amb_c64());
        amb_verify(map, "after the Reset");
        amb_check_block(map, o, "volume draw after the Reset");

        /* a map inside a GMA (map b's bytes): read through a handle of the proxy's own on the .gma, never through the shared one; map b's voxels */
        LONG opens0 = ctl->opens, closes0 = ctl->closes;
        ctl->set_map(6, "maps/gma.bsp");
        CHECK(amb_wait(o, 1.0f, 30000), "A the map inside a GMA did not come up: c64.y %g", amb_c64());
        amb_verify(map_b, "a map inside a GMA");
        amb_check_block(map_b, o, "volume draw on a map inside a GMA");
        printf("  A a map inside a GMA: %ld opened, %ld closed, %ld calls on the shared handle, c64.y %g\n", (long)(ctl->opens - opens0), (long)(ctl->closes - closes0),
               (long)ctl->pack_used, amb_c64());
        CHECK(ctl->opens - opens0 == 1 && ctl->closes - closes0 == 1 && !ctl->pack_used, "A the map inside a GMA went through the filesystem");
        /* the engine's offset is another map's (map a's, listed under another name): refused by the GMA's index, the cube; then an unmounted GMA, the cube */
        static const char *const refused[2] = { "maps/gmalie.bsp", "maps/gmagone.bsp" };
        for (int k = 0; k < 2; k++) {
            closes0 = ctl->closes;
            ctl->set_map(6, refused[k]);
            DWORD t0 = GetTickCount();
            while (ctl->closes == closes0 && GetTickCount() - t0 < 5000) { amb_present(); amb_draw(o, 1); Sleep(1); }
            for (int i = 0; i < 200; i++) { amb_present(); amb_draw(o, 1); Sleep(1); }
            printf("  A %s: closed %ld, %ld calls on the shared handle, c64.y %g\n", refused[k], (long)(ctl->closes - closes0), (long)ctl->pack_used, amb_c64());
            CHECK(ctl->closes - closes0 == 1 && !ctl->pack_used && amb_c64() == 0.0f, "A %s was used", refused[k]);
        }

        /* a map change: the other map's voxels */
        o[0] = -64; o[1] = 1200; o[2] = 0;
        ctl->set_origin(o[0], o[1], o[2]);
        ctl->set_map(6, "maps/b.bsp");
        CHECK(amb_wait(o, 1.0f, 30000), "A map b did not come up: c64.y %g", amb_c64());
        amb_verify(map_b, "after a map change");
        amb_check_block(map_b, o, "volume draw on map b");
        printf("  A counters: draws with weight %lld; weight 0: route %lld, gate %lld, range %lld, rest %lld; c59..c62 writes %lld, c64 writes %lld; first gates saw gate %lld route %lld rest %lld\n",
               (long long)v->vol_draws, (long long)v->vol_off[1], (long long)v->vol_off[2], (long long)v->vol_off[3], (long long)v->vol_off[4], (long long)v->vol_const_writes,
               (long long)v->vol_weight_writes, off_gate, off_route, off_rest);
        CHECK(off_gate >= 2 && off_route >= 1 && off_rest >= 1 && v->vol_draws > 0, "A the gate counters");
        /* leave a window baking for main's Shutdown: the map's other end */
        ctl->set_origin(600, 1800, 0);
        amb_present();
    }
    r[0x1874] = saved_en;
    *(uint64_t *)(r + 0x1850) = saved_h;
    cd->real_vtbl = saved_dev_vt;
    g_fake_fs = NULL;
    amb_map_free(map);
    amb_map_free(map_b);
    /* bsp and bsp_b stay: a worker may still be reading until Shutdown */
}

int vlg_tests(PhotonStats *st, HMODULE hs, HMODULE hp, void *api_w, void *api_r, void *sh_w, void *sh_r, void *hw_r) {
    char mode[16] = "", gdir[MAX_PATH] = "", vidsp[MAX_PATH] = "", pidsp[MAX_PATH] = "", failk[8] = "ps";
    char bvidsp[MAX_PATH] = "", bpidsp[MAX_PATH] = "", svidsp[MAX_PATH] = "", spidsp[MAX_PATH] = "", pairs[32] = "vlg,bump,skin";
    GetEnvironmentVariableA("PHOTON_TEST_SKIN_VS_IDS", svidsp, sizeof svidsp);
    GetEnvironmentVariableA("PHOTON_TEST_SKIN_PS_IDS", spidsp, sizeof spidsp);
    GetEnvironmentVariableA("PHOTON_TEST_BUMP_VS_IDS", bvidsp, sizeof bvidsp);
    GetEnvironmentVariableA("PHOTON_TEST_BUMP_PS_IDS", bpidsp, sizeof bpidsp);
    GetEnvironmentVariableA("PHOTON_TEST_VLG_PAIRS", pairs, sizeof pairs);
    GetEnvironmentVariableA("PHOTON_TEST_VLG", mode, sizeof mode);
    GetEnvironmentVariableA("PHOTON_GAME_DIR", gdir, sizeof gdir);
    GetEnvironmentVariableA("PHOTON_TEST_VLG_VS_IDS", vidsp, sizeof vidsp);
    GetEnvironmentVariableA("PHOTON_TEST_VLG_PS_IDS", pidsp, sizeof pidsp);
    GetEnvironmentVariableA("PHOTON_TEST_VLG_FAIL", failk, sizeof failk);
    int on = strcmp(mode, "on") == 0, fail_vs = strcmp(failk, "vs") == 0;
    const PhotonVlgStats *v = st->vlg;
    printf("vlg: mode %s; stats: wanted %d active %ld shadow_hooked %d api_hooked %d VS statics %d PS statics %d reason"
           " \"%s\"\n", mode, v->wanted, (long)v->active, v->shadow_hooked, v->api_hooked, v->nvs, v->nps, v->reason);
    CHECK(v->version == 11, "stats version %d", v->version);
    CHECK(v->wanted == on && v->active == on && v->shadow_hooked == on && v->api_hooked == on,
          "enable state does not match mode %s", mode);
    g_sh = sh_w; g_api = api_w;
    read_ids(vidsp, &g_vids);
    read_ids(pidsp, &g_pids);
    while (in_ids(&g_pids, (uint32_t)g_ps_unlisted)) g_ps_unlisted++;
    read_ids(bvidsp, &g_bvids);
    read_ids(bpidsp, &g_bpids);
    read_ids(svidsp, &g_svids);
    read_ids(spidsp, &g_spids);
    g_bump_on = on && strstr(pairs, "bump") != NULL;
    g_skin_on = on && strstr(pairs, "skin") != NULL;
    printf("vlg: pairs expected in use: vlg%s; bump ids: VS %d (%s), PS %d (%s); proxy: bump VS statics %d, PS statics %d\n",
           g_bump_on ? ", bump" : "", g_bvids.n, bvidsp, g_bpids.n, bpidsp, v->nbvs, v->nbps);
    CHECK(g_bump_on ? v->nbvs == g_bvids.n && v->nbps == g_bpids.n && g_bvids.n > 0 && g_bpids.n > 0
                    : v->nbvs == 0 && v->nbps == 0, "bump pair use differs from PHOTON_TEST_VLG_PAIRS \"%s\"", pairs);
    printf("vlg: skin %s; skin ids: VS %d (%s), PS %d (%s); proxy: skin VS statics %d, PS statics %d\n",
           g_skin_on ? "expected in use" : "not expected", g_svids.n, svidsp, g_spids.n, spidsp, v->nsvs, v->nsps);
    CHECK(g_skin_on ? v->nsvs == g_svids.n && v->nsps == g_spids.n && g_svids.n > 0 && g_spids.n > 0
                    : v->nsvs == 0 && v->nsps == 0, "skin pair use differs from PHOTON_TEST_VLG_PAIRS \"%s\"", pairs);
    kinds();

    /* hook ownership: all 14 slots hooked iff on */
    static const int api_slots[] = { 40, 109, 124, 125, 127, 128, 129, 130, 170, 207, 260 }, sh_slots[] = { 0, 23, 24 };
    /* PHOTON_DIAG=1 hooks slot 128 itself; in off mode that slot is then left out of the count */
    char de[8];
    int diag = GetEnvironmentVariableA("PHOTON_DIAG", de, sizeof de) > 0 && de[0] == '1', owned = 0, counted = 0;
    for (size_t i = 0; i < sizeof api_slots / sizeof api_slots[0]; i++)
        if (on || !diag || api_slots[i] != 128) { owned += owned_by(hp, VT(api_w)[api_slots[i]]); counted++; }
    for (size_t i = 0; i < sizeof sh_slots / sizeof sh_slots[0]; i++) { owned += owned_by(hp, VT(sh_w)[sh_slots[i]]); counted++; }
    printf("vlg: %d of %d slots hooked by the proxy\n", owned, counted);
    CHECK(owned == (on ? counted : 0), "%d of %d slots hooked in mode %s", owned, counted, mode);

    /* real device, planted as stock's g_pD3DDevice */
    HMODULE d9 = GetModuleHandleA("d3d9.dll");
    IDirect3D9 *(WINAPI *create)(UINT) = d9 ? (IDirect3D9 *(WINAPI *)(UINT))(void *)GetProcAddress(d9, "Direct3DCreate9") : NULL;
    IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    HWND hwnd = CreateWindowExA(0, "STATIC", "vlgtest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    D3DPRESENT_PARAMETERS pp = { .BackBufferWidth = 64, .BackBufferHeight = 64, .BackBufferFormat = D3DFMT_X8R8G8B8,
                                 .BackBufferCount = 1, .SwapEffect = D3DSWAPEFFECT_DISCARD, .hDeviceWindow = hwnd,
                                 .Windowed = TRUE };
    HRESULT hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g_dev)
                     : E_FAIL;
    if (FAILED(hr) || !g_dev) { printf("FAIL vlg: no D3D9 device (hr 0x%lx)\n", (unsigned long)hr); return ++g_fails; }
    void **gdev = (void **)((char *)hs + 0xa6b18);
    *gdev = g_dev;
    void *val = st->d3d_device();
    printf("vlg: D3D9 device %p planted; proxy d3d_device() -> %p\n", (void *)g_dev, val);
    CHECK(val == g_dev, "the proxy did not validate the real device");
    void **dvt = VT(g_dev);
    DWORD old;
    VirtualProtect(&dvt[109], sizeof(void *), PAGE_READWRITE, &old);
    g_orig_setpsf = dvt[109];
    dvt[109] = (void *)rec_setpsf;
    VirtualProtect(&dvt[109], sizeof(void *), old, &old);
    static const float zero24[NB][4];
    g_dev->lpVtbl->SetPixelShaderConstantF(g_dev, 32, &zero24[0][0], NB);   /* known start state (recorded, reset below) */

    /* fakes */
    PhotonWrapClass *ca = NULL, *cs = NULL;
    for (size_t i = 0; i < st->nclasses; i++) {
        if (strcmp(st->classes[i]->name, "ShaderApi030") == 0) ca = st->classes[i];
        if (strcmp(st->classes[i]->name, "ShaderShadow010") == 0) cs = st->classes[i];
    }
    void **sva, **svs;
    void **va = swap_vtbl(api_r, ca->nslots, &sva), **vs = swap_vtbl(sh_r, cs->nslots, &svs);
    vs[0] = (void *)fake_sh_default; vs[23] = (void *)fake_sh_vs; vs[24] = (void *)fake_sh_ps;
    int real_vsidx = *(void **)((char *)hs + 0x9c440) != NULL;   /* stock SetVertexShaderIndex writes g_pShaderManager+8 */
    if (!real_vsidx) va[40] = (void *)fake_vsidx;
    printf("vlg: fakes in place; SetVertexShaderIndex is %s\n", real_vsidx ? "the real stock function" : "a fake (no shader manager)");
    va[109] = (void *)fake_take; va[125] = (void *)fake_rp; va[170] = (void *)fake_170; va[260] = (void *)fake_260;
    /* caps: MaxNumLights = 4 (stock SetLight/GetMaxLights read [stock+0x9b008]+0x54c; 0 in this host), dxlevel 95 */
    char *caps = *(char **)((char *)hs + 0x9b008);
    int saved_max = *(int *)(caps + 0x54c), saved_dx = *(int *)(caps + 0x4f8);
    *(int *)(caps + 0x54c) = 4;
    *(int *)(caps + 0x4f8) = 95;
    /* stock's r_pixelfog ConVar: drive m_nValue (+0x58, stored xor the low 32 bits of this) and read it back through
     * its own GetInt (vtable slot 13), the call stock and the proxy make */
    char *cv = (char *)hs + 0xa2a90;
    int (*cv_getint)(void *) = (int (*)(void *))(*(void ***)cv)[13];
    uint32_t saved_cv = *(volatile uint32_t *)(cv + 0x58);
    char *fffog = (char *)api_r + 0x132d;
    char saved_fog = *fffog;
    *fffog = 0;
    printf("vlg: caps object %p (HWConfig012 real %p), MaxNumLights %d -> 4, dxlevel %d -> 95; r_pixelfog GetInt %d;"
           " ids: VS %d (%s), PS %d (%s)\n", (void *)caps, hw_r, saved_max, saved_dx, cv_getint(cv), g_vids.n, vidsp,
           g_pids.n, pidsp);
    rec_reset();
    LONG rec0 = g_rec_total;
    char ambm[16] = "";
    int amb = on && GetEnvironmentVariableA("PHOTON_TEST_AMB", ambm, sizeof ambm) > 0;
    if (amb) {
        amb_tests(st, hs, hp, api_r, ambm, &pp);
        goto after_cases;
    }

    static const float cube[6][4] = { { 0.1f, 0.2f, 0.3f, 0 }, { 0.4f, 0.5f, 0.6f, 0 }, { 0.7f, 0.8f, 0.9f, 0 },
                                      { 1.0f, 1.1f, 1.2f, 0 }, { 1.3f, 1.4f, 1.5f, 0 }, { 1.6f, 1.7f, 1.8f, 0 } };
    const TLightDesc lp = { .type = 1, .color = { 2.0f, 1.5f, 1.0f }, .pos = { 10, -20, 30 }, .dir = { 0, 0, -1 },
                            .range = 500, .falloff = 1, .att0 = 0, .att1 = 0, .att2 = 1, .theta = 0.5f, .phi = 1.0f };
    const TLightDesc ls = { .type = 3, .color = { 0.5f, 0.25f, 4.0f }, .pos = { -5, 7, 100 }, .dir = { 0.6f, 0, -0.8f },
                            .range = 300, .falloff = 2, .att0 = 1, .att1 = 0.01f, .att2 = 0.001f, .theta = 0.6f, .phi = 1.2f };
    const TLightDesc ld = { .type = 2, .color = { 0.9f, 0.8f, 0.7f }, .pos = { 0, 0, 2e6f }, .dir = { 0.267261f, 0.534522f, -0.801784f },
                            .range = 0, .falloff = 0, .att0 = 1, .att1 = 0, .att2 = 0, .theta = 0, .phi = 0 };
    const TLightDesc lx = { .type = 0 };
    /* clamps: theta > phi - 0.001 and phi > pi */
    const TLightDesc lc = { .type = 3, .color = { 1, 2, 3 }, .pos = { 1, 2, 3 }, .dir = { 0, 1, 0 }, .range = 50,
                            .falloff = 5, .att0 = 0.5f, .att1 = 0.25f, .att2 = 0.125f, .theta = 4.5f, .phi = 4.0f };
    const float origin[3] = { 3, 4, 5 };

    if (!on) {
        /* OFF: stock receives the caller's own pointers for the whole matrix, and nothing reaches the device */
        MatrixResult m = matrix(st, 0, NSEQ, 20000, "off");
        setcube(cube); setlight(0, &lp); setorigin(origin);
        int w = 0;
        for (int i = 0; i < m.cases; i++) w += draw(20000 + i, 2, 1);
        printf("vlg off: matrix %d cases (%d VS kinds x %d PS kinds x %d sequences): %d wrong, fake-stock pairing violations"
               " %d; %d draws -> %d device writes; VS renames %lld PS renames %lld uploads %lld\n", m.cases, g_nvk, g_npk,
               NSEQ, m.bad, m.fk_viol, m.cases, w, (long long)v->vs_renames, (long long)v->ps_renames, (long long)v->uploads);
        CHECK(m.bad == 0 && m.fk_viol == 0, "off: matrix");
        CHECK(w == 0 && g_rec_total == rec0, "off: %d device writes", (int)(g_rec_total - rec0));
        CHECK(v->vs_renames == 0 && v->ps_renames == 0 && v->uploads == 0 && v->rp_total == 0, "off: counters moved");
        CHECK(t_rp_calls == m.cases, "off: RenderPass did not reach stock");
    } else {
        /* --- G: the fog gate first, while the device has never been enabled --- */
        int pf_ok = set_pixelfog(cv, 0);
        long long vr0 = v->vs_renames, pr0 = v->ps_renames, fb0 = v->fog_blocks, mk0 = v->snapshots_marked;
        MatrixResult g1 = matrix(st, 0, 2, 30000, "G-pixelfog0");
        int w = 0;
        for (int i = 0; i < g1.cases; i++) w += draw(30000 + i, 2, 1);
        long long fb1 = v->fog_blocks;
        set_pixelfog(cv, 1);
        *(int *)(caps + 0x4f8) = 85;
        MatrixResult g2 = matrix(st, 0, 2, 31000, "G-dx85");
        for (int i = 0; i < g2.cases; i++) w += draw(31000 + i, 2, 1);
        *(int *)(caps + 0x4f8) = 95;
        int pf1 = cv_getint(cv);
        printf("  G r_pixelfog 0 (set ok %d): %d cases, %d wrong; dxlevel 85: %d cases, %d wrong; fog-gate blocks +%lld/+%lld;"
               " renames VS +%lld PS +%lld, marks +%lld; %d draws -> %d device writes; r_pixelfog back to %d\n", pf_ok,
               g1.cases, g1.bad, g2.cases, g2.bad, fb1 - fb0, v->fog_blocks - fb1, v->vs_renames - vr0,
               v->ps_renames - pr0, v->snapshots_marked - mk0, g1.cases + g2.cases, w, pf1);
        CHECK(pf_ok && pf1 == 1 && g1.bad == 0 && g2.bad == 0 && g1.fk_viol == 0 && g2.fk_viol == 0 && fb1 > fb0 &&
              v->fog_blocks > fb1 && v->vs_renames == vr0 && v->ps_renames == pr0 && v->snapshots_marked == mk0 && w == 0 &&
              g_rec_total == rec0, "G fog gate");

        /* --- R: renames --- */
        int rb_vs = g_bvids.n ? (int)g_bvids.id[0] : 2, rb_ps = g_bpids.n ? (int)g_bpids.id[0] : 3;
        int rs_ps = g_spids.n ? (int)g_spids.id[0] : 0;
        struct { const char *vs; int vsi; const char *ps; int psi; int id; int flags; const char *what; } R[] = {
            { VS_STOCK, 0, PS_STOCK, 4 * PSN, 100, 1, "pilot PS 4, VS 0" },
            { VS_STOCK, 0, PS_STOCK, g_ps_unlisted * PSN, 101, 0, "unlisted PS" },
            { VS_STOCK, 1 * VSN, PS_STOCK, 4 * PSN, 102, 0, "pilot, VS VERTEXCOLOR=1" },
            { VS_STOCK, 4 * VSN, PS_STOCK, 4 * PSN, 103, 3, "pilot, VS HALFLAMBERT=1" },
            { VS_STOCK, 0, PS_STOCK, 4 * PSN + 1, 104, 0, "PS index not static*12" },
            { VS_STOCK, 0, PS_DECAL, 0, 105, 0, "DecalModulate PS" },
            { VS30, 0, PS_STOCK, 4 * PSN, 106, 0, "other VS name" },
            { NULL, 0, PS_STOCK, 4 * PSN, 107, 0, "no VS since SetDefaultState" },
            { VS_STOCK, 4 * VSN + 3, PS_STOCK, 4 * PSN, 108, 0, "VS index not static*384" },
            { BVS_STOCK, rb_vs * BVSN, BPS_STOCK, rb_ps * BPSN, 109, g_bump_on ? 5 : 0, "bump pair (listed VS + PS)" },
            { VS_STOCK, 0, BPS_STOCK, rb_ps * BPSN, 110, 0, "vlg VS + bump pilot PS" },
            { BVS_STOCK, rb_vs * BVSN, PS_STOCK, 4 * PSN, 111, 0, "bump VS + vlg pilot PS" },
            { SVS_STOCK, 0, SPS_STOCK, rs_ps * SPSN, 120, g_skin_on ? 9 : 0, "skin pair (VS 0 + listed PS)" },
            { SVS_STOCK, SVSN, SPS_STOCK, rs_ps * SPSN, 121, g_skin_on ? 9 : 0, "skin pair (VS 1 + listed PS)" },
            { VS_STOCK, 0, SPS_STOCK, rs_ps * SPSN, 122, 0, "vlg VS + skin pilot PS" },
            { SVS_STOCK, 0, PS_STOCK, 4 * PSN, 123, 0, "skin VS + vlg pilot PS" },
            { SVS_STOCK, 0, BPS_STOCK, rb_ps * BPSN, 124, 0, "skin VS + bump pilot PS" },
            { BVS_STOCK, rb_vs * BVSN, SPS_STOCK, rs_ps * SPSN, 125, 0, "bump VS + skin pilot PS" },
            { SVS_STOCK, 0, SPS_STOCK, rs_ps * SPSN + 1, 126, 0, "skin PS index not static*80" },
            { SVS_STOCK, 1, SPS_STOCK, rs_ps * SPSN, 127, 0, "skin VS index not static*48" },
        };
        int rbad = 0;
        for (size_t k = 0; k < sizeof R / sizeof R[0]; k++) {
            if (R[k].vs) snap2(R[k].vs, R[k].vsi, R[k].ps, R[k].psi, R[k].id);
            else { Step s1 = { 1, R[k].ps, R[k].psi }; snap(&s1, 1, R[k].id); }
            int ours = R[k].flags != 0, bk = (R[k].flags & 4) ? 2 : (R[k].flags & 8) ? 3 : 1;
            int ok = ours ? is(t_snap_vs, ours_vs_name(bk)) && is(t_snap_ps, ours_ps_name(bk))
                          : t_snap_vs == R[k].vs && t_snap_ps == R[k].ps;
            ok = ok && t_snap_psi == R[k].psi && t_snap_vsi == (R[k].vs ? R[k].vsi : 0) && st->vlg_snapshot_flags(R[k].id) == R[k].flags;
            printf("  R%zu %-30s -> stock holds \"%s\" + \"%s\"%s, mark %d %s\n", k + 1, R[k].what, t_snap_vs ? t_snap_vs : "NULL",
                   t_snap_ps, t_snap_ps == R[k].ps ? " (caller's pointers)" : "", st->vlg_snapshot_flags(R[k].id), ok ? "ok" : "WRONG");
            rbad += !ok;
        }
        CHECK(rbad == 0, "%d rename cases wrong", rbad);
        /* every id of both installed .ids is renamed (paired with a listed partner); the next id up is not */
        int nbad = 0, nneg = 0;
        long long vr1 = v->vs_renames, pr1 = v->ps_renames;
        for (int i = 0; i < g_vids.n; i++) {
            int sid = (int)g_vids.id[i];
            snap2(VS_STOCK, sid * VSN, PS_STOCK, 4 * PSN, 200);
            nbad += !(is(t_snap_vs, VS_OURS) && t_snap_vsi == sid * VSN && is(t_snap_ps, PS_OURS));
            if (!in_ids(&g_vids, sid + 1) && sid + 1 < 1536) {
                snap2(VS_STOCK, (sid + 1) * VSN, PS_STOCK, 4 * PSN, 201);
                nbad += !(t_snap_vs == VS_STOCK && t_snap_ps == PS_STOCK); nneg++;
            }
        }
        for (int i = 0; i < g_pids.n; i++) {
            int sid = (int)g_pids.id[i];
            snap2(VS_STOCK, 0, PS_STOCK, sid * PSN, 202);
            nbad += !(is(t_snap_vs, VS_OURS) && is(t_snap_ps, PS_OURS) && t_snap_psi == sid * PSN);
            if (!in_ids(&g_pids, sid + 1)) {
                snap2(VS_STOCK, 0, PS_STOCK, (sid + 1) * PSN, 203);
                nbad += !(t_snap_vs == VS_STOCK && t_snap_ps == PS_STOCK); nneg++;
            }
        }
        printf("  RL vlg: all %d VS ids + %d PS ids renamed as pairs, %d unlisted neighbour ids not: %s (VS renames +%lld, PS"
               " renames +%lld)\n", g_vids.n, g_pids.n, nneg, nbad ? "WRONG" : "ok", v->vs_renames - vr1, v->ps_renames - pr1);
        CHECK(g_vids.n == v->nvs && g_pids.n == v->nps && g_vids.n > 0 && g_pids.n > 0 && nbad == 0 &&
              v->ps_renames - pr1 == g_vids.n + g_pids.n, "id list renames (%d bad)", nbad);
        /* RL bump: the same for the bump files (renamed iff the bump pair is in use) */
        {
            int bbad = 0, bneg = 0;
            long long bv0 = v->bump_vs_renames, bp0 = v->bump_ps_renames;
            for (int i = 0; i < g_bvids.n; i++) {
                int sid = (int)g_bvids.id[i];
                snap2(BVS_STOCK, sid * BVSN, BPS_STOCK, rb_ps * BPSN, 204);
                bbad += g_bump_on ? !(is(t_snap_vs, BVS_OURS) && t_snap_vsi == sid * BVSN && is(t_snap_ps, BPS_OURS) &&
                                      st->vlg_snapshot_flags(204) == 5)
                                  : !(t_snap_vs == BVS_STOCK && t_snap_ps == BPS_STOCK && st->vlg_snapshot_flags(204) == 0);
                if (!in_ids(&g_bvids, sid + 1) && sid + 1 < 8) {
                    snap2(BVS_STOCK, (sid + 1) * BVSN, BPS_STOCK, rb_ps * BPSN, 205);
                    bbad += !(t_snap_vs == BVS_STOCK && t_snap_ps == BPS_STOCK); bneg++;
                }
            }
            for (int i = 0; i < g_bpids.n; i++) {
                int sid = (int)g_bpids.id[i];
                snap2(BVS_STOCK, rb_vs * BVSN, BPS_STOCK, sid * BPSN, 206);
                bbad += g_bump_on ? !(is(t_snap_vs, BVS_OURS) && is(t_snap_ps, BPS_OURS) && t_snap_psi == sid * BPSN)
                                  : !(t_snap_vs == BVS_STOCK && t_snap_ps == BPS_STOCK);
                if (!in_ids(&g_bpids, sid + 1)) {
                    snap2(BVS_STOCK, rb_vs * BVSN, BPS_STOCK, (sid + 1) * BPSN, 207);
                    bbad += !(t_snap_vs == BVS_STOCK && t_snap_ps == BPS_STOCK); bneg++;
                }
            }
            long long nb = g_bump_on ? g_bvids.n + g_bpids.n : 0;
            printf("  RL bump: all %d VS ids + %d PS ids %s, %d unlisted neighbour ids not: %s (bump VS renames +%lld, PS"
                   " renames +%lld)\n", g_bvids.n, g_bpids.n, g_bump_on ? "renamed as pairs" : "NOT renamed (bump not in use)",
                   bneg, bbad ? "WRONG" : "ok", v->bump_vs_renames - bv0, v->bump_ps_renames - bp0);
            CHECK(bbad == 0 && v->bump_ps_renames - bp0 == nb && v->bump_vs_renames - bv0 >= nb, "bump id renames (%d bad)", bbad);
        }
        /* RL skin: the same for the skin files (renamed iff the skin pair is in use); skin_vs20 has 2 statics, both listed,
         * so its negative is the out-of-range static 2 */
        {
            int sbad = 0, sneg = 0;
            long long sv0 = v->skin_vs_renames, sp0 = v->skin_ps_renames;
            for (int i = 0; i < g_svids.n; i++) {
                int sid = (int)g_svids.id[i];
                snap2(SVS_STOCK, sid * SVSN, SPS_STOCK, rs_ps * SPSN, 208);
                sbad += g_skin_on ? !(is(t_snap_vs, SVS_OURS) && t_snap_vsi == sid * SVSN && is(t_snap_ps, SPS_OURS) &&
                                      st->vlg_snapshot_flags(208) == 9)
                                  : !(t_snap_vs == SVS_STOCK && t_snap_ps == SPS_STOCK && st->vlg_snapshot_flags(208) == 0);
                if (!in_ids(&g_svids, sid + 1)) {
                    snap2(SVS_STOCK, (sid + 1) * SVSN, SPS_STOCK, rs_ps * SPSN, 209);
                    sbad += !(t_snap_vs == SVS_STOCK && t_snap_ps == SPS_STOCK && st->vlg_snapshot_flags(209) == 0); sneg++;
                }
            }
            for (int i = 0; i < g_spids.n; i++) {
                int sid = (int)g_spids.id[i];
                snap2(SVS_STOCK, 0, SPS_STOCK, sid * SPSN, 210);
                sbad += g_skin_on ? !(is(t_snap_vs, SVS_OURS) && is(t_snap_ps, SPS_OURS) && t_snap_psi == sid * SPSN &&
                                      st->vlg_snapshot_flags(210) == 9)
                                  : !(t_snap_vs == SVS_STOCK && t_snap_ps == SPS_STOCK);
                if (!in_ids(&g_spids, sid + 1)) {
                    snap2(SVS_STOCK, 0, SPS_STOCK, (sid + 1) * SPSN, 211);
                    sbad += !(t_snap_vs == SVS_STOCK && t_snap_ps == SPS_STOCK && st->vlg_snapshot_flags(211) == 0); sneg++;
                }
            }
            long long ns = g_skin_on ? g_svids.n + g_spids.n : 0;
            printf("  RL skin: all %d VS ids + %d PS ids %s, %d unlisted neighbour ids not: %s (skin VS renames +%lld, PS"
                   " renames +%lld)\n", g_svids.n, g_spids.n, g_skin_on ? "renamed as pairs" : "NOT renamed (skin not in use)",
                   sneg, sbad ? "WRONG" : "ok", v->skin_vs_renames - sv0, v->skin_ps_renames - sp0);
            CHECK(sbad == 0 && v->skin_ps_renames - sp0 == ns && v->skin_vs_renames - sv0 >= ns && sneg > 0,
                  "skin id renames (%d bad)", sbad);
        }

        /* --- P: the exhaustive pairing matrix --- */
        long long rv0 = v->pair_reverts;
        MatrixResult m = matrix(st, 1, NSEQ, 20000, "");
        CHECK(m.cases <= 10000, "matrix ids overlap the F matrix (40000..)");
        printf("  P pairing matrix: %d cases (%d VS kinds x %d PS kinds x %d sequences), %d renamed as pairs (%d bump, %d"
               " skin), %d wrong, %d wrong call orders; pair reverts +%lld; pairing violations seen by the fake stock: %d;"
               " proxy pair violations %lld\n", m.cases, g_nvk, g_npk, NSEQ, m.renamed, m.renamed_bump, m.renamed_skin,
               m.bad, m.order_bad, v->pair_reverts - rv0, m.fk_viol, (long long)v->pair_violations);
        CHECK(m.bad == 0 && m.order_bad == 0 && m.fk_viol == 0 && m.renamed > 0 && (m.renamed_bump > 0) == g_bump_on &&
              (m.renamed_skin > 0) == g_skin_on && v->pair_reverts >= rv0 && v->pair_violations == 0, "P matrix");

        /* --- N: the TakeSnapshot net: stock's own SetVertexShader (not through the wrapper) after our pair --- */
        long long pv0 = v->pair_violations;
        LONG fv0 = g_fk_viol;
        sh_default(); sh_vs(VS_STOCK, 0); sh_ps(PS_STOCK, 4 * PSN);
        ((void (*)(void *, const char *, int))vs[23])(sh_r, VS_OTHER, 7);   /* bypasses the proxy */
        take(300);
        int n_ok = v->pair_violations == pv0 + 1 && g_fk_viol == fv0 && t_snap_vs == VS_OTHER && is(t_snap_ps, PS_STOCK) &&
                   t_snap_psi == 4 * PSN && st->vlg_snapshot_flags(300) == 0;
        printf("  N bypassing SetVertexShader after our pair: proxy pair violations +%lld, stock holds %s + %s/%d, mark %d: %s\n",
               v->pair_violations - pv0, t_snap_vs ? t_snap_vs : "NULL", t_snap_ps ? t_snap_ps : "NULL", t_snap_psi,
               st->vlg_snapshot_flags(300), n_ok ? "net re-sent the stock PS" : "WRONG");
        CHECK(n_ok, "N TakeSnapshot net");
        /* N2: the same for the skin pair (a pair kind other than vlg: the net re-sends skin_ps20b) */
        if (g_skin_on) {
            int sp = g_spids.n ? (int)g_spids.id[0] : 0;
            pv0 = v->pair_violations; fv0 = g_fk_viol;
            sh_default(); sh_vs(SVS_STOCK, 0); sh_ps(SPS_STOCK, sp * SPSN);
            ((void (*)(void *, const char *, int))vs[23])(sh_r, VS_OTHER, 7);   /* bypasses the proxy */
            take(304);
            int n2 = v->pair_violations == pv0 + 1 && g_fk_viol == fv0 && t_snap_vs == VS_OTHER && is(t_snap_ps, SPS_STOCK) &&
                     t_snap_psi == sp * SPSN && st->vlg_snapshot_flags(304) == 0;
            printf("  N2 skin pair, then a bypassing SetVertexShader: proxy pair violations +%lld, stock holds %s + %s/%d, mark"
                   " %d: %s\n", v->pair_violations - pv0, t_snap_vs ? t_snap_vs : "NULL", t_snap_ps ? t_snap_ps : "NULL",
                   t_snap_psi, st->vlg_snapshot_flags(304), n2 ? "net re-sent skin_ps20b" : "WRONG");
            CHECK(n2, "N2 TakeSnapshot net (skin)");
        }
        pv0 = v->pair_violations;

        /* --- S: a reused handle: the shader manager hands one of our (released) PS handles to another shader. The
         * net must leave that shader alone, also when its index happens to be one our file holds (only the exact
         * (handle, index) pairs recorded at the rename count as ours). --- */
        {
            int other = -1;
            for (int i = 0; i < g_pids.n; i++) if (g_pids.id[i] != 4) { other = (int)g_pids.id[i]; break; }
            long long sh0 = v->stale_handles;
            LONG fvs0 = g_fk_viol;
            sh_default(); sh_vs(VS_STOCK, 0); sh_ps(PS_STOCK, 4 * PSN); take(301);   /* records (our PS handle, 48) */
            int m301 = st->vlg_snapshot_flags(301);
            const int sidx[2] = { 1, other * PSN };
            int s_ok = m301 != 0 && other >= 0;
            for (int k = 0; k < 2; k++) {
                sh_default(); sh_vs(VS_OTHER, 7);
                g_fk_force_ps_h = fk_handle(PS_OURS, 4 * PSN);
                sh_ps(PS_OTHER, sidx[k]);
                take(302 + k);
                int ok = t_snap_vs == VS_OTHER && t_snap_ps == PS_OTHER && t_snap_psi == sidx[k] &&
                         st->vlg_snapshot_flags(302 + k) == 0;
                printf("  S reused PS handle of ours on %s/%d: stock holds %s/%d + %s/%d, mark %d: %s\n", PS_OTHER, sidx[k],
                       t_snap_vs ? t_snap_vs : "NULL", t_snap_vsi, t_snap_ps ? t_snap_ps : "NULL", t_snap_psi,
                       st->vlg_snapshot_flags(302 + k), ok ? "left alone" : "WRONG");
                s_ok &= ok;
            }
            printf("  S stale handles +%lld, proxy pair violations +%lld, fake-stock violations +%ld\n",
                   v->stale_handles - sh0, v->pair_violations - pv0, (long)(g_fk_viol - fvs0));
            CHECK(s_ok && v->stale_handles - sh0 == 2 && v->pair_violations == pv0 && g_fk_viol == fvs0, "S reused handle");
        }

        /* --- D: light block --- */
        snap2(VS_STOCK, 0, PS_STOCK, 4 * PSN, 100);
        snap2(VS_STOCK, 0, PS_STOCK, g_ps_unlisted * PSN, 101);
        snap2(VS_STOCK, 4 * VSN, PS_STOCK, 4 * PSN, 103);
        setcube(cube); setorigin(origin);
        setlight(0, &lp); setlight(1, &ls); setlight(2, &ld); setlight(3, &lx);
        printf("  layout check vs stock D3DLIGHT9: ok %lld mismatch %lld (lights set %lld)\n", (long long)v->layout_ok,
               (long long)v->layout_mismatch, (long long)v->lights_set);
        CHECK(v->layout_ok == 3 && v->layout_mismatch == 0 && v->active, "LightDesc_t layout check");
        PhotonLightBlock want;
        memset(&want, 0, sizeof want);
        want.flags[0] = 1; want.flags[1] = 3;
        memcpy(want.ambient, cube, sizeof cube);
        exp_light(want.lights[0], &lp); exp_light(want.lights[1], &ls); exp_light(want.lights[2], &ld);
        want.tuning[0] = 1.5f; want.tuning[1] = 0.75f; want.tuning[2] = 0; want.tuning[3] = 0;   /* env 1.5 beats cfg 3 */
        exp_occ(want.envocc, 1, 0, 1, 0.5);                                   /* the occlusion defaults */
        rec_reset();
        w = draw(100, 2, 1);
        int bad = w == 1 && g_rec[0].start == 32 && g_rec[0].count == NB
                  ? cmp_block(&g_rec[0].v[0][0], &want.flags[0], NB, "D1 sent") : 999;
        bad += cmp_block(&t_rp_c[0][0], &want.flags[0], NB, "D1 device at draw");
        printf("  D1 marked, DL: %d write(s) (start %u count %u), block %s; device at draw c32 = %g %g %g %g\n", w,
               g_rec[0].start, g_rec[0].count, bad ? "WRONG" : "= expected", t_rp_c[0][0], t_rp_c[0][1], t_rp_c[0][2], t_rp_c[0][3]);
        CHECK(bad == 0, "D1 light block");
        /* D2: half-Lambert snapshot; other vs20 dynamic bits (compressed, fog, skinning, NUM_LIGHTS) do not matter */
        rec_reset();
        w = draw(103, 2 | 1 | 16 | 32 | 256, 1);
        want.flags[2] = 1;
        bad = w == 1 && g_rec[0].count == NB ? cmp_block(&g_rec[0].v[0][0], &want.flags[0], NB, "D2") : 999;
        printf("  D2 half-Lambert snapshot, VS idx 0x133 (NUM_LIGHTS 2): %d write(s), %s\n", w, bad ? "WRONG" : "flags.z = 1, rest = D1");
        CHECK(bad == 0, "D2");
        want.flags[2] = 0;
        /* D13: a bump draw of ours: the whole block at any VS index (the bump VS has no lighting-mode dynamics; idx 0 =
         * no DYNAMIC_LIGHT bit, which a vlg draw would treat as unlit); c32.z from the config only (no VS HL bit) */
        if (g_bump_on) {
            snap2(BVS_STOCK, rb_vs * BVSN, BPS_STOCK, rb_ps * BPSN, 112);
            long long rb0 = v->rp_ours_bump;
            rec_reset();
            w = draw(112, 0, 1);
            int fl = st->vlg_snapshot_flags(112);
            bad = w == 1 && g_rec[0].start == 32 && g_rec[0].count == NB ? cmp_block(&g_rec[0].v[0][0], &want.flags[0], NB, "D13") : 999;
            printf("  D13 bump snapshot (mark %d), VS idx 0: %d write(s), %s; RenderPass ours bump +%lld\n", fl, w,
                   bad ? "WRONG" : "block = D1", v->rp_ours_bump - rb0);
            CHECK(bad == 0 && fl == 5 && v->rp_ours_bump - rb0 == 1, "D13 bump draw");
        }
        /* D16: a skin draw of ours. The block (= D1: c32.z from the config only, skin's half-Lambert is a PS static) at
         * any skin_vs20 dynamic index < 48 without LIGHTING_PREVIEW (x8): idx 0 (no vlg DYNAMIC_LIGHT bit), idx 1|2|4|32
         * (compressed, fog, skinning, NUM_LIGHTS 2). Stock look (c32 = 0, counted as skin unlit): LIGHTING_PREVIEW, idx
         * 48, -1. With a shader manager its index is used without slot 40 (8: stock look, then 16: lit). */
        if (g_skin_on) {
            int sp = g_spids.n ? (int)g_spids.id[0] : 0;
            snap2(SVS_STOCK, SVSN, SPS_STOCK, sp * SPSN, 114);
            int fl = st->vlg_snapshot_flags(114);
            long long rs0 = v->rp_ours_skin, su0 = v->ours_skin_unlit, un0 = v->ours_unlit;
            rec_reset();
            int wa = draw(114, 0, 1);                    /* S3: the device must hold the block; a cached one is not re-sent */
            bad = wa <= 1 ? cmp_block(&t_rp_c[0][0], &want.flags[0], NB, "D16a") : 999;
            rec_reset();
            int wb = draw(114, 1 | 2 | 4 | 32, 1);
            bad += wb <= 1 ? cmp_block(&t_rp_c[0][0], &want.flags[0], NB, "D16b") : 999;
            float cb = t_rp_c[0][0];
            rec_reset();
            int wc = draw(114, 8, 1);                    /* LIGHTING_PREVIEW: c32 = 0 (one write; c56 unchanged) */
            int c_ok = wc == 1 && g_rec[0].start == 32 && g_rec[0].count == 1 && t_rp_c[0][0] == 0;
            int wd = draw(114, SVSN, 1) + draw(114, -1, 1);   /* out of range: stock look, nothing new to write */
            char *mg = *(char **)((char *)hs + 0x9c440);
            int we = 1, wf = 1;
            float ce = 0, cf = 1;
            long long fm0 = v->vsidx_from_mgr;
            if (mg) {
                draw(114, 0, 1);                                            /* lit again */
                *(volatile int *)(mg + 8) = 8; we = draw(114, 0, 0); ce = t_rp_c[0][0];
                *(volatile int *)(mg + 8) = 16; wf = draw(114, 0, 0); cf = t_rp_c[0][0];
            }
            long long nr = v->rp_ours_skin - rs0, nu = v->ours_skin_unlit - su0;
            printf("  D16 skin snapshot (mark %d), VS idx 0: %d write(s), idx 0x27: %d (c32.x %g), block %s; LIGHTING_PREVIEW:"
                   " %d write c32=0 %s; idx 48/-1: %d; %s: LP %d write(s) (c32.x %g), NUM_LIGHTS 1 %d (c32.x %g), from manager"
                   " +%lld; RenderPass ours skin +%lld, skin unlit +%lld\n", fl, wa, wb, cb, bad ? "WRONG" : "= D1", wc,
                   c_ok ? "ok" : "WRONG", wd, mg ? "shader manager index" : "no manager", we, ce, wf, cf,
                   v->vsidx_from_mgr - fm0, nr, nu);
            CHECK(fl == 9 && bad == 0 && cb == 1 && c_ok && wd == 0 &&
                  (mg ? we == 1 && ce == 0 && wf == 1 && cf == 1 && v->vsidx_from_mgr - fm0 == 2 && nr == 8 && nu == 4
                      : nr == 5 && nu == 3) && v->ours_unlit - un0 == nu, "D16 skin draw");
            if (!mg) draw(100, 2, 1);                    /* leave the device lit, as D13 does, for D3 */
        }
        /* D3..D7: the stock-look cases */
        static const float zero4[4];
        rec_reset();
        int w3 = draw(100, 2 | 4, 1);
        int d3 = w3 == 1 && g_rec[0].start == 32 && g_rec[0].count == 1 && !cmp_block(&g_rec[0].v[0][0], zero4, 1, "D3") &&
                 t_rp_c[0][0] == 0;
        int w4 = draw(100, 2 | 4, 1);
        int w5a = draw(100, 2, 1), w5b = draw(101, 2, 1), w5c = draw(101, 2, 1);
        float c5 = t_rp_c[0][0];
        long long nv0 = v->ours_no_vsidx, un0 = v->ours_unlit, mg0 = v->vsidx_from_mgr;
        /* D6: no slot 40 since the last RenderPass. VLG's vs20 path sets the VS index through the command buffer, whose
         * ExecuteCommandBuffer writes g_pShaderManager (stock+0x9c440) +8 directly (T3.4f A5 finding): the proxy then
         * uses that index, the one stock binds. Here mgr+8 is written as the command buffer would: 0 (no DL: stock
         * look), then 2 (DL: lit). Without a shader manager (fake slot 40) the old rule holds: no index = stock look. */
        char *mgr = *(char **)((char *)hs + 0x9c440);
        int w6a = draw(100, 2, 1), w6b, w6c = 1;
        float c6, c6c = 1;
        if (mgr) {
            *(volatile int *)(mgr + 8) = 0; w6b = draw(100, 0, 0); c6 = t_rp_c[0][0];
            *(volatile int *)(mgr + 8) = 2; w6c = draw(100, 0, 0); c6c = t_rp_c[0][0];
        } else { w6b = draw(100, 0, 0); c6 = t_rp_c[0][0]; }
        long long nv6 = v->ours_no_vsidx - nv0, mg6 = v->vsidx_from_mgr - mg0;
        /* D7 (stock look): one c32 = 0 write when D6 left the device lit (manager case), else none */
        int w7 = draw(100, 2 | 8, 1) + draw(100, 2 | 64, 1) + draw(100, 0, 1) + draw(100, -1, 1) + draw(100, VSN + 2, 1);
        printf("  D3 static prop (DL+SL): %d write c32=0 %s; D4 again: %d; D5 lit %d, unmarked %d then %d (c32.x %g);"
               " D6 lit %d, no slot 40 with %s: %d (c32.x %g) then %d (c32.x %g), from manager %lld, no index %lld;"
               " D7 SLL/LP/no-DL/-1/384+2: %d writes (unlit +%lld)\n",
               w3, d3 ? "ok" : "WRONG", w4, w5a, w5b, w5c, c5, w6a, mgr ? "manager index 0 then 2" : "no manager", w6b, c6,
               w6c, c6c, mg6, nv6, w7, v->ours_unlit - un0);
        CHECK(d3 && w4 == 0 && w5a == 1 && w5b == 1 && w5c == 0 && c5 == 0 && w6a == 1 && w6b == 1 && c6 == 0 &&
              w6c == 1 && c6c == 1 && (mgr ? (mg6 == 2 && nv6 == 0) : (mg6 == 0 && nv6 == 1)) && w7 == (mgr ? 1 : 0) &&
              v->ours_unlit - un0 == (mgr ? 6 : 5), "D3..D7");
        /* D8: second light set: disabled slot 0, clamped spot in slot 3 */
        setlight(0, &lx); setlight(3, &lc);
        rec_reset();
        w = draw(100, 2, 1);
        PhotonLightBlock wb = want;
        memset(wb.lights, 0, sizeof wb.lights);
        wb.flags[1] = 3;
        exp_light(wb.lights[0], &ls); exp_light(wb.lights[1], &ld); exp_light(wb.lights[2], &lc);
        bad = w == 1 && g_rec[0].count == NB ? cmp_block(&g_rec[0].v[0][0], &wb.flags[0], NB, "D8") : 999;
        printf("  D8 lights {off, spot, dir, clamped spot theta 4.5 phi 4.0}: %s (layout ok %lld)\n", bad ? "WRONG" : "= expected",
               (long long)v->layout_ok);
        CHECK(bad == 0 && v->layout_ok == 4, "D8");
        /* D9/D10: DisableAllLocalLights; out-of-range SetLight ignored */
        disablelights();
        setlight(4, &lp); setlight(-1, &lp);
        rec_reset();
        w = draw(100, 2, 1);
        PhotonLightBlock wc = want;
        memset(wc.lights, 0, sizeof wc.lights);
        wc.flags[1] = 0;
        bad = w == 1 && g_rec[0].count == NB ? cmp_block(&g_rec[0].v[0][0], &wc.flags[0], NB, "D9") : 999;
        printf("  D9 DisableAllLocalLights + SetLight(4)/(-1): n = %g, %s\n", g_rec[0].v[0][1], bad ? "WRONG" : "= expected");
        CHECK(bad == 0, "D9");
        /* FOG: a draw of ours with D3D FF fog on (r_pixelfog changed after the snapshot) is counted */
        long long ff0 = v->ours_ff_fog;
        *fffog = 1;
        int wf1 = draw(100, 2, 1);
        float cf1 = t_rp_c[0][0];
        int wf2 = draw(101, 2, 1);
        *fffog = 0;
        int wf3 = draw(100, 2, 1);
        float cf3 = t_rp_c[0][0];
        printf("  FOG FF fog on: ours %d write(s), stock %d; counted +%lld; fog off again: %d write(s)\n", wf1, wf2,
               v->ours_ff_fog - ff0, wf3);
        CHECK(v->ours_ff_fog - ff0 == 1 && wf1 <= 1 && cf1 == 1 && wf3 == 1 && cf3 == 1, "FOG");   /* S3: wf1 may be cached */
        /* D11: live config re-read */
        write_cfg(gdir, "// photon.cfg written by the test\nphoton_light_scale 3\nphoton_ambient_scale 0.5\nphoton_halflambert 1\n"
                        "photon_debug 2\n");
        int ch = st->vlg_poll_config(0);
        rec_reset();
        w = draw(100, 2, 1);
        int d11 = ch == 1 && w == 1 && g_rec[0].v[0][2] == 1 && g_rec[0].v[23][0] == 1.5f && g_rec[0].v[23][1] == 0.5f &&
                  g_rec[0].v[23][3] == 2;
        write_cfg(gdir, "photon_lighting \"0\"\n");
        int ch2 = st->vlg_poll_config(1);
        rec_reset();
        int w11 = draw(100, 2, 1);
        float c11 = t_rp_c[0][0];
        /* lighting off: c32 = 0 and c56.x = 0 (the occlusion is part of photon_lighting) */
        int d11b = ch2 == 1 && w11 == 2 && g_rec[0].count == 1 && g_rec[1].start == 56 && g_rec[1].count == 1 && c11 == 0 &&
                   t_rp_c[24][0] == 0;
        write_cfg(gdir, "photon_ambient_scale 0.75\n");
        int ch3 = st->vlg_poll_config(1);
        int w11c = draw(100, 2, 1);
        printf("  D11 live cfg: re-read %d -> HL %g scale %g ambient %g debug %g %s; photon_lighting 0 -> %d write(s) c32.x"
               " %g %s; back on -> %d write(s) (reloads %lld)\n", ch, g_rec[0].v[0][2], g_rec[0].v[23][0],
               g_rec[0].v[23][1], g_rec[0].v[23][3], d11 ? "ok" : "WRONG", w11, c11, d11b ? "ok" : "WRONG", w11c,
               (long long)v->config_reloads);
        CHECK(d11 && d11b && ch3 == 1 && w11c == 1, "D11 live config");
        /* D14: the occlusion keys, folded into c56 (checked against the shader formula, exp_occ); a draw of ours
         * without the light block writes c56 alone, and only when it changed */
        {
            write_cfg(gdir, "photon_ambient_scale 0.75\nphoton_envmap_occlusion 0.5\nphoton_envmap_occlusion_min 0.2\n"
                            "photon_envmap_occlusion_max 1.5\nphoton_envmap_calibration 2\n");
            int ch4 = st->vlg_poll_config(1);
            float o14[4], z4[4] = { 0, 0, 0, 1 };
            exp_occ(o14, 0.5, 0.2, 1.5, 2);
            long long ow0 = v->occ_writes;
            rec_reset();
            int a = draw(100, 2 | 4, 1);                 /* static prop (unlit): c32 = 0, then c56 (changed) */
            int a_ok = a == 2 && g_rec[0].start == 32 && g_rec[0].count == 1 && g_rec[1].start == 56 && g_rec[1].count == 1 &&
                       !cmp_block(&g_rec[1].v[0][0], o14, 1, "D14 c56 alone") &&
                       !cmp_block(&t_rp_c[24][0], o14, 1, "D14 device");
            int b = draw(100, 2 | 4, 1);                 /* unchanged: nothing */
            rec_reset();
            int c = draw(100, 2, 1);                     /* lit: one block, c56 inside */
            /* S3: a full block, or (block cached) c32 alone, plus c56 alone only if it changed; the device holds both */
            int c_ok = c >= 1 && c <= 2 && t_rp_c[0][0] == 1 && !cmp_block(&t_rp_c[24][0], o14, 1, "D14 c56 at the lit draw");
            int d = draw(100, 2 | 8, 1);                 /* SLL (unlit): c32 = 0 only, c56 already right */
            write_cfg(gdir, "photon_ambient_scale 0.75\nphoton_envmap_occlusion 0\n");
            int ch5 = st->vlg_poll_config(1);
            rec_reset();
            int e = draw(100, 2 | 4, 1);                 /* strength 0: c56 = (0, 0, 0, 1) */
            int e_ok = e == 1 && g_rec[0].start == 56 && !cmp_block(&g_rec[0].v[0][0], z4, 1, "D14 off");
            int f15 = draw(101, 2, 1);                   /* not ours: c32 only (already 0), c56 untouched */
            write_cfg(gdir, "photon_ambient_scale 0.75\n");
            int ch6 = st->vlg_poll_config(1);
            printf("  D14 occlusion keys 0.5/0.2/1.5/2 -> c56 %g %g %g %g: unlit draw %d write(s) %s, again %d, lit %d %s, SLL %d;"
                   " strength 0 -> %d write %s; not ours %d; occlusion-only writes +%lld\n", o14[0], o14[1], o14[2],
                   o14[3], a, a_ok ? "ok" : "WRONG", b, c, c_ok ? "ok" : "WRONG", d, e, e_ok ? "ok" : "WRONG", f15,
                   v->occ_writes - ow0);
            CHECK(ch4 == 1 && ch5 == 1 && ch6 == 1 && a_ok && b == 0 && c_ok && d == 1 && e_ok && f15 == 0 &&
                  v->occ_writes - ow0 == 2, "D14 occlusion");
            draw(100, 2, 1);                             /* back to the defaults on the device */
        }
        /* D12: snapshot clears */
        setlight(0, &lp); setlight(1, &ls); setlight(2, &ld); setlight(3, &lx);
        clearsnapshots();
        int m170 = st->vlg_snapshot_flags(100) | st->vlg_snapshot_flags(103);
        rec_reset();
        int w12 = draw(100, 2, 1);
        snap2(VS_STOCK, 0, PS_STOCK, 4 * PSN, 100);
        int re = st->vlg_snapshot_flags(100);
        slot260();
        int m260 = st->vlg_snapshot_flags(100);
        printf("  D12 ClearSnapshots: marks %d, draw on the old id -> %d write(s) (c32=0 %s), stock reached %ld; re-marked %d;"
               " slot 260: marks %d, stock reached %ld\n", m170, w12, g_rec[0].count == 1 ? "yes" : "no", (long)g_c170, re,
               m260, (long)g_c260);
        CHECK(m170 == 0 && w12 == 1 && g_rec[0].count == 1 && re == 1 && m260 == 0 && g_c170 == 1 && g_c260 == 1, "D12");
        /* D15: slot 260 (device-reset path) invalidated the c56 cache: the next unlit draw of ours writes c56 again
         * although the config did not change */
        {
            snap2(VS_STOCK, 0, PS_STOCK, 4 * PSN, 100);
            rec_reset();
            int w15 = draw(100, 2 | 4, 1);
            int n56 = 0;
            for (int i = 0; i < g_nrec; i++) n56 += g_rec[i].start == 56;
            float o15[4];
            exp_occ(o15, 1, 0, 1, 0.5);
            int ok15 = n56 == 1 && !cmp_block(&t_rp_c[24][0], o15, 1, "D15");
            printf("  D15 after slot 260: unlit draw of ours %d write(s), %d of c56: %s\n", w15, n56, ok15 ? "rewritten" : "WRONG");
            CHECK(ok15, "D15 c56 cache after slot 260");
        }
        /* D17 (S3): the light-block upload cache. A lit draw with the light state the device already holds writes
         * nothing; after a not-ours draw cleared c32 only c32 is re-sent; a light change sends the whole block. The
         * device must hold the same c32..c56 at every draw. */
        {
            rec_reset();
            int a = draw(100, 2, 1);                     /* after slot 260 (D15): cache invalid, full block */
            int a_ok = a == 1 && g_rec[0].start == 32 && g_rec[0].count == NB;
            float c1[NB][4];
            memcpy(c1, t_rp_c, sizeof c1);
            long long sk0 = v->upload_skips, fo0 = v->flag_on_writes, up16 = v->uploads;
            int b = draw(100, 2, 1);                     /* same state: no write */
            int b_ok = b == 0 && memcmp(c1, t_rp_c, sizeof c1) == 0;
            int c = draw(101, 2, 1);                     /* not ours: c32 = 0 */
            rec_reset();
            int d = draw(100, 2, 1);                     /* cached block: c32 alone */
            int d_ok = d == 1 && g_rec[0].start == 32 && g_rec[0].count == 1 && memcmp(c1, t_rp_c, sizeof c1) == 0;
            setlight(1, &lp);                            /* light 1 changes: whole block */
            rec_reset();
            int e = draw(100, 2, 1);
            int e_ok = e == 1 && g_rec[0].start == 32 && g_rec[0].count == NB && memcmp(c1, t_rp_c, sizeof c1) != 0;
            setlight(1, &ls);
            rec_reset();
            int f = draw(100, 2, 1);                     /* back: whole block again, equal to the first */
            int f_ok = f == 1 && g_rec[0].count == NB && memcmp(c1, t_rp_c, sizeof c1) == 0;
            printf("  D17 upload cache: first %d %s; same state %d write(s) %s; not ours %d; after it %d %s; light change %d %s;"
                   " restored %d %s; skips +%lld, c32-only +%lld, uploads +%lld\n", a, a_ok ? "ok" : "WRONG", b,
                   b_ok ? "ok" : "WRONG", c, d, d_ok ? "ok" : "WRONG", e, e_ok ? "ok" : "WRONG", f, f_ok ? "ok" : "WRONG",
                   v->upload_skips - sk0, v->flag_on_writes - fo0, v->uploads - up16);
            CHECK(a_ok && b_ok && c == 1 && d_ok && e_ok && f_ok && v->upload_skips - sk0 == 2 &&
                  v->flag_on_writes - fo0 == 1 && v->uploads - up16 == 2, "D17 upload cache");
        }
        /* D18 (S3): a device Reset zeroes the constants. Without the invalidation the cache skips the writes and the draw
         * runs on zeros (shown first, so the check has power); after RestoreShaderObjects' invalidation it rewrites. */
        {
            draw(100, 2, 1);
            float c1[NB][4];
            memcpy(c1, t_rp_c, sizeof c1);
            static const float zero18[NB][4];
            g_dev->lpVtbl->SetPixelShaderConstantF(g_dev, 32, &zero18[0][0], NB);
            int a = draw(100, 2, 1);
            int a_stale = a == 0 && t_rp_c[0][0] == 0;
            g_dev->lpVtbl->SetPixelShaderConstantF(g_dev, 32, &zero18[0][0], NB);
            long long dr0 = v->device_resets;
            st->vlg_device_state_lost();
            rec_reset();
            int b = draw(100, 2, 1);
            int b_ok = b == 1 && g_rec[0].count == NB && memcmp(c1, t_rp_c, sizeof c1) == 0;
            printf("  D18 device reset: resets seen through the restore hook %lld; no invalidation: %d write(s), c32.x %g (%s); after"
                   " it: %d write(s), %s\n", dr0, a, a_stale ? 0.0 : t_rp_c[0][0], a_stale ? "stale, as expected" : "UNEXPECTED", b,
                   b_ok ? "device = block" : "WRONG");
            CHECK(a_stale && b_ok && v->device_resets == dr0 + 1 && dr0 >= 1, "D18 device reset");
            draw(100, 2, 1);
        }
        /* D19 (P3c): the call-order route, analysis/15d section 3. Each row is a stream of shaderapi calls, then a lit VLG
         * draw; the counters say which route it took. With PHOTON_TEST_ROUTE=1 an ENGINE draw must reach the device with
         * c33..c38 and the light scale zeroed, and an OVERRIDE draw with them intact. */
        {
            static const float white[6][4] = { { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 } };
            static const float black[6][4] = { { 0 } };
            char rt[8] = "";
            int black_test = GetEnvironmentVariableA("PHOTON_TEST_ROUTE", rt, sizeof rt) > 0 && rt[0] == '1';
            enum { O, A0, A1, CW, CK, CX, L, D, END };
            static const struct { const char *name; int ev[8]; int engine; } rows[] = {
                { "engine dynamic: origin, cube, 129(0), lights", { O, CX, A0, L, END }, 1 },
                { "second mesh of that model: no calls", { END }, 1 },
                { "mat_fullbright: origin, 129(1), white cube, disable", { O, A1, CW, D, END }, 0 },
                { "static array start: disable, black cube, 129(0)", { D, CK, A0, END }, 1 },
                { "static array prop: cube, origin, lights", { CX, O, L, END }, 1 },
                { "render.SuppressEngineLighting(true): white cube, disable", { CW, D, END }, 0 },
                { "after Suppress(false), no calls: still override", { END }, 0 },
                { "engine again", { O, CX, A0, L, END }, 1 },
                { "DModelPanel: origin, cube", { O, CX, END }, 0 },
                { "engine again", { O, CX, A0, L, END }, 1 },
                { "ResetModelLighting alone", { CX, END }, 0 },
                { "C++ spawn icon / model panel: origin, 129, cube, lights", { O, A0, CX, L, END }, 0 },
                { "suppress, then reset, then origin", { CW, D, CX, O, END }, 0 },
                { "engine again", { O, CX, A0, L, END }, 1 },
                { "known misroute (15d): unsuppressed reset, origin, with the latch set", { CX, O, END }, 1 },
            };
            int wrong = 0, blk = 0;
            for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
                for (const int *e = rows[i].ev; *e != END; e++)
                    switch (*e) {
                    case O: setorigin(origin); break;
                    case A0: setambient(0, 0, 0); break;
                    case A1: setambient(1, 1, 1); break;
                    case CW: setcube(white); break;
                    case CK: setcube(black); break;
                    case CX: setcube(cube); break;
                    case L: setlight(0, &lp); break;
                    case D: disablelights(); setlight(0, &lp); break;   /* keeps the draw lit for the black check */
                    }
                long long e0 = v->route_engine[0], o0 = v->route_override[0];
                draw(100, 2, 1);
                int eng = v->route_engine[0] - e0 == 1, ovr = v->route_override[0] - o0 == 1;
                if (eng + ovr != 1 || eng != rows[i].engine) { wrong++; printf("    D19 WRONG: %s: engine +%d override +%d\n", rows[i].name, eng, ovr); }
                float amb = 0;
                for (int k = 1; k <= 6; k++) for (int c = 0; c < 3; c++) amb += t_rp_c[k][c];
                int zeroed = amb == 0 && t_rp_c[23][0] == 0;
                if (zeroed != (black_test && eng)) {
                    blk++; printf("    D19 block WRONG: %s: ambient sum %g, light scale %g\n", rows[i].name, amb, t_rp_c[23][0]);
                }
            }
            printf("  D19 route: %d call streams, %d misclassified, %d wrong blocks (PHOTON_TEST_ROUTE %s)\n",
                   (int)(sizeof rows / sizeof rows[0]), wrong, blk, black_test ? "1: engine black" : "off");
            CHECK(wrong == 0 && blk == 0, "D19 route");
            setcube(cube); setorigin(origin);
            setlight(0, &lp); setlight(1, &ls); setlight(2, &ld); setlight(3, &lx);
            draw(100, 2, 1);
        }
        /* D20: the cases mutation testing showed D17..D19 do not exercise (independent review, 2026-11-04) */
        {
            static const float cube2[6][4] = { { 0.11f, 0.2f, 0.3f, 0 }, { 0.4f, 0.5f, 0.6f, 0 }, { 0.7f, 0.8f, 0.9f, 0 },
                                               { 1.0f, 1.1f, 1.2f, 0 }, { 1.3f, 1.4f, 1.5f, 0 }, { 1.6f, 1.7f, 1.8f, 0 } };
            static const float white20[6][4] = { { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 }, { 1, 1, 1, 0 } };
            static const float zero20[4]; static const float black20[6][4] = { { 0 } };
            float o05[4], odef[4];
            exp_occ(o05, 0.5, 0, 1, 0.5); exp_occ(odef, 1, 0, 1, 0.5);
            write_cfg(gdir, "photon_ambient_scale 0.75\n"); st->vlg_poll_config(1);
            draw(100, 2, 1);
            /* a: ambient only */
            setcube(cube2); rec_reset();
            int a = draw(100, 2, 1);
            int a_ok = a == 1 && g_rec[0].count == NB && t_rp_c[1][0] == cube2[0][0];
            setcube(cube); draw(100, 2, 1);
            /* b: tuning only */
            write_cfg(gdir, "photon_ambient_scale 0.25\n"); st->vlg_poll_config(1); rec_reset();
            int b = draw(100, 2, 1);
            int b_ok = b == 1 && g_rec[0].count == NB && t_rp_c[23][1] == 0.25f;
            /* c: c56 only, block cached: the lit draw must send c56 alone */
            write_cfg(gdir, "photon_ambient_scale 0.25\nphoton_envmap_occlusion 0.5\n"); st->vlg_poll_config(1); rec_reset();
            int c = draw(100, 2, 1);
            int c_ok = c == 1 && g_rec[0].start == 56 && !cmp_block(&t_rp_c[24][0], o05, 1, "D20c");
            /* d: a full upload refreshes the c56 cache: device = default via the block, then back to 0.5 on an unlit draw */
            write_cfg(gdir, "photon_ambient_scale 0.5\n"); st->vlg_poll_config(1); draw(100, 2, 1);
            write_cfg(gdir, "photon_ambient_scale 0.5\nphoton_envmap_occlusion 0.5\n"); st->vlg_poll_config(1);
            draw(100, 2 | 4, 1);
            int d_ok = !cmp_block(&t_rp_c[24][0], o05, 1, "D20d");
            /* e: state lost drops the c56 cache for an unlit draw too */
            g_dev->lpVtbl->SetPixelShaderConstantF(g_dev, 56, zero20, 1);
            st->vlg_device_state_lost();
            draw(100, 2 | 4, 1);
            int e_ok = !cmp_block(&t_rp_c[24][0], o05, 1, "D20e");
            write_cfg(gdir, "photon_ambient_scale 0.75\n"); st->vlg_poll_config(1); draw(100, 2, 1);
            printf("  D20 cache: ambient only %d %s; tuning only %d %s; c56 only %d %s; full upload refreshes c56 cache %s; state lost drops c56 cache %s\n",
                   a, a_ok ? "ok" : "WRONG", b, b_ok ? "ok" : "WRONG", c, c_ok ? "ok" : "WRONG", d_ok ? "ok" : "WRONG", e_ok ? "ok" : "WRONG");
            CHECK(a_ok && b_ok && c_ok && d_ok && e_ok, "D20 cache");
            /* route, with the latch set by a real engine sequence right before each stream and no draw in between */
#define ENG20() do { setorigin(origin); setcube(cube); setambient(0, 0, 0); setlight(0, &lp); draw(100, 2, 1); } while (0)
#define OVR20(what) do { long long e0 = v->route_engine[0], o0 = v->route_override[0]; draw(100, 2, 1); int ok = v->route_engine[0] == e0 && v->route_override[0] == o0 + 1; \
                printf("  D20 route %s: %s\n", what, ok ? "override" : "WRONG (engine)"); CHECK(ok, "D20 route %s", what); } while (0)
            ENG20(); setcube(cube); setambient(1, 1, 1); OVR20("cube then 129(1,1,1)");
            ENG20(); setcube(cube); setambient(0, 1, 0); OVR20("cube then 129(0,1,0)");
            ENG20(); setcube(cube); draw(100, 2, 1); setorigin(origin); OVR20("cube, draw, origin");
            ENG20(); setcube(cube); setlight(0, &lp); setorigin(origin); OVR20("cube, 127, origin");
            ENG20(); setcube(white20); disablelights(); setcube(cube); setorigin(origin); OVR20("suppress 130 207 (no 127), reset, origin");
            ENG20(); setcube(white20); disablelights(); setcube(cube); setorigin(origin); setambient(0, 0, 0); OVR20("suppress, reset, origin, 129(0)");
            ENG20(); setcube(cube); setlight(0, &lp); setlight(1, &ls); setlight(2, &ld); setlight(3, &lx); OVR20("slot 21 SetupLighting: 130, 127 x4");
#define SHOW20(what) do { long long e0 = v->route_engine[0]; draw(100, 2, 1); printf("  D20 demo %s: %s\n", what, v->route_engine[0] != e0 ? "ENGINE" : "OVERRIDE"); } while (0)
            ENG20(); SHOW20("engine model, then a non-model lit VLG draw with no lighting calls");
            setcube(white20); disablelights(); disablelights(); setcube(black20); setambient(0, 0, 0); setcube(cube); setorigin(origin); setlight(0, &lp); draw(100, 2, 1); SHOW20("Suppress(true), nested static array (RenderView), then a suppressed DrawModel");
            ENG20(); setcube(cube); setambient(0, 0, 0); SHOW20("Lua cube then SetAmbientLight(0,0,0) (known misroute)");
            setcube(cube); setorigin(origin);
            setlight(0, &lp); setlight(1, &ls); setlight(2, &ld); setlight(3, &lx);
            draw(100, 2, 1);
        }
        /* T: threaded */
        g_th_nlights = 3;
        g_rec_store = 0;
        long long up0 = v->uploads + v->upload_skips, rvt0 = v->pair_reverts;
        LONG fvt0 = g_fk_viol;
        HANDLE th[2] = { CreateThread(NULL, 0, th_snap, NULL, 0, NULL), CreateThread(NULL, 0, th_draw, NULL, 0, NULL) };
        WaitForMultipleObjects(2, th, TRUE, 60000);
        CloseHandle(th[0]); CloseHandle(th[1]);
        g_rec_store = 1;
        int marked = 0, wrong_marks = 0;
        for (int i = 0; i < TH_N; i++) {
            int fl = st->vlg_snapshot_flags(TH_BASE + i);
            marked += fl == 1;
            wrong_marks += fl != ((i % 3) ? 1 : 0);
        }
        printf("  T threads: %d snapshots (%d marked, %d wrong marks, pair reverts +%lld, fake-stock pairing violations %ld)"
               " on one thread while %ld draws ran on another (%ld lit): %ld device-state mismatches at draw; uploads +%lld\n",
               TH_N, marked, wrong_marks, v->pair_reverts - rvt0, (long)(g_fk_viol - fvt0), (long)g_th_draws,
               (long)g_th_lit, (long)g_th_mismatch, v->uploads + v->upload_skips - up0);
        CHECK(wrong_marks == 0 && g_fk_viol == fvt0 && v->pair_reverts - rvt0 == 0 && g_th_draws == TH_DRAWS &&
              g_th_mismatch == 0 && v->uploads + v->upload_skips - up0 == g_th_lit, "threads");
        /* F: load failure -> fallback to the stock names, lighting off for the session, then no renames at all */
        if (fail_vs) g_fail_vs = 1; else g_fail_ps = 1;
        long long lf0 = v->load_fallbacks;
        snap2(VS_STOCK, 0, PS_STOCK, 4 * PSN, 400);
        int nl = t_nlog;
        /* deferred VS rename: vs: VS stock, VS ours (invalid), VS stock, PS stock; ps: VS stock, VS ours, PS ours (invalid), PS stock, VS stock */
        int f1 = fail_vs ? nl == 4 && t_log[0].name == VS_STOCK && is(t_log[1].name, VS_OURS) && t_log[2].name == VS_STOCK &&
                               t_log[3].name == PS_STOCK
                         : nl == 5 && t_log[0].name == VS_STOCK && is(t_log[1].name, VS_OURS) && is(t_log[2].name, PS_OURS) &&
                               t_log[3].name == PS_STOCK && t_log[4].name == VS_STOCK && t_log[4].idx == 0;
        f1 = f1 && t_snap_vs == VS_STOCK && t_snap_ps == PS_STOCK && st->vlg_snapshot_flags(400) == 0;
        long act = v->active;
        g_fail_vs = g_fail_ps = 0;
        MatrixResult mf = matrix(st, 0, NSEQ, 40000, "F-after");   /* ids clear of P's 20000.. (2394 cases) */
        rec_reset();
        int wf = draw(TH_BASE + 1, 2, 1) + draw(TH_BASE + 1, 2, 1);
        printf("  F %s load failure: %d calls reached stock (%s), active %ld, fallbacks +%lld; then the matrix: %d cases, %d"
               " wrong (no renames), fake-stock violations %d; draws on a marked id: %d write(s), c32.x %g c56.x %g (reason"
               " \"%s\")\n", fail_vs ? "VS" : "PS", nl, f1 ? "fallback order ok" : "WRONG", act, v->load_fallbacks - lf0,
               mf.cases, mf.bad, mf.fk_viol, wf, t_rp_c[0][0], t_rp_c[24][0], v->reason);
        CHECK(f1 && act == 0 && v->load_fallbacks - lf0 == 1 && mf.bad == 0 && mf.fk_viol == 0 && wf <= 2 &&
              t_rp_c[0][0] == 0 && t_rp_c[24][0] == 0, "F fallback");
        CHECK(v->pair_violations == pv0, "proxy pair violations after N: +%lld", v->pair_violations - pv0);
    }
after_cases:
    printf("vlg: totals: VS renames %lld, PS renames %lld, pair reverts %lld, proxy pair violations %lld, fog-gate blocks %lld;"
           " pairing violations seen by the fake stock: %ld (of %ld snapshots)\n", (long long)v->vs_renames,
           (long long)v->ps_renames, (long long)v->pair_reverts, (long long)v->pair_violations, (long long)v->fog_blocks,
           (long)g_fk_viol, (long)g_fk_takes);
    CHECK(g_fk_viol == 0, "the fake stock shadow saw %ld snapshots with our shader paired with a stock one", (long)g_fk_viol);
    /* P3h: photon_ambient defaults to 0, and then the volume's registers c59..c64 are never touched and no draw enters its path */
    if (!amb) {
        long long vol = v->vol_draws + v->vol_const_writes + v->vol_weight_writes;
        for (int i = 0; i < 5; i++) vol += v->vol_off[i];
        printf("  D21 photon_ambient 0: writes above c56: %ld, volume path entries: %lld\n", (long)g_rec_above_block, vol);
        CHECK(g_rec_above_block == 0 && vol == 0, "D21 photon_ambient 0");
    }

    /* restore */
    *(volatile uint32_t *)(cv + 0x58) = saved_cv;
    *fffog = saved_fog;
    *(int *)(caps + 0x54c) = saved_max;
    *(int *)(caps + 0x4f8) = saved_dx;
    VT(api_r) = sva; VT(sh_r) = svs;
    VirtualProtect(&dvt[109], sizeof(void *), PAGE_READWRITE, &old);
    dvt[109] = g_orig_setpsf;
    VirtualProtect(&dvt[109], sizeof(void *), old, &old);
    *gdev = NULL;
    g_dev->lpVtbl->Release(g_dev);
    g_dev = NULL;
    d3d->lpVtbl->Release(d3d);
    DestroyWindow(hwnd);
    if (g_fails) printf("VLG FAIL (%d)\n", g_fails);
    else printf("VLG OK (%s)\n", mode);
    return g_fails;
}
