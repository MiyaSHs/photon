/* photon_shaders.dll: ShaderDLL004 (IShaderDLLInternal) object, shader registry and name filter (analysis/11 §1, §3.1).
 *
 * materialsystem contract (analysis/scratch/m3_contract.md §1.1, verified in game by T3.2a): GetProcAddress
 * ("CreateInterface") -> CreateInterface("ShaderDLL004") -> slot 0 Connect(materialsystem factory, false) must return
 * true -> slot 2 ShaderCount -> slot 3 GetShader(i) -> IShader::GetName (strdup'ed as the dict key); unload calls
 * slot 1 Disconnect(false). The IShader objects must live until Disconnect (they are static here).
 *
 * Load mode (analysis/11 §1): the Connect call is identical as a base DLL (proxy HWConfig slot 73) and as a mod DLL
 * (GAMEBIN game_shader_dx9.dll). Base mode = our module basename is exactly photon_shaders.dll AND
 * shaderapidx9_stock.dll is loaded (proves our proxy is in the process). Anything else is mod mode.
 * materialsystem checks every name a mod DLL registers against all base DLLs and calls the fatal tier0 Error("Game
 * shader ... trying to override a base shader") on a clash (analysis/10 §1.4 [confirmed]); mod DLLs load in ModInit,
 * after every base DLL (10 §1.1, §1.3).
 * Registration filter, applied at every Connect:
 *   - never the 14 game_shader_generic_garrysmod names (runtime enumeration, analysis/11 §0), in any mode;
 *   - mod mode while our base copy photon_shaders.dll is also in the process (proxy install plus a fallback
 *     game_shader_dx9.dll copy): register nothing, since every name we have is already a base name (§1.4 reverse
 *     case, "a base DLL must still not register a name that a later mod DLL registers");
 *   - whether a name is a stock shadow is decided by the name, not by PhotonShaderDesc.stock_name: anything without
 *     the "Photon_" prefix is a shadow. Shadows only in base mode and only when listed in PHOTON_SHADOW (env, else
 *     the line "PHOTON_SHADOW=..." in <our dir>\..\..\garrysmod\cfg\photon_shaders.txt); mod mode registers only
 *     Photon_* names (analysis/11 §1). T3.2 ships no shadow; the test-variant clones (PHOTON_TEST_CLONES builds only)
 *     prove the filter;
 *   - no duplicate names. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "abi.h"
#include "baseshader.h"
#include "log.h"

PhotonEnv g_photon_env;

extern const PhotonShaderDesc photon_debug_desc, photon_debug30_desc, photon_debug32_desc, photon_debugvol_desc;
#ifdef PHOTON_TEST_CLONES
extern const PhotonShaderDesc photon_test_vlg_desc, photon_test_lmg_desc, photon_test_bloom_desc;
#endif

enum { MAX_SHADERS = 32 };
static IShader g_shader_objs[MAX_SHADERS];      /* one per candidate descriptor, static lifetime */
static IShader *g_registry[MAX_SHADERS];        /* rebuilt in Connect only, never touched by draws */
static int g_count;

static const char *const k_gmod_names[14] = {   /* game_shader_generic_garrysmod.dll, analysis/11 §0 [confirmed] */
    "g_bloom", "g_blurx", "g_blury", "g_bokehblur", "g_colourmodify", "g_downsample", "gmodscreenspace",
    "g_premultiplied", "g_refract", "g_sharpen", "g_sky", "g_sunbeams", "g_texturize", "Sobel",
};

static int is_gmod_name(const char *n) {
    for (int i = 0; i < 14; i++) if (_stricmp(n, k_gmod_names[i]) == 0) return 1;
    return 0;
}

#ifdef PHOTON_TEST_CLONES
static int env_flag(const char *name) {
    char v[8];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v && v[0] == '1';
}
#endif

/* PHOTON_SHADOW from the environment, else from the config file next to the game tree. The path is derived from our
 * own module path (bin\win64\photon_shaders.dll -> ..\..\garrysmod\cfg\photon_shaders.txt), never from the
 * working directory, which nothing guarantees to be the game root. */
static void read_shadow_list(const wchar_t *self_path, char *out, size_t cap, char *src, size_t srccap) {
    out[0] = 0;
    snprintf(src, srccap, "none");
    DWORD n = GetEnvironmentVariableA("PHOTON_SHADOW", out, (DWORD)cap);
    if (n > 0 && n < cap) { snprintf(src, srccap, "env"); return; }
    out[0] = 0;
    if (!self_path || !self_path[0]) return;
    wchar_t rel[MAX_PATH], full[MAX_PATH];
    const wchar_t *base = self_path;
    for (const wchar_t *p = self_path; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    int dirlen = (int)(base - self_path);
    static const wchar_t tail[] = L"..\\..\\garrysmod\\cfg\\photon_shaders.txt";
    if (dirlen + (int)(sizeof tail / sizeof tail[0]) > MAX_PATH) return;
    memcpy(rel, self_path, (size_t)dirlen * sizeof(wchar_t));
    memcpy(rel + dirlen, tail, sizeof tail);
    DWORD fn = GetFullPathNameW(rel, MAX_PATH, full, NULL);
    const wchar_t *path = (fn > 0 && fn < MAX_PATH) ? full : rel;
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, path8, (int)sizeof path8, NULL, NULL);
    FILE *f = _wfopen(path, L"r");
    snprintf(src, srccap, "%s %s", path8, f ? "(read)" : "(absent)");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "PHOTON_SHADOW=", 14) == 0) {
            snprintf(out, cap, "%s", line + 14);
            out[strcspn(out, "\r\n")] = 0;
            break;
        }
    fclose(f);
}

static int in_list(const char *list, const char *name) {
    size_t len = strlen(name);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == len && _strnicmp(p, name, len) == 0) return 1;
        p = e;
    }
    return 0;
}

/* Stock names are everything not prefixed "Photon_" (case-insensitive, like materialsystem's name lookups). */
static int is_photon_name(const char *n) { return _strnicmp(n, "Photon_", 7) == 0; }

typedef struct LoadMode {
    HMODULE self;
    wchar_t path[MAX_PATH];       /* our module's full path ("" if unknown) */
    char modname[MAX_PATH];       /* basename, UTF-8 */
    int base;                     /* 1 = base DLL (proxy route) */
    HMODULE base_copy;            /* another loaded module named photon_shaders.dll (our base copy), else NULL */
} LoadMode;

static void detect_mode(LoadMode *m) {
    memset(m, 0, sizeof *m);
    snprintf(m->modname, sizeof m->modname, "?");
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)detect_mode, &m->self)) {
        DWORD n = GetModuleFileNameW(m->self, m->path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) m->path[0] = 0;
    }
    HMODULE pb = GetModuleHandleA("photon_shaders.dll");
    m->base_copy = (pb && pb != m->self) ? pb : NULL;
    if (!m->path[0]) return;                        /* unknown module: mod mode (the safe side) */
    const wchar_t *base = m->path;
    for (const wchar_t *p = m->path; *p; p++) if (*p == L'\\' || *p == L'/') base = p + 1;
    WideCharToMultiByte(CP_UTF8, 0, base, -1, m->modname, (int)sizeof m->modname, NULL, NULL);
    m->base = _wcsicmp(base, L"photon_shaders.dll") == 0 && GetModuleHandleA("shaderapidx9_stock.dll") != NULL;
}

static void build_registry(void) {
    const PhotonShaderDesc *cand[MAX_SHADERS];
    int nc = 0;
    cand[nc++] = &photon_debug_desc;
    cand[nc++] = &photon_debug30_desc;   /* T3.3: own SM3 VCS (garrysmod/shaders/fxc/photon_debug30_*.vcs) */
    cand[nc++] = &photon_debugvol_desc;  /* P3d: samples the ambient volume the proxy binds on s15 */
    cand[nc++] = &photon_debug32_desc;   /* T3.4a2: PS output = c32, written by the proxy straight to the D3D9 device */
#ifdef PHOTON_TEST_CLONES
    if (env_flag("PHOTON_TEST_FILTER")) {
        cand[nc++] = &photon_test_vlg_desc; cand[nc++] = &photon_test_lmg_desc; cand[nc++] = &photon_test_bloom_desc;
    }
#endif

    LoadMode m;
    char shadow[512], src[MAX_PATH * 3 + 16];
    detect_mode(&m);
    g_count = 0;
    if (m.base) read_shadow_list(m.path, shadow, sizeof shadow, src, sizeof src);
    else { shadow[0] = 0; snprintf(src, sizeof src, "not read in mod mode"); }
    photon_log("mode: %s (module %s, shaderapidx9_stock %s, base copy photon_shaders.dll %s), PHOTON_SHADOW=\"%s\" [%s]",
               m.base ? "base" : "mod", m.modname, GetModuleHandleA("shaderapidx9_stock.dll") ? "loaded" : "absent",
               m.base_copy ? "loaded" : "absent", shadow, src);
    if (!m.base && m.base_copy) {
        /* proxy install plus a fallback mod copy: our base copy already registers every name we have, and any name a
         * mod DLL repeats from a base DLL is the fatal "trying to override a base shader" Error (10 §1.4) */
        photon_log("  mod copy with base photon_shaders.dll %p in the process: registering nothing", (void *)m.base_copy);
        return;
    }

    for (int i = 0; i < nc; i++) {
        const PhotonShaderDesc *d = cand[i];
        const char *why = NULL;
        int shadows = !is_photon_name(d->name);
        if (shadows != (d->stock_name != 0) && !is_gmod_name(d->name))
            photon_log("  note %s: stock_name=%d disagrees with its name; the name decides (%s)", d->name,
                       d->stock_name, shadows ? "stock shadow" : "Photon_ name");
        if (is_gmod_name(d->name)) why = "GMod game_shader_generic_garrysmod name (would be fatal)";
        else if (shadows && !m.base) why = "non-Photon_ name in mod mode (would be fatal)";
        else if (shadows && !in_list(shadow, d->name)) why = "stock shadow not opted in (PHOTON_SHADOW)";
        for (int j = 0; !why && j < g_count; j++)
            if (_stricmp(g_registry[j]->desc->name, d->name) == 0) why = "duplicate name";
        if (why) { photon_log("  drop %s: %s", d->name, why); continue; }
        g_shader_objs[i].vt = &g_photon_shader_vt;
        g_shader_objs[i].desc = d;
        g_registry[g_count++] = &g_shader_objs[i];
        photon_log("  register %s (%d params)", d->name, P_NUM_STANDARD + d->nparams);
    }
}

typedef struct { const char *name; void **out; int required; } Want;

static msbool sd_Connect(ShaderDLL *self, CreateInterfaceFn factory, int bIsMaterialSystem) {
    (void)self;
    int is_ms = (bIsMaterialSystem & 0xff) != 0;
    PhotonEnv e = { 0 };
    void *cfg = NULL;
    const Want want[3] = {
        { "MaterialSystemHardwareConfig012", (void **)&e.hw, 1 },
        { "VMaterialSystemConfig002", &cfg, 1 },
        { "ShaderSystem002", (void **)&e.sys, 1 },
    };
    photon_log("ShaderDLL004::Connect(factory %p, is_material_system=%d raw=0x%x)", (void *)factory, is_ms,
               (unsigned)bIsMaterialSystem);
    g_count = 0;
    if (!factory) return 0;
    for (int i = 0; i < 3; i++) {
        *want[i].out = factory(want[i].name, NULL);
        if (!*want[i].out) {
            photon_log("  %s missing -> Connect fails", want[i].name);
            memset(&g_photon_env, 0, sizeof g_photon_env);
            return 0;
        }
    }
    e.cfg = cfg;
    if (!is_ms) e.cvar = factory("VEngineCvar007", NULL);   /* optional; ConVars are a later task */
    g_photon_env = e;
    photon_log("  hw %p cfg %p sys %p cvar %p", (void *)e.hw, (const void *)e.cfg, (void *)e.sys, e.cvar);
    build_registry();
    return 1;
}

static void sd_Disconnect(ShaderDLL *self, int bIsMaterialSystem) {
    (void)self;
    photon_log("ShaderDLL004::Disconnect(is_material_system=%d)", (bIsMaterialSystem & 0xff) != 0);
    for (int i = 0; i < g_count; i++) {
        const PhotonShaderDesc *d = g_registry[i]->desc;
        photon_log("  %s: snapshot %ld dynamic %ld (draw-thread changes %ld)", d->name, d->stats->snapshot,
                   d->stats->dynamic, d->stats->tid_changes);
    }
    g_count = 0;
    memset(&g_photon_env, 0, sizeof g_photon_env);
}

static int sd_ShaderCount(ShaderDLL *self) {
    (void)self;
    photon_log("ShaderDLL004::ShaderCount -> %d", g_count);
    return g_count;
}
static IShader *sd_GetShader(ShaderDLL *self, int i) {
    (void)self;
    return (i >= 0 && i < g_count) ? g_registry[i] : NULL;
}
static msbool sd_Connect1(ShaderDLL *self, CreateInterfaceFn f) { return sd_Connect(self, f, 0); }
static void sd_Disconnect0(ShaderDLL *self) { sd_Disconnect(self, 0); }

static const ShaderDLLVtbl g_dll_vt = { sd_Connect, sd_Disconnect, sd_ShaderCount, sd_GetShader, sd_Connect1,
                                        sd_Disconnect0 };
static ShaderDLL g_dll = { &g_dll_vt };

__declspec(dllexport) void *CreateInterface(const char *name, int *rc) {
    if (name && strcmp(name, "ShaderDLL004") == 0) { if (rc) *rc = 0; return &g_dll; }
    photon_log("CreateInterface(\"%s\") -> NULL", name ? name : "(null)");
    if (rc) *rc = 1;
    return NULL;
}
