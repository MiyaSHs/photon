/* Photon CPU: shared declarations of the module's own files (the host contract is src/common/photon_cpu_host.h). */
#ifndef PHOTON_CPU_H
#define PHOTON_CPU_H
#include <stdint.h>
#include <stddef.h>
#include "photon_cpu_host.h"

const PhotonCpuHost *cpu_host(void);
#define cpu_log(...) cpu_host()->log(__VA_ARGS__)
uint32_t cpu_fnv(const void *p, size_t n);   /* FNV-1a, as the proxy's build checks */
double cpu_tsc_mhz(void);                    /* rdtsc ticks per microsecond, measured once */

int cpu_caller_stack(void **out, int n);     /* the caller's return addresses, frames inside photon_cpu.dll skipped */
const char *cpu_where(void *addr, char *buf, size_t n);   /* "module+rva" */
int cpu_module_range(const char *name, uintptr_t *lo, uintptr_t *hi);
int cpu_setting(const char *key, const char *env, int def);   /* env var, else data/photon/settings.txt, else def */

void probe_bones_present(uint64_t frame);    /* C3: the bone probe and the C3c fix */
void probe_views_present(uint64_t frame);    /* C4: PHOTON_CPU_PROBE_VIEWS=1 */
void water_present(uint64_t frame);          /* C4a: PHOTON_CPU_WATER=1, PHOTON_CPU_PROBE_WATER=1 */
void waterclip_push(int mode, uintptr_t engine, void *rv, void **rv_vt, const void *main_view_setup, int vol, int eye_in_water,
                    const void *view, void *rt, void *frustum);   /* C4c: PHOTON_CPU_WATERCLIP=1|2 (water.c installs it) */
void waterclip_report(void);
const void *waterclip_visdata(void);
void waterclip_cancel(void);
void lua_load_init(void);                    /* phase 2 F1: PHOTON_CPU_PROBE_LUALOAD=1 */
void lua_load_present(uint64_t frame);
void fs_cache_init(void);                    /* phase 2 F5: on by default; PHOTON_CPU_FSCACHE=0 off, =2 check */
void fs_cache_present(uint64_t frame);
void jitprof_register(void *L);               /* phase 2 E1: PHOTON_CPU_PROBE_JIT=1 */
void physvalid_init(void);                   /* GORE: on by default; PHOTON_CPU_PHYSVALID=0 off, =2 check */
void physvalid_present(uint64_t frame);
void tickcap_init(void);                     /* P1: PHOTON_CPU_TICKCAP=N (setting "photon_cpu_tickcap"), off by default */
void tickcap_present(uint64_t frame);
int dyn_steps(const char *const **names);   /* DYN: the CPU steps for Photon Render's controller (host contract v3, pending) */
void dyn_set(int step, int engaged);
int dyn_engaged(const char *name);
double dyn_main_busy_ms(void);
int dyn_available(int step);
double dyn_saving(int step);                 /* host contract v5 (pending): expected ms a frame saved */                 /* host contract v4 (pending): the step has something to act on */
void dyn_present(uint64_t frame);
void sampler_init(void);                     /* PHYS diagnosis: the main-thread sampling profiler (PHOTON_CPU_SAMPLE=1) */
void physgov_init(void);                     /* P2: the physics governor, on by default (photon_cpu_physgov) */
void physgov_present(uint64_t frame);
#endif
