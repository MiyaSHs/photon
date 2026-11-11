/* VK0 (TASKS, analysis/42): the Vulkan route. Under DXVK, GMod's IDirect3DDevice9 answers QueryInterface for ID3D9VkInteropDevice
 * (refs/dxvk/d3d9_interfaces.h, DXVK's public interop): the VkInstance / VkDevice / VkQueue behind the D3D9 device, D3D9 images with extra
 * Vulkan usage (CreateImage: storage for compute), layout transitions recorded into DXVK's own stream, and a lock around our own queue
 * submissions. Native D3D9 answers E_NOINTERFACE and the bridge stays off.
 * Each frame (just before Present, the device thread): the passes' images go to GENERAL in DXVK's stream, DXVK's commands are flushed
 * (so ours run after the frame's work so far), one command buffer from a ring of NSLOT records every enabled pass between GPU
 * timestamps, with a full barrier after each (compute writes visible to whatever runs next on the queue, DXVK's later work included),
 * it is submitted under DXVK's queue lock, and the images go back to their D3D9 layout. A slot is reused only after its fence, so the CPU
 * runs up to NSLOT-1 frames ahead; the slot's timestamps are read then (per pass GPU ms, logged every 600 frames). Vulkan entry points
 * come from the loader DXVK uses (vulkan-1.dll on Windows; winevulkan.dll under Proton). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vkbridge.h"
#include "d3dconst.h"
#include "log.h"

/* DXVK's interop interfaces (d3d9_interfaces.h), as C vtables */
typedef struct {
    D3DRESOURCETYPE Type; UINT Width, Height, Depth, MipLevels; DWORD Usage; D3DFORMAT Format; D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSample; DWORD MultiSampleQuality; uint8_t Discard, IsAttachmentOnly, IsLockable; VkImageUsageFlags ImageUsage;
} ExtImageDesc;   /* C++ bool is one byte */
typedef struct InteropTex InteropTex;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(InteropTex *, REFIID, void **);
    ULONG (WINAPI *AddRef)(InteropTex *);
    ULONG (WINAPI *Release)(InteropTex *);
    HRESULT (WINAPI *GetVulkanImageInfo)(InteropTex *, VkImage *, VkImageLayout *, VkImageCreateInfo *);
} InteropTexVtbl;
struct InteropTex { const InteropTexVtbl *v; };
typedef struct InteropDev InteropDev;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(InteropDev *, REFIID, void **);
    ULONG (WINAPI *AddRef)(InteropDev *);
    ULONG (WINAPI *Release)(InteropDev *);
    void (WINAPI *GetVulkanHandles)(InteropDev *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    void (WINAPI *GetSubmissionQueue)(InteropDev *, VkQueue *, uint32_t *, uint32_t *);
    void (WINAPI *TransitionTextureLayout)(InteropDev *, InteropTex *, const VkImageSubresourceRange *, VkImageLayout, VkImageLayout);
    void (WINAPI *FlushRenderingCommands)(InteropDev *);
    void (WINAPI *LockSubmissionQueue)(InteropDev *);
    void (WINAPI *ReleaseSubmissionQueue)(InteropDev *);
    void (WINAPI *LockDevice)(InteropDev *);
    void (WINAPI *UnlockDevice)(InteropDev *);
    uint8_t (WINAPI *WaitForResource)(InteropDev *, IDirect3DResource9 *, DWORD);
    HRESULT (WINAPI *CreateImage)(InteropDev *, const ExtImageDesc *, IDirect3DResource9 **);
} InteropDevVtbl;
struct InteropDev { const InteropDevVtbl *v; };
static const GUID IID_InteropDev = { 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };
static const GUID IID_InteropTex = { 0xd56344f5, 0x8d35, 0x46fd, { 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1 } };

enum { NSLOT = 3, MAXPASS = 16 };
static int g_state;   /* 0 not tried, 1 up, -1 off */
static InteropDev *g_io;
static PhotonVk g_vk;
static PhotonVkPass *g_pass[MAXPASS]; static int g_pass_ok[MAXPASS]; static volatile LONG g_npass;
static VkCommandPool g_cpool; static VkCommandBuffer g_cb[NSLOT]; static VkFence g_fence[NSLOT]; static int g_used[NSLOT];
static int g_slot_pass[NSLOT][MAXPASS];   /* which passes the slot's timestamps cover */
static VkQueryPool g_qp; static int g_have_ts; static double g_ts_ns;
static double g_gpu_ms[MAXPASS]; static long long g_gpu_n[MAXPASS];
static long long g_frames, g_fail;
static unsigned g_slot;

#define VKF(n) static PFN_##n p##n
VKF(vkGetDeviceProcAddr); VKF(vkCreateImageView); VKF(vkCreateCommandPool); VKF(vkAllocateCommandBuffers); VKF(vkCreateFence);
VKF(vkWaitForFences); VKF(vkResetFences); VKF(vkResetCommandBuffer); VKF(vkBeginCommandBuffer); VKF(vkEndCommandBuffer);
VKF(vkCmdPipelineBarrier); VKF(vkQueueSubmit); VKF(vkCreateQueryPool); VKF(vkCmdResetQueryPool); VKF(vkCmdWriteTimestamp);
VKF(vkGetQueryPoolResults);
static PFN_vkGetInstanceProcAddr g_gipa;

const PhotonVk *photon_vk(void) { return g_state == 1 ? &g_vk : NULL; }
int photon_vk_available(void) { return g_state == 1; }
PFN_vkVoidFunction photon_vk_proc(const char *name) { return g_state == 1 ? pvkGetDeviceProcAddr(g_vk.dev, name) : NULL; }
PFN_vkGetInstanceProcAddr photon_vk_gipa(void) { return g_state == 1 ? g_gipa : NULL; }

int photon_vk_add_pass(PhotonVkPass *p) {
    LONG i = InterlockedIncrement(&g_npass) - 1;
    if (i >= MAXPASS) { InterlockedDecrement(&g_npass); photon_log("vk: pass %s: too many passes", p->name); return 0; }
    g_pass[i] = p;
    return 1;
}

static int fail(const char *what, VkResult r) { photon_log("vk: %s failed (%d): bridge off", what, (int)r); g_state = -1; return 0; }

static VkFormat vk_format(D3DFORMAT f) {
    switch ((int)f) {
    case D3DFMT_A8B8G8R8: return VK_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_A16B16G16R16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case D3DFMT_A32B32G32R32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case D3DFMT_R32F: return VK_FORMAT_R32_SFLOAT;
    case D3DFMT_G16R16F: return VK_FORMAT_R16G16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int photon_vk_image(UINT w, UINT h, D3DFORMAT fmt, DWORD d3d_usage, PhotonVkImage *out) {
    memset(out, 0, sizeof *out);
    if (g_state != 1) return 0;
    ExtImageDesc desc = { D3DRTYPE_TEXTURE, w, h, 1, 1, d3d_usage, fmt, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 0, 0, 0,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
    IDirect3DResource9 *res = NULL;
    HRESULT hr = g_io->v->CreateImage(g_io, &desc, &res);
    if (FAILED(hr) || !res) { photon_log("vk: CreateImage %ux%u format %d failed (0x%08lx)", w, h, (int)fmt, (unsigned long)hr); return 0; }
    InteropTex *it = NULL;
    if (FAILED(IDirect3DResource9_QueryInterface(res, &IID_InteropTex, (void **)&it)) || !it) { IDirect3DResource9_Release(res); return 0; }
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;   /* DXVK fills the info only when the type is set */
    it->v->GetVulkanImageInfo(it, &out->img, &out->d3d_layout, &ici);
    out->format = ici.format != VK_FORMAT_UNDEFINED ? ici.format : vk_format(fmt);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, VK_IMAGE_VIEW_TYPE_2D, out->format,
                                  { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkResult r = pvkCreateImageView(g_vk.dev, &vci, NULL, &out->view);
    if (r) { photon_log("vk: vkCreateImageView failed (%d)", (int)r); it->v->Release(it); IDirect3DResource9_Release(res); return 0; }
    out->tex = (IDirect3DTexture9 *)res; out->itex = it; out->w = w; out->h = h; out->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    photon_log("vk: image %ux%u format %d (vk %d), usage 0x%x, D3D9 layout %d", w, h, (int)fmt, (int)out->format, (unsigned)ici.usage, (int)out->d3d_layout);
    return 1;
}

static int setup(void) {
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return 0;
    if (FAILED(IDirect3DDevice9_QueryInterface(d, &IID_InteropDev, (void **)&g_io)) || !g_io) {
        photon_log("vk: the D3D9 device has no DXVK interop (native D3D9): bridge off"); g_state = -1; return 0;
    }
    uint32_t qi;
    g_io->v->GetVulkanHandles(g_io, &g_vk.instance, &g_vk.phys, &g_vk.dev);
    g_io->v->GetSubmissionQueue(g_io, &g_vk.queue, &qi, &g_vk.family);
    /* the loader DXVK uses: vulkan-1.dll on Windows; under Wine / Proton DXVK calls winevulkan.dll directly (vulkan-1.dll is loaded on demand) */
    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk) vk = GetModuleHandleA("winevulkan.dll");
    if (!vk) vk = LoadLibraryA("vulkan-1.dll");
    g_gipa = vk ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr") : NULL;
    if (!g_gipa) { photon_log("vk: no Vulkan loader (vulkan-1.dll / winevulkan.dll): bridge off"); g_state = -1; return 0; }
    pvkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)g_gipa(g_vk.instance, "vkGetDeviceProcAddr");
    g_vk.get_proc = pvkGetDeviceProcAddr;
#define LD(n) if (!(p##n = (PFN_##n)pvkGetDeviceProcAddr(g_vk.dev, #n))) return fail("loading " #n, 0)
    LD(vkCreateImageView); LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers); LD(vkCreateFence); LD(vkWaitForFences); LD(vkResetFences);
    LD(vkResetCommandBuffer); LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer); LD(vkCmdPipelineBarrier); LD(vkQueueSubmit);
    LD(vkCreateQueryPool); LD(vkCmdResetQueryPool); LD(vkCmdWriteTimestamp); LD(vkGetQueryPoolResults);
    /* GPU timestamps: the queue family's valid bits and the device's period */
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)g_gipa(g_vk.instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_gipa(g_vk.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    VkPhysicalDeviceProperties pr;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "?";
    if (gpp) { gpp(g_vk.phys, &pr); g_ts_ns = pr.limits.timestampPeriod; memcpy(gpu, pr.deviceName, sizeof gpu); }
    if (gqf) {
        VkQueueFamilyProperties qf[16]; uint32_t n = 16;
        gqf(g_vk.phys, &n, qf);
        g_have_ts = g_vk.family < n && qf[g_vk.family].timestampValidBits > 0 && g_ts_ns > 0;
    }
    photon_log("vk: DXVK interop: %s, VkDevice %p, queue %p (index %u, family %u), GPU timestamps %s", gpu, (void *)g_vk.dev, (void *)g_vk.queue, qi,
               g_vk.family, g_have_ts ? "on" : "off");
    VkResult r;
    VkCommandPoolCreateInfo cpo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_vk.family };
    if ((r = pvkCreateCommandPool(g_vk.dev, &cpo, NULL, &g_cpool))) return fail("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, NSLOT };
    if ((r = pvkAllocateCommandBuffers(g_vk.dev, &cba, g_cb))) return fail("vkAllocateCommandBuffers", r);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    for (int i = 0; i < NSLOT; i++) if ((r = pvkCreateFence(g_vk.dev, &fci, NULL, &g_fence[i]))) return fail("vkCreateFence", r);
    if (g_have_ts) {
        VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * MAXPASS * 2, 0 };
        if (pvkCreateQueryPool(g_vk.dev, &qpi, NULL, &g_qp)) g_have_ts = 0;
    }
    g_state = 1;
    photon_log("vk: bridge up");
    return 1;
}

static int is_depth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}
static int has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }

int photon_vk_wrap(IDirect3DTexture9 *tex, PhotonVkImage *out) {
    memset(out, 0, sizeof *out);
    if (g_state != 1 || !tex) return 0;
    InteropTex *it = NULL;
    if (FAILED(IDirect3DTexture9_QueryInterface(tex, &IID_InteropTex, (void **)&it)) || !it) return 0;
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    it->v->GetVulkanImageInfo(it, &out->img, &out->d3d_layout, &ici);
    out->format = ici.format;
    VkImageAspectFlags view_aspect = is_depth(ici.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    out->aspect = is_depth(ici.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(ici.format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, VK_IMAGE_VIEW_TYPE_2D, ici.format,
                                  { 0 }, { view_aspect, 0, 1, 0, 1 } };
    VkResult r = pvkCreateImageView(g_vk.dev, &vci, NULL, &out->view);
    if (r) { photon_log("vk: wrap: vkCreateImageView failed (%d)", (int)r); it->v->Release(it); return 0; }
    IDirect3DTexture9_AddRef(tex);
    out->tex = tex; out->itex = it; out->w = ici.extent.width; out->h = ici.extent.height;
    photon_log("vk: wrapped texture %p: %ux%u vk format %d, usage 0x%x, D3D9 layout %d", (void *)tex, out->w, out->h, (int)ici.format, (unsigned)ici.usage, (int)out->d3d_layout);
    return 1;
}

uint32_t photon_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    static VkPhysicalDeviceMemoryProperties mp; static int have;
    if (!have) {
        PFN_vkGetPhysicalDeviceMemoryProperties f = (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(g_vk.instance, "vkGetPhysicalDeviceMemoryProperties");
        if (!f) return ~0u;
        f(g_vk.phys, &mp); have = 1;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void transition(int to_general, const int *sel) {
    for (int i = 0; i < g_npass && i < MAXPASS; i++) {
        PhotonVkPass *p = g_pass[i];
        if (!sel[i]) continue;
        for (int k = 0; k < p->nimages; k++) {
            PhotonVkImage *im = p->images[k];
            if (!im || !im->itex) continue;
            VkImageSubresourceRange rng = { im->aspect ? im->aspect : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (to_general) g_io->v->TransitionTextureLayout(g_io, im->itex, &rng, im->d3d_layout, VK_IMAGE_LAYOUT_GENERAL);
            else g_io->v->TransitionTextureLayout(g_io, im->itex, &rng, VK_IMAGE_LAYOUT_GENERAL, im->d3d_layout);
        }
    }
}

static void run(PhotonVkPass *only);
void photon_vk_present(void) { run(NULL); }
void photon_vk_run(PhotonVkPass *p) { if (p) run(p); }
/* only NULL: every enabled pass that is not manual (Present); else just that pass */
static void run(PhotonVkPass *only) {
    if (g_state < 0 || g_npass == 0) return;
    if (g_state == 0 && !setup()) return;
    /* each pass's setup once, when the bridge first sees it: a pass registered after the bridge came up (FSR 2 under PT-RT) too */
    static unsigned char done[MAXPASS];
    for (int i = 0; i < g_npass && i < MAXPASS; i++) {
        if (done[i]) continue;
        done[i] = 1;
        g_pass_ok[i] = !g_pass[i]->setup || g_pass[i]->setup(g_pass[i]->user);
        photon_log("vk: pass %s: %s", g_pass[i]->name, g_pass_ok[i] ? "ready" : "off (its setup failed)");
    }
    int sel[MAXPASS], any = 0;
    for (int i = 0; i < g_npass && i < MAXPASS; i++) {
        sel[i] = g_pass_ok[i] && g_pass[i]->enabled && (only ? g_pass[i] == only : !g_pass[i]->manual);
        any |= sel[i];
    }
    if (!any) return;
    unsigned s = g_slot++ % NSLOT;
    if (g_used[s]) {   /* the slot's last frame finished: its timestamps */
        pvkWaitForFences(g_vk.dev, 1, &g_fence[s], VK_TRUE, UINT64_MAX);
        pvkResetFences(g_vk.dev, 1, &g_fence[s]);
        if (g_have_ts) {
            uint64_t ts[MAXPASS * 2];
            if (pvkGetQueryPoolResults(g_vk.dev, g_qp, s * MAXPASS * 2, MAXPASS * 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                for (int i = 0; i < MAXPASS; i++) if (g_slot_pass[s][i]) {
                    g_gpu_ms[i] += (double)(ts[2 * i + 1] - ts[2 * i]) * g_ts_ns * 1e-6; g_gpu_n[i]++;
                }
        }
        g_used[s] = 0;
    }
    transition(1, sel);
    g_io->v->FlushRenderingCommands(g_io);   /* DXVK's commands so far (and the transitions) reach the queue before ours */
    VkCommandBuffer cb = g_cb[s];
    pvkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    pvkBeginCommandBuffer(cb, &bi);
    if (g_have_ts) pvkCmdResetQueryPool(cb, g_qp, s * MAXPASS * 2, MAXPASS * 2);
    barrier_all(cb);   /* the frame's earlier writes (DXVK's) before ours */
    for (int i = 0; i < g_npass && i < MAXPASS; i++) {
        PhotonVkPass *p = g_pass[i];
        g_slot_pass[s][i] = 0;
        if (!sel[i]) continue;
        if (g_have_ts) pvkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qp, s * MAXPASS * 2 + 2 * i);
        p->record(cb, p->user);
        barrier_all(cb);
        if (g_have_ts) { pvkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, s * MAXPASS * 2 + 2 * i + 1); g_slot_pass[s][i] = 1; }
    }
    pvkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    g_io->v->LockSubmissionQueue(g_io);
    VkResult r = pvkQueueSubmit(g_vk.queue, 1, &si, g_fence[s]);
    g_io->v->ReleaseSubmissionQueue(g_io);
    transition(0, sel);
    if (r) { if (g_fail++ < 5) photon_log("vk: vkQueueSubmit failed (%d)", (int)r); return; }
    g_used[s] = 1;
    for (int i = 0; i < g_npass && i < MAXPASS; i++) if (sel[i] && g_pass[i]->after) g_pass[i]->after(g_pass[i]->user);
    if (++g_frames == 1 || g_frames % 600 == 0) {
        char line[512]; int n = 0;
        for (int i = 0; i < g_npass && i < MAXPASS && n < (int)sizeof line - 64; i++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %.3f ms", g_pass[i]->name, g_gpu_n[i] ? g_gpu_ms[i] / (double)g_gpu_n[i] : 0.0);
        photon_log("vk: frames %lld (failures %lld); GPU time per frame:%s", g_frames, g_fail, n ? line : " (no timestamps)");
    }
}
