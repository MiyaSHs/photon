/* Photon CPU (START.md): the module the proxy loads (src/common/photon_cpu_host.h). It logs once at init and every 600 presented frames,
 * and drives the probes and optimizations from the per-Present callback. Keep PhotonCpuInit's signature. */
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

static const PhotonCpuHost *g_host;
const PhotonCpuHost *cpu_host(void) { return g_host; }

uint32_t cpu_fnv(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x01000193u;
    return h;
}

double cpu_tsc_mhz(void) {
    static double mhz;
    if (mhz == 0) {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        unsigned long long t0 = __rdtsc();
        Sleep(20);
        unsigned long long t1 = __rdtsc();
        QueryPerformanceCounter(&b);
        mhz = (double)(t1 - t0) / ((double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart);
        g_host->log("photon_cpu: rdtsc %.0f MHz", mhz);
    }
    return mhz;
}

int cpu_module_range(const char *name, uintptr_t *lo, uintptr_t *hi) {
    HMODULE m = name ? GetModuleHandleA(name) : NULL;
    if (!name && !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCSTR)(void *)cpu_module_range, &m)) return 0;
    if (!m) return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    *lo = (uintptr_t)m;
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
    return 1;
}

int cpu_caller_stack(void **out, int n) {
    static uintptr_t self_lo, self_hi;
    if (!self_lo) cpu_module_range(NULL, &self_lo, &self_hi);
    void *fr[12] = { 0 };
    USHORT got = RtlCaptureStackBackTrace(0, 12, fr, NULL);
    int k = 0;
    for (USHORT i = 0; i < got && k < n; i++)
        if ((uintptr_t)fr[i] < self_lo || (uintptr_t)fr[i] >= self_hi) out[k++] = fr[i];
    for (int i = k; i < n; i++) out[i] = NULL;
    return k;
}

const char *cpu_where(void *a, char *buf, size_t n) {
    HMODULE m = NULL;
    if (!a || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)) {
        snprintf(buf, n, "%p", a);
        return buf;
    }
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(m, path, sizeof path);
    const char *base = path;
    for (DWORD i = 0; i < len; i++) if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
    snprintf(buf, n, "%s+%llx", len ? base : "?", (unsigned long long)((uintptr_t)a - (uintptr_t)m));
    return buf;
}

/* a CPU switch: the PHOTON_CPU_* environment variable if set (tests, launch options), else the key in Photon's settings file
 * (garrysmod/data/photon/settings.txt, "key value" lines written by the Photon settings window), else def */
int cpu_setting(const char *key, const char *env, int def) {
    char e[16];
    if (env && GetEnvironmentVariableA(env, e, sizeof e) > 0) return atoi(e);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (!n || n >= sizeof path) return def;
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    path[n] = 0;
    size_t l = strlen(path);
    if (l > 10 && !_stricmp(path + l - 10, "bin\\win64\\")) path[l - 10] = 0;   /* the x64 build runs from bin\win64 */
    if (strlen(path) + 40 >= sizeof path) return def;
    strcat(path, "garrysmod\\data\\photon\\settings.txt");
    FILE *f = fopen(path, "r");
    if (!f) return def;
    char line[256];
    int v = def;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && (line[kl] == ' ' || line[kl] == '\t')) { v = atoi(line + kl + 1); break; }
    fclose(f);
    return v;
}

/* shader packs' per-frame channel (host v8, photon-6d's channels.c): Lua's photon_cpu.Push (lua_bin.c) -> here -> the proxy. Main
 * thread. Returns 1 when the proxy took it, 0 when it has no push (older proxy). */
__declspec(dllexport) int photon_cpu_push(const char *channel, const float *v, int n) {
    if (!g_host || g_host->version < 8 || !g_host->push || !channel || n < 0) return 0;
    g_host->push(channel, v, n);
    return 1;
}

/* a Photon setting as resolved now (defaults, photon.cfg, settings.txt, shader packs, env; host v7), for the Lua module */
__declspec(dllexport) int photon_cpu_setting(const char *key, float *out) {
    int found = 0;
    if (!g_host || g_host->version < 7 || !g_host->setting || !key || !out) return 0;
    *out = g_host->setting(key, &found);
    return found;
}

static void on_present(void) {
    uint64_t f = g_host->frame_count();
    if (f % 600 == 0) g_host->log("photon_cpu: alive at frame %llu", (unsigned long long)f);
    probe_bones_present(f);
    probe_views_present(f);
    water_present(f);
    lua_load_present(f);
    fs_cache_present(f);
    physvalid_present(f);
    tickcap_present(f);
    dyn_present(f);
    physgov_present(f);
}

static const PhotonCpuModule k_module = { PHOTON_CPU_HOST_VERSION, on_present, dyn_steps, dyn_set, dyn_main_busy_ms, dyn_available,
                                          dyn_saving };   /* v3-5: DYN (dyn.c) */

__declspec(dllexport) const PhotonCpuModule *PhotonCpuInit(const PhotonCpuHost *host) {
    if (!host || host->version < 1) return NULL;
    g_host = host;
    host->log("photon_cpu: init (host version %d, main thread %lu)", host->version, host->main_thread);
    fs_cache_init();   /* before the load probe, which then times the cached calls */
    lua_load_init();
    physvalid_init();
    tickcap_init();
    physgov_init();
    sampler_init();
    return &k_module;
}
