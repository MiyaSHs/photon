/* VK0 (TASKS, analysis/42): the Vulkan route. Photon's own Vulkan work inside GMod's D3D9 frame through DXVK's D3D9 interop.
 * A service: passes register once (any time, any thread before the first frame they need); the bridge comes up on the first Present
 * after a pass registered, if the D3D9 device is DXVK's; each frame it records every enabled pass into one command buffer, submits it on
 * DXVK's queue under DXVK's lock, and times each pass on the GPU. Native D3D9 leaves it off and no pass runs. See vkbridge.c. */
#ifndef PHOTON_VKBRIDGE_H
#define PHOTON_VKBRIDGE_H
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

typedef struct PhotonVk {
    VkInstance instance; VkPhysicalDevice phys; VkDevice dev; VkQueue queue; uint32_t family;
    PFN_vkGetDeviceProcAddr get_proc;
} PhotonVk;
const PhotonVk *photon_vk(void);                       /* NULL unless the bridge is up */
PFN_vkVoidFunction photon_vk_proc(const char *name);   /* a device-level function (NULL if missing or the bridge is down) */
PFN_vkGetInstanceProcAddr photon_vk_gipa(void);        /* the loader's instance-level lookup (NULL while the bridge is down) */

/* a D3D9 texture that is also a Vulkan image the passes can write (storage) and read (sampled), plus its view */
typedef struct PhotonVkImage {
    IDirect3DTexture9 *tex; void *itex;   /* itex: DXVK's ID3D9VkInteropTexture */
    VkImage img; VkImageView view; VkImageLayout d3d_layout; VkFormat format; UINT w, h;
    VkImageAspectFlags aspect;   /* for layout transitions: 0 = colour */
} PhotonVkImage;
/* fmt: a D3DFORMAT with a storage-capable Vulkan twin (A8B8G8R8, A16B16G16R16F, A32B32G32R32F, R32F, G16R16F); d3d_usage e.g.
 * D3DUSAGE_RENDERTARGET when D3D9 must also render into it or read it back. The bridge must be up (call from a pass's setup). */
int photon_vk_image(UINT w, UINT h, D3DFORMAT fmt, DWORD d3d_usage, PhotonVkImage *out);

/* an existing D3D9 texture (e.g. the scene's INTZ depth) as a Vulkan image the passes can sample; depth formats get a depth-only view */
int photon_vk_wrap(IDirect3DTexture9 *tex, PhotonVkImage *out);
uint32_t photon_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);   /* a memory type index for an allocation, or ~0u */

typedef struct PhotonVkPass {
    const char *name;
    int (*setup)(void *user);                     /* once, when the bridge is up (device thread); 0 = this pass stays off */
    void (*record)(VkCommandBuffer cb, void *user);   /* each frame: its commands (barriers between passes are the bridge's) */
    void (*after)(void *user);                    /* each frame after the submission (D3D9 work that uses the results), may be NULL */
    PhotonVkImage **images; int nimages;          /* the images this pass touches: in VK_IMAGE_LAYOUT_GENERAL while the passes run */
    void *user;
    volatile LONG enabled;                        /* 0: skipped this frame (setup still done) */
    int manual;                                   /* 1: not run at Present; the owner runs it with photon_vk_run at its point in the frame */
} PhotonVkPass;
void photon_vk_run(PhotonVkPass *p);   /* a manual pass, now (the device thread): its own submission, ordered after the frame's work so far */
int photon_vk_add_pass(PhotonVkPass *p);   /* the pass struct stays the caller's; up to 16 passes */

void photon_vk_present(void);   /* the device thread, before IDirect3DDevice9::Present */
int photon_vk_available(void);  /* 1 once the bridge is up */
#endif
