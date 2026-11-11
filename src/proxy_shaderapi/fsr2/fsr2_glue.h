/* UPSCALE U4 (analysis/45): AMD FidelityFX FSR 2.2 on Photon's Vulkan bridge, behind a C API (fsr2_glue.cpp, built into
 * third_party/fsr2_build/libffx_fsr2_vk.a by scripts/build_fsr2.sh). */
#ifndef PHOTON_FSR2_GLUE_H
#define PHOTON_FSR2_GLUE_H
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PhotonFsr2Img { VkImage img; VkImageView view; uint32_t w, h; VkFormat fmt; VkImageAspectFlags aspect; } PhotonFsr2Img;   /* aspect 0: colour */
/* the context for a display size and the largest render size; 0 on failure (the log says why) */
int photon_fsr2_init(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa,
                     uint32_t max_w, uint32_t max_h, uint32_t disp_w, uint32_t disp_h, void (*log)(const char *msg));
/* one frame into cb: colour, depth and motion vectors at render size (in the top-left render_w x render_h of their images), the output
 * (storage) at display size. Jitter in render pixels (FSR's convention), motion vectors x mv_scale = pixels, pointing to last frame. */
int photon_fsr2_dispatch(VkCommandBuffer cb, const PhotonFsr2Img *color, const PhotonFsr2Img *depth, const PhotonFsr2Img *motion,
                         const PhotonFsr2Img *out, float jitter_x, float jitter_y, float mv_scale_x, float mv_scale_y, uint32_t render_w,
                         uint32_t render_h, float sharpness, float frame_ms, int reset, float cam_near, float cam_far, float fov_y);
void photon_fsr2_shutdown(void);
int photon_fsr2_ready(void);
#ifdef __cplusplus
}
#endif
#endif
