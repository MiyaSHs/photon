/* DYN dynamic quality (user, 2026-11-09: "retain 60 fps by lowering or increasing quality", with Photon CPU: "a configurable list of
 * things to first try"; analysis/34 "DYN"). One controller for the whole of Photon, replacing UI2's shadow-only one. It holds
 * photon_target_fps with an ordered list of named steps, each one cheaper than the user's settings (their settings are the ceiling) and
 * tagged with the side it saves (GPU or CPU); photon_dyn_order sets the order and which steps take part (empty: Photon CPU's steps in its
 * order, then the GPU ones). The rules guard the 1% lows:
 *   - per Present: the interval since the last Present, the time blocked inside the real Present (the driver's / DXVK's frame-latency
 *     wait: a GPU-bound frame waits there), and Photon CPU's main-thread busy time when it provides it;
 *   - engage before the budget is crossed ("prevent more than fix"): when the 99th percentile of the last 1.5 s is over 90% of the budget
 *     (1000 / target ms), or rising so fast it would pass the budget within a second; which step: the one expected to save the most now
 *     (Photon CPU's live estimates, host v5, x a correction each step's checks learn) above 3% of the budget, near-ties (within 20%) and
 *     unmeasured steps by the user's order; on
 *     the bound side (GPU when Present waits more than 15% of the interval; CPU otherwise; either when both are near the budget), else
 *     the first free one of the other side;
 *   - an engaged step that does not make the next 1.5 s's mean >= 2% faster was the wrong side: released, that side held 10 s (doubling to
 *     160 s while it keeps not helping);
 *   - release the last engaged step (LIFO) only after 4 s with the 99th percentile under 75% of the budget; for 2 s after, a 99th
 *     percentile over 90% of the budget engages it again and bars its release for 30 s (doubling to 240 s): no ping-pong;
 *   - 0.5 s between any two moves, and only samples taken since the last move count.
 * Photon's GPU steps: shadows1 (hard edges, one lamp), shadows2 (half distance), shadows3 (half resolution), shadows4 (quarter distance),
 * shadows5 (the engine's shadows), nanitelod (Nanite takes the models' lower LODs at half the distance), ptcheck (PT-RT traces half the
 * pixels each frame in a checkerboard, the others keep their reprojected history). */
#ifndef PHOTON_DYNQ_H
#define PHOTON_DYNQ_H

void photon_dynq_present_begin(void);   /* around the real IDirect3DDevice9::Present (proxy.c), on the presenting thread */
void photon_dynq_present_end(void);
int photon_dynq_shadow_level(void);     /* 0 (settings) .. 5 (the engine's shadows): the highest shadowsN engaged; 0 while off */
int photon_dynq_nanite_lod_bias(void);  /* 1 while nanitelod is engaged */
int photon_dynq_ptrt_checker(void);     /* 1 while ptcheck is engaged */
/* Photon CPU's steps in its default order (CPU side; a name "gpu:<name>" marks one that saves GPU time, such as the render scale); set(i, engaged) applies step i (any thread, takes effect within a tick or two);
 * busy_ms (may be NULL): the main thread's CPU time over the last frame; avail (may be NULL): 0 when step i has nothing to act on now
 * (skipped instead of engaged, checked and held) */
void photon_dynq_set_cpu_steps(int n, const char *const *names, void (*set)(int step, int engaged), double (*busy_ms)(void), int (*avail)(int step),
                               double (*saving)(int step));   /* saving (may be NULL): the ms a frame step i would save now (< 0 unknown) */
#endif
