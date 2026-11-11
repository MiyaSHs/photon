/* The contract between Photon Render's proxy (shaderapidx9.dll) and Photon CPU (photon_cpu.dll, modules/cpu). MODULES.md: this file is
 * shared; change it only with both sessions agreeing, and only by appending fields (bump PHOTON_CPU_HOST_VERSION).
 *
 * The proxy is loaded early in every game start (materialsystem asks it for interfaces before any map). On its first CreateInterface it
 * loads photon_cpu.dll from its own directory if the file is there and PHOTON_CPU is not "0", and calls PhotonCpuInit once, outside the
 * loader lock. A missing DLL, a missing export or a NULL return just means no CPU module; the proxy logs it and carries on. */
#ifndef PHOTON_CPU_HOST_H
#define PHOTON_CPU_HOST_H
#include <stdint.h>

enum { PHOTON_CPU_HOST_VERSION = 8 };   /* 2: water_pixels (W1 / C4b, 2026-11-09); 3: the DYN step API (2026-11-09); 4: dyn_available (2026-11-10);
                                         * 5: dyn_saving (2026-11-10); 6: hiz_hidden / hiz_view_moved (T4.7b, 2026-11-11);
                                         * 7: setting (2026-11-11); 8: push (2026-11-11) */

typedef struct PhotonCpuHost {
    int version;                                    /* PHOTON_CPU_HOST_VERSION of the proxy */
    void (*log)(const char *fmt, ...);              /* the proxy's log (photon_proxy.log, PHOTON_LOG_DIR), thread-safe */
    uint64_t (*frame_count)(void);                  /* frames presented so far */
    unsigned long main_thread;                      /* the engine's main thread id (the thread that loaded the proxy) */
    /* version >= 2. W1: the latest frame (frame_count numbering) whose water draws' occlusion queries have all answered, usually 1-2
     * frames back: the water-shader draws in it and the depth-test samples they passed, summed over all water (UINT64_MAX: some draw
     * was not measured, count it as seen). draws 0: no water surface was drawn, so no evidence either way. Returns 0 when nothing has
     * been answered yet. The queries start at the first call (nothing costs before). Any thread. */
    int (*water_pixels)(uint64_t *frame, uint32_t *draws, uint64_t *samples);
    /* version >= 6. T4.7b (hiz.c, PHOTON_HIZ=1 / photon_hiz): the render module's world occlusion of model draws. hiz_hidden(entity
     * index): 1 if that entity's model draw in the main view was culled as hidden behind the world at its latest test, and that test was
     * no more than 2 Hi-Z frames ago; 0 otherwise (visible, not tested lately, off, or the camera moved). hiz_view_moved(): 1 while the
     * camera has moved more than the culling's limit since the depth it tests against (nothing is culled then: trust no result).
     * Any thread; cheap (a table read). Hidden this frame does not mean hidden next frame: the cull re-tests every frame. */
    int (*hiz_hidden)(int entity_index);
    int (*hiz_view_moved)(void);
    /* version >= 7. A Photon setting by name ("photon_cpu_anim_step", any key of vlg.c's table) as resolved now: defaults, photon.cfg,
     * settings.txt (the menu), the enabled shader packs' features (docs/SHADER_PACKS.md), environment. *found = 0 for an unknown key.
     * Any thread; a short shared lock; settings change within seconds of the files, so polling every few frames is enough. */
    float (*setting)(const char *key, int *found);
    /* version >= 8. Lua -> proxy data (shader packs, docs/SHADER_PACKS.md): Lua's photon_cpu.Push(name, floats) hands a named float array
     * to Photon's features (latest value per channel wins; up to 16 channels of up to 4096 floats; the proxy copies it). Main thread. */
    void (*push)(const char *channel, const float *v, int n);
} PhotonCpuHost;

typedef struct PhotonCpuModule {
    int version;                                    /* PHOTON_CPU_HOST_VERSION the module was built against */
    void (*on_present)(void);                       /* may be NULL; called at every Present, on the thread that presents: the material
                                                     * system's queue thread in queued mode (mat_queue_mode 2), else the main thread. Keep it
                                                     * short; never block there */
    /* version >= 3. DYN (the frame-rate target, modules/cpu TASKS DYN): the module's steps for the proxy's controller (dynq.c). A step
     * is a named way to make frames cheaper on the CPU side, always relative to the user's own settings (never above them). The
     * controller merges them with its GPU steps in the user's order (setting photon_dyn_order) and engages or releases them. Any
     * function may be NULL (no steps). */
    int (*dyn_steps)(const char *const **names);    /* the step names, the module's default order (cheapest quality loss first);
                                                     * returns their count; the array is static */
    void (*dyn_set)(int step, int engaged);         /* engage (1) or release (0) step `step`; any thread; takes effect within a tick
                                                     * or two */
    double (*main_busy_ms)(void);                   /* the engine main thread's CPU time over the last presented frame, in ms (for
                                                     * telling CPU-bound frames from GPU-bound ones); any thread */
    /* version >= 4 */
    int (*dyn_available)(int step);                 /* 1 when the step has something to act on now (loot to remove, more corpses than its
                                                     * limit...), 0 when engaging it would do nothing (the controller skips it instead of
                                                     * checking it for 2 s and holding the side); any thread; may be NULL (all available) */
    /* version >= 5 */
    double (*dyn_saving)(int step);                 /* the ms per frame engaging the step is expected to save now, from live measurements
                                                     * (what is actually using the time); 0 = nothing to save, < 0 = not measured (use the
                                                     * user's order for it). The controller engages the
                                                     * largest saving on the bound side first, the user's order breaking near-ties and
                                                     * disabled steps never used; any thread; may be NULL (the user's order alone) */
} PhotonCpuModule;

/* exported by photon_cpu.dll; returns its module table (static storage) or NULL to stay unloaded */
typedef const PhotonCpuModule *(*PhotonCpuInitFn)(const PhotonCpuHost *host);
#define PHOTON_CPU_INIT_EXPORT "PhotonCpuInit"
#endif
