/* VK0 spike as a pass (PHOTON_VK_TEST=1, analysis/42): a compute shader writes a moving pattern into a 256x256 D3D9 texture each frame;
 * after the submission the texture is copied to the back buffer's top-left corner (StretchRect: DXVK copies between any images), and at
 * frame 30 it is read back through D3D9 and saved as vk_test.ppm in PHOTON_LOG_DIR (the check: the harness's jpeg is taken before
 * Present, so it never shows the corner). */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include "vkbridge.h"
#include "vktest.h"
#include "d3dconst.h"
#include "log.h"

static const uint32_t k_spv[] =
#include "shaders/vk/test_pattern.spv.inc"
;
enum { TEX = 256 };
static PhotonVkImage g_img; static PhotonVkImage *g_imgs[1] = { &g_img };
static VkDescriptorSetLayout g_dsl; static VkDescriptorPool g_dpool; static VkDescriptorSet g_set;
static VkPipelineLayout g_pl; static VkPipeline g_pipe;
static PFN_vkCmdBindPipeline pBind; static PFN_vkCmdBindDescriptorSets pSets; static PFN_vkCmdPushConstants pPush; static PFN_vkCmdDispatch pDispatch;
static LARGE_INTEGER g_t0, g_freq;
static long long g_frames;

static int setup(void *u) {
    (void)u;
    const PhotonVk *vk = photon_vk();
#define P(t, n) PFN_##n n = (PFN_##n)photon_vk_proc(#n); if (!n) return 0
    P(, vkCreateDescriptorSetLayout); P(, vkCreateDescriptorPool); P(, vkAllocateDescriptorSets); P(, vkUpdateDescriptorSets);
    P(, vkCreatePipelineLayout); P(, vkCreateShaderModule); P(, vkCreateComputePipelines);
    pBind = (PFN_vkCmdBindPipeline)photon_vk_proc("vkCmdBindPipeline"); pSets = (PFN_vkCmdBindDescriptorSets)photon_vk_proc("vkCmdBindDescriptorSets");
    pPush = (PFN_vkCmdPushConstants)photon_vk_proc("vkCmdPushConstants"); pDispatch = (PFN_vkCmdDispatch)photon_vk_proc("vkCmdDispatch");
    if (!pBind || !pSets || !pPush || !pDispatch) return 0;
    if (!photon_vk_image(TEX, TEX, D3DFMT_A8B8G8R8, D3DUSAGE_RENDERTARGET, &g_img)) return 0;
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    if (vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &g_dsl)) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    if (vkCreateDescriptorPool(vk->dev, &dpi, NULL, &g_dpool)) return 0;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, 1, &g_dsl };
    if (vkAllocateDescriptorSets(vk->dev, &dai, &g_set)) return 0;
    VkDescriptorImageInfo dii = { VK_NULL_HANDLE, g_img.view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dii, NULL, NULL };
    vkUpdateDescriptorSets(vk->dev, 1, &w, 0, NULL);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 1, &pcr };
    if (vkCreatePipelineLayout(vk->dev, &pli, NULL, &g_pl)) return 0;
    VkShaderModule sm;
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof k_spv, k_spv };
    if (vkCreateShaderModule(vk->dev, &smi, NULL, &sm)) return 0;
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL },
                                        g_pl, VK_NULL_HANDLE, -1 };
    if (vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &g_pipe)) return 0;
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
    return 1;
}

static void record(VkCommandBuffer cb, void *u) {
    (void)u;
    pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_set, 0, NULL);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float t = (float)((double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart);
    pPush(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &t);
    pDispatch(cb, TEX / 8, TEX / 8, 1);
}

static void readback(void) {
    IDirect3DDevice9 *d = photon_d3d_device();
    IDirect3DSurface9 *src = NULL, *sys = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_img.tex, 0, &src))) return;
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(d, TEX, TEX, D3DFMT_A8B8G8R8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(d, src, sys);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        const uint8_t *px = lr.pBits;
        unsigned long long sum = 0;
        for (int y = 0; y < TEX; y++) for (int x = 0; x < TEX * 4; x++) sum += px[y * lr.Pitch + x];
        photon_log("vk: test readback: pixel (128,128) %08x, byte sum %llu (all zero = nothing written)", *(const uint32_t *)(px + 128 * lr.Pitch + 512), sum);
        char dir[MAX_PATH], path[MAX_PATH + 16];
        DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
        if (n && n < sizeof dir) {
            snprintf(path, sizeof path, "%s\\vk_test.ppm", dir);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", TEX, TEX);
                for (int y = 0; y < TEX; y++) for (int x = 0; x < TEX; x++) { const uint8_t *q = px + y * lr.Pitch + x * 4; fputc(q[0], f); fputc(q[1], f); fputc(q[2], f); }
                fclose(f);
            }
        }
        IDirect3DSurface9_UnlockRect(sys);
    } else photon_log("vk: test readback failed (0x%08lx)", (unsigned long)hr);
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(src);
}

static void after(void *u) {
    (void)u;
    IDirect3DDevice9 *d = photon_d3d_device();
    IDirect3DSurface9 *src = NULL, *bb = NULL;
    if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(g_img.tex, 0, &src)) && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
        RECT dst = { 16, 16, 16 + TEX, 16 + TEX };
        IDirect3DDevice9_StretchRect(d, src, NULL, bb, &dst, D3DTEXF_NONE);
    }
    if (src) IDirect3DSurface9_Release(src);
    if (bb) IDirect3DSurface9_Release(bb);
    if (++g_frames == 30) readback();
}

static PhotonVkPass g_pass = { "test", setup, record, after, g_imgs, 1, NULL, 1, 0 };
void photon_vktest_init(void) {
    char e[4];
    if (GetEnvironmentVariableA("PHOTON_VK_TEST", e, sizeof e) > 0 && e[0] == '1') photon_vk_add_pass(&g_pass);
}
