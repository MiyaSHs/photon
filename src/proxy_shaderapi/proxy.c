/* Photon proxy shaderapidx9.dll (FEASIBILITY.md section 8).
 *
 * Installation (section 8.1): the launcher loads "shaderapidx9.dll" by a hard-coded name, so this file takes that
 * name, and the original is renamed to shaderapidx9_stock.dll in the same directory. The only export is
 * CreateInterface: no PE imports from shaderapidx9, and exporting BuiltDebug would make Sys_LoadModule reject us.
 *
 * T1.2: the interfaces materialsystem drives are returned as generic per-slot thunk wrappers (wrap.c) that count
 *       calls per vtable slot.
 * T1.3: re-entry factories. Without these hooks materialsystem receives raw stock pointers again after the first
 *       mode set or alt-tab, and the proxy silently drops out (section 8.1, "the critical pitfall"):
 *   - IShaderDeviceMgr::Connect (slot 0): the materialsystem factory is wrapped, so VShaderUtil001 is wrapped.
 *   - IShaderUtil::RestoreShaderObjects (slot 16, materialsystem FUN_180022470): its shader factory argument is
 *     replaced by ours. The stock ReacquireResources passes its internal factory FUN_180023f80 here.
 *   - IShaderDeviceMgr::SetMode (slot 16, FUN_180027400): the factory it returns is wrapped.
 *   - IShaderDeviceMgr::QueryInterface (slot 2): results are wrapped by name.
 * T1.4: IShaderDevice::Present (slot 9) is the frame boundary; per-frame CSVs go to PHOTON_LOG_DIR (frames.c).
 * T1.5: build guard. Wrapping assumes the vtable layouts of the validated build. If the stock DLL's SHA1 or
 *       materialsystem's PE timestamp differs, the proxy passes everything through raw (PHOTON_FORCE=1 overrides).
 * T3.6 prerequisites: PHOTON_DIAG=1 adds diagnostic hooks (diag.h: R15 caps, R1 int render parm histogram, order
 *       trace); unset, nothing changes.
 * T3.4a2: d3dconst.h writes PS constants c32+ straight to stock's IDirect3DDevice9 (shaderapi's PS constant state
 *       ends at c31); exposed to tests through PhotonProxyStats001 (version 2). PHOTON_TEST_C32="r g b a" hooks
 *       RenderPass to write c32 for the Photon_Debug32 test; unset, no hook is installed.
 * T3.4c/f + plan v2 P1/P2: vlg.h, lighting (Option A): renames the SM2 pairs vertexlit_and_unlit_generic_vs20/_ps20b
 *       and _bump_vs20/_bump_ps20b to our SM3 photon_vlg_* / photon_bump_* for the pilot statics (pairing rule: both
 *       of one pair or neither), and writes the light block c32..c55 and the envmap occlusion c56 at RenderPass. Off
 *       (no hook at all) unless at least one pair's .vcs/.ids are installed (complete) and photon_lighting != 0
 *       (PHOTON_LIGHTING or garrysmod/cfg/photon.cfg); no renames while r_pixelfog is 0. Stats version 3
 *       (PhotonVlgStats version 3).
 * S1: IShaderDeviceMgr::Shutdown/Disconnect (slots 4/1) stop the P0 meter thread before the game unloads us; the
 *       thread pins this DLL until it exits (frames.h), and DllMain does no work at process termination.
 * P3g/P3h: ambvol.h, ambwork.h: the ambient volume, photon_ambient 1 only. Its workers stop at the same two hooks, first.
 * PHOTON_PASSTHROUGH=1 disables all wrapping (raw stock pointers). */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vkbridge.h"
#include "passtime.h"
#include "vktest.h"
#include "hiz.h"
#include "channels.h"
#include "ptrt.h"
#include "nanite.h"
#include "log.h"
#include "wrap.h"
#include "photon_stats.h"
#include "frames.h"
#include "diag.h"
#include "d3dconst.h"
#include "vlg.h"
#include "ambvol.h"
#include "cubemap.h"
#include "p6.h"
#include "shadows.h"
#include "cull.h"
#include "dmirror.h"
#include "lights.h"
#include <stdarg.h>
#include "../common/photon_cpu_host.h"
#include "dynq.h"
#include "taa.h"
#include "waterq.h"
#include "lodwork.h"
static const PhotonCpuModule *g_cpu;   /* Photon CPU, MODULES.md: set once by cpu_module_load */

typedef void *(*CreateInterfaceFn)(const char *name, int *ret);

static HMODULE g_self, g_stock;
static CreateInterfaceFn g_stock_ci;
static INIT_ONCE g_load_once = INIT_ONCE_STATIC_INIT;
static int g_passthrough;
static unsigned long long frames_u(void) { return photon_frame_count(); }
PhotonStats g_photon_stats = { .version = 3, .unwrap = photon_unwrap, .frame_end = photon_frame_end,
                                .flush = photon_frames_flush, .frames = frames_u,
                                .diag_frame = photon_diag_frame, .diag_dump = photon_diag_dump,
                                .d3d_device = photon_d3d_device, .d3d_set_ps_const = photon_d3d_set_ps_const,
                                .vlg = &g_photon_vlg, .vlg_snapshot_flags = photon_vlg_snapshot_flags,
                                .vlg_poll_config = photon_vlg_poll_config,
                                .vlg_device_state_lost = photon_vlg_device_state_lost };
#define g_stats g_photon_stats
static SRWLOCK g_cls_lock = SRWLOCK_INIT;

/* Factories captured from the game, called by our wrapping factories below. */
static CreateInterfaceFn volatile g_ms_factory;    /* materialsystem's factory, passed to IShaderDeviceMgr::Connect */
static CreateInterfaceFn volatile g_inner_factory; /* shaderapidx9's internal factory (SetMode / Restore) */

/* Wrapped interfaces, with slot counts measured on build 26229091 (T1.2). */
static const struct { const char *name; size_t expected; } k_wrapped[] = {
    { "ShaderApi030", 289 }, { "ShaderDevice001", 41 }, { "ShaderDeviceMgr001", 21 }, { "ShaderShadow010", 57 },
    { "VShaderUtil001", 0 },  /* materialsystem's IShaderUtil (CMaterialSystem secondary vtable); size measured */
    { "MaterialSystemHardwareConfig012", 78 },  /* T3.2a: slot 73 names our shader DLL (analysis/10) */
};
enum { SLOT_CONNECT = 0, SLOT_DISCONNECT = 1, SLOT_QUERYINTERFACE = 2, SLOT_MGR_SHUTDOWN = 4, SLOT_MGR_SETMODE = 16, SLOT_UTIL_RELEASE = 15, SLOT_UTIL_RESTORE = 16, SLOT_DEV_PRESENT = 9,
       SLOT_HW_SHADERDLL = 73 };

static void install_hooks(PhotonWrapClass *cls);

/* Returns the class for `name` (created from real's vtable on first use), or NULL if `name` is not wrapped. */
static int direct_wanted(const char *name);   /* below: PHOTON_DIRECT_ONLY */
static PhotonWrapClass *class_for(const char *name, void *real) {
    size_t k, nk = sizeof k_wrapped / sizeof k_wrapped[0];
    for (k = 0; k < nk; k++) if (strcmp(name, k_wrapped[k].name) == 0) break;
    if (k == nk) return NULL;
    AcquireSRWLockExclusive(&g_cls_lock);
    PhotonWrapClass *cls = NULL;
    for (size_t i = 0; i < g_stats.nclasses; i++)
        if (strcmp(g_stats.classes[i]->name, k_wrapped[k].name) == 0) cls = g_stats.classes[i];
    if (!cls && g_stats.nclasses < PHOTON_MAX_CLASSES) {
        void **vt = *(void ***)real;
        uintptr_t lo, hi;
        if (photon_module_text(vt[0], &lo, &hi))
            /* T4.6: direct on the validated build, except IShaderDeviceMgr (its hooks are the factory machinery, 23 calls a run;
             * analysis/20). Unknown builds (PHOTON_FORCE) stay wrapped: the internal-call guard is build-specific. */
            cls = photon_wrap_class(k_wrapped[k].name, vt, lo, hi, k_wrapped[k].expected,
                                    g_stats.build_ok && strcmp(k_wrapped[k].name, "ShaderDeviceMgr001") != 0 && direct_wanted(k_wrapped[k].name));
        if (cls) { install_hooks(cls); g_stats.classes[g_stats.nclasses++] = cls; }
    }
    ReleaseSRWLockExclusive(&g_cls_lock);
    return cls;
}

/* PHOTON_TEST_NOWRAP=name,name,...: those interfaces are handed out unwrapped (no hooks, no counts). Test only: it measures what T4.6
 * (analysis/20) could gain, with lighting off, where nothing on them needs a hook (analysis/19). */
static int in_list(const char *list, const char *name) {
    size_t k = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += k)
        if ((p == list || p[-1] == ',') && (p[k] == 0 || p[k] == ',')) return 1;
    return 0;
}
static int env_list(const char *var, char *list, size_t n, const char *what) {
    DWORD k = GetEnvironmentVariableA(var, list, (DWORD)n);
    if (k == 0 || k >= n) { list[0] = 0; return 0; }
    photon_log("TEST %s=%s: %s", var, list, what);
    return 1;
}
static int test_nowrap(const char *name) {
    static char list[512];
    static int read;
    if (!read) { env_list("PHOTON_TEST_NOWRAP", list, sizeof list, "those interfaces are not wrapped"); read = 1; }
    return in_list(list, name);
}
/* PHOTON_DIRECT_ONLY=name,...: only those interfaces go direct (T4.6 bisecting); unset = all eligible ones. */
static int direct_wanted(const char *name) {
    static char list[512];
    static int read, set;
    if (!read) { set = env_list("PHOTON_DIRECT_ONLY", list, sizeof list, "only those interfaces go direct"); read = 1; }
    return !set || in_list(list, name);
}

static void *maybe_wrap(const char *name, void *real) {
    if (!real || !name || g_passthrough) return real;
    if (test_nowrap(name)) return real;
    if (photon_wrap_known(real) && !photon_wrap_of(real)->direct) return real;   /* already one of our wrappers */
    PhotonWrapClass *cls = class_for(name, real);
    void *w = cls ? photon_wrap(cls, real) : real;
    if (cls) photon_diag_note(cls->name, w, real);   /* PHOTON_DIAG=1 only */
    if (cls) photon_vol_note_iface(cls->name, real);
    return w;
}

/* --- wrapping factories ------------------------------------------------------------------------------------- */
static void *ms_factory_wrap(const char *name, int *ret) {
    CreateInterfaceFn f = g_ms_factory;
    void *p = f ? f(name, ret) : NULL;
    if (!f && ret) *ret = 1;
    void *w = maybe_wrap(name, p);
    photon_log("ms-factory(\"%s\") -> %p%s", name ? name : "(null)", w, w != p ? " (wrapped)" : "");
    return w;
}
static void *inner_factory_wrap(const char *name, int *ret) {
    CreateInterfaceFn f = g_inner_factory;
    void *p = f ? f(name, ret) : NULL;
    if (!f && ret) *ret = 1;
    void *w = maybe_wrap(name, p);
    photon_log("inner-factory(\"%s\") -> %p%s", name ? name : "(null)", w, w != p ? " (wrapped)" : "");
    return w;
}

void *photon_ms_factory_query(const char *name) {
    CreateInterfaceFn f = g_ms_factory;
    return f ? f(name, NULL) : NULL;
}

/* Formats a code address as module+RVA for logs (who called a hook). */
static const char *where(void *addr, char *buf, size_t n) {
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        char *b = strrchr(name, '\\');
        if (b) memmove(name, b + 1, strlen(b));
    }
    snprintf(buf, n, "%s+0x%llx", name, (unsigned long long)((char *)addr - (char *)m));
    return buf;
}

/* Diagnostic (build-specific, guarded by T1.5): materialsystem's own interface globals must hold our wrappers.
 * RVAs from materialsystem CMaterialSystem::Connect FUN_180019990 / FUN_180022470 (build 26229091). */
static void check_ms_globals(void) {
    HMODULE ms = GetModuleHandleA("materialsystem.dll");
    if (!ms || !g_stats.build_ok) return;
    static const struct { const char *name; uintptr_t rva; } g[] = {
        { "g_pShaderAPI", 0x20ff878 }, { "g_pShaderDeviceMgr", 0x20ff880 }, { "g_pShaderDevice", 0x20ff888 },
        { "g_pShaderShadow", 0x20ff890 },
    };
    char line[256]; int n = 0, raw = 0;
    for (size_t i = 0; i < sizeof g / sizeof g[0]; i++) {
        void *p = *(void **)((char *)ms + g[i].rva);
        PhotonWrap *w = p ? photon_wrap_of(p) : NULL;
        raw += !w;
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%s", g[i].name, w ? (w->direct ? "direct" : "wrapped") : (p ? "RAW" : "null"));
    }
    photon_log("materialsystem globals (frame %llu):%s%s", (unsigned long long)photon_frame_count(), line,
               raw ? "  <-- WARNING raw pointer" : "");
}

/* --- hooks (called with our wrapper as `this`) ---------------------------------------------------------------- */
typedef unsigned char (*ConnectFn)(void *self, CreateInterfaceFn factory);
typedef void *(*QueryInterfaceFn)(void *self, const char *name);
typedef CreateInterfaceFn (*SetModeFn)(void *self, void *hwnd, int adapter, const void *mode);
typedef void (*RestoreShaderObjectsFn)(void *self, CreateInterfaceFn factory, int change_flags);

static unsigned char hook_mgr_connect(void *self, CreateInterfaceFn factory) {
    PhotonWrap *w = photon_wrap_of(self);
    if (factory && factory != ms_factory_wrap) g_ms_factory = factory;
    unsigned char ok = ((ConnectFn)w->cls->real_vtbl[SLOT_CONNECT])(w->real, factory ? ms_factory_wrap : NULL);
    photon_log("IShaderDeviceMgr::Connect(%p) -> %d", (void *)factory, ok);
    return ok;
}
/* S1 (hang on quit): materialsystem shuts the device manager down (Shutdown and Disconnect) before the engine
 * FreeLibrary's us; the meter thread is stopped here, outside the loader lock (frames.h). */
static void hook_mgr_shutdown(void *self) {
    PhotonWrap *w = photon_wrap_of(self);
    photon_amb_shutdown("IShaderDeviceMgr::Shutdown");   /* P3g: the ambient workers, same rules as the meter thread */
    photon_cube_shutdown("IShaderDeviceMgr::Shutdown");  /* P4a: the pick table's worker (PHOTON_TEST_CUBEPROBE only) */
    photon_lg_shutdown("IShaderDeviceMgr::Shutdown");    /* P5: the light grid's worker and texture (photon_lights 1 only) */
    photon_frames_shutdown("IShaderDeviceMgr::Shutdown");
    photon_vol_release();   /* stock's own shutdown path does not go through ReleaseShaderObjects */
    ((void (*)(void *))w->cls->real_vtbl[SLOT_MGR_SHUTDOWN])(w->real);
}
static void hook_mgr_disconnect(void *self) {
    PhotonWrap *w = photon_wrap_of(self);
    photon_amb_shutdown("IShaderDeviceMgr::Disconnect");
    photon_cube_shutdown("IShaderDeviceMgr::Disconnect");
    photon_lg_shutdown("IShaderDeviceMgr::Disconnect");
    photon_frames_shutdown("IShaderDeviceMgr::Disconnect");
    ((void (*)(void *))w->cls->real_vtbl[SLOT_DISCONNECT])(w->real);
}
static void *hook_mgr_queryinterface(void *self, const char *name) {
    PhotonWrap *w = photon_wrap_of(self);
    void *p = ((QueryInterfaceFn)w->cls->real_vtbl[SLOT_QUERYINTERFACE])(w->real, name);
    void *r = maybe_wrap(name, p);
    photon_log("IShaderDeviceMgr::QueryInterface(\"%s\") -> %p%s", name ? name : "(null)", r, r != p ? " (wrapped)" : "");
    return r;
}
static CreateInterfaceFn hook_mgr_setmode(void *self, void *hwnd, int adapter, const void *mode) {
    PhotonWrap *w = photon_wrap_of(self);
    CreateInterfaceFn f = ((SetModeFn)w->cls->real_vtbl[SLOT_MGR_SETMODE])(w->real, hwnd, adapter, mode);
    char buf[MAX_PATH + 32];
    photon_log("IShaderDeviceMgr::SetMode(%p, %d) -> factory %p from %s", hwnd, adapter, (void *)f,
               where(__builtin_return_address(0), buf, sizeof buf));
    if (!f) return f;
    if (f != inner_factory_wrap) g_inner_factory = f;
    return inner_factory_wrap;
}
static void hook_util_restore(void *self, CreateInterfaceFn factory, int change_flags) {
    PhotonWrap *w = photon_wrap_of(self);
    char buf[MAX_PATH + 32];
    photon_log("IShaderUtil::RestoreShaderObjects(%p, %d) from %s", (void *)factory, change_flags,
               where(__builtin_return_address(0), buf, sizeof buf));
    if (factory && factory != inner_factory_wrap) g_inner_factory = factory;
    photon_vlg_device_state_lost();
    photon_vol_restore();
    photon_lg_restore();
    ((RestoreShaderObjectsFn)w->cls->real_vtbl[SLOT_UTIL_RESTORE])(w->real, factory ? inner_factory_wrap : NULL,
                                                                    change_flags);
    if (!g_passthrough) check_ms_globals();   /* materialsystem re-fetched its interfaces inside that call */
}

/* IShaderUtil::ReleaseShaderObjects (slot 15): stock's last call before a device Reset (FUN_180026eb0). */
static void hook_util_release(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    photon_vol_release();
    photon_lg_release();
    ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t))w->cls->real_vtbl[SLOT_UTIL_RELEASE])(w->real, a, b, c);
}

/* IShaderDevice::Present (slot 9, stock 0x180026900, the only IDirect3DDevice9::Present call site): frame boundary. */
static void hook_dev_present(void *self) {
    PhotonWrap *w = photon_wrap_of(self);
    photon_dmirror_flush();      /* PHOTON_BATCH=replay: the engine's device state back before Present (dmirror.c lazy restore) */
    photon_shadows_present();   /* T4.2: the frame's last segment ends (the pass may run) */
    photon_cull_pre_present();   /* PHOTON_CULL=1 only (T4.7a): box queries against the finished frame's depth */
    photon_vk_present();   /* VK0: Photon's Vulkan passes through DXVK's interop (only once a pass registered) */
    photon_dynq_present_begin();   /* DYN: the time blocked in Present tells a GPU-bound frame */
    ((void (*)(void *))w->cls->real_vtbl[SLOT_DEV_PRESENT])(w->real);
    photon_dynq_present_end();     /* DYN: the controller */
    photon_taa_present();          /* AA: the next frame's jitter (PHOTON_TAA=1) */
    photon_frame_end();
    photon_passtime_frame();   /* OPT-VK: PHOTON_PASSTIME=1 only */
    unsigned long long f = photon_frame_count();
    static DWORD last_tid;
    DWORD tid = GetCurrentThreadId();
    int tid_changed = last_tid && tid != last_tid;   /* e.g. queued mode starting: Present moves to MatQueue */
    if (tid_changed) photon_log("Present thread changed %lu -> %lu at frame %llu", last_tid, tid, f);
    last_tid = tid;
    if (!g_passthrough && (f == 1 || f % 600 == 0 || tid_changed)) check_ms_globals();
    photon_diag_frame();   /* PHOTON_DIAG=1 only (T3.6 prerequisites, diag.h) */
    photon_d3d_frame();    /* PHOTON_TEST_C32 only: periodic summary */
    photon_vlg_frame();    /* T3.4c lighting: config re-read, periodic summary (only when lighting is wanted) */
    photon_vol_frame();    /* photon_ambient 1 or PHOTON_TEST_VOL only */
    photon_cube_frame();   /* PHOTON_TEST_CUBEPROBE only (P4a) */
    photon_p6_frame();     /* PHOTON_P6=1 only: hooks the engine's DrawModelExecute once in game */
    photon_shadows_frame();   /* T4.2 stage 0: caps and depth-surface probe, once */
    photon_dmirror_frame();   /* PHOTON_BATCH=mirror only (T4.5 stage 2a) */
    photon_lg_frame();     /* photon_lights 1 only (P5): the engine's lights to the grid worker */
    photon_waterq_present();   /* W1: water occlusion answers (only once photon_cpu asked, or PHOTON_WATERQ=1) */
    photon_lod_frame();    /* F3: PHOTON_LOD_PROBE=1 only, for now */
    if (g_cpu && g_cpu->on_present) g_cpu->on_present();   /* Photon CPU (MODULES.md) */
}

/* IHardwareConfigInternal slot 73 (+0x248, GetHWSpecificShaderDLLName; stock 0x180004790 returns NULL). materialsystem
 * loads the named DLL as a BASE shader DLL right after stdshader_dx9, so its shaders shadow stock names without the
 * fatal "Game shader ... trying to override a base shader" check that GAMEBIN DLLs hit (analysis/10). We return our
 * name only if bin/win64/photon_shaders.dll exists, wrapping is on, and PHOTON_NO_SHADERDLL is not set. */
static const char k_shader_dll[] = "photon_shaders.dll";
static int g_shader_dll_present = -1;
static const char *hook_hw_shaderdll(void *self) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SLOT_HW_SHADERDLL, -1)) return ((const char *(*)(void *))w->cls->real_vtbl[SLOT_HW_SHADERDLL])(w->real);
    const char *stock = ((const char *(*)(void *))w->cls->real_vtbl[SLOT_HW_SHADERDLL])(w->real);
    if (stock && stock[0]) return stock;                  /* never override a name the stock config set */
    if (g_shader_dll_present < 0) {
        char path[MAX_PATH], env[8];
        DWORD n = GetModuleFileNameA(g_self, path, sizeof path);
        char *slash = n && n < sizeof path ? strrchr(path, '\\') : NULL;
        int present = 0;
        if (slash && (size_t)(slash + 1 - path) + sizeof k_shader_dll <= sizeof path) {
            strcpy(slash + 1, k_shader_dll);
            present = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }
        if (GetEnvironmentVariableA("PHOTON_NO_SHADERDLL", env, sizeof env) > 0 && env[0] == '1') present = 0;
        g_shader_dll_present = present;
        photon_log("HWConfig slot 73: shader DLL %s %s", path, present ? "present -> will be loaded" : "absent/disabled");
    }
    return g_shader_dll_present ? k_shader_dll : stock;
}

static void install_hooks(PhotonWrapClass *cls) {
    if (strcmp(cls->name, "ShaderDevice001") == 0) photon_wrap_set_hook(cls, SLOT_DEV_PRESENT, (void *)hook_dev_present);
    else if (strcmp(cls->name, "ShaderDeviceMgr001") == 0) {
        photon_wrap_set_hook(cls, SLOT_CONNECT, (void *)hook_mgr_connect);
        photon_wrap_set_hook(cls, SLOT_DISCONNECT, (void *)hook_mgr_disconnect);
        photon_wrap_set_hook(cls, SLOT_MGR_SHUTDOWN, (void *)hook_mgr_shutdown);
        photon_wrap_set_hook(cls, SLOT_QUERYINTERFACE, (void *)hook_mgr_queryinterface);
        photon_wrap_set_hook(cls, SLOT_MGR_SETMODE, (void *)hook_mgr_setmode);
    } else if (strcmp(cls->name, "MaterialSystemHardwareConfig012") == 0) {
        photon_wrap_set_hook(cls, SLOT_HW_SHADERDLL, (void *)hook_hw_shaderdll);
    } else if (strcmp(cls->name, "VShaderUtil001") == 0) {
        photon_wrap_set_hook(cls, SLOT_UTIL_RESTORE, (void *)hook_util_restore);
        if (photon_vol_on || photon_amb_on || photon_lg_on) photon_wrap_set_hook(cls, SLOT_UTIL_RELEASE, (void *)hook_util_release);
    }
    photon_diag_install(cls);   /* no-op unless PHOTON_DIAG=1 */
    photon_d3d_install(cls);    /* ShaderApi030: slot 31 guard; RenderPass hook only with PHOTON_TEST_C32 */
    photon_vlg_install(cls);    /* T3.4c lighting v0: no-op unless photon_vlg_setup() found it wanted */
    photon_vol_install(cls);    /* P3d: no-op unless PHOTON_TEST_VOL=1 */
    photon_cube_install(cls);   /* P4a: no-op unless PHOTON_TEST_CUBEPROBE=1 */
}


/* --- T1.5 build guard ------------------------------------------------------------------------------------------- */
/* Validated build: Steam buildid 26229091, x86-64 branch (FEASIBILITY.md header; game/stock/SHA1SUMS). */
static const char k_stock_sha1[] = "bcd58cdef176cae8066a4aee5766ba8a665b60c4";   /* shaderapidx9.dll */
static const DWORD k_ms_timestamp = 0x6adfbd0c;                                 /* materialsystem.dll PE stamp */

static int sha1_file(const char *path, char out_hex[41]) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; int ok = 0;
    unsigned char buf[65536], dig[20]; DWORD got;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        ok = 1;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) if (BCryptHashData(h, buf, got, 0) != 0) { ok = 0; break; }
        if (ok && BCryptFinishHash(h, dig, sizeof dig, 0) == 0)
            for (int i = 0; i < 20; i++) sprintf(out_hex + 2 * i, "%02x", dig[i]);
        else ok = 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

/* Returns 1 if the build matches. materialsystem is checked only when it is loaded (in-game it always is,
 * because it loads us). */
static int check_build(const char *stock_path) {
    char hex[41] = {0};
    int ok = sha1_file(stock_path, hex) && strcmp(hex, k_stock_sha1) == 0;
    photon_log("build guard: stock sha1 %s (%s)", hex[0] ? hex : "?", ok ? "match" : "MISMATCH");
    HMODULE ms = GetModuleHandleA("materialsystem.dll");
    if (ms) {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)ms;
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((unsigned char *)ms + dos->e_lfanew);
        int ms_ok = nt->FileHeader.TimeDateStamp == k_ms_timestamp;
        photon_log("build guard: materialsystem timestamp 0x%lx (%s)", nt->FileHeader.TimeDateStamp,
                   ms_ok ? "match" : "MISMATCH");
        ok = ok && ms_ok;
    }
    return ok;
}

/* --- loading and the exported factory ------------------------------------------------------------------------ */
/* Photon CPU (MODULES.md, src/common/photon_cpu_host.h): photon_cpu.dll beside this DLL, unless PHOTON_CPU=0. Called from load_stock, outside
 * the loader lock. */
static void cpu_log(const char *fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    photon_log("cpu: %s", b);
}
static void cpu_module_load(const char *stock_path) {
    char e[4];
    if (GetEnvironmentVariableA("PHOTON_CPU", e, sizeof e) > 0 && e[0] == '0') return;
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s", stock_path);
    char *slash = strrchr(p, '\\');
    if (!slash || (size_t)(slash + 1 - p) + sizeof "photon_cpu.dll" > sizeof p) return;
    strcpy(slash + 1, "photon_cpu.dll");
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return;
    HMODULE m = LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    PhotonCpuInitFn init = m ? (PhotonCpuInitFn)(void *)GetProcAddress(m, PHOTON_CPU_INIT_EXPORT) : NULL;
    static PhotonCpuHost host;
    host.version = PHOTON_CPU_HOST_VERSION;
    host.log = cpu_log;
    host.frame_count = photon_frame_count;
    host.main_thread = GetCurrentThreadId();
    host.water_pixels = photon_waterq_last;
    host.hiz_hidden = photon_hiz_hidden; host.hiz_view_moved = photon_hiz_view_moved;   /* version 6 (T4.7b) */
    host.setting = photon_vlg_setting;   /* version 7 */
    host.push = photon_chan_push;        /* version 8 */
    g_cpu = init ? init(&host) : NULL;
    if (g_cpu) photon_waterq_set_possible();
    if (g_cpu && g_cpu->version >= 3 && g_cpu->dyn_steps && g_cpu->dyn_set) {   /* DYN: its steps into the controller's list */
        const char *const *names = NULL;
        int n = g_cpu->dyn_steps(&names);
        if (n > 0 && names) photon_dynq_set_cpu_steps(n, names, g_cpu->dyn_set, g_cpu->main_busy_ms, g_cpu->version >= 4 ? g_cpu->dyn_available : NULL,
                                                    g_cpu->version >= 5 ? g_cpu->dyn_saving : NULL);
    }
    photon_log("photon_cpu.dll: %s (module %p, init %p, version %d)", g_cpu ? "loaded" : m ? "present but not initialised" : "load failed",
               (void *)m, (void *)init, g_cpu ? g_cpu->version : 0);
}

static BOOL CALLBACK load_stock(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    photon_frames_note_main(GetCurrentThreadId());   /* P0 meters: the main thread (frames.h) */
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_self, path, sizeof path);
    char pt[8];
    g_passthrough = GetEnvironmentVariableA("PHOTON_PASSTHROUGH", pt, sizeof pt) > 0 && pt[0] == '1';
    g_stats.passthrough = g_passthrough;
    if (n == 0 || n >= sizeof path) return TRUE;
    char *slash = strrchr(path, '\\');
    if (!slash || (size_t)(slash + 1 - path) + sizeof "shaderapidx9_stock.dll" > sizeof path) return TRUE;
    strcpy(slash + 1, "shaderapidx9_stock.dll");
    /* Altered search path: the stock DLL's own imports (tier0, vstdlib, d3d9, ...) resolve from its directory. */
    g_stats.build_ok = check_build(path);
    char force[8];
    int forced = GetEnvironmentVariableA("PHOTON_FORCE", force, sizeof force) > 0 && force[0] == '1';
    if (!g_stats.build_ok && !forced) {
        g_passthrough = 1;
        g_stats.passthrough = 1;
        photon_log("build guard: unknown build -> pass-through (set PHOTON_FORCE=1 to wrap anyway)");
    }
    g_stock = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_stock) g_stock_ci = (CreateInterfaceFn)(void *)GetProcAddress(g_stock, "CreateInterface");
    photon_d3d_setup(g_stock, g_stats.build_ok && !g_passthrough);   /* build-specific RVAs: real build only */
    photon_vlg_setup(g_stock, g_stats.build_ok && !g_passthrough);   /* T3.4c: env/cfg + installed files */
    if (g_stats.build_ok && !g_passthrough) photon_vol_setup();      /* photon_ambient 1 (P3g, P3h) or the P3d probe, PHOTON_TEST_VOL=1 */
    if (g_stats.build_ok && !g_passthrough) photon_dmirror_setup();  /* T4.5 stage 2a, PHOTON_BATCH=mirror */
    if (g_stats.build_ok && !g_passthrough) photon_vktest_init();    /* VK0: the bridge's test pass (PHOTON_VK_TEST=1) */
    if (g_stats.build_ok && !g_passthrough) photon_ptrt_init();      /* PT-RT (PHOTON_PTRT=1) */
    if (g_stats.build_ok && !g_passthrough) photon_nanite_init();    /* NANITE (PHOTON_NANITE=1) */
    if (g_stats.build_ok && !g_passthrough) photon_hiz_init();       /* T4.7b (PHOTON_HIZ=1) */
    if (g_stats.build_ok && !g_passthrough) photon_p6_setup();       /* P6 stage 1, PHOTON_P6=1 (before the cubemap setup: P6 turns the pick on) */
    if (g_stats.build_ok && !g_passthrough) photon_cube_setup();
    if (g_stats.build_ok && !g_passthrough) photon_lg_setup();       /* P5: photon_lights 1 */
    photon_log("load stock %s -> module %p CreateInterface %p (err %lu) passthrough=%d", path, (void *)g_stock,
               (void *)g_stock_ci, g_stock ? 0UL : GetLastError(), g_passthrough);
    cpu_module_load(path);
    return TRUE;
}

__declspec(dllexport) void *CreateInterface(const char *name, int *ret) {
    InitOnceExecuteOnce(&g_load_once, load_stock, NULL, NULL);
    if (name && strcmp(name, PHOTON_STATS_VERSION) == 0) {
        if (ret) *ret = 0;
        return &g_stats;
    }
    if (!g_stock_ci) {
        if (ret) *ret = 1; /* IFACE_FAILED */
        return NULL;
    }
    void *real = g_stock_ci(name, ret);
    void *p = maybe_wrap(name, real);
    photon_log("CreateInterface(\"%s\") -> %p%s", name ? name : "(null)", p, p != real ? " (wrapped)" : "");
    return p;
}

/* DLL_PROCESS_DETACH runs under the loader lock: no waiting on threads here (S1).
 * - reserved == NULL: the game's FreeLibrary at quit (the normal case; other threads still run, so taking our log/CRT
 *   locks is safe): the exit summaries and photon_slots.csv are written.
 * - reserved != NULL: process termination. Every other thread was already killed, possibly inside a lock (heap, CRT
 *   stream, our log lock), so nothing is done. This happens when the module stayed loaded past the game's
 *   FreeLibrary, i.e. the meter thread still held its reference (no Shutdown, or the bounded stop wait timed out). */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        photon_wrap_restore_all();   /* T4.6: no object keeps a vtable in this DLL once it is unmapped */
        photon_p6_restore();
        photon_dmirror_restore();
        photon_diag_dump("exit");
        photon_d3d_dump("exit");
        photon_vlg_dump("exit");
        photon_vol_dump("exit");
        photon_cube_dump("exit");
        photon_p6_dump("exit");
        photon_shadows_dump("exit");
        photon_dmirror_dump("exit");
        photon_frames_flush();
    }
    return TRUE;
}
