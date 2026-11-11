/* PT-RT (TASKS, analysis/43): the live path-traced mode on the Vulkan bridge, step 1: one bounce ray per pixel (half resolution) through
 * the voxel cascades, lit by the sun (a voxel shadow ray) and the sky, into a D3D9 texture. PHOTON_PTRT=1 (off otherwise); not yet
 * composited into the frame: PHOTON_PTRT_DUMP=N saves frame N's result as ptrt.ppm in PHOTON_LOG_DIR (the check against tools/pt_ref.c).
 * Worker thread: the map (read as l4work.c reads it), its SunMap and lights, vox_cascades_new; then, while in game, the cascades follow
 * the camera (vox_cascades_update when it moved a brick) and are packed (vox_cascades_pack) into a buffer the device thread copies to
 * one of two host-visible GPU buffers, never one a frame still in flight may read (each buffer remembers the bridge frame that last used
 * it; a copy waits until NSLOT frames have passed). Device thread (the bridge's pass): the scene depth (shadows.c's INTZ texture) wrapped
 * as a Vulkan image, the main view's matrices from shadows.c's snapshot, the dispatch. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vlg.h"
#include "ptrt.h"
#include "vkbridge.h"
#include "shadows.h"
#include "d3dconst.h"
#include "log.h"
#include "l4work.h"
#include "dynq.h"
#include "lodwork.h"
#include "../ambient/vox_world.h"
#include "../ambient/lg_map.h"
#include "../ambient/sun_vis.h"

static const uint32_t k_spv[] =
#include "shaders/vk/ptrt_trace.spv.inc"
;
long photon_shadows_view(float iv[4][4], float vp[4][4], float cam[3], void **depth, unsigned *w, unsigned *h);   /* shadows.c */

enum { CAP_WORDS = 64 + VOXC_LEVELS * VOXC_WIN * VOXC_WIN * VOXC_WIN + 6144 * 1024, NBUF = 2, INFLIGHT = 4 };
static int g_on = -1;
int photon_ptrt_on(void) {
    /* PHOTON_PTRT=1/0 (tests), else the menu's Path traced in real time (photon_pathtrace 2), read once: the passes register at the start */
    if (g_on < 0) { char e[4]; g_on = GetEnvironmentVariableA("PHOTON_PTRT", e, sizeof e) > 0 ? e[0] == '1' : photon_vlg_pathtrace_rt(); }
    return g_on;
}
/* PT-RT models: Photon's model shaders draw their surface colour only (vlg.c's light block: no lamps, ambient exactly 1) and the composite
 * lights them with the path traced light and the sun, as the world; PHOTON_PTRT_MODELS=0: they keep the engine's light (A/B) */
int photon_ptrt_models(void) {
    static int v = -1;
    if (v < 0) { char e[4]; v = !(GetEnvironmentVariableA("PHOTON_PTRT_MODELS", e, sizeof e) > 0 && e[0] == '0'); }
    return photon_ptrt_on() && v;
}

/* --- worker ---------------------------------------------------------------------------------------------------------------------- */
static SRWLOCK g_l = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static char g_want_level[260], g_key_level[260];
static const AmbWorkFiles *g_files;
static volatile int g_cancel;
static double g_cam[3]; static int g_have_cam;
static uint32_t *g_pack; static size_t g_pack_words; static volatile LONG g_pack_seq;   /* the newest pack, under g_l */
static int g_started;
/* the world's triangles (model 0, as the voxels take them), for the sun map (photon_ptrt_world_copy) */
static float *g_wtri; static unsigned g_wntri; static long g_wseq;   /* under g_l */
static float *g_wpbr;   /* PBR step 2 (analysis/41): per world triangle its material's roughness and metalness (2 floats), with g_wtri */
/* PBR packs (pbrpack.c): per world triangle 14 floats: its atlas tile (-1 none; + PBR_TILE_HAS_NORMAL), the texture's u and v axes (unit),
 * and each corner's texture coordinates (in repetitions of the texture); with the map's atlases, for the world-normals pass */
#include "pbrpack.h"
static float *g_wex; static PbrAtlas *g_atlas; static long g_atlas_seq;   /* under g_l */
enum { WEX = 14 };
typedef struct { float *t, *pbr, *ex; size_t n, cap; const uint8_t *ti; int nti; const uint8_t *td; int ntd; const PbrAtlas *at; } TriBuf;
static void collect_world(void *ctx, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]) {
    (void)ref;
    TriBuf *tb = ctx;
    if (tb->n == tb->cap) {
        size_t nc = tb->cap ? tb->cap * 2 : 65536;
        float *t = realloc(tb->t, sizeof(float) * 9 * nc), *q = t ? realloc(tb->pbr, sizeof(float) * 2 * nc) : NULL;
        if (t) tb->t = t;
        if (!t || !q) return;
        tb->pbr = q;
        float *x = realloc(tb->ex, sizeof(float) * WEX * nc);
        if (!x) return;
        tb->ex = x; tb->cap = nc;
    }
    float *o = tb->t + 9 * tb->n;
    for (int k = 0; k < 3; k++) { o[k] = (float)a[k]; o[3 + k] = (float)b[k]; o[6 + k] = (float)c[k]; }
    tb->pbr[2 * tb->n] = col[0]; tb->pbr[2 * tb->n + 1] = col[1];   /* the "colour" passed per texdata is its (roughness, metalness, 1) */
    float *x = tb->ex + WEX * tb->n;
    memset(x, 0, sizeof(float) * WEX); x[0] = -1;
    int ti = vox_tri_texinfo;
    if (tb->ti && tb->at && ti >= 0 && ti < tb->nti) {
        float tv[2][4]; int32_t tdi;
        memcpy(tv, tb->ti + 72 * (size_t)ti, sizeof tv); memcpy(&tdi, tb->ti + 72 * (size_t)ti + 68, 4);
        if (tdi >= 0 && tdi < tb->ntd && tdi < tb->at->ntd && tb->at->tile[tdi] >= 0) {
            int32_t W, H; memcpy(&W, tb->td + 32 * (size_t)tdi + 16, 4); memcpy(&H, tb->td + 32 * (size_t)tdi + 20, 4);
            float ls = sqrtf(tv[0][0] * tv[0][0] + tv[0][1] * tv[0][1] + tv[0][2] * tv[0][2]), lt = sqrtf(tv[1][0] * tv[1][0] + tv[1][1] * tv[1][1] + tv[1][2] * tv[1][2]);
            if (W > 0 && H > 0 && ls > 0 && lt > 0) {
                x[0] = (float)tb->at->tile[tdi];
                for (int k = 0; k < 3; k++) { x[1 + k] = tv[0][k] / ls; x[4 + k] = tv[1][k] / lt; }
                const double *v[3] = { a, b, c };
                for (int k = 0; k < 3; k++) {
                    x[7 + 2 * k] = (float)((v[k][0] * tv[0][0] + v[k][1] * tv[0][1] + v[k][2] * tv[0][2] + tv[0][3]) / W);
                    x[8 + 2 * k] = (float)((v[k][0] * tv[1][0] + v[k][1] * tv[1][1] + v[k][2] * tv[1][2] + tv[1][3]) / H);
                }
            }
        }
    }
    tb->n++;
}
/* PBR: each texdata's material through the engine's filesystem (mtl_pbr.c), as (roughness, metalness, 1) per texdata for vox_triangles */
#include "../lod/mtl_pbr.h"
#include "pbrpack.h"
static uint8_t *pbr_read(void *ctx, const char *path, size_t *n) { (void)ctx; unsigned u = 0; uint8_t *b = photon_fs_slurp(path, &u); *n = u; return b; }
static float (*texdata_pbr(const uint8_t *bsp, size_t size, int *count))[3] {
    *count = 0;
    if (size < 8 + 16 * 64) return NULL;
    const int32_t *lump = (const int32_t *)(bsp + 8);
    int32_t o2 = lump[4 * 2], l2 = lump[4 * 2 + 1], o43 = lump[4 * 43], l43 = lump[4 * 43 + 1], o44 = lump[4 * 44], l44 = lump[4 * 44 + 1];
    if (o2 < 0 || l2 <= 0 || (size_t)o2 + (size_t)l2 > size || o43 < 0 || (size_t)o43 + (size_t)l43 > size || o44 < 0 || (size_t)o44 + (size_t)l44 > size) return NULL;
    int n = l2 / 32;
    float (*out)[3] = calloc((size_t)n, sizeof *out);
    if (!out) return NULL;
    int types[24] = { 0 }, nskip = 0;
    for (int d = 0; d < n; d++) {
        int32_t si; memcpy(&si, bsp + o2 + 32 * d + 12, 4);   /* nameStringTableID */
        MtlPbr pb = { 0.75f, 0, -1, 0 };
        if (si >= 0 && si < l44 / 4) {
            int32_t so; memcpy(&so, bsp + o44 + 4 * si, 4);
            if (so >= 0 && so < l43) {
                char name[256]; size_t k = 0;
                while (so + (int32_t)k < l43 && bsp[o43 + so + k] && k + 1 < sizeof name) { name[k] = (char)bsp[o43 + so + k]; k++; }
                name[k] = 0;
                mtl_pbr_get(name, pbr_read, NULL, &pb);
            }
        }
        static int forced = -1; static float fr, fm;   /* PHOTON_PBR_FORCE="roughness metalness" (or "r,m"): every material (a test of the glossy path) */
        if (forced < 0) { char e[32]; forced = GetEnvironmentVariableA("PHOTON_PBR_FORCE", e, sizeof e) > 0 && sscanf(e, "%f%*[ ,]%f", &fr, &fm) == 2; }
        if (forced && !pb.skip) { pb.roughness = fr; pb.metalness = fm; }
        out[d][0] = pb.roughness; out[d][1] = pb.metalness; out[d][2] = 1;
        if (pb.skip) nskip++; else types[pb.type + 1 >= 0 && pb.type + 1 < 24 ? pb.type + 1 : 0]++;
    }
    char kinds[512]; int kl = 0;   /* by kind (mtl_pbr.c's table; index 0 = unknown) */
    for (int k = 0; k < 24 && kl < (int)sizeof kinds - 32; k++)
        if (types[k]) kl += snprintf(kinds + kl, sizeof kinds - (size_t)kl, "%s%s %d", kl ? ", " : "", k ? mtl_pbr_type_name(k - 1) : "other", types[k]);
    photon_log("ptrt: PBR: %d materials (%s; %d not surfaces)", n, kl ? kinds : "none", nskip);
    *count = n;
    return out;
}
/* smooth vertex normals (18 floats per triangle: position and normal per corner): a corner takes the mean of the face normals of the
 * triangles sharing its position whose normal is within 45 degrees of its own face's, so displacement terrain shades smoothly (as the
 * engine's displacement normals do) and brush edges stay sharp. Positions hashed on a 1/8-unit grid. */
static float *smooth_world(const float *t, size_t n) {
    float *o = malloc(sizeof(float) * 18 * (n ? n : 1)), (*fn)[3] = malloc(sizeof *fn * (n ? n : 1));
    size_t cap = 1; while (cap < 6 * n) cap <<= 1;
    int32_t *head = malloc(sizeof(int32_t) * cap), *next = malloc(sizeof(int32_t) * 3 * (n ? n : 1));
    if (!o || !fn || !head || !next) { free(o); free(fn); free(head); free(next); return NULL; }
    memset(head, 0xff, sizeof(int32_t) * cap);
    for (size_t i = 0; i < n; i++) {
        const float *a = t + 9 * i, *b = a + 3, *c = a + 6;
        float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0], l = sqrtf(x * x + y * y + z * z);
        fn[i][0] = l > 0 ? x / l : 0; fn[i][1] = l > 0 ? y / l : 0; fn[i][2] = l > 0 ? z / l : 1;
    }
    #define PKEY(p) ((uint64_t)(int64_t)lrintf((p)[0] * 8) * 73856093ull ^ (uint64_t)(int64_t)lrintf((p)[1] * 8) * 19349663ull ^ (uint64_t)(int64_t)lrintf((p)[2] * 8) * 83492791ull)
    for (size_t k = 0; k < 3 * n; k++) { size_t h = (size_t)(PKEY(t + 3 * k) & (cap - 1)); next[k] = head[h]; head[h] = (int32_t)k; }
    for (size_t k = 0; k < 3 * n; k++) {
        const float *p = t + 3 * k, *f = fn[k / 3];
        float m[3] = { 0, 0, 0 };
        for (int32_t q = head[PKEY(p) & (cap - 1)]; q >= 0; q = next[q]) {
            const float *pq = t + 3 * (size_t)q, *g = fn[q / 3];
            if (fabsf(pq[0] - p[0]) > 0.13f || fabsf(pq[1] - p[1]) > 0.13f || fabsf(pq[2] - p[2]) > 0.13f) continue;
            if (f[0] * g[0] + f[1] * g[1] + f[2] * g[2] < 0.7071f) continue;
            m[0] += g[0]; m[1] += g[1]; m[2] += g[2];
        }
        float l = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float *ov = o + 18 * (k / 3) + 6 * (k % 3);
        memcpy(ov, p, 12);
        ov[3] = l > 0 ? m[0] / l : f[0]; ov[4] = l > 0 ? m[1] / l : f[1]; ov[5] = l > 0 ? m[2] / l : f[2];
    }
    #undef PKEY
    free(fn); free(head); free(next);
    return o;
}
float *photon_ptrt_world_pbr_copy(unsigned n) {   /* PBR: per triangle (roughness, metalness), for the copy just taken (n triangles); caller frees */
    float *c = NULL;
    AcquireSRWLockShared(&g_l);
    if (g_wpbr && n == g_wntri && (c = malloc(sizeof(float) * 2 * (size_t)n)) != NULL) memcpy(c, g_wpbr, sizeof(float) * 2 * (size_t)n);
    ReleaseSRWLockShared(&g_l);
    return c;
}
float *photon_ptrt_world_ex_copy(unsigned n) {   /* PBR packs: per triangle WEX floats (tile, u / v axes, corner uvs), or NULL; caller frees */
    float *c = NULL;
    AcquireSRWLockShared(&g_l);
    if (g_wex && n == g_wntri && (c = malloc(sizeof(float) * WEX * (size_t)n)) != NULL) memcpy(c, g_wex, sizeof(float) * WEX * (size_t)n);
    ReleaseSRWLockShared(&g_l);
    return c;
}
/* PBR packs: the atlases (DXT1 blocks) handed to fn when newer than *seq, under the lock (fn copies them into textures); 1 if fn ran */
int photon_ptrt_atlas_with(long *seq, void (*fn)(void *ctx, const uint8_t *rmh, const uint8_t *nrm), void *ctx) {
    int ran = 0;
    AcquireSRWLockShared(&g_l);
    if (g_atlas_seq != *seq) { *seq = g_atlas_seq; fn(ctx, g_atlas ? g_atlas->rmh : NULL, g_atlas ? g_atlas->nrm : NULL); ran = 1; }
    ReleaseSRWLockShared(&g_l);
    return ran;
}
int photon_ptrt_world_copy(float **out, unsigned *n, long *seq) {   /* a copy when newer than *seq (18 floats a triangle; the caller frees it) */
    int got = 0;
    AcquireSRWLockShared(&g_l);
    if (g_wseq != *seq && g_wtri && g_wntri) {
        float *c = malloc(sizeof(float) * 18 * (size_t)g_wntri);
        if (c) { memcpy(c, g_wtri, sizeof(float) * 18 * (size_t)g_wntri); *out = c; *n = g_wntri; *seq = g_wseq; got = 1; }
    }
    ReleaseSRWLockShared(&g_l);
    return got;
}
static uint8_t *fs_read(void *ctx, const char *path, size_t *n) {   /* the engine's filesystem (GAME paths), as F3's worker reads models */
    (void)ctx;
    unsigned u = 0;
    uint8_t *b = photon_fs_slurp(path, &u);
    *n = u;
    return b;
}

static uint8_t *read_map(const AmbWorkFiles *files, const char *level, long long *size) {
    void *pin = files->pin ? files->pin() : NULL;
    int gma = 0;
    void *fh = files->open(level, &gma, &g_cancel);
    uint8_t *bsp = NULL;
    *size = 0;
    if (fh) {
        *size = files->size(fh);
        if (*size > 1036 && *size < (1LL << 30) && (bsp = malloc((size_t)*size)) != NULL)
            for (long long o = 0; o < *size && !g_cancel;) {
                int n = (int)(*size - o > (1 << 20) ? (1 << 20) : *size - o);
                if (files->read_at(fh, o, bsp + o, n) != n) { free(bsp); bsp = NULL; break; }
                o += n;
            }
        files->close(fh);
    }
    if (pin && files->unpin) files->unpin(pin);
    return bsp;
}

static DWORD WINAPI worker(LPVOID self) {
    char level[260] = "";
    uint8_t *bsp = NULL; SunMap *sm = NULL; VoxCascades *vc = NULL;
    float sun[3] = { 0, 0, 1 }, sunc[3] = { 0, 0, 0 }, sky[3] = { 0, 0, 0 };
    enum { MAXL = 1024 };
    float *lamps = malloc(sizeof(float) * MAXL * VOX_LIGHT_WORDS); int nlamp = 0;
    uint32_t *buf = malloc((size_t)CAP_WORDS * 4);
    for (;;) {
        AcquireSRWLockExclusive(&g_l);
        SleepConditionVariableSRW(&g_cv, &g_l, 100, 0);
        char want[260]; memcpy(want, g_want_level, sizeof want);
        const AmbWorkFiles *files = g_files;
        double cam[3] = { g_cam[0], g_cam[1], g_cam[2] }; int have = g_have_cam;
        g_cancel = 0;
        ReleaseSRWLockExclusive(&g_l);
        if (strcmp(want, level) != 0) {   /* a new map (or none) */
            vox_cascades_free(vc); vc = NULL; sun_map_free(sm); sm = NULL; free(bsp); bsp = NULL;
            snprintf(level, sizeof level, "%s", want);
            if (!level[0] || !files) continue;
            long long size = 0;
            bsp = read_map(files, level, &size);
            if (!bsp) { photon_log("ptrt: %s: the map could not be read", level); continue; }
            LgMap *lm = NULL;
            if (sun_map_load(bsp, (size_t)size, 1, &sm) || lg_map_load(bsp, (size_t)size, 1, &lm)) { photon_log("ptrt: %s: the map did not load", level); continue; }
            int nl = 0;
            const LgWorldLight *wl = lg_map_lights(lm, &nl);
            nlamp = 0;
            for (int i = 0; i < nl; i++) {
                if (lamps && nlamp < MAXL && (wl[i].type == LG_EMIT_POINT || wl[i].type == LG_EMIT_SPOT) && wl[i].style == 0) {   /* switchable lamps start off */
                    const LgWorldLight *L = &wl[i];
                    float *r = lamps + (size_t)nlamp++ * VOX_LIGHT_WORDS;
                    memset(r, 0, sizeof(float) * VOX_LIGHT_WORDS);
                    r[0] = L->origin[0]; r[1] = L->origin[1]; r[2] = L->origin[2]; r[3] = (float)L->type;
                    r[4] = L->intensity[0]; r[5] = L->intensity[1]; r[6] = L->intensity[2]; r[7] = L->radius;
                    float c = L->constant_attn, li = L->linear_attn, q = L->quadratic_attn;
                    if (c == 0 && li == 0 && q == 0) c = 1;
                    r[8] = c; r[9] = li; r[10] = q; r[11] = L->exponent > 0 ? L->exponent : 1;
                    r[12] = L->normal[0]; r[13] = L->normal[1]; r[14] = L->normal[2]; r[15] = L->stopdot; r[16] = L->stopdot2;
                }
                if (wl[i].type == LG_EMIT_SKYLIGHT) for (int k = 0; k < 3; k++) { sun[k] = -wl[i].normal[k]; sunc[k] = wl[i].intensity[k]; }
                if (wl[i].type == LG_EMIT_SKYAMBIENT) for (int k = 0; k < 3; k++) sky[k] += wl[i].intensity[k];
            }
            lg_map_free(lm);
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            { char e[8]; if (GetEnvironmentVariableA("PHOTON_DISP_PARITY", e, sizeof e) > 0) vox_set_disp_parity(atoi(e)); }   /* test switch */
            if (vox_cascades_new(bsp, (size_t)size, sm, NULL, 0, &vc)) { photon_log("ptrt: %s: no voxel world", level); continue; }
            {   /* the same triangles for the sun map (the world's sharp sun shadows in the composite) */
                TriBuf tb = { 0 }; long nf2 = 0, nd2 = 0;
                int ntd = 0;
                float (*tdp)[3] = texdata_pbr(bsp, (size_t)size, &ntd);   /* PBR: the triangles carry their material's roughness / metalness */
                PbrAtlas *at = pbr_atlas_build(bsp, (size_t)size);   /* PBR packs: per-texel maps where an installed pack covers the material */
                if (at) {
                    photon_log("ptrt: PBR pack: %d of %d materials in the atlas (%d with a generated normal map; %d not in a pack, %d too small; %d pack GMAs)",
                               at->ntiles, at->ntd, at->with_normal, at->missing, at->small, at->gmas);
                    if (!at->ntiles) { pbr_atlas_free(at); at = NULL; }
                }
                if (at) {
                    const int32_t *lmp = (const int32_t *)(bsp + 8);
                    int32_t o6 = lmp[4 * 6], l6 = lmp[4 * 6 + 1], o2 = lmp[4 * 2], l2 = lmp[4 * 2 + 1];
                    if (o6 >= 0 && l6 > 0 && (size_t)o6 + (size_t)l6 <= (size_t)size) { tb.ti = bsp + o6; tb.nti = l6 / 72; }
                    tb.td = bsp + o2; tb.ntd = l2 / 32; tb.at = at;
                }
                vox_triangles(bsp, (size_t)size, sm, (const float (*)[3])tdp, ntd, collect_world, &tb, &nf2, &nd2);
                free(tdp);
                if (!tdp) for (size_t i = 0; tb.pbr && i < tb.n; i++) { tb.pbr[2 * i] = 0.75f; tb.pbr[2 * i + 1] = 0; }
                float *sw = smooth_world(tb.t, tb.n);
                free(tb.t);
                AcquireSRWLockExclusive(&g_l);
                free(g_wtri); g_wtri = sw; g_wntri = sw ? (unsigned)tb.n : 0; g_wseq++;
                free(g_wpbr); g_wpbr = sw ? tb.pbr : NULL; if (!sw) free(tb.pbr);
                free(g_wex); g_wex = sw && at ? tb.ex : NULL; if (!(sw && at)) free(tb.ex);
                pbr_atlas_free(g_atlas); g_atlas = at; g_atlas_seq++;
                ReleaseSRWLockExclusive(&g_l);
            }
            VoxPropsInfo pi;   /* PT-PROPS: the static props (trees, rocks) as placed models */
            vox_cascades_add_props(vc, bsp, (size_t)size, sm, fs_read, NULL, &pi);
            QueryPerformanceCounter(&t1);
            photon_log("ptrt: %s: static props %ld of %ld placed (%ld in the 3D skybox, %ld missing), %ld of %ld models", level, pi.placed, pi.props,
                       pi.skybox, pi.missing, pi.models_loaded, pi.models);
            VoxCascadeInfo ci; vox_cascades_info(vc, &ci);
            photon_log("ptrt: %s: %d lamps; %ld triangles (%ld faces, %ld displacements) in %.0f ms; sun %.2f %.2f %.2f colour %.2f %.2f %.2f, sky %.2f %.2f %.2f", level,
                       nlamp, ci.triangles, ci.faces, ci.disps, (double)(t1.QuadPart - t0.QuadPart) * 1e3 / (double)f.QuadPart, sun[0], sun[1], sun[2],
                       sunc[0], sunc[1], sunc[2], sky[0], sky[1], sky[2]);
        }
        if (!vc || !have || !buf) continue;
        VoxCascadeInfo before; vox_cascades_info(vc, &before);
        vox_cascades_update(vc, cam);
        VoxCascadeInfo after; vox_cascades_info(vc, &after);
        if (after.rebuilds == before.rebuilds && g_pack_seq) continue;   /* nothing moved */
        size_t n = vox_cascades_pack(vc, buf, CAP_WORDS, sun, sunc, sky, lamps, nlamp);
        if (!n) { photon_log("ptrt: the cascades do not fit the GPU buffer"); continue; }
        AcquireSRWLockExclusive(&g_l);
        uint32_t *nb = realloc(g_pack, n * 4);
        if (nb) { memcpy(nb, buf, n * 4); g_pack = nb; g_pack_words = n; g_pack_seq++; }
        ReleaseSRWLockExclusive(&g_l);
        static long logged;
        if (logged++ < 3 || after.rebuilds % 50 == 0)
            photon_log("ptrt: cascades at %.0f %.0f %.0f: %ld rebuilds, bricks %ld %ld %ld %ld %ld, %.1f MiB packed", cam[0], cam[1], cam[2], after.rebuilds,
                       after.bricks[0], after.bricks[1], after.bricks[2], after.bricks[3], after.bricks[4], n * 4 / 1048576.0);
    }
    (void)self;
    return 0;
}

void photon_ptrt_frame(const AmbWorkFiles *files, const char *level) {
    if (!photon_ptrt_on()) return;
    if (!level) level = "";
    if (!g_started && level[0]) {
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!h) { photon_log("ptrt: no worker thread"); return; }
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
        g_started = 1;
    }
    if (strcmp(level, g_key_level) == 0) return;
    snprintf(g_key_level, sizeof g_key_level, "%s", level);
    AcquireSRWLockExclusive(&g_l);
    snprintf(g_want_level, sizeof g_want_level, "%s", level);
    g_files = files; g_cancel = 1;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_l);
    photon_log("ptrt: map %s", level[0] ? level : "(none)");
}

static long g_rad_kept, g_rad_new;   /* bricks carried over / new, over all packs (the dump's log line) */
/* the carry-over table for a new pack (ptrt_remap.comp): per new brick, the old pack's number of the same world brick (level and
 * position: the level's origin plus the page's cell), 0 where it is new. The pages of both packs: words 64 + level x 4096 + cell. */
static int remap_table(const uint32_t *old, const uint32_t *nw, uint32_t *out) {
    uint32_t n = nw[32];
    if (n > 6144) return 0;
    memset(out, 0, (1 + (size_t)n) * 4);
    out[0] = n;
    long kept = 0;
    for (int l = 0; l < VOXC_LEVELS; l++) {
        int d[3];
        for (int a = 0; a < 3; a++) d[a] = (int)nw[l * 4 + a] - (int)old[l * 4 + a];   /* new origin - old origin, in bricks */
        for (int c = 0; c < 4096; c++) {
            uint32_t b = nw[64 + l * 4096 + c];
            if (!b) continue;
            int k[3] = { c % 16 + d[0], (c / 16) % 16 + d[1], c / 256 + d[2] };   /* the same world brick in the old window */
            if (k[0] < 0 || k[0] > 15 || k[1] < 0 || k[1] > 15 || k[2] < 0 || k[2] > 15) continue;
            uint32_t o = old[64 + l * 4096 + k[0] + 16 * k[1] + 256 * k[2]];
            if (o && o <= 6144) { out[b] = o; kept++; }
        }
    }
    g_rad_kept += kept; g_rad_new += (long)n - kept;
    return 1;
}

/* --- the bridge pass ------------------------------------------------------------------------------------------------------------- */
static const uint32_t k_atrous_spv[] =
#include "shaders/vk/atrous.spv.inc"
;
static const uint32_t k_cache_spv[] =
#include "shaders/vk/ptrt_cache.spv.inc"
;
static const uint32_t k_ao_spv[] =
#include "shaders/vk/ptrt_ao.spv.inc"
;
static int g_lsplit_on;
/* SP layer 2 (analysis/47): the engine's dynamic lights this frame (lights.c), per UBO slot in a host-visible ring: slot x DYN_SLOT floats */
enum { MAXDYN = 96, DYN_SLOT = MAXDYN * VOX_LIGHT_WORDS };
static float g_dyn[DYN_SLOT]; static int g_ndyn; static SRWLOCK g_dyn_l = SRWLOCK_INIT;
static VkBuffer g_dynbuf; static VkDeviceMemory g_dynbuf_mem; static float *g_dynmap;
void photon_ptrt_set_dyn_lamps(const float *rec, int n) {
    if (n > MAXDYN) n = MAXDYN;
    AcquireSRWLockExclusive(&g_dyn_l);
    if (n > 0) memcpy(g_dyn, rec, sizeof(float) * VOX_LIGHT_WORDS * (size_t)n);
    g_ndyn = n < 0 ? 0 : n;
    ReleaseSRWLockExclusive(&g_dyn_l);
}
static VkPipelineLayout g_aopl; static VkPipeline g_aopipe; static float g_ao_radius = 24.0f, g_ao_strength = 1.5f;   /* item 4: contact AO */
static const uint32_t k_remap_spv[] =
#include "shaders/vk/ptrt_remap.spv.inc"
;
/* the radiance cache (ptrt_vox.glsl): 2 words per voxel side (2) of the pack's brick capacity, on the GPU; PHOTON_PTRT_CACHE voxels gathered per
 * frame (a slice of all the cascades' voxel slots, page-major; empty ones cost a read). One cache per pack buffer: a new pack's cache is
 * the old one's entries carried over brick by brick (ptrt_remap.comp, analysis/46 item 1: the same world brick under its new number), so
 * a cascade scroll no longer clears the gathered light; PHOTON_PTRT_REMAP=0 clears as before */
enum { RAD_WORDS = 6144 * 512 * 4, VOX_SLOTS = VOXC_LEVELS * VOXC_WIN * VOXC_WIN * VOXC_WIN * 512 };
static VkBuffer g_rad[2]; static VkDeviceMemory g_rad_mem[2];
static VkBuffer g_remap[2]; static VkDeviceMemory g_remap_mem[2]; static uint32_t *g_remap_map[2];   /* per pack buffer: its bricks' old numbers */
static VkDescriptorSetLayout g_rdsl; static VkDescriptorSet g_rset[2]; static VkPipelineLayout g_rpl; static VkPipeline g_rpipe;
static int g_remap_on = 1, g_remap_pending = -1;   /* g_remap_pending: the buffer whose cache to fill next pass */
static VkBuffer g_radrb; static VkDeviceMemory g_radrb_mem; static void *g_radrb_map; static int g_radrb_done;   /* the dump's cache copy */
static long long g_dump_at = -2;
/* auto exposure: the trace pass sums log2 luminance per frame slot (NUBO slots, read INFLIGHT frames later); the composite's exposure
 * adapts toward 0.3 / the geometric mean (an eye's adaptation: a dim interior is brought up, never below 1 so lit scenes keep the map's
 * brightness, at most 6), with a 0.8 s time constant; PHOTON_PTRT_EXPOSURE=x fixes it instead */
static VkBuffer g_lumbuf; static VkDeviceMemory g_lumbuf_mem; static int32_t *g_lum; static int g_auto = 1; static float g_expo = 1;
static LARGE_INTEGER g_expo_t; static float g_expo_gm;
static VkDescriptorSetLayout g_cdsl; static VkDescriptorSet g_cset[NBUF]; static VkPipelineLayout g_cpl; static VkPipeline g_cpipe;
static long g_rad_seq = -1, g_rad_clears; static int g_rad_cur = -1; static uint32_t g_rad_start, g_rad_budget = 262144;   /* a full sweep about every 40 frames */ static PFN_vkCmdFillBuffer pFill;
enum { ASTEPS = 4 };   /* the a-trous filter's steps (analysis/46 item 3: 4, was 3) */
enum { NUBO = 8, UBO_STRIDE = 256, NIMG = 18 };
/* images: the scene depth (wrapped), the accumulated light and the eye distances, each twice (this frame's / last frame's), and the
 * filter's two buffers (the result ends in g_fa) */
static PhotonVkImage g_depth, g_hist[2], g_dist[2], g_fa, g_fb, g_sunvis;
static PhotonVkImage g_motion;   /* the moving models' motion vectors (full size; drawn by shadows.c before the trace) */
static PhotonVkImage g_mnrm;     /* the models' world normals (PT-RT models: shadows.c's velocity pass) */
static PhotonVkImage g_pbr_img;  /* PBR step 3: the world's roughness / metalness (shadows.c's world-normals pass, MRT 1) */
static PhotonVkImage g_la, g_lb;   /* item 2: the lamps' own filter chain (lhist -> la -> lb -> la -> lb), no brightness stop */
static PhotonVkImage g_nrmimg;   /* item 3: the trace's normals, for the filter's normal weight */
static PhotonVkImage g_lhist[2];   /* analysis/46 item 2: the lamps' direct light, its own short history (this frame's / last frame's) */
static PhotonVkImage g_spec[2];  /* PBR step 3: the glossy reflections (half size), accumulated as the light (this frame's / last frame's) */
static void *g_wpbr_tex; static volatile int g_pbr_on;
void photon_ptrt_pbr_drawn(int on) { g_pbr_on = on; }
static void *g_mnrm_tex;
static volatile int g_motion_on;  /* this frame's motion vectors were drawn: the history follows them */
static void *g_motion_tex;        /* the D3D9 texture wrapped in g_motion when the pass was bound (NULL: none) */
void *photon_ptrt_motion_tex(void) { return g_motion_tex; }
void *photon_ptrt_model_nrm_tex(void) { return g_mnrm_tex; }
static volatile int g_mnrm_on;
void photon_ptrt_model_nrm_drawn(int on) { g_mnrm_on = on; }
void photon_ptrt_motion_drawn(int on) { g_motion_on = on && g_motion_tex && photon_shadows_motion_tex() == g_motion_tex; }
static PhotonVkImage *g_imgs[NIMG] = { &g_depth, &g_hist[0], &g_hist[1], &g_dist[0], &g_dist[1], &g_fa, &g_fb, &g_sunvis, &g_motion, &g_mnrm, &g_pbr_img, &g_spec[0], &g_spec[1], &g_lhist[0], &g_lhist[1], &g_nrmimg, &g_la, &g_lb };
static void *g_depth_tex;
static VkBuffer g_buf[NBUF], g_ubo; static VkDeviceMemory g_mem[NBUF], g_ubo_mem; static void *g_map[NBUF], *g_ubo_map;
static long long g_buf_frame[NBUF]; static int g_cur = -1;
static long g_up_seq;
static VkSampler g_lin, g_near;
static VkDescriptorSetLayout g_dsl, g_adsl; static VkDescriptorPool g_dpool;
static VkDescriptorSet g_tset[NBUF][2], g_aset[2][ASTEPS], g_lset[2][ASTEPS];   /* trace: [pack buffer][parity]; filter: [parity][step] */
static VkPipelineLayout g_pl, g_apl; static VkPipeline g_pipe, g_apipe;
static PFN_vkCmdBindPipeline pBind; static PFN_vkCmdBindDescriptorSets pSets; static PFN_vkCmdPushConstants pPush; static PFN_vkCmdDispatch pDispatch;
static PFN_vkUpdateDescriptorSets pUpd; static PFN_vkCmdPipelineBarrier pBarrier;
static long long g_frame, g_dispatches;
static float g_iv[4][4], g_vp[4][4], g_eye[3], g_pvp[4][4], g_peye[3];
static int g_have_prev, g_ready, g_par;
static float g_strength = 1.0f, g_maxhist = 32.0f;   /* g_strength: the composite's exposure (PHOTON_PTRT_EXPOSURE) */

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    const PhotonVk *vk = photon_vk();
    PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)photon_vk_proc("vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements gr = (PFN_vkGetBufferMemoryRequirements)photon_vk_proc("vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)photon_vk_proc("vkAllocateMemory");
    PFN_vkBindBufferMemory bm = (PFN_vkBindBufferMemory)photon_vk_proc("vkBindBufferMemory");
    PFN_vkMapMemory mm = (PFN_vkMapMemory)photon_vk_proc("vkMapMemory");
    if (!cb || !gr || !am || !bm || !mm) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (cb(vk->dev, &bi, NULL, b)) return 0;
    VkMemoryRequirements mr; gr(vk->dev, *b, &mr);
    uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == ~0u) return 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    return !am(vk->dev, &ai, NULL, m) && !bm(vk->dev, *b, *m, 0) && !mm(vk->dev, *m, 0, VK_WHOLE_SIZE, 0, map);
}
static int make_device_buffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m) {
    const PhotonVk *vk = photon_vk();
    PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)photon_vk_proc("vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements gr = (PFN_vkGetBufferMemoryRequirements)photon_vk_proc("vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)photon_vk_proc("vkAllocateMemory");
    PFN_vkBindBufferMemory bm = (PFN_vkBindBufferMemory)photon_vk_proc("vkBindBufferMemory");
    if (!cb || !gr || !am || !bm) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (cb(vk->dev, &bi, NULL, b)) return 0;
    VkMemoryRequirements mr; gr(vk->dev, *b, &mr);
    uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == ~0u) return 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    return !am(vk->dev, &ai, NULL, m) && !bm(vk->dev, *b, *m, 0);
}
static VkPipeline make_pipe(const uint32_t *code, size_t bytes, VkPipelineLayout pl) {
    const PhotonVk *vk = photon_vk();
    PFN_vkCreateShaderModule csm = (PFN_vkCreateShaderModule)photon_vk_proc("vkCreateShaderModule");
    PFN_vkCreateComputePipelines ccp = (PFN_vkCreateComputePipelines)photon_vk_proc("vkCreateComputePipelines");
    VkShaderModule sm; VkPipeline p = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code };
    if (!csm || !ccp || csm(vk->dev, &smi, NULL, &sm)) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL },
                                        pl, VK_NULL_HANDLE, -1 };
    if (ccp(vk->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p)) return VK_NULL_HANDLE;
    return p;
}

static int setup(void *u) {
    (void)u;
    const PhotonVk *vk = photon_vk();
#define P(n) PFN_##n n = (PFN_##n)photon_vk_proc(#n); if (!n) return 0
    P(vkCreateSampler); P(vkCreateDescriptorSetLayout); P(vkCreateDescriptorPool); P(vkAllocateDescriptorSets); P(vkCreatePipelineLayout);
    pBind = (PFN_vkCmdBindPipeline)photon_vk_proc("vkCmdBindPipeline"); pSets = (PFN_vkCmdBindDescriptorSets)photon_vk_proc("vkCmdBindDescriptorSets");
    pPush = (PFN_vkCmdPushConstants)photon_vk_proc("vkCmdPushConstants"); pDispatch = (PFN_vkCmdDispatch)photon_vk_proc("vkCmdDispatch");
    pUpd = (PFN_vkUpdateDescriptorSets)photon_vk_proc("vkUpdateDescriptorSets"); pBarrier = (PFN_vkCmdPipelineBarrier)photon_vk_proc("vkCmdPipelineBarrier");
    pFill = (PFN_vkCmdFillBuffer)photon_vk_proc("vkCmdFillBuffer");
    if (!pBind || !pSets || !pPush || !pDispatch || !pUpd || !pBarrier || !pFill) return 0;
    { char e[4]; g_remap_on = !(GetEnvironmentVariableA("PHOTON_PTRT_REMAP", e, sizeof e) > 0 && e[0] == '0'); }
    for (int i = 0; i < 2; i++) {
        if (!make_device_buffer((VkDeviceSize)RAD_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_rad[i], &g_rad_mem[i])) return 0;
        if (!make_buffer((VkDeviceSize)(1 + 6144) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_remap[i], &g_remap_mem[i], (void **)&g_remap_map[i])) return 0;
    }
    { char e[16]; g_dump_at = GetEnvironmentVariableA("PHOTON_PTRT_DUMP", e, sizeof e) > 0 ? atoll(e) : -1; }
    if (g_dump_at >= 0 && !make_buffer((VkDeviceSize)RAD_WORDS * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_radrb, &g_radrb_mem, &g_radrb_map)) g_radrb = VK_NULL_HANDLE;
    for (int i = 0; i < NBUF; i++) {
        if (!make_buffer((VkDeviceSize)CAP_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_buf[i], &g_mem[i], &g_map[i])) return 0;
        g_buf_frame[i] = -INFLIGHT;
    }
    if (!make_buffer(NUBO * UBO_STRIDE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &g_ubo, &g_ubo_mem, &g_ubo_map)) return 0;
    if (!make_buffer((VkDeviceSize)NUBO * DYN_SLOT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_dynbuf, &g_dynbuf_mem, (void **)&g_dynmap)) return 0;
    if (!make_buffer(NUBO * 2 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_lumbuf, &g_lumbuf_mem, (void **)&g_lum)) return 0;
    memset(g_lum, 0, NUBO * 2 * 4);
    for (int k = 0; k < 2; k++) {
        VkFilter f = k ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, f, f, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_FALSE };
        if (vkCreateSampler(vk->dev, &si, NULL, k ? &g_lin : &g_near)) return 0;
    }
    const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding tb[21] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL },
                                           { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL }, { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL },
                                           { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL }, { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL },
                                           { 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, C, NULL }, { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL },
                                           { 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL }, { 9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL },
                                           { 10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL }, { 11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL },
                                           { 12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL }, { 13, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL },
                                           { 14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL }, { 15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL },
                                           { 16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL }, { 17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL },
                                           { 18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL }, { 19, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL },
                                           { 20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 21, tb };
    if (vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &g_dsl)) return 0;
    VkDescriptorSetLayoutBinding ab[4] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL }, { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL },
                                           { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, C, NULL }, { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, C, NULL } };
    VkDescriptorSetLayoutCreateInfo adli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, ab };
    if (vkCreateDescriptorSetLayout(vk->dev, &adli, NULL, &g_adsl)) return 0;
    VkDescriptorSetLayoutBinding cbd[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL }, { 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL } };
    VkDescriptorSetLayoutCreateInfo cdli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, cbd };
    if (vkCreateDescriptorSetLayout(vk->dev, &cdli, NULL, &g_cdsl)) return 0;
    VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 160 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 96 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 40, 4, ps };
    if (vkCreateDescriptorPool(vk->dev, &dpi, NULL, &g_dpool)) return 0;
    VkDescriptorSetLayout tl[4] = { g_dsl, g_dsl, g_dsl, g_dsl }, al[4 * ASTEPS]; for (int i = 0; i < 4 * ASTEPS; i++) al[i] = g_adsl;
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, 4, tl };
    if (vkAllocateDescriptorSets(vk->dev, &dai, &g_tset[0][0])) return 0;
    VkDescriptorSetAllocateInfo adai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, 2 * ASTEPS, al };
    if (vkAllocateDescriptorSets(vk->dev, &adai, &g_aset[0][0])) return 0;
    if (vkAllocateDescriptorSets(vk->dev, &adai, &g_lset[0][0])) return 0;   /* the lamps' chain: as many again */
    VkDescriptorSetLayout cl[NBUF]; for (int i = 0; i < NBUF; i++) cl[i] = g_cdsl;
    VkDescriptorSetAllocateInfo cdai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, NBUF, cl };
    if (vkAllocateDescriptorSets(vk->dev, &cdai, g_cset)) return 0;
    for (int i = 0; i < NBUF; i++) {
        VkDescriptorBufferInfo pb = { g_buf[i], 0, VK_WHOLE_SIZE }, rb = { g_rad[i], 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_cset[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &pb, NULL },
                                       { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_cset[i], 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &rb, NULL } };
        pUpd(vk->dev, 2, wr, 0, NULL);
    }
    VkPushConstantRange cpc = { C, 0, 16 };
    VkPipelineLayoutCreateInfo cpli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_cdsl, 1, &cpc };
    if (vkCreatePipelineLayout(vk->dev, &cpli, NULL, &g_cpl)) return 0;
    if (!(g_cpipe = make_pipe(k_cache_spv, sizeof k_cache_spv, g_cpl))) return 0;
    {   /* the cache carry-over: set i fills buffer i's cache from the other one's */
        VkDescriptorSetLayoutBinding rbd[3] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL },
                                                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, C, NULL } };
        VkDescriptorSetLayoutCreateInfo rdli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, rbd };
        if (vkCreateDescriptorSetLayout(vk->dev, &rdli, NULL, &g_rdsl)) return 0;
        VkDescriptorSetLayout rl[2] = { g_rdsl, g_rdsl };
        VkDescriptorSetAllocateInfo rdai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, 2, rl };
        if (vkAllocateDescriptorSets(vk->dev, &rdai, g_rset)) return 0;
        for (int i = 0; i < 2; i++) {
            VkDescriptorBufferInfo ob = { g_rad[1 - i], 0, VK_WHOLE_SIZE }, nb = { g_rad[i], 0, VK_WHOLE_SIZE }, mb = { g_remap[i], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet wr[3] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_rset[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &ob, NULL },
                                           { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_rset[i], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &nb, NULL },
                                           { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_rset[i], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &mb, NULL } };
            pUpd(vk->dev, 3, wr, 0, NULL);
        }
        VkPipelineLayoutCreateInfo rpli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_rdsl, 0, NULL };
        if (vkCreatePipelineLayout(vk->dev, &rpli, NULL, &g_rpl)) return 0;
        if (!(g_rpipe = make_pipe(k_remap_spv, sizeof k_remap_spv, g_rpl))) return 0;
    }
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 0, NULL };
    if (vkCreatePipelineLayout(vk->dev, &pli, NULL, &g_pl)) return 0;
    {   /* contact AO (ptrt_ao.comp): the trace's set, plus its radius and strength; PHOTON_PTRT_AO="radius strength" (0: off) */
        VkPushConstantRange aopc = { C, 0, 12 };
        VkPipelineLayoutCreateInfo aoli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 1, &aopc };
        char e[32];
        if (GetEnvironmentVariableA("PHOTON_PTRT_AO", e, sizeof e) > 0) { g_ao_strength = 1.5f; if (sscanf(e, "%f %f", &g_ao_radius, &g_ao_strength) < 1) g_ao_radius = 0; }
        /* always made: it also adds the lamps' chain back (radius 0: no AO) */
        if (vkCreatePipelineLayout(vk->dev, &aoli, NULL, &g_aopl) || !(g_aopipe = make_pipe(k_ao_spv, sizeof k_ao_spv, g_aopl))) g_aopipe = VK_NULL_HANDLE;
    }
    VkPushConstantRange apc = { C, 0, 8 };
    VkPipelineLayoutCreateInfo apli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_adsl, 1, &apc };
    if (vkCreatePipelineLayout(vk->dev, &apli, NULL, &g_apl)) return 0;
    if (!(g_pipe = make_pipe(k_spv, sizeof k_spv, g_pl)) || !(g_apipe = make_pipe(k_atrous_spv, sizeof k_atrous_spv, g_apl))) return 0;
    char e[32];
    if (GetEnvironmentVariableA("PHOTON_PTRT_EXPOSURE", e, sizeof e) > 0) { g_strength = (float)atof(e); g_auto = 0; }
    if (GetEnvironmentVariableA("PHOTON_PTRT_CACHE", e, sizeof e) > 0) g_rad_budget = (uint32_t)atol(e);
    photon_log("ptrt: pass ready (2 x %.0f MiB pack buffers, a %.0f MiB radiance cache gathering %u of %u voxel slots a frame, exposure %.2f)",
               CAP_WORDS * 4 / 1048576.0, RAD_WORDS * 4 / 1048576.0, g_rad_budget, (unsigned)VOX_SLOTS, g_strength);
    return 1;
}

static void write_img(VkDescriptorSet set, uint32_t binding, VkDescriptorType t, VkSampler s, const PhotonVkImage *im, VkWriteDescriptorSet *w, VkDescriptorImageInfo *ii) {
    *ii = (VkDescriptorImageInfo){ s, im->view, VK_IMAGE_LAYOUT_GENERAL };
    *w = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, binding, 0, 1, t, ii, NULL, NULL };
}
/* the images that depend on the scene's size and their descriptor sets */
static int bind_images(void *depth, unsigned w, unsigned h) {
    if (depth == g_depth_tex && g_fa.tex) return 1;
    if (g_fa.tex || g_depth.tex) return 0;   /* v0: no resize (a new depth texture after a video change leaves the pass off) */
    unsigned hw = (w + 1) / 2, hh = (h + 1) / 2;
    if (!photon_vk_wrap(depth, &g_depth)) return 0;
    for (int k = 0; k < 2; k++)
        if (!photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_hist[k]) || !photon_vk_image(hw, hh, D3DFMT_R32F, 0, &g_dist[k])) return 0;
    if (!photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, D3DUSAGE_RENDERTARGET, &g_fa) || !photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_fb)) return 0;
    if (!photon_vk_image(hw, hh, D3DFMT_R32F, 0, &g_sunvis)) return 0;
    for (int k = 0; k < 2; k++) if (!photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_lhist[k])) return 0;
    if (!photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_nrmimg)) return 0;
    if (!photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_la) || !photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_lb)) return 0;
    /* the models' motion: shadows.c's own D3D9 target, wrapped as the depth is (an interop-created image drawn by D3D9 and read here lost
     * the device on lavapipe); none yet (motion vectors off): the sun visibility image stands in, never read (opt.w 0) */
    void *mt = photon_shadows_motion_tex();
    if (mt && !photon_vk_wrap(mt, &g_motion)) return 0;
    g_motion_tex = mt;
    void *pt = photon_shadows_world_pbr_tex();
    if (pt && !photon_vk_wrap(pt, &g_pbr_img)) return 0;
    g_wpbr_tex = pt;
    for (int k = 0; k < 2; k++) if (!photon_vk_image(hw, hh, D3DFMT_A16B16G16R16F, 0, &g_spec[k])) return 0;
    void *nt = photon_shadows_model_nrm_tex();
    if (nt && !photon_vk_wrap(nt, &g_mnrm)) return 0;
    g_mnrm_tex = nt;
    g_depth_tex = depth;
    const VkDescriptorType SI = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, ST = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    for (int b = 0; b < NBUF; b++)
        for (int p = 0; p < 2; p++) {
            VkDescriptorSet s = g_tset[b][p];
            VkDescriptorBufferInfo dbi = { g_buf[b], 0, VK_WHOLE_SIZE }, ubi = { g_ubo, 0, UBO_STRIDE };
            VkWriteDescriptorSet wr[21]; VkDescriptorImageInfo ii[16];
            write_img(s, 15, ST, VK_NULL_HANDLE, &g_lhist[p], &wr[15], &ii[11]);
            write_img(s, 16, SI, g_lin, &g_lhist[1 - p], &wr[16], &ii[12]);
            write_img(s, 17, ST, VK_NULL_HANDLE, &g_fa, &wr[17], &ii[13]);
            write_img(s, 18, ST, VK_NULL_HANDLE, &g_nrmimg, &wr[18], &ii[14]);
            write_img(s, 19, SI, g_near, &g_lb, &wr[19], &ii[15]);
            VkDescriptorBufferInfo dyi = { g_dynbuf, 0, VK_WHOLE_SIZE };
            wr[20] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 20, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dyi, NULL };
            write_img(s, 9, ST, VK_NULL_HANDLE, &g_sunvis, &wr[9], &ii[5]);
            write_img(s, 10, SI, g_near, g_motion_tex ? &g_motion : &g_sunvis, &wr[10], &ii[6]);
            write_img(s, 11, SI, g_near, g_mnrm_tex ? &g_mnrm : &g_sunvis, &wr[11], &ii[7]);
            write_img(s, 12, SI, g_near, g_wpbr_tex ? &g_pbr_img : &g_sunvis, &wr[12], &ii[8]);
            write_img(s, 13, ST, VK_NULL_HANDLE, &g_spec[p], &wr[13], &ii[9]);
            write_img(s, 14, SI, g_lin, &g_spec[1 - p], &wr[14], &ii[10]);
            VkDescriptorBufferInfo lbi = { g_lumbuf, 0, VK_WHOLE_SIZE };
            wr[8] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 8, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &lbi, NULL };
            VkDescriptorBufferInfo rbi = { g_rad[b], 0, VK_WHOLE_SIZE };
            wr[7] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &rbi, NULL };
            wr[0] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
            write_img(s, 1, SI, g_near, &g_depth, &wr[1], &ii[0]);
            write_img(s, 2, ST, VK_NULL_HANDLE, &g_hist[p], &wr[2], &ii[1]);
            write_img(s, 3, SI, g_lin, &g_hist[1 - p], &wr[3], &ii[2]);
            write_img(s, 4, ST, VK_NULL_HANDLE, &g_dist[p], &wr[4], &ii[3]);
            write_img(s, 5, SI, g_near, &g_dist[1 - p], &wr[5], &ii[4]);
            wr[6] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 6, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &ubi, NULL };
            pUpd(photon_vk()->dev, 21, wr, 0, NULL);
        }
    for (int p = 0; p < 2; p++) {   /* the filter (4 steps: 1, 2, 4, 8 px): fa (the trace's bounce + lamps) -> fb -> fa -> fb -> fa; distances from dist[p] */
        const PhotonVkImage *src[ASTEPS] = { &g_fa, &g_fb, &g_fa, &g_fb }, *dst[ASTEPS] = { &g_fb, &g_fa, &g_fb, &g_fa };
        for (int k = 0; k < ASTEPS; k++) {
            VkWriteDescriptorSet wr[4]; VkDescriptorImageInfo ii[4];
            write_img(g_aset[p][k], 0, SI, g_near, src[k], &wr[0], &ii[0]);
            write_img(g_aset[p][k], 1, SI, g_near, &g_dist[p], &wr[1], &ii[1]);
            write_img(g_aset[p][k], 2, ST, VK_NULL_HANDLE, dst[k], &wr[2], &ii[2]);
            write_img(g_aset[p][k], 3, SI, g_near, &g_nrmimg, &wr[3], &ii[3]);
            pUpd(photon_vk()->dev, 4, wr, 0, NULL);
        }
        const PhotonVkImage *ls[ASTEPS] = { &g_lhist[p], &g_la, &g_lb, &g_la }, *ld[ASTEPS] = { &g_la, &g_lb, &g_la, &g_lb };   /* ends in lb */
        for (int k = 0; k < ASTEPS; k++) {
            VkWriteDescriptorSet wr[4]; VkDescriptorImageInfo ii[4];
            write_img(g_lset[p][k], 0, SI, g_near, ls[k], &wr[0], &ii[0]);
            write_img(g_lset[p][k], 1, SI, g_near, &g_dist[p], &wr[1], &ii[1]);
            write_img(g_lset[p][k], 2, ST, VK_NULL_HANDLE, ld[k], &wr[2], &ii[2]);
            write_img(g_lset[p][k], 3, SI, g_near, &g_nrmimg, &wr[3], &ii[3]);
            pUpd(photon_vk()->dev, 4, wr, 0, NULL);
        }
    }
    return 1;
}

static void cbarrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    pBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void record(VkCommandBuffer cb, void *u) {
    (void)u;
    if (!g_ready || g_cur < 0) return;
    g_buf_frame[g_cur] = g_frame;
    /* this frame's view into its UBO slot (the slot is reused NUBO frames later, past every frame in flight) */
    uint32_t slot = (uint32_t)(g_frame % NUBO) * UBO_STRIDE;
    float *v = (float *)((uint8_t *)g_ubo_map + slot);
    memcpy(v, g_iv, 64); memcpy(v + 16, g_have_prev ? g_pvp : g_vp, 64);
    v[32] = g_eye[0]; v[33] = g_eye[1]; v[34] = g_eye[2]; v[35] = (float)(g_frame % 65536);
    const float *pe = g_have_prev ? g_peye : g_eye;
    v[36] = pe[0]; v[37] = pe[1]; v[38] = pe[2]; v[39] = g_maxhist;
    static int dbg = -1;
    if (dbg < 0) { char e[4]; dbg = GetEnvironmentVariableA("PHOTON_PTRT_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    v[40] = photon_dynq_ptrt_checker() ? 1.0f : 0.0f; v[41] = (float)dbg; v[42] = (float)(g_frame % NUBO); v[43] = g_motion_on ? 1.0f : 0.0f;
    v[44] = g_mnrm_on && g_mnrm_tex && photon_shadows_model_nrm_tex() == g_mnrm_tex ? 1.0f : 0.0f; v[45] = g_pbr_on && g_wpbr_tex && photon_shadows_world_pbr_tex() == g_wpbr_tex ? 1.0f : 0.0f; /* off by default: it now works (its own filter chain, no darkening) but costs ~3 ms on the laptop, and the lamps are the map's
     * static lights, so lag shows only at disocclusions; worth it once dynamic lamps join (analysis/46 item 2) */
    static int lsplit = -1; if (lsplit < 0) { char e[4]; lsplit = GetEnvironmentVariableA("PHOTON_PTRT_LAMPHIST", e, sizeof e) > 0 && e[0] == '1'; }
    static int ffcap = -1; if (ffcap < 0) { char e[4]; ffcap = GetEnvironmentVariableA("PHOTON_PTRT_FFCAP", e, sizeof e) > 0 && e[0] == '1'; }
    v[46] = (float)lsplit; v[47] = (float)ffcap; g_lsplit_on = lsplit;
    static float rayofs = -1; if (rayofs < 0) { char e[16]; rayofs = GetEnvironmentVariableA("PHOTON_PTRT_RAYOFS", e, sizeof e) > 0 ? (float)atof(e) : 2.0f; }
    static int norep = -1; if (norep < 0) { char e[4]; norep = GetEnvironmentVariableA("PHOTON_PTRT_NOREPROJ", e, sizeof e) > 0 && e[0] == '1'; }
    {   /* SP layer 2: this frame's dynamic lamps into the slot's part of the ring (opt3.z its first float, opt3.w the count) */
        int sl = (int)(g_frame % NUBO);
        AcquireSRWLockShared(&g_dyn_l);
        int nd = g_ndyn;
        if (nd) memcpy(g_dynmap + (size_t)sl * DYN_SLOT, g_dyn, sizeof(float) * VOX_LIGHT_WORDS * (size_t)nd);
        ReleaseSRWLockShared(&g_dyn_l);
        static long long logged; if (nd && logged++ % 600 == 0) photon_log("ptrt: %d dynamic lamp(s) this frame (the engine's dlights / elights)", nd);
        v[48] = rayofs; v[49] = (float)norep; v[50] = (float)(sl * DYN_SLOT); v[51] = (float)nd;
    }   /* opt3.x: the pixel rays' start, units out along the normal (2, as before; 9 was a wrong lead for the stripes) */   /* opt2.z: the lamps' own history (analysis/46 item 2) */   /* opt2.x: the models' normals; y: PBR (glossy reflections) */
    static long long mframes;
    if (g_motion_on && (++mframes == 1 || mframes % 100 == 0)) photon_log("ptrt: history follows the models' motion vectors (%lld frames)", mframes);   /* DYN ptcheck; the cache view */
    /* the radiance cache: a new pack's cache carried over from the old one's (ptrt_remap.comp) or cleared, then this frame's slice gathered */
    if (g_rad_cur != g_cur || g_up_seq != g_rad_seq) {
        if (g_remap_pending == g_cur) {
            pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rpipe);
            pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rpl, 0, 1, &g_rset[g_cur], 0, NULL);
            pDispatch(cb, (g_remap_map[g_cur][0] * 2048u + 63) / 64, 1, 1);
            cbarrier(cb);
        } else {
            pFill(cb, g_rad[g_cur], 0, VK_WHOLE_SIZE, 0);
            VkMemoryBarrier fb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
            pBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fb, 0, NULL, 0, NULL);
            g_rad_clears++;
        }
        g_remap_pending = -1; g_rad_cur = g_cur; g_rad_seq = g_up_seq;
    }
    if (g_rad_budget) {
        struct { uint32_t start, count, frame, total; } cpc = { g_rad_start, g_rad_budget, (uint32_t)g_frame, VOX_SLOTS };
        pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_cpipe);
        pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_cpl, 0, 1, &g_cset[g_cur], 0, NULL);
        pPush(cb, g_cpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cpc, &cpc);
        pDispatch(cb, (g_rad_budget + 63) / 64, 1, 1);
        g_rad_start = (g_rad_start + g_rad_budget) % VOX_SLOTS;
        cbarrier(cb);
    }
    pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &g_tset[g_cur][g_par], 1, &slot);
    pDispatch(cb, (g_fa.w + 7) / 8, (g_fa.h + 7) / 8, 1);
    pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_apipe);
    for (int k = 0; k < ASTEPS; k++) {
        cbarrier(cb);
        static int fresh = -1;   /* PHOTON_PTRT_FRESHBLUR=0: the filter's brightness stop fixed (not widened for pixels with little history) */
        if (fresh < 0) { char e[4]; fresh = !(GetEnvironmentVariableA("PHOTON_PTRT_FRESHBLUR", e, sizeof e) > 0 && e[0] == '0'); }
        static int nsteps = -1;   /* PHOTON_PTRT_ASTEPS=n: only the first n steps filter, the rest copy (step 0: every tap the centre) */
        if (nsteps < 0) { char e[4]; nsteps = GetEnvironmentVariableA("PHOTON_PTRT_ASTEPS", e, sizeof e) > 0 ? atoi(e) : ASTEPS; }
        struct { int step; float sigma; } pc = { k < nsteps ? 1 << k : 0, fresh ? 4.0f : -4.0f };
        pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_apl, 0, 1, &g_aset[g_par][k], 0, NULL);
        pPush(cb, g_apl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        pDispatch(cb, (g_fa.w + 7) / 8, (g_fa.h + 7) / 8, 1);
    }
    if (g_lsplit_on) {   /* item 2: the lamps' chain (depth and normal weights only), lhist[par] -> ... -> lb */
        for (int k = 0; k < ASTEPS; k++) {
            cbarrier(cb);
            struct { int step; float sigma; } pc = { 1 << k, 0.0f };
            pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_apl, 0, 1, &g_lset[g_par][k], 0, NULL);
            pPush(cb, g_apl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            pDispatch(cb, (g_fa.w + 7) / 8, (g_fa.h + 7) / 8, 1);
        }
    }
    if (g_aopipe && (g_ao_radius > 0 || g_lsplit_on)) {   /* the lamps added back, then contact AO on the filtered light, in place (analysis/46 item 4) */
        cbarrier(cb);
        struct { float r, s, l; } ap = { g_ao_radius, g_ao_strength, g_lsplit_on ? 1.0f : 0.0f };
        pBind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aopipe);
        pSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_aopl, 0, 1, &g_tset[g_cur][g_par], 1, &slot);
        pPush(cb, g_aopl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof ap, &ap);
        pDispatch(cb, (g_fa.w + 7) / 8, (g_fa.h + 7) / 8, 1);
    }
    if (g_radrb && g_dispatches + 1 == g_dump_at) {   /* the dump: the whole cache to a host buffer, written next to ptrt.ppm (rad.bin) */
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
        pBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferCopy bc = { 0, 0, (VkDeviceSize)RAD_WORDS * 4 };
        ((PFN_vkCmdCopyBuffer)photon_vk_proc("vkCmdCopyBuffer"))(cb, g_rad[g_cur], g_radrb, 1, &bc);
        g_radrb_done = 1;
    }
    g_dispatches++;
}

/* at the main view's end (shadows.c, the device thread), while the scene depth still holds the 3D view (GMod clears it for the view model
 * and the HUD before Present): the view, the images, the newest pack into a free buffer, the pass now, then the composite */
static PhotonVkPass g_pass;
void photon_shadows_ptrt_composite(void *tex, void *sunvis, void *spec, float strength, const float sun[3], const float sunc[3]);   /* shadows.c */
void photon_ptrt_main_view(void) {
    if (!photon_ptrt_on() || !photon_vk_available()) return;
    void *depth; unsigned w, h;
    float iv[4][4], vp[4][4], cam[3];
    long seq = photon_shadows_view(iv, vp, cam, &depth, &w, &h);
    if (!seq || !depth || !w) { g_pass.enabled = 0; return; }
    if (g_ready) { memcpy(g_pvp, g_vp, sizeof g_vp); memcpy(g_peye, g_eye, sizeof g_eye); g_have_prev = 1; }
    memcpy(g_iv, iv, sizeof iv); memcpy(g_vp, vp, sizeof vp); memcpy(g_eye, cam, sizeof cam);
    AcquireSRWLockExclusive(&g_l);
    g_cam[0] = cam[0]; g_cam[1] = cam[1]; g_cam[2] = cam[2]; g_have_cam = 1;
    WakeAllConditionVariable(&g_cv);
    if (g_pack_seq != g_up_seq && g_pack) {   /* a newer pack: into the buffer not in use, if its last frame is out of flight */
        int nb = g_cur < 0 ? 0 : 1 - g_cur;
        if (g_frame - g_buf_frame[nb] >= INFLIGHT) {
            memcpy(g_map[nb], g_pack, g_pack_words * 4);
            g_remap_pending = g_remap_on && g_cur >= 0 && g_rad_cur == g_cur && remap_table(g_map[g_cur], g_map[nb], g_remap_map[nb]) ? nb : -1;
            g_cur = nb; g_up_seq = g_pack_seq;
        }
    }
    ReleaseSRWLockExclusive(&g_l);
    g_ready = bind_images(depth, w, h) && g_cur >= 0;
    g_pass.enabled = g_ready;
    if (!g_ready) return;
    g_frame++;
    g_par = (int)(g_frame & 1);
    photon_vk_run(&g_pass);
    /* auto exposure from the frame INFLIGHT back (its sums are complete), then its slot cleared for reuse NUBO frames on */
    if (g_auto && g_frame > INFLIGHT) {
        int sl = (int)((g_frame - INFLIGHT) % NUBO);
        int32_t sum = g_lum[2 * sl], cnt = g_lum[2 * sl + 1];
        g_lum[2 * sl] = g_lum[2 * sl + 1] = 0;
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        double dt = g_expo_t.QuadPart ? (double)(now.QuadPart - g_expo_t.QuadPart) / (double)fq.QuadPart : 0;
        g_expo_t = now;
        if (cnt > 0) {
            float gm = exp2f((float)sum / 256.0f / (float)cnt), want = 0.3f / (gm > 1e-4f ? gm : 1e-4f);
            want = want < 1 ? 1 : want > 6 ? 6 : want;
            g_expo_gm = gm;
            float k = dt > 0 && dt < 1 ? 1.0f - expf((float)(-dt / 0.8)) : 1.0f;
            g_expo += (want - g_expo) * k;
            static long long logged;
            if (g_frame - logged >= 600) { logged = g_frame; photon_log("ptrt: auto exposure %.2f (geometric mean light %.3f, target %.2f)", g_expo, gm, want); }
        }
        g_strength = g_expo;
    }
    /* the composite only once the flat lightmaps are in the engine (before that the frame still holds the map's own light) */
    if (g_strength > 0 && photon_l4_applied_mode() == 3) {   /* the sun (direction, colour) from the pack in use: the composite's sharp sun */
        float sun[3] = { 0, 0, 1 }, sunc[3] = { 0, 0, 0 };
        if (g_cur >= 0) { memcpy(sun, (uint32_t *)g_map[g_cur] + 20, 12); memcpy(sunc, (uint32_t *)g_map[g_cur] + 24, 12); }
        photon_shadows_ptrt_composite(g_fa.tex, g_sunvis.tex, g_pbr_on && g_wpbr_tex ? g_spec[g_par].tex : NULL, g_strength, sun, sunc);
    }
}

/* the check: the filtered result read back through D3D9 at dispatch PHOTON_PTRT_DUMP (ptrt.ppm, tone-mapped) */
static void after(void *u) {
    (void)u;
    static long long want = -2;
    if (want == -2) { char e[16]; want = GetEnvironmentVariableA("PHOTON_PTRT_DUMP", e, sizeof e) > 0 ? atoll(e) : -1; }
    if (want < 0 || g_dispatches != want) return;
    if (g_radrb_done) {   /* the cache (rad.bin) and the pack it indexes (pack.bin), for tools/ptrt_cache_check.py */
        char dir[MAX_PATH], path[MAX_PATH + 16];
        DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
        if (n && n < sizeof dir) {
            FILE *f;
            snprintf(path, sizeof path, "%s\\rad.bin", dir);
            if ((f = fopen(path, "wb"))) { fwrite(g_radrb_map, 4, RAD_WORDS, f); fclose(f); }
            snprintf(path, sizeof path, "%s\\pack.bin", dir);
            if (g_cur >= 0 && (f = fopen(path, "wb"))) { fwrite(g_map[g_cur], 4, g_pack_words, f); fclose(f); }
            photon_log("ptrt: radiance cache and pack written (rad.bin, pack.bin: %zu words)", g_pack_words);
        }
    }
    IDirect3DDevice9 *d = photon_d3d_device();
    IDirect3DSurface9 *src = NULL, *sys = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_fa.tex, 0, &src))) return;
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(d, g_fa.w, g_fa.h, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(d, src, sys);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        char dir[MAX_PATH], path[MAX_PATH + 16];
        DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
        FILE *f = NULL;
        if (n && n < sizeof dir) { snprintf(path, sizeof path, "%s\\ptrt.ppm", dir); f = fopen(path, "wb"); }
        double sum = 0, hist = 0; long cnt = 0;
        if (f) fprintf(f, "P6\n%u %u\n255\n", g_fa.w, g_fa.h);
        for (unsigned y = 0; y < g_fa.h; y++)
            for (unsigned x = 0; x < g_fa.w; x++) {
                const uint16_t *h4 = (const uint16_t *)((const uint8_t *)lr.pBits + y * lr.Pitch + x * 8);
                for (int k = 0; k < 4; k++) {
                    uint16_t hv = h4[k];   /* half to float */
                    int e = (hv >> 10) & 31, m = hv & 1023;
                    float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : ldexpf(1.0f + m / 1024.0f, e - 15);
                    if (hv & 0x8000) v = -v;
                    if (k == 3) { hist += v; continue; }
                    sum += v; cnt++;
                    float t = v * 1.5f; t = t / (1 + t);   /* a simple tone map for looking at it */
                    if (f) fputc((int)(powf(t < 0 ? 0 : t, 1 / 2.2f) * 255 + 0.5f), f);
                }
            }
        if (f) fclose(f);
        photon_log("ptrt: dump at dispatch %lld: %ux%u, mean light %.4f, mean history %.1f frames; packs uploaded %ld, radiance cache cleared %ld times (bricks carried over %ld, new %ld); exposure %.2f (geometric mean %.3f)",
                   g_dispatches, g_fa.w, g_fa.h, cnt ? sum / cnt : 0, cnt ? hist / (cnt / 3.0) : 0, g_up_seq, g_rad_clears, g_rad_kept, g_rad_new, g_strength, g_expo_gm);
        IDirect3DSurface9_UnlockRect(sys);
    } else photon_log("ptrt: dump failed (0x%08lx)", (unsigned long)hr);
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(src);
}

void photon_ptrt_init(void) {
    if (!photon_ptrt_on()) return;
    g_pass = (PhotonVkPass){ "ptrt", setup, record, after, g_imgs, NIMG, NULL, 0, 1 };
    photon_vk_add_pass(&g_pass);
    photon_log("ptrt: on (PT-RT: voxel bounce light, accumulated over frames, filtered, composited)");
}
