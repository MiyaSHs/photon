/* W1 (TASKS.md, with Photon CPU's C4b): did any water pixel pass the depth test this frame? Every draw of a water-shader snapshot (vlg.c marks
 * PHOTON_VLG_SNAP_WATER when the shadow's PS name starts with "water": water_ps20b, watercheap_ps20b, ...) is wrapped in a D3D9 occlusion
 * query; the samples are summed per frame and published once all of that frame's queries answered. Photon CPU reads the answer through
 * PhotonCpuHost.water_pixels and skips the water reflection / refraction views while the water is hidden.
 *
 * Idle (no query, no cost) until the first photon_waterq_last call or PHOTON_WATERQ=1 (which also logs a summary every 600 frames). */
#ifndef PHOTON_WATERQ_H
#define PHOTON_WATERQ_H
#include <stdint.h>

/* draw thread, around the stock RenderPass of a water snapshot */
void photon_waterq_begin(void);
void photon_waterq_end(void);
/* draw thread, after the real Present (photon_frame_count already advanced): collects answers, publishes finished frames */
void photon_waterq_present(void);
/* main thread (p6.c's IVRenderView hooks): the view on top is (1) or is not (0) the outermost screen 3D view. Only water drawn while it is
 * on top counts (Photon CPU's request: reflection / refraction targets, Lua cameras and monitors must not keep the water views alive or
 * fake a 0). Passed to the draw thread in order through the material queue. Without these calls (views not hooked) every water draw counts. */
void photon_waterq_view(int main);
/* proxy.c: photon_cpu.dll is loaded, so the views are worth hooking for us (p6.c asks photon_waterq_possible) */
void photon_waterq_set_possible(void);
int photon_waterq_possible(void);
/* device lost / reset: drop every query */
void photon_waterq_device_lost(void);
/* any thread: the latest frame whose water queries all answered; 0 if none yet. A frame without water draws is published with draws 0. */
int photon_waterq_last(uint64_t *frame, uint32_t *draws, uint64_t *samples);
#endif
