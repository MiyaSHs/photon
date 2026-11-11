/* T4.7b (analysis/25; user idea 2026-11-06: props hidden behind walls cost the engine its full per-model work): occlusion culling of
 * model draws (props, ragdolls, NPCs, vehicles: everything through CModelRender::DrawModelExecute) against a Hi-Z of the world.
 *
 * - The occluders are the world alone (brushes and displacements: shadows.c's world-only depth copy, taken at the first model draw after
 *   the main view's depth clear), so a moving model never hides another: what is hidden stays hidden until the camera sees past the wall.
 * - At the main view's end (device thread) a manual Vulkan pass (hiz.comp) writes the far depth of each 8 x 8 block into a host-visible
 *   ring slot, together with that frame's view-projection rows. The main thread uses a slot only once INFLIGHT frames have passed (the
 *   bridge never has more in flight), so it never waits on the GPU; coarser levels are built on the CPU when a slot is first used.
 * - DrawModelExecute (main thread, any threading mode, the outermost screen 3D view only: not the 3D skybox, water views, the viewmodel,
 *   shadow depth or monitors): the model's render bounds, enlarged by PAD units, under its model-to-world matrix, are projected with the
 *   slot's own rows (so the test is exact for that frame's world); hidden when every covered Hi-Z texel lies nearer than the box's nearest
 *   point. Boxes not wholly on the slot's screen are never culled; when the camera has moved more than MOVE units since that slot (3
 *   frames old), nothing is culled.
 * DXVK only (the Vulkan bridge); native D3D9 culls nothing. PHOTON_HIZ=1 / the setting photon_hiz. */
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hiz.h"
#include "vkbridge.h"
#include "log.h"
long photon_shadows_view(float iv[4][4], float vp[4][4], float cam[3], void **depth, unsigned *w, unsigned *h);   /* shadows.c */

static const uint32_t k_spv[] =
#include "shaders/vk/hiz.spv.inc"
;
enum { NR = 8, INFLIGHT = 3, MAXW = 512, MAXH = 288, NLEV = 6 };   /* ring slots; frames before a slot is read (vkbridge.c's NSLOT 3: a
                                                                      * submission 3 runs old has been waited for); base level up to 4096 x 2304 */
static const float PAD = 4.0f, MOVE = 24.0f;   /* box padding (units); the camera's travel since the slot that stops culling (turning
                                                 * reveals nothing behind a wall: what turns into view was off the slot's screen, never culled) */

static int g_on = -1;
static PhotonVkImage g_src; static PhotonVkImage *g_imgs[1] = { &g_src }; static void *g_src_tex;
static VkBuffer g_buf; static VkDeviceMemory g_mem; static float *g_map;
static VkSampler g_near; static VkDescriptorSetLayout g_dsl; static VkDescriptorPool g_dpool; static VkDescriptorSet g_set;
static VkPipelineLayout g_pl; static VkPipeline g_pipe;
static PFN_vkCmdBindPipeline pBind; static PFN_vkCmdBindDescriptorSets pSets; static PFN_vkCmdPushConstants pPush; static PFN_vkCmdDispatch pDispatch;
static PFN_vkUpdateDescriptorSets pUpd;

/* the ring: written by the device thread (meta under g_l), read by the main thread */
typedef struct { long long seq; float vp[4][4], cam[3]; int w, h; } Meta;
static Meta g_meta[NR]; static long long g_seq; static int g_cur_slot; static SRWLOCK g_l = SRWLOCK_INIT;
static struct { int w, h; float m[4][4], cam[3]; } g_pending;   /* the frame being recorded */
static volatile int g_pass_armed;
static long long g_after_runs, g_published;

/* the main thread's copy of the newest usable slot, with its levels */
static long long g_used_seq = -1;
static volatile LONG g_moved_now;   /* the latest test found the camera moved past MOVE (host version 6) */
static Meta g_use;
static float *g_lev[NLEV]; static int g_lw[NLEV], g_lh[NLEV], g_nlev;
static float g_view_cam[3], g_view_fwd[3]; static int g_view_ok;   /* the latest main view (photon_hiz_view) */
static long long g_tests, g_hidden, g_moved, g_offscreen, g_near_clip, g_noslot;
static long long g_frames;

int photon_hiz_on(void) {
    if (g_on < 0) { char e[4]; g_on = GetEnvironmentVariableA("PHOTON_HIZ", e, sizeof e) > 0 && e[0] == '1'; }
    return g_on;
}

static int setup(void *u) {
    (void)u;
    const PhotonVk *vk = photon_vk();
#define P(n) PFN_##n n = (PFN_##n)photon_vk_proc(#n); if (!n) return 0
    P(vkCreateDescriptorSetLayout); P(vkCreateDescriptorPool); P(vkAllocateDescriptorSets); P(vkCreatePipelineLayout);
    P(vkCreateShaderModule); P(vkCreateComputePipelines); P(vkCreateSampler); P(vkCreateBuffer); P(vkGetBufferMemoryRequirements);
    P(vkAllocateMemory); P(vkBindBufferMemory); P(vkMapMemory);
#undef P
    pBind = (PFN_vkCmdBindPipeline)photon_vk_proc("vkCmdBindPipeline"); pSets = (PFN_vkCmdBindDescriptorSets)photon_vk_proc("vkCmdBindDescriptorSets");
    pPush = (PFN_vkCmdPushConstants)photon_vk_proc("vkCmdPushConstants"); pDispatch = (PFN_vkCmdDispatch)photon_vk_proc("vkCmdDispatch");
    pUpd = (PFN_vkUpdateDescriptorSets)photon_vk_proc("vkUpdateDescriptorSets");
    if (!pBind || !pSets || !pPush || !pDispatch || !pUpd) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)NR * MAXW * MAXH * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (vkCreateBuffer(vk->dev, &bi, NULL, &g_buf)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk->dev, g_buf, &mr);
    uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == ~0u) return 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    if (vkAllocateMemory(vk->dev, &ai, NULL, &g_mem) || vkBindBufferMemory(vk->dev, g_buf, g_mem, 0) ||
        vkMapMemory(vk->dev, g_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g_map)) return 0;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_FALSE };
    if (vkCreateSampler(vk->dev, &si, NULL, &g_near)) return 0;
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b };
    if (vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &g_dsl)) return 0;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    if (vkCreateDescriptorPool(vk->dev, &dpi, NULL, &g_dpool)) return 0;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, 1, &g_dsl };
    if (vkAllocateDescriptorSets(vk->dev, &dai, &g_set)) return 0;
    VkDescriptorBufferInfo dbi = { g_buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
    pUpd(vk->dev, 1, &w, 0, NULL);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 20 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 1, &pcr };
    if (vkCreatePipelineLayout(vk->dev, &pli, NULL, &g_pl)) return 0;
    VkShaderModule sm;
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof k_spv, k_spv };
    if (vkCreateShaderModule(vk->dev, &smi, NULL, &sm)) return 0;
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL },
                                        g_pl, VK_NULL_HANDLE, -1 };
    if (vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &g_pipe)) return 0;
    photon_log("hiz: pass ready (a %d-slot ring of up to %dx%d far depths, read %d frames late)", NR, MAXW, MAXH, INFLIGHT);
    return 1;
}

static void record(VkCommandBuffer cb, void *u) {
    (void)u;
    if (!g_pass_armed) return;
    int dw = (g_pending.w + 7) / 8, dh = (g_pending.h + 7) / 8;
    struct { uint32_t ofs; int sw, sh, dw, dh; } pc = { (uint32_t)(g_cur_slot * MAXW * MAXH), g_pending.w, g_pending.h, dw, dh };
    pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_set, 0, NULL);
    pPush(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    pDispatch(cb, (uint32_t)(dw + 7) / 8, (uint32_t)(dh + 7) / 8, 1);
}

/* after the Present-time submission: the slot is published with the view it was made from */
static void after(void *u) {
    (void)u;
    g_after_runs++;
    if (!g_pass_armed) return;
    g_published++;
    g_pass_armed = 0;
    AcquireSRWLockExclusive(&g_l);
    Meta *m = &g_meta[g_cur_slot];
    m->seq = ++g_seq; memcpy(m->vp, g_pending.m, sizeof m->vp); memcpy(m->cam, g_pending.cam, sizeof m->cam);
    m->w = (g_pending.w + 7) / 8; m->h = (g_pending.h + 7) / 8;
    ReleaseSRWLockExclusive(&g_l);
}
/* not manual: run with the frame's own submission at Present (the world-only depth copy still holds this frame's world then; a manual run
 * mid-frame made DXVK flush there and doubled a draw-heavy frame on the laptop, 16.4 -> 34.9 ms) */
static PhotonVkPass g_pass = { "hiz", setup, record, after, g_imgs, 1, NULL, 0, 0 };

void photon_hiz_init(void) {
    if (!photon_hiz_on()) return;
    photon_vk_add_pass(&g_pass);
    photon_log("hiz: on (T4.7b: model draws hidden behind the world are skipped)");
}

/* device thread, at the main view's end once the world-only depth copy exists: this frame's far depths into the next ring slot */
static long long g_mve_calls, g_mve_novk;
void photon_hiz_main_view_end(void *world_depth_tex, unsigned w, unsigned h, const float vp[4][4], const float cam[3]) {
    if (++g_mve_calls % 600 == 300)
        photon_log("hiz: main view ends %lld (no bridge %lld), pass runs %lld, slots published %lld, ring seq %lld, depth %p %ux%u",
                   g_mve_calls, g_mve_novk, g_after_runs, g_published, g_seq, world_depth_tex, w, h);
    if (!photon_vk_available()) g_mve_novk++;
    if (!photon_hiz_on() || !photon_vk_available() || !world_depth_tex || !w || !h) return;
    if ((w + 7) / 8 > MAXW || (h + 7) / 8 > MAXH) return;
    if (world_depth_tex != g_src_tex) {   /* (a new depth copy after a video change) */
        if (g_src_tex) return;   /* v0: no rewrap */
        if (!photon_vk_wrap((IDirect3DTexture9 *)world_depth_tex, &g_src)) return;
        g_src_tex = world_depth_tex;
        VkDescriptorImageInfo ii = { g_near, g_src.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL };
        if (!pUpd) return;
        pUpd(photon_vk()->dev, 1, &wr, 0, NULL);
    }
    if (g_pass_armed) return;   /* one a frame */
    AcquireSRWLockExclusive(&g_l);
    g_cur_slot = (int)(g_seq % NR);
    g_meta[g_cur_slot].seq = -1;   /* being rewritten */
    ReleaseSRWLockExclusive(&g_l);
    g_pending.w = (int)w; g_pending.h = (int)h; memcpy(g_pending.m, vp, sizeof g_pending.m); memcpy(g_pending.cam, cam, sizeof g_pending.cam);
    g_pass_armed = 1;
    g_pass.enabled = 1;   /* runs at this frame's Present */
}

/* the newest slot at least INFLIGHT frames old, copied with its levels; 0 if none */
static int take_slot(void) {
    AcquireSRWLockShared(&g_l);
    long long now = g_seq, best = -1; int bs = -1;
    for (int i = 0; i < NR; i++) if (g_meta[i].seq > 0 && now - g_meta[i].seq >= INFLIGHT && g_meta[i].seq > best) { best = g_meta[i].seq; bs = i; }
    if (bs >= 0 && best != g_used_seq) {
        g_use = g_meta[bs];
        int w = g_use.w, h = g_use.h;
        if (!g_lev[0]) for (int k = 0; k < NLEV; k++) g_lev[k] = malloc(sizeof(float) * MAXW * MAXH);
        if (g_lev[NLEV - 1]) {
            memcpy(g_lev[0], g_map + (size_t)bs * MAXW * MAXH, sizeof(float) * (size_t)w * (size_t)h);
            g_lw[0] = w; g_lh[0] = h; g_nlev = 1;
            while (g_nlev < NLEV && (g_lw[g_nlev - 1] > 2 || g_lh[g_nlev - 1] > 2)) {   /* each level: the far depth of 2 x 2 */
                int pw = g_lw[g_nlev - 1], ph = g_lh[g_nlev - 1], nw = (pw + 1) / 2, nh = (ph + 1) / 2;
                const float *s = g_lev[g_nlev - 1]; float *d = g_lev[g_nlev];
                for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
                    float m = 0;
                    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) { int sx = 2 * x + i < pw ? 2 * x + i : pw - 1, sy = 2 * y + j < ph ? 2 * y + j : ph - 1; float v = s[sy * pw + sx]; if (v > m) m = v; }
                    d[y * nw + x] = m;
                }
                g_lw[g_nlev] = nw; g_lh[g_nlev] = nh; g_nlev++;
            }
            g_used_seq = best;
        }
    }
    ReleaseSRWLockShared(&g_l);
    return g_used_seq > 0;
}

/* main thread, each main view (from p6's view tracking): the camera now, against which the slot's age is judged */
void photon_hiz_view(const float cam[3], const float fwd[3]) {
    memcpy(g_view_cam, cam, sizeof g_view_cam); memcpy(g_view_fwd, fwd, sizeof g_view_fwd); g_view_ok = 1;
    if (++g_frames % 300 == 150)
        photon_log("hiz: tests %lld, hidden %lld (%.1f%%); not culled: camera moved %lld, off screen %lld, at the near plane %lld, no slot yet %lld",
                   g_tests, g_hidden, g_tests ? 100.0 * (double)g_hidden / (double)g_tests : 0.0, g_moved, g_offscreen, g_near_clip, g_noslot);
}

int photon_hiz_occluded(const float mins[3], const float maxs[3], const float m34[3][4]) {
    if (!take_slot()) { if (++g_noslot % 50000 == 1) photon_log("hiz: no usable slot yet (%lld tests asked)", g_noslot); return 0; }
    g_tests++;
    static long long vseq = -1;   /* the camera now (the latest main view), refreshed once per ring slot */
    if (vseq != g_seq) {
        float iv[4][4], vp[4][4], cam[3]; void *dep; unsigned w, h;
        if (photon_shadows_view(iv, vp, cam, &dep, &w, &h)) {
            float l = sqrtf(vp[3][0] * vp[3][0] + vp[3][1] * vp[3][1] + vp[3][2] * vp[3][2]);
            float fwd[3] = { l > 0 ? vp[3][0] / l : 0, l > 0 ? vp[3][1] / l : 0, l > 0 ? vp[3][2] / l : 0 };
            photon_hiz_view(cam, fwd);
        }
        vseq = g_seq;
    }
    if (!g_view_ok) return 0;
    float dx = g_view_cam[0] - g_use.cam[0], dy = g_view_cam[1] - g_use.cam[1], dz = g_view_cam[2] - g_use.cam[2];
    const float *r3 = g_use.vp[3];   /* clip.w = row 3 . p: its xyz is the slot's forward axis (scaled) */
    float fl = sqrtf(r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2]);
    float c = fl > 0 ? (r3[0] * g_view_fwd[0] + r3[1] * g_view_fwd[1] + r3[2] * g_view_fwd[2]) / fl : 0;
    (void)c;
    g_moved_now = dx * dx + dy * dy + dz * dz > MOVE * MOVE;
    if (g_moved_now) { g_moved++; return 0; }
    float u0 = 2, u1 = -1, v0 = 2, v1 = -1, zmin = 2;
    for (int k = 0; k < 8; k++) {
        float l[3] = { (k & 1 ? maxs[0] + PAD : mins[0] - PAD), (k & 2 ? maxs[1] + PAD : mins[1] - PAD), (k & 4 ? maxs[2] + PAD : mins[2] - PAD) };
        float p[3];
        for (int a = 0; a < 3; a++) p[a] = m34[a][0] * l[0] + m34[a][1] * l[1] + m34[a][2] * l[2] + m34[a][3];
        float cl[4];
        for (int r = 0; r < 4; r++) cl[r] = g_use.vp[r][0] * p[0] + g_use.vp[r][1] * p[1] + g_use.vp[r][2] * p[2] + g_use.vp[r][3];
        if (cl[3] < 1.0f) { g_near_clip++; return 0; }   /* a corner at or behind the near plane: visible */
        float x = cl[0] / cl[3], y = cl[1] / cl[3], z = cl[2] / cl[3];
        float u = x * 0.5f + 0.5f, v = 0.5f - y * 0.5f;
        u0 = fminf(u0, u); u1 = fmaxf(u1, u); v0 = fminf(v0, v); v1 = fmaxf(v1, v); zmin = fminf(zmin, z);
    }
    /* partly or wholly off the slot's screen: never culled (its unseen part may be in view now, after a turn) */
    if (u0 < 0 || u1 > 1 || v0 < 0 || v1 > 1) { g_offscreen++; return 0; }
    int L = 0;   /* the level where the box covers at most 4 x 4 texels */
    while (L + 1 < g_nlev && ((u1 - u0) * (float)g_lw[L] > 4 || (v1 - v0) * (float)g_lh[L] > 4)) L++;
    int w = g_lw[L], h = g_lh[L];
    int x0 = (int)(u0 * (float)w), x1 = (int)(u1 * (float)w), y0 = (int)(v0 * (float)h), y1 = (int)(v1 * (float)h);
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    const float *lv = g_lev[L];
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (lv[y * w + x] >= zmin) return 0;   /* some texel lies behind the box */
    g_hidden++;
    return 1;
}

/* host version 6 (photon-a0's C3d bone skipping): per entity index, the Hi-Z frame of its latest main-view test and its result */
enum { NENT = 8192 };
static volatile LONG64 g_ent_seq[NENT]; static volatile LONG g_ent_hidden[NENT];
void photon_hiz_note_entity(int e, int hidden) {
    if (e < 0 || e >= NENT) return;
    g_ent_hidden[e] = hidden; g_ent_seq[e] = g_used_seq;
}
int photon_hiz_hidden(int e) {
    if (!photon_hiz_on() || e < 0 || e >= NENT || g_moved_now || !g_ent_hidden[e]) return 0;
    return g_used_seq - g_ent_seq[e] <= 2;
}
int photon_hiz_view_moved(void) { return photon_hiz_on() ? (int)g_moved_now : 0; }
