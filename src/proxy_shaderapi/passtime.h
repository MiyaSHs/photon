/* OPT-VK (TASKS): the cost of Photon's own per-frame passes, CPU on the render thread and GPU (D3D9 timestamp queries, native and DXVK),
 * to choose what moves to Vulkan compute first. PHOTON_PASSTIME=1 (off otherwise: no queries). See passtime.c. */
#ifndef PHOTON_PASSTIME_H
#define PHOTON_PASSTIME_H
enum { PT_L3, PT_L1, PT_CAST, PT_NEAR, PT_SCREEN, PT_OUTLINE, PT_N };
void photon_passtime_begin(int seg);
void photon_passtime_end(int seg);
void photon_passtime_frame(void);   /* the Present hook */
#endif
