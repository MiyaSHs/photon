/* shaderhost: mock materialsystem host for photon_shaders.dll under Wine (T3.2, analysis/11_shaderdll_design.md §8).
 * Productised from analysis/scratch/m3_design/shaderhost_probe.c.
 *   shaderhost.exe <bindir> test|release
 * <bindir> (built by scripts/run_shaderhost.sh, laid out as <root>\bin\win64 so the module-relative config path
 * <root>\garrysmod\cfg\photon_shaders.txt can be exercised) holds photon_shaders.dll (the test variant
 * photon_shaders_test.dll renamed, or the release build), a copy named game_shader_dx9.dll, the stock
 * game DLLs (tier0, stdshader_dbg.dll for the differential reference) and shaderapidx9_stock.dll (stock shaderapidx9,
 * whose presence marks base mode).
 * Fakes (recording; every call into them is appended to the calling thread's trace):
 *   HW  = MaterialSystemHardwareConfig012 (78 slots; 32/44 -> 95, 56/64 -> 1, 62 -> configurable)
 *   SYS = ShaderSystem002 (7), SH = IShaderShadow (52), API = IShaderDynamicAPI (100), CFG = 4 KiB zero blob,
 *   VAR = IMaterialVar (31 slots; 3/4/9/10/11/17 implemented, field layout +0x10 int, +0x14 float[4]).
 * Prints "SHADERHOST OK" and exits 0 when every check passes. */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef void *(*CreateInterfaceFn)(const char *, int *);
typedef unsigned char msbool;

/* ---------------- trace ---------------- */
typedef struct Rec { const char *tag; int slot; void *self; long long a, b, c, d; float f[4]; char s[40]; } Rec;
typedef struct Trace { Rec r[128]; int n; int overflow; } Trace;
static __thread Trace *tl_trace;

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Rec *rec_new(const char *tag, int slot, void *self) {
    Trace *t = tl_trace;
    if (!t) return NULL;
    if (t->n >= (int)(sizeof t->r / sizeof t->r[0])) { t->overflow = 1; return NULL; }
    Rec *r = &t->r[t->n++];
    memset(r, 0, sizeof *r);
    r->tag = tag; r->slot = slot; r->self = self;
    return r;
}

/* ---------------- generic recording fakes ---------------- */
typedef struct Fake { void **vt; const char *tag; long long ret[100]; } Fake;

static long long record(int slot, Fake *s, long long a, long long b, long long c, long long d) {
    Rec *r = rec_new(s->tag, slot, s);
    if (r) {
        r->a = a; r->b = b; r->c = c; r->d = d;
        if (!strcmp(s->tag, "SH") && (slot == 23 || slot == 24) && a) snprintf(r->s, sizeof r->s, "%s", (const char *)a);
        if (!strcmp(s->tag, "API") && (slot == 30 || slot == 31) && b) memcpy(r->f, (const void *)b, sizeof r->f);
    }
    return s->ret[slot];
}
#define R(n) static long long r##n(Fake *s, long long a, long long b, long long c, long long d) { return record(n, s, a, b, c, d); }
#define R10(t) R(t##0) R(t##1) R(t##2) R(t##3) R(t##4) R(t##5) R(t##6) R(t##7) R(t##8) R(t##9)
R(0) R(1) R(2) R(3) R(4) R(5) R(6) R(7) R(8) R(9) R10(1) R10(2) R10(3) R10(4) R10(5) R10(6) R10(7) R10(8) R10(9)
#define P10(t) r##t##0, r##t##1, r##t##2, r##t##3, r##t##4, r##t##5, r##t##6, r##t##7, r##t##8, r##t##9
static void *recs[100] = { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, P10(1), P10(2), P10(3), P10(4), P10(5), P10(6),
                           P10(7), P10(8), P10(9) };
static void *vt_hw[78], *vt_sys[7], *vt_sh[52], *vt_api[100];
static Fake hw = { vt_hw, "HW", {0} }, sys = { vt_sys, "SYS", {0} };
static unsigned char cfg[4096];

static void fake_init(Fake *f, void **vt, const char *tag) { memset(f, 0, sizeof *f); f->vt = vt; f->tag = tag; }

/* ---------------- IMaterialVar fake ---------------- */
typedef struct FakeVar {
    void **vt; char *str; int ival; float vec[4]; unsigned char type; unsigned char pad[3]; int defined; int idx;
} FakeVar;
_Static_assert(offsetof(FakeVar, ival) == 0x10 && offsetof(FakeVar, vec) == 0x14 && offsetof(FakeVar, type) == 0x24,
               "IMaterialVar field layout");
static Rec *var_rec(FakeVar *v, int slot) { return rec_new("VAR", slot, v); }
static void v_SetFloat(FakeVar *v, float x) {
    Rec *r = var_rec(v, 3); if (r) r->f[0] = x;
    v->vec[0] = v->vec[1] = v->vec[2] = v->vec[3] = x; v->ival = (int)x; v->defined = 1;
}
static void v_SetInt(FakeVar *v, int x) {
    Rec *r = var_rec(v, 4); if (r) r->a = x;
    v->ival = x; v->vec[0] = v->vec[1] = v->vec[2] = v->vec[3] = (float)x; v->defined = 1;
}
static void v_SetVec4(FakeVar *v, float x, float y, float z, float w) {
    Rec *r = var_rec(v, 9); if (r) { r->f[0] = x; r->f[1] = y; r->f[2] = z; r->f[3] = w; }
    v->vec[0] = x; v->vec[1] = y; v->vec[2] = z; v->vec[3] = w; v->defined = 1;
}
static void v_SetVec3(FakeVar *v, float x, float y, float z) {
    Rec *r = var_rec(v, 10); if (r) { r->f[0] = x; r->f[1] = y; r->f[2] = z; }
    v->vec[0] = x; v->vec[1] = y; v->vec[2] = z; v->defined = 1;
}
static void v_SetVec2(FakeVar *v, float x, float y) {
    Rec *r = var_rec(v, 11); if (r) { r->f[0] = x; r->f[1] = y; }
    v->vec[0] = x; v->vec[1] = y; v->defined = 1;
}
static msbool v_IsDefined(FakeVar *v) { var_rec(v, 17); return v->defined ? 1 : 0; }
#define VR(n) static long long vr##n(FakeVar *v) { var_rec(v, n); return 0; }
VR(0) VR(1) VR(2) VR(5) VR(6) VR(7) VR(8) VR(12) VR(13) VR(14) VR(15) VR(16) VR(18) VR(19) VR(20) VR(21) VR(22) VR(23)
VR(24) VR(25) VR(26) VR(27) VR(28) VR(29) VR(30)
static void *vt_var[31] = { vr0, vr1, vr2, v_SetFloat, v_SetInt, vr5, vr6, vr7, vr8, v_SetVec4, v_SetVec3, v_SetVec2,
                            vr12, vr13, vr14, vr15, vr16, v_IsDefined, vr18, vr19, vr20, vr21, vr22, vr23, vr24, vr25,
                            vr26, vr27, vr28, vr29, vr30 };

typedef struct Params { FakeVar v[64]; FakeVar *p[64]; } Params;
/* Fresh params as materialsystem hands them to InitShaderParams: all undefined except what the VMT set. */
static void params_reset(Params *P) {
    memset(P, 0, sizeof *P);
    for (int i = 0; i < 64; i++) { P->v[i].vt = vt_var; P->v[i].idx = i; P->p[i] = &P->v[i]; }
}
/* Params after materialsystem's defaulting (COLOR 1,1,1, ALPHA 1) [C §3] */
static void params_defaulted(Params *P, float r, float g, float b) {
    params_reset(P);
    for (int i = 0; i < 64; i++) P->v[i].defined = 1;
    for (int k = 0; k < 3; k++) P->v[4].vec[k] = 1.0f;
    P->v[5].vec[0] = 1.0f;
    P->v[13].vec[0] = r; P->v[13].vec[1] = g; P->v[13].vec[2] = b;
}

/* ---------------- canonical trace text ---------------- */
static int is_bool_sh(int s) { return s == 2 || s == 3 || s == 4 || s == 13 || s == 14 || s == 15 || s == 20 || s == 27 || s == 48; }
static void fmt_rec(const Rec *r, char *o, size_t n) {
    if (!strcmp(r->tag, "SH")) {
        if (r->slot == 23 || r->slot == 24) snprintf(o, n, "SH[%d](\"%s\",%d)", r->slot, r->s, (int)r->b);
        else if (r->slot == 22) snprintf(o, n, "SH[22](%d,%d,%s,%d)", (int)r->a, (int)r->b, r->c ? "ptr" : "0", (int)r->d);
        else if (is_bool_sh(r->slot)) snprintf(o, n, "SH[%d](%d)", r->slot, (int)(r->a & 0xff));
        else if (r->slot == 1) snprintf(o, n, "SH[1](%d)", (int)r->a);
        else if (r->slot == 16 || r->slot == 19) snprintf(o, n, "SH[%d](%d,%d)", r->slot, (int)r->a, (int)r->b);
        else snprintf(o, n, "SH[%d]", r->slot);
    } else if (!strcmp(r->tag, "API")) {
        if (r->slot == 30 || r->slot == 31)
            snprintf(o, n, "API[%d](%d,{%g,%g,%g,%g},%d,%d)", r->slot, (int)r->a, r->f[0], r->f[1], r->f[2], r->f[3],
                     (int)r->c, (int)(r->d & 0xff));
        else if (r->slot == 40 || r->slot == 41) snprintf(o, n, "API[%d](%d)", r->slot, (int)r->a);
        else snprintf(o, n, "API[%d]", r->slot);
    } else if (!strcmp(r->tag, "SYS")) {
        if (r->slot == 4) snprintf(o, n, "SYS[4](%d)", (int)(r->a & 0xff));
        else snprintf(o, n, "SYS[%d]", r->slot);
    } else if (!strcmp(r->tag, "VAR")) {
        int idx = ((FakeVar *)r->self)->idx;
        if (r->slot == 4) snprintf(o, n, "VAR%d[4](0x%x)", idx, (unsigned)r->a);
        else if (r->slot == 3) snprintf(o, n, "VAR%d[3](%g)", idx, r->f[0]);
        else if (r->slot >= 9 && r->slot <= 11) snprintf(o, n, "VAR%d[%d](%g,%g,%g,%g)", idx, r->slot, r->f[0], r->f[1], r->f[2], r->f[3]);
        else snprintf(o, n, "VAR%d[%d]", idx, r->slot);
    } else snprintf(o, n, "%s[%d]", r->tag, r->slot);
}
/* records [from, to) joined with "; " */
static const char *trace_text(const Trace *t, int from, int to, char *buf, size_t n) {
    size_t used = 0; buf[0] = 0;
    for (int i = from; i < to && i < t->n; i++) {
        char one[160]; fmt_rec(&t->r[i], one, sizeof one);
        used += snprintf(buf + used, used < n ? n - used : 0, "%s%s", i > from ? "; " : "", one);
    }
    if (t->overflow) snprintf(buf + (used < n ? used : n - 1), used < n ? n - used : 1, " <overflow>");
    return buf;
}
static int trace_find(const Trace *t, int from, const char *tag, int slot) {
    for (int i = from; i < t->n; i++) if (!strcmp(t->r[i].tag, tag) && t->r[i].slot == slot) return i;
    return -1;
}
static int trace_count_tag(const Trace *t, const char *tag) {
    int k = 0; for (int i = 0; i < t->n; i++) k += !strcmp(t->r[i].tag, tag); return k;
}

/* ---------------- factories ---------------- */
static char g_asked[16][64]; static int g_nasked;
static int g_withhold_sys;
static void *factory(const char *name, int *rc) {
    void *r = NULL;
    if (g_nasked < 16) snprintf(g_asked[g_nasked++], 64, "%s", name);
    if (!strcmp(name, "MaterialSystemHardwareConfig012")) r = &hw;
    else if (!strcmp(name, "VMaterialSystemConfig002")) r = cfg;
    else if (!strcmp(name, "ShaderSystem002") && !g_withhold_sys) r = &sys;
    if (rc) *rc = r ? 0 : 1;
    return r;
}
static const char *asked_text(char *buf, size_t n) {
    size_t u = 0; buf[0] = 0;
    for (int i = 0; i < g_nasked; i++) u += snprintf(buf + u, u < n ? n - u : 0, "%s%s", i ? "," : "", g_asked[i]);
    return buf;
}

/* ---------------- ABI call helpers ---------------- */
typedef struct { void **vt; } Obj;
#define VT(o) (*(void ***)(o))
typedef msbool (*ConnectFn)(void *, CreateInterfaceFn, int);
typedef void (*DisconnectFn)(void *, int);
typedef void (*DrawFn)(void *, void *, int, void *, void *, int, void **);
static msbool dll_connect(void *d, CreateInterfaceFn f, int b) { return ((ConnectFn)VT(d)[0])(d, f, b); }
static void dll_disconnect(void *d, int b) { ((DisconnectFn)VT(d)[1])(d, b); }
static int dll_count(void *d) { return ((int (*)(void *))VT(d)[2])(d); }
static void *dll_get(void *d, int i) { return ((void *(*)(void *, int))VT(d)[3])(d, i); }
static const char *sh_name(void *s) { return ((const char *(*)(void *))VT(s)[0])(s); }
static int sh_nparams(void *s) { return ((int (*)(void *))VT(s)[2])(s); }
static const char *sh_pname(void *s, int i) { return ((const char *(*)(void *, int))VT(s)[6])(s, i); }
static const char *sh_phelp(void *s, int i) { return ((const char *(*)(void *, int))VT(s)[7])(s, i); }
static int sh_ptype(void *s, int i) { return ((int (*)(void *, int))VT(s)[8])(s, i); }
static const char *sh_pdef(void *s, int i) { return ((const char *(*)(void *, int))VT(s)[9])(s, i); }
static int sh_pflags(void *s, int i) { return ((int (*)(void *, int))VT(s)[14])(s, i); }
static void sh_draw(void *s, Params *P, void *shadow, void *api, void **ctx) { ((DrawFn)VT(s)[5])(s, P->p, 0, shadow, api, 0, ctx); }
static void *find_shader(void *dll, const char *name) {
    int n = dll_count(dll);
    for (int i = 0; i < n; i++) { void *s = dll_get(dll, i); if (s && !strcmp(sh_name(s), name)) return s; }
    return NULL;
}
static const char *names_text(void *dll, char *buf, size_t n) {
    size_t u = 0; buf[0] = 0; int c = dll_count(dll);
    for (int i = 0; i < c; i++) u += snprintf(buf + u, u < n ? n - u : 0, "%s%s", i ? "," : "", sh_name(dll_get(dll, i)));
    return buf;
}

static void *load_dll4(const char *dir, const char *file, HMODULE *hm) {
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\%s", dir, file);
    HMODULE h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (hm) *hm = h;
    if (!h) { printf("FAIL load %s (%lu)\n", path, GetLastError()); return NULL; }
    CreateInterfaceFn ci = (CreateInterfaceFn)(void *)GetProcAddress(h, "CreateInterface");
    return ci ? ci("ShaderDLL004", NULL) : NULL;
}

/* ---------------- tests ---------------- */
static Trace g_main_trace;
static void tr_reset(void) { memset(&g_main_trace, 0, sizeof g_main_trace); tl_trace = &g_main_trace; }

static void expect_trace(const char *what, const char *want) {
    static char buf[16384];
    trace_text(&g_main_trace, 0, g_main_trace.n, buf, sizeof buf);
    CHECK(!strcmp(buf, want), "%s:\n   got  %s\n   want %s", what, buf, want);
    if (!strcmp(buf, want)) printf("  ok %s: %s\n", what, buf);
}

static int g_test_variant;   /* 1 = photon_shaders_test build (PHOTON_TEST_CLONES), 0 = release build */

/* <bindir>\..\..\garrysmod\cfg\photon_shaders.txt: the module-relative config path (bindir mimics bin\win64) */
static void cfg_path(const char *bindir, char *out, size_t n) {
    snprintf(out, n, "%s\\..\\..\\garrysmod\\cfg\\photon_shaders.txt", bindir);
}

static void expect_names(void *dll, const char *what, const char *want_test, const char *want_release) {
    char b[512];
    const char *want = g_test_variant ? want_test : want_release;
    CHECK(dll_connect(dll, factory, 0), "connect (%s)", what);
    names_text(dll, b, sizeof b);
    CHECK(!strcmp(b, want), "%s: got \"%s\", want \"%s\"", what, b, want);
    if (!strcmp(b, want)) printf("  ok %s: \"%s\"\n", what, b);
    dll_disconnect(dll, 0);
}

static void *g_mod_copy;   /* game_shader_dx9.dll copy (mod mode), kept for the final no-base-copy case */

static void test_filter(const char *bindir, void *photon) {
    printf("[4] registration filter (%s build; PHOTON_TEST_FILTER=1 adds clones VertexLitGeneric [stock_name=1], "
           "LightmappedGeneric [stock_name=0], g_bloom)\n", g_test_variant ? "test" : "release");
    SetEnvironmentVariableA("PHOTON_TEST_FILTER", "1");
    SetEnvironmentVariableA("PHOTON_SHADOW", "VertexLitGeneric,LightmappedGeneric");
    tl_trace = NULL;
    CHECK(GetModuleHandleA("shaderapidx9_stock.dll") == NULL, "stock shaderapi already loaded");
    expect_names(photon, "photon_shaders.dll, no proxy, shadow=VLG,LMG (mod mode)", "Photon_Debug,Photon_Debug30,Photon_Debug32", "Photon_Debug,Photon_Debug30,Photon_Debug32");

    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\shaderapidx9_stock.dll", bindir);
    HMODULE stock = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(stock != NULL, "load %s (%lu)", path, GetLastError());
    expect_names(photon, "photon_shaders.dll, proxy present, shadow=VLG,LMG",
                 "Photon_Debug,Photon_Debug30,Photon_Debug32,VertexLitGeneric,LightmappedGeneric", "Photon_Debug,Photon_Debug30,Photon_Debug32");
    SetEnvironmentVariableA("PHOTON_SHADOW", "VertexLitGeneric");
    expect_names(photon, "photon_shaders.dll, proxy present, shadow=VLG (LMG without the flag needs opt-in too)",
                 "Photon_Debug,Photon_Debug30,Photon_Debug32,VertexLitGeneric", "Photon_Debug,Photon_Debug30,Photon_Debug32");
    SetEnvironmentVariableA("PHOTON_SHADOW", NULL);
    expect_names(photon, "photon_shaders.dll, proxy present, no opt-in", "Photon_Debug,Photon_Debug30,Photon_Debug32", "Photon_Debug,Photon_Debug30,Photon_Debug32");

    /* config file, found relative to the module (the cwd is build/tests/shaderhost, which has no garrysmod\cfg) */
    char cp[MAX_PATH]; cfg_path(bindir, cp, sizeof cp);
    FILE *f = fopen(cp, "w");
    CHECK(f != NULL, "write %s", cp);
    if (f) { fputs("// photon test\nPHOTON_SHADOW=VertexLitGeneric\n", f); fclose(f); }
    CHECK(GetFileAttributesA("garrysmod\\cfg\\photon_shaders.txt") == INVALID_FILE_ATTRIBUTES,
          "cwd-relative config exists: the test would not prove the module-relative path");
    expect_names(photon, "photon_shaders.dll, proxy present, opt-in from <dll>\\..\\..\\garrysmod\\cfg\\photon_shaders.txt",
                 "Photon_Debug,Photon_Debug30,Photon_Debug32,VertexLitGeneric", "Photon_Debug,Photon_Debug30,Photon_Debug32");
    DeleteFileA(cp);

    /* proxy install + fallback copy: the mod copy must register nothing while our base copy is loaded (analysis/10
     * §1.4: any repeated name is the fatal "trying to override a base shader" Error) */
    SetEnvironmentVariableA("PHOTON_SHADOW", "VertexLitGeneric,LightmappedGeneric");
    g_mod_copy = load_dll4(bindir, "game_shader_dx9.dll", NULL);
    CHECK(g_mod_copy && g_mod_copy != photon, "game_shader_dx9.dll copy is a separate module");
    if (g_mod_copy) {
        CHECK(GetModuleHandleA("photon_shaders.dll") != NULL, "base copy loaded");
        CHECK(dll_connect(photon, factory, 0), "connect base copy");
        expect_names(g_mod_copy, "game_shader_dx9.dll, proxy present, base copy loaded+connected", "", "");
        dll_disconnect(photon, 0);
        expect_names(g_mod_copy, "game_shader_dx9.dll, proxy present, base copy loaded", "", "");
    }
    SetEnvironmentVariableA("PHOTON_TEST_FILTER", NULL);
    SetEnvironmentVariableA("PHOTON_SHADOW", NULL);
}

/* Last: unload the base copy, then the mod copy (stock shaderapi still loaded) must register only Photon_* names. */
static void test_mod_without_base(HMODULE hp) {
    printf("[11] mod copy without the base copy\n");
    if (!g_mod_copy) { CHECK(0, "no mod copy"); return; }
    CHECK(FreeLibrary(hp), "FreeLibrary photon_shaders.dll");
    CHECK(GetModuleHandleA("photon_shaders.dll") == NULL, "photon_shaders.dll still loaded");
    SetEnvironmentVariableA("PHOTON_TEST_FILTER", "1");
    SetEnvironmentVariableA("PHOTON_SHADOW", "VertexLitGeneric,LightmappedGeneric");
    expect_names(g_mod_copy, "game_shader_dx9.dll, proxy present, no base copy, shadow=VLG,LMG", "Photon_Debug,Photon_Debug30,Photon_Debug32",
                 "Photon_Debug,Photon_Debug30,Photon_Debug32");
    SetEnvironmentVariableA("PHOTON_TEST_FILTER", NULL);
    SetEnvironmentVariableA("PHOTON_SHADOW", NULL);
}

/* stock SetInitialShadowState prefix of Fillrate's snapshot: records before the first SH[15], minus Fillrate's own
 * leading EnableDepthTest(0), EnableDepthWrites(0) (FUN_180003d00, C §4.2). */
static void stock_prefix(char *out, size_t n) {
    int e = trace_find(&g_main_trace, 0, "SH", 15);
    if (e < 2) { snprintf(out, n, "<no SH[15] in stock trace>"); return; }
    trace_text(&g_main_trace, 0, e - 2, out, n);
}
static void our_prefix(char *out, size_t n) {
    int e = trace_find(&g_main_trace, 0, "SH", 22);
    trace_text(&g_main_trace, 0, e < 0 ? g_main_trace.n : e, out, n);
}
/* the two records right before the first SYS[3] (CBaseShader::Draw override) */
static void pre_snapshot(char *out, size_t n) {
    int e = trace_find(&g_main_trace, 0, "SYS", 3);
    if (e < 2) { snprintf(out, n, "<none>"); return; }
    trace_text(&g_main_trace, e - 2, e, out, n);
}

typedef struct ThreadArg { void *shader; int t, iters, errors; Fake api; Fake shadow; Params P; Trace tr; char first_err[4096]; } ThreadArg;
static DWORD WINAPI draw_thread(LPVOID pv) {
    ThreadArg *A = pv;
    tl_trace = &A->tr;
    params_defaulted(&A->P, 0, 0, 0);
    A->api.ret[4] = A->t == 0 ? 0 : 2;    /* fog: thread 1 in water-fog mode */
    A->api.ret[34] = A->t == 0 ? 0 : 3;   /* bones */
    int vsi = (A->t == 0 ? 0 : 1) + (A->t == 0 ? 0 : 2);
    for (int i = 0; i < A->iters; i++) {
        float r = 0.1f * (float)(A->t + 1), g = (float)(i % 97) / 97.0f, b = (float)(i % 13) / 13.0f;
        A->P.v[13].vec[0] = r; A->P.v[13].vec[1] = g; A->P.v[13].vec[2] = b;
        A->P.v[5].vec[0] = A->t == 0 ? 1.0f : 0.5f;
        memset(&A->tr, 0, sizeof A->tr);
        void *ctx = NULL;
        int snap = (i % 100) == 0;
        if (snap) sh_draw(A->shader, &A->P, &A->shadow, NULL, &ctx);
        sh_draw(A->shader, &A->P, NULL, &A->api, &ctx);
        char want[512], got[2048];
        float c[4] = { 2.0f * r * 1.0f, 2.0f * g * 1.0f, 2.0f * b * 1.0f, 2.0f * A->P.v[5].vec[0] };
        snprintf(want, sizeof want, "%sAPI[31](0,{%g,%g,%g,%g},1,0); API[4]; API[34]; API[40](%d); API[41](0); SYS[4](1)",
                 snap ? "SH[0]; SH[22](1,1,0,0); SH[23](\"fillrate_vs20\",0); HW[56]; HW[62]; SH[24](\"fillrate_ps20b\",0); SYS[3]; " : "",
                 c[0], c[1], c[2], c[3], vsi);
        trace_text(&A->tr, 0, A->tr.n, got, sizeof got);
        int bad = strcmp(got, want) != 0 || ctx != NULL;
        for (int k = 0; k < A->tr.n && !bad; k++) {
            const Rec *rr = &A->tr.r[k];
            if (!strcmp(rr->tag, "API") && rr->self != &A->api) bad = 1;
            if (!strcmp(rr->tag, "SH") && rr->self != &A->shadow) bad = 1;
        }
        if (bad && A->errors++ == 0) snprintf(A->first_err, sizeof A->first_err, "iter %d: got %s | want %s", i, got, want);
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3 || (strcmp(argv[2], "test") && strcmp(argv[2], "release"))) {
        printf("usage: shaderhost <bindir> test|release\n"); return 2;
    }
    const char *bindir = argv[1];
    g_test_variant = !strcmp(argv[2], "test");
    for (int i = 0; i < 78; i++) vt_hw[i] = recs[i];
    for (int i = 0; i < 7; i++) vt_sys[i] = recs[i];
    for (int i = 0; i < 52; i++) vt_sh[i] = recs[i];
    for (int i = 0; i < 100; i++) vt_api[i] = recs[i];
    hw.ret[32] = hw.ret[44] = 95; hw.ret[56] = hw.ret[64] = 1;
    SetDllDirectoryA(bindir);
    char b[2048];

    /* [2] CreateInterface */
    printf("[2] CreateInterface\n");
    HMODULE hp;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\photon_shaders.dll", bindir);
    hp = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hp) { printf("FAIL load %s (%lu)\nSHADERHOST FAIL\n", path, GetLastError()); return 1; }
    CHECK(GetProcAddress(hp, "BuiltDebug") == NULL, "BuiltDebug exported");
    CreateInterfaceFn ci = (CreateInterfaceFn)(void *)GetProcAddress(hp, "CreateInterface");
    CHECK(ci != NULL, "no CreateInterface");
    if (!ci) { printf("SHADERHOST FAIL\n"); return 1; }
    int rc = 7;
    void *photon = ci("ShaderDLL004", &rc);
    CHECK(photon && rc == 0, "ShaderDLL004 -> %p rc %d", photon, rc);
    rc = 7; void *other = ci("ShaderDLL003", &rc);
    CHECK(!other && rc == 1, "ShaderDLL003 -> %p rc %d", other, rc);
    CHECK(ci("ShaderDLL004", NULL) == photon, "NULL rc pointer accepted");
    printf("  ok ShaderDLL004 %p rc 0; ShaderDLL003 NULL rc 1\n", photon);
    if (!photon) { printf("SHADERHOST FAIL\n"); return 1; }

    /* [3] Connect: failure path, then real argument shape with garbage bool upper bits */
    printf("[3] Connect\n");
    tr_reset(); g_nasked = 0; g_withhold_sys = 1;
    msbool ok = dll_connect(photon, factory, 0xdeadbe00);
    CHECK((ok & 0xff) == 0, "Connect without ShaderSystem002 must fail (got %d)", ok);
    asked_text(b, sizeof b);
    CHECK(!strcmp(b, "MaterialSystemHardwareConfig012,VMaterialSystemConfig002,ShaderSystem002"), "factory calls: %s", b);
    CHECK(g_main_trace.n == 0, "no calls into fakes after a failed Connect (%d)", g_main_trace.n);
    CHECK(dll_count(photon) == 0, "no shaders after a failed Connect");
    printf("  ok withheld ShaderSystem002 -> 0; asked %s; %d fake calls\n", b, g_main_trace.n);
    g_withhold_sys = 0; g_nasked = 0;
    ok = dll_connect(photon, factory, 0xdeadbe01);   /* true with garbage upper bits: no VEngineCvar007 */
    CHECK((ok & 0xff) == 1, "Connect(.., 0xdeadbe01) -> %d", ok);
    asked_text(b, sizeof b);
    CHECK(!strcmp(b, "MaterialSystemHardwareConfig012,VMaterialSystemConfig002,ShaderSystem002"), "factory calls (is_ms): %s", b);
    printf("  ok bIsMaterialSystem=0xdeadbe01 (true) -> 1; asked %s\n", b);
    dll_disconnect(photon, 0xdeadbe01);
    g_nasked = 0;
    ok = dll_connect(photon, factory, 0xdeadbe00);   /* false with garbage upper bits: the real materialsystem shape */
    CHECK((ok & 0xff) == 1, "Connect(.., 0xdeadbe00) -> %d", ok);
    asked_text(b, sizeof b);
    CHECK(!strcmp(b, "MaterialSystemHardwareConfig012,VMaterialSystemConfig002,ShaderSystem002,VEngineCvar007"),
          "factory calls (not ms): %s", b);
    printf("  ok bIsMaterialSystem=0xdeadbe00 (false) -> 1; asked %s (VEngineCvar007 NULL accepted)\n", b);
    CHECK(g_main_trace.n == 0, "Connect made %d calls into fakes", g_main_trace.n);

    /* [4] ShaderCount / GetShader / names */
    printf("[4] ShaderCount / GetShader\n");
    int n = dll_count(photon);
    CHECK(n == 3, "ShaderCount %d", n);
    CHECK(dll_get(photon, n) == NULL && dll_get(photon, -1) == NULL, "GetShader out of range");
    void *dbg = dll_get(photon, 0);
    CHECK(dbg && !strcmp(sh_name(dbg), "Photon_Debug"), "shader 0 name %s", dbg ? sh_name(dbg) : "(null)");
    void *dbg30 = dll_get(photon, 1);
    CHECK(dbg30 && !strcmp(sh_name(dbg30), "Photon_Debug30"), "shader 1 name %s", dbg30 ? sh_name(dbg30) : "(null)");
    printf("  ok count %d, [0] %s, [1] %s, GetShader(%d) = GetShader(-1) = NULL\n", n, dbg ? sh_name(dbg) : "?",
           dbg30 ? sh_name(dbg30) : "?", n);
    dll_disconnect(photon, 0);
    test_filter(bindir, photon);
    tr_reset();
    CHECK(dll_connect(photon, factory, 0) == 1, "reconnect");
    dbg = find_shader(photon, "Photon_Debug");
    if (!dbg) { printf("FAIL no Photon_Debug\nSHADERHOST FAIL\n"); return 1; }

    /* [5] params vs stock Fillrate */
    printf("[5] params vs stock stdshader_dbg Fillrate\n");
    void *dbgdll = load_dll4(bindir, "stdshader_dbg.dll", NULL);
    CHECK(dbgdll != NULL, "stdshader_dbg ShaderDLL004");
    void *fill = NULL;
    if (dbgdll) {
        tl_trace = NULL;
        CHECK(dll_connect(dbgdll, factory, 1) == 1, "stock Connect");   /* is_ms=1: no ConVar registration here */
        fill = find_shader(dbgdll, "Fillrate");
        CHECK(fill != NULL, "stock Fillrate");
    }
    CHECK(sh_nparams(dbg) == 14, "GetNumParams %d", sh_nparams(dbg));
    if (fill) {
        int same = 0;
        for (int i = 0; i < 13; i++) {
            const char *a = sh_pname(dbg, i), *s = sh_pname(fill, i);
            int eq = a && s && !strcmp(a, s) && sh_ptype(dbg, i) == sh_ptype(fill, i) && sh_pflags(dbg, i) == sh_pflags(fill, i)
                     && !strcmp(sh_pdef(dbg, i), sh_pdef(fill, i));
            CHECK(eq, "param %d: ours %s/%d/%d/\"%s\" stock %s/%d/%d/\"%s\"", i, a, sh_ptype(dbg, i), sh_pflags(dbg, i),
                  sh_pdef(dbg, i), s, sh_ptype(fill, i), sh_pflags(fill, i), sh_pdef(fill, i));
            same += eq;
        }
        printf("  ok %d/13 standard params equal stock (name, type, flags, default)\n", same);
    }
    CHECK(!strcmp(sh_pname(dbg, 13), "$photoncolor") && sh_ptype(dbg, 13) == 2 && !strcmp(sh_pdef(dbg, 13), "[1 0 1]"),
          "param 13 %s type %d", sh_pname(dbg, 13), sh_ptype(dbg, 13));
    CHECK(sh_pname(dbg, 14) == NULL && sh_pname(dbg, -1) == NULL && sh_pname(dbg, 1000) == NULL, "out-of-range name");
    CHECK(sh_ptype(dbg, 14) == 0 && sh_pflags(dbg, 99) == 0 && !strcmp(sh_pdef(dbg, 14), "") && !strcmp(sh_phelp(dbg, -5), ""),
          "out-of-range getters");
    printf("  ok [13] $photoncolor type 2 default \"[1 0 1]\"; out-of-range indices safe\n");

    /* [6] InitShaderParams */
    printf("[6] InitShaderParams / InitShaderInstance\n");
    static Params P;
    params_reset(&P);
    tr_reset();
    ((void (*)(void *, void *, const char *))VT(dbg)[3])(dbg, P.p, "shaderhost");
    expect_trace("undefined vars", "VAR13[17]; VAR13[10](1,0,1,0); VAR2[4](0x40000)");
    params_reset(&P); P.v[13].defined = 1; P.v[13].vec[0] = 0.2f; P.v[2].ival = 0x10;
    tr_reset();
    ((void (*)(void *, void *, const char *))VT(dbg)[3])(dbg, P.p, "shaderhost");
    expect_trace("$photoncolor defined, FLAGS2 0x10", "VAR13[17]; VAR2[4](0x40010)");
    tr_reset();
    ((void (*)(void *, void *, void *, const char *, const char *))VT(dbg)[4])(dbg, P.p, &sys, "shaderhost", "Other");
    expect_trace("InitShaderInstance", "");

    /* [7] snapshot */
    printf("[7] DrawElements snapshot\n");
    static Fake shadow, api;
    fake_init(&shadow, vt_sh, "SH"); fake_init(&api, vt_api, "API");
    void *ctx = NULL;
    params_defaulted(&P, 0.2f, 0.6f, 1.0f);
    tr_reset(); sh_draw(dbg, &P, &shadow, NULL, &ctx);
    expect_trace("snapshot (HW 56=1, 62=0)",
                 "SH[0]; SH[22](1,1,0,0); SH[23](\"fillrate_vs20\",0); HW[56]; HW[62]; SH[24](\"fillrate_ps20b\",0); SYS[3]");
    hw.ret[62] = 1;
    tr_reset(); sh_draw(dbg, &P, &shadow, NULL, &ctx);
    expect_trace("snapshot (HW 62=1)",
                 "SH[0]; SH[22](1,1,0,0); SH[23](\"fillrate_vs20\",0); HW[56]; HW[62]; SH[24](\"fillrate_ps20b\",1); SYS[3]");
    hw.ret[62] = 0; hw.ret[56] = 0;
    tr_reset(); sh_draw(dbg, &P, &shadow, NULL, &ctx);
    expect_trace("snapshot (no ps20b)", "SH[0]; SH[22](1,1,0,0); SH[23](\"fillrate_vs20\",0); HW[56]; SH[24](\"fillrate_ps20\",0); SYS[3]");
    hw.ret[56] = 1;
    CHECK(ctx == NULL, "ctx set by snapshot");
    /* differential: SetInitialShadowState under FLAGS bits vs stock Fillrate (CBaseShader in shaderlib) */
    static const int flagsets[] = { 1 << 15, 1 << 13, 1 << 28, 1 << 16, 1 << 10, 1 << 29,
                                    (1 << 15) | (1 << 13) | (1 << 28) | (1 << 16) | (1 << 10) | (1 << 29) };
    for (unsigned k = 0; fill && k < sizeof flagsets / sizeof flagsets[0]; k++) {
        char ours[1024], stock[1024];
        params_defaulted(&P, 0.2f, 0.6f, 1.0f); P.v[0].ival = flagsets[k];
        tr_reset(); sh_draw(dbg, &P, &shadow, NULL, &ctx); our_prefix(ours, sizeof ours);
        CHECK(trace_count_tag(&g_main_trace, "API") == 0, "snapshot touched api");
        params_defaulted(&P, 0, 0, 0); P.v[0].ival = flagsets[k];
        tr_reset(); sh_draw(fill, &P, &shadow, NULL, &ctx); stock_prefix(stock, sizeof stock);
        CHECK(!strcmp(ours, stock), "flags 0x%x: ours %s | stock %s", flagsets[k], ours, stock);
        if (!strcmp(ours, stock)) printf("  ok flags 0x%08x initial state = stock: %s\n", flagsets[k], ours);
    }
    /* differential: CBaseShader::Draw override (config +0x42, FLAGS NO_DEBUG_OVERRIDE) */
    for (int nodbg = 0; fill && nodbg < 2; nodbg++) {
        char ours[512], stock[512];
        cfg[0x42] = 1;
        params_defaulted(&P, 0.2f, 0.6f, 1.0f); P.v[0].ival = nodbg ? 2 : 0;
        tr_reset(); sh_draw(dbg, &P, &shadow, NULL, &ctx); pre_snapshot(ours, sizeof ours);
        params_defaulted(&P, 0, 0, 0); P.v[0].ival = nodbg ? 2 : 0;
        tr_reset(); sh_draw(fill, &P, &shadow, NULL, &ctx); pre_snapshot(stock, sizeof stock);
        cfg[0x42] = 0;
        CHECK(!strcmp(ours, stock), "cfg+0x42 override (NO_DEBUG_OVERRIDE=%d): ours %s | stock %s", nodbg, ours, stock);
        if (!strcmp(ours, stock)) printf("  ok cfg+0x42=1 NO_DEBUG_OVERRIDE=%d before TakeSnapshot = stock: %s\n", nodbg, ours);
    }

    /* [8] dynamic */
    printf("[8] DrawElements dynamic\n");
    params_defaulted(&P, 0.2f, 0.6f, 1.0f);
    tr_reset(); ctx = NULL; sh_draw(dbg, &P, NULL, &api, &ctx);
    expect_trace("dynamic (0.2,0.6,1) x 1, alpha 1",
                 "API[31](0,{0.4,1.2,2,2},1,0); API[4]; API[34]; API[40](0); API[41](0); SYS[4](1)");
    CHECK(trace_count_tag(&g_main_trace, "SH") == 0 && ctx == NULL, "dynamic touched shadow or ctx");
    params_defaulted(&P, 0.5f, 1.0f, 1.0f); P.v[4].vec[0] = 0.2f; P.v[5].vec[0] = 0.5f;
    api.ret[4] = 2; api.ret[34] = 3;
    tr_reset(); sh_draw(dbg, &P, NULL, &api, &ctx);
    expect_trace("dynamic $color (0.2,1,1), alpha 0.5, water fog, 3 bones",
                 "API[31](0,{0.2,2,2,1},1,0); API[4]; API[34]; API[40](3); API[41](0); SYS[4](1)");
    api.ret[4] = api.ret[34] = 0;

    /* [8b] Photon_Debug30: own SM3 VCS (T3.3, analysis/11 §6.2); HW[64] = SupportsShaderModel_3_0 */
    printf("[8b] Photon_Debug30\n");
    dbg30 = find_shader(photon, "Photon_Debug30");
    CHECK(dbg30 != NULL, "no Photon_Debug30");
    if (dbg30) {
        CHECK(sh_nparams(dbg30) == 14 && !strcmp(sh_pname(dbg30, 13), "$photoncolor") && sh_ptype(dbg30, 13) == 2 &&
              !strcmp(sh_pdef(dbg30, 13), "[1 0 1]"), "Photon_Debug30 params");
        for (int i = 0; i < 13; i++)
            CHECK(!strcmp(sh_pname(dbg30, i), sh_pname(dbg, i)) && !strcmp(sh_pdef(dbg30, i), sh_pdef(dbg, i)),
                  "Photon_Debug30 param %d differs from Photon_Debug", i);
        printf("  ok 14 params, same as Photon_Debug ($photoncolor type 2 default \"[1 0 1]\")\n");
        params_reset(&P);
        tr_reset();
        ((void (*)(void *, void *, const char *))VT(dbg30)[3])(dbg30, P.p, "shaderhost");
        expect_trace("SM3 init (no HW skinning: the VS has no bones)", "VAR13[17]; VAR13[10](1,0,1,0); HW[64]");
        params_defaulted(&P, 0.2f, 0.6f, 1.0f);
        tr_reset(); ctx = NULL; sh_draw(dbg30, &P, &shadow, NULL, &ctx);
        expect_trace("SM3 snapshot", "SH[0]; HW[64]; SH[22](1,1,0,0); SH[23](\"photon_debug30_vs30\",0); "
                     "SH[24](\"photon_debug30_ps30\",0); SYS[3]");
        tr_reset(); sh_draw(dbg30, &P, NULL, &api, &ctx);
        expect_trace("SM3 dynamic (0.2,0.6,1), alpha 1: c0 not doubled",
                     "HW[64]; API[31](0,{0.2,0.6,1,1},1,0); API[40](0); API[41](0); SYS[4](1)");
        params_defaulted(&P, 0.5f, 1.0f, 1.0f); P.v[4].vec[0] = 0.2f; P.v[5].vec[0] = 0.5f;
        api.ret[4] = 2; api.ret[34] = 3;
        tr_reset(); sh_draw(dbg30, &P, NULL, &api, &ctx);
        expect_trace("SM3 dynamic $color (0.2,1,1), alpha 0.5, fog/bones ignored",
                     "HW[64]; API[31](0,{0.1,1,1,0.5},1,0); API[40](0); API[41](0); SYS[4](1)");
        api.ret[4] = api.ret[34] = 0;
        CHECK(ctx == NULL, "Photon_Debug30 set ctx");
        hw.ret[64] = 0;   /* no SM3: Photon_Debug's fillrate path, HW skinning advertised */
        params_reset(&P);
        tr_reset();
        ((void (*)(void *, void *, const char *))VT(dbg30)[3])(dbg30, P.p, "shaderhost");
        expect_trace("no-SM3 init", "VAR13[17]; VAR13[10](1,0,1,0); HW[64]; VAR2[4](0x40000)");
        params_defaulted(&P, 0.2f, 0.6f, 1.0f);
        tr_reset(); sh_draw(dbg30, &P, &shadow, NULL, &ctx);
        expect_trace("no-SM3 snapshot", "SH[0]; HW[64]; SH[22](1,1,0,0); SH[23](\"fillrate_vs20\",0); HW[56]; HW[62]; "
                     "SH[24](\"fillrate_ps20b\",0); SYS[3]");
        tr_reset(); sh_draw(dbg30, &P, NULL, &api, &ctx);
        expect_trace("no-SM3 dynamic (doubled for fillrate_ps20b)",
                     "HW[64]; API[31](0,{0.4,1.2,2,2},1,0); API[4]; API[34]; API[40](0); API[41](0); SYS[4](1)");
        hw.ret[64] = 1;
    }

    /* [8c] Photon_Debug32 (T3.4a2): PS output = c32, which the shader itself never writes (the proxy writes it on the
     * D3D9 device); no-SM3 fallback = fillrate magenta, never $photoncolor */
    printf("[8c] Photon_Debug32\n");
    void *dbg32 = find_shader(photon, "Photon_Debug32");
    CHECK(dbg32 != NULL, "no Photon_Debug32");
    if (dbg32) {
        CHECK(sh_nparams(dbg32) == 14 && !strcmp(sh_pname(dbg32, 13), "$photoncolor"), "Photon_Debug32 params");
        params_reset(&P);
        tr_reset();
        ((void (*)(void *, void *, const char *))VT(dbg32)[3])(dbg32, P.p, "shaderhost");
        expect_trace("SM3 init", "VAR13[17]; VAR13[10](1,0,1,0); HW[64]");
        params_defaulted(&P, 0.2f, 0.6f, 1.0f);
        tr_reset(); ctx = NULL; sh_draw(dbg32, &P, &shadow, NULL, &ctx);
        expect_trace("SM3 snapshot", "SH[0]; HW[64]; SH[22](1,1,0,0); SH[23](\"photon_debug30_vs30\",0); "
                     "SH[24](\"photon_debug32_ps30\",0); SYS[3]");
        tr_reset(); sh_draw(dbg32, &P, NULL, &api, &ctx);
        expect_trace("SM3 dynamic: no PS constant written (c32 belongs to the proxy)",
                     "HW[64]; API[40](0); API[41](0); SYS[4](1)");
        CHECK(ctx == NULL, "Photon_Debug32 set ctx");
        hw.ret[64] = 0;
        tr_reset(); sh_draw(dbg32, &P, NULL, &api, &ctx);
        expect_trace("no-SM3 dynamic: fillrate magenta (doubled), not $photoncolor",
                     "HW[64]; API[31](0,{2,0,2,2},1,0); API[4]; API[34]; API[40](0); API[41](0); SYS[4](1)");
        hw.ret[64] = 1;
    }

    /* [9] misc + reload */
    printf("[9] misc slots, Disconnect, reload\n");
    params_defaulted(&P, 0, 0, 0);
    int mod = ((int (*)(void *, void *, void *))VT(dbg)[10])(dbg, P.p, &api);
    int tr0 = ((msbool (*)(void *, void *))VT(dbg)[13])(dbg, P.p) & 0xff;
    P.v[0].ival = 1 << 21;
    int tr1 = ((msbool (*)(void *, void *))VT(dbg)[13])(dbg, P.p) & 0xff;
    const char *fb = ((const char *(*)(void *, void *))VT(dbg)[1])(dbg, P.p);
    int fl = ((int (*)(void *))VT(dbg)[15])(dbg);
    int p2 = ((msbool (*)(void *, void *, int))VT(dbg)[11])(dbg, P.p, 0x7700) & 0xff;
    int ff = ((msbool (*)(void *, void *, int))VT(dbg)[12])(dbg, P.p, 0x7701) & 0xff;
    void *swvs = ((void *(*)(void *))VT(dbg)[16])(dbg);
    CHECK(mod == 0 && tr0 == 0 && tr1 == 1 && fb == NULL && fl == 1 && !p2 && !ff && !swvs,
          "mod %d translucent %d/%d fallback %s flags %d pow2 %d full %d swvs %p", mod, tr0, tr1, fb, fl, p2, ff, swvs);
    printf("  ok ComputeModulationFlags 0, IsTranslucent 0/1 (bit 21), fallback NULL, GetFlags 1, Needs*FB 0\n");
    dll_disconnect(photon, 0);
    CHECK(dll_count(photon) == 0, "count after Disconnect");
    CHECK(dll_connect(photon, factory, 0) == 1 && dll_count(photon) == 3 && dll_get(photon, 0) == dbg,
          "second Connect cycle");
    printf("  ok Disconnect(0); second Connect -> same IShader %p\n", dbg);

    /* [10] two threads x 10k dynamic draws with per-thread fakes and params */
    printf("[10] threads\n");
    static ThreadArg T[2];
    HANDLE th[2];
    for (int t = 0; t < 2; t++) {
        memset(&T[t], 0, sizeof T[t]);
        T[t].shader = dbg; T[t].t = t; T[t].iters = 10000;
        fake_init(&T[t].api, vt_api, "API"); fake_init(&T[t].shadow, vt_sh, "SH");
        th[t] = CreateThread(NULL, 0, draw_thread, &T[t], 0, NULL);
    }
    WaitForMultipleObjects(2, th, TRUE, INFINITE);
    for (int t = 0; t < 2; t++) {
        CHECK(T[t].errors == 0, "thread %d: %d bad iterations; first: %s", t, T[t].errors, T[t].first_err);
        if (!T[t].errors) printf("  ok thread %d: %d dynamic draws (+%d snapshots), traces exact, no cross-talk\n", t,
                                 T[t].iters, T[t].iters / 100);
        CloseHandle(th[t]);
    }
    dll_disconnect(photon, 0);
    if (dbgdll) dll_disconnect(dbgdll, 1);
    test_mod_without_base(hp);

    if (g_fail) { printf("SHADERHOST FAIL (%d)\n", g_fail); return 1; }
    printf("SHADERHOST OK\n");
    return 0;
}
