/* UPSCALE U4 (analysis/45): AMD FSR 2.2 as the upscale (photon_upscale 3), a manual pass on the Vulkan bridge. Built in when
 * third_party/fsr2_build/libffx_fsr2_vk.a exists (scripts/build_fsr2.sh, PHOTON_FSR2); otherwise photon_fsr2_frame returns 0 and the
 * caller keeps Photon's TAAU. */
#ifndef PHOTON_FSR2_H
#define PHOTON_FSR2_H
/* one frame: colour, depth and full motion vectors (D3D9 textures of the display size holding the render_w x render_h view at the
 * top left; motion in the target's uv, pointing to last frame), jitter in NDC (taa.c's); *out: the D3D9 texture with the result
 * (A16B16G16R16F, display size). Device thread. 0: not available (yet), nothing drawn. */
int photon_fsr2_frame(void *color, void *depth, void *motion, unsigned disp_w, unsigned disp_h, unsigned render_w, unsigned render_h,
                      float jitter_ndc_x, float jitter_ndc_y, float sharpness, int reset, void **out);
#endif
