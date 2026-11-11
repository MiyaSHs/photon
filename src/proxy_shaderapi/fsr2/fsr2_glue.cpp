// UPSCALE U4 (analysis/45): FSR 2.2's C++ API (third_party/fsr2, MIT) for Photon's C code. The Vulkan backend calls a few instance-level
// functions directly; Photon has no loader import (the bridge gets DXVK's device through its interop), so they are defined here and
// resolved through vkGetInstanceProcAddr.
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "ffx_fsr2.h"
#include "vk/ffx_fsr2_vk.h"
#include "fsr2_glue.h"

static PFN_vkGetInstanceProcAddr s_gipa;
static VkInstance s_inst;
template <typename F> static F inst_fn(const char *n) { return s_gipa ? reinterpret_cast<F>(s_gipa(s_inst, n)) : nullptr; }
extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d, const char *n) {
    static PFN_vkGetDeviceProcAddr f = inst_fn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr"); return f ? f(d, n) : nullptr; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice p, VkPhysicalDeviceProperties *o) {
    static PFN_vkGetPhysicalDeviceProperties f = inst_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties"); if (f) f(p, o); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *o) {
    static PFN_vkGetPhysicalDeviceProperties2 f = inst_fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2"); if (f) f(p, o); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2 *o) {
    static PFN_vkGetPhysicalDeviceFeatures2 f = inst_fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2"); if (f) f(p, o); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *o) {
    static PFN_vkGetPhysicalDeviceMemoryProperties f = inst_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties"); if (f) f(p, o); }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice p, const char *l, uint32_t *n, VkExtensionProperties *o) {
    static PFN_vkEnumerateDeviceExtensionProperties f = inst_fn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    return f ? f(p, l, n, o) : VK_ERROR_INITIALIZATION_FAILED; }
}

static FfxFsr2Context s_ctx;
static PFN_vkCmdPipelineBarrier s_barrier;
static void *s_scratch;
static int s_ready;
static void (*s_log)(const char *);
static void say(const char *fmt, ...) {
    if (!s_log) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); s_log(b);
}
static void on_message(FfxFsr2MsgType t, const wchar_t *m) {
    char b[400]; size_t i = 0; for (; m && m[i] && i < sizeof b - 1; i++) b[i] = (char)m[i]; b[i] = 0;
    say("fsr2: %s: %s", t == FFX_FSR2_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}
extern "C" void photon_fsr2_fail(int line) {   // the backend's "API error" returns (build_fsr2.sh patches them to call this)
    static int n; if (n++ < 8) say("fsr2: the Vulkan backend failed at ffx_fsr2_vk.cpp:%d", line);
}
extern "C" int photon_fsr2_ready(void) { return s_ready; }
extern "C" int photon_fsr2_init(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa,
                                uint32_t max_w, uint32_t max_h, uint32_t disp_w, uint32_t disp_h, void (*log)(const char *)) {
    s_log = log; s_gipa = gipa; s_inst = inst;
    s_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gdpa(dev, "vkCmdPipelineBarrier"));
    if (s_ready) photon_fsr2_shutdown();
    size_t sz = ffxFsr2GetScratchMemorySizeVK(pd);
    s_scratch = calloc(1, sz);
    FfxFsr2ContextDescription d;
    memset(&d, 0, sizeof d);
    FfxErrorCode e = ffxFsr2GetInterfaceVK(&d.callbacks, s_scratch, sz, pd, gdpa);
    if (e != FFX_OK) { say("fsr2: ffxFsr2GetInterfaceVK failed (%d)", (int)e); free(s_scratch); s_scratch = nullptr; return 0; }
    d.device = ffxGetDeviceVK(dev);
    d.maxRenderSize.width = max_w; d.maxRenderSize.height = max_h;
    d.displaySize.width = disp_w; d.displaySize.height = disp_h;
    // LDR colour (the post-processed frame), standard depth (0 near, 1 far), motion vectors at render size that carry the jitter (from the
    // jittered pixel to last frame's unjittered point), auto exposure
    d.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    d.fpMessage = on_message;
    e = ffxFsr2ContextCreate(&s_ctx, &d);
    if (e != FFX_OK) { say("fsr2: ffxFsr2ContextCreate failed (%d)", (int)e); free(s_scratch); s_scratch = nullptr; return 0; }
    s_ready = 1;
    say("fsr2: context %ux%u (render up to %ux%u), scratch %zu bytes", disp_w, disp_h, max_w, max_h, sz);
    return 1;
}
static FfxResource res(const PhotonFsr2Img *i, const wchar_t *name, FfxResourceStates st) {
    return ffxGetTextureResourceVK(&s_ctx, i->img, i->view, i->w, i->h, i->fmt, name, st);
}
extern "C" int photon_fsr2_dispatch(VkCommandBuffer cb, const PhotonFsr2Img *color, const PhotonFsr2Img *depth, const PhotonFsr2Img *motion,
                                    const PhotonFsr2Img *out, float jx, float jy, float mvx, float mvy, uint32_t rw, uint32_t rh,
                                    float sharp, float frame_ms, int reset, float cn, float cf, float fov) {
    if (!s_ready) return 0;
    FfxFsr2DispatchDescription d;
    memset(&d, 0, sizeof d);
    d.commandList = ffxGetCommandListVK(cb);
    // the bridge holds every image of the pass in GENERAL (FSR's UNORDERED_ACCESS); FSR moves its inputs to SHADER_READ_ONLY to read them
    // and does not move them back (below)
    d.color = res(color, L"photon_color", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.depth = res(depth, L"photon_depth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.motionVectors = res(motion, L"photon_motion", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.output = res(out, L"photon_output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x = jx; d.jitterOffset.y = jy;
    d.motionVectorScale.x = mvx; d.motionVectorScale.y = mvy;
    d.renderSize.width = rw; d.renderSize.height = rh;
    d.enableSharpening = sharp > 0; d.sharpness = sharp;
    d.frameTimeDelta = frame_ms; d.preExposure = 1.0f; d.reset = reset != 0;
    d.cameraNear = cn; d.cameraFar = cf; d.cameraFovAngleVertical = fov; d.viewSpaceToMetersFactor = 0.0254f;   // Hammer units are inches
    FfxErrorCode e = ffxFsr2ContextDispatch(&s_ctx, &d);
    if (s_barrier) {   // the inputs back to GENERAL, where the bridge expects them
        VkImageMemoryBarrier b[3];
        const PhotonFsr2Img *in[3] = { color, depth, motion };
        for (int k = 0; k < 3; k++) {
            b[k] = VkImageMemoryBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, in[k]->img,
                                         { in[k]->aspect ? in[k]->aspect : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        }
        s_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 3, b);
    }
    static int fails;
    if (e != FFX_OK && fails++ < 5) say("fsr2: dispatch failed (%d)", (int)e);
    return e == FFX_OK;
}
extern "C" void photon_fsr2_shutdown(void) {
    if (s_ready) ffxFsr2ContextDestroy(&s_ctx);
    s_ready = 0;
    free(s_scratch); s_scratch = nullptr;
}
