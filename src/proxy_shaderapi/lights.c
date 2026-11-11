/* P5: see lights.h. */
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ptrt.h"
#include "texfilter.h"
#include "../ambient/vox_world.h"
#include "nanite.h"
#include "lights.h"
#include "p6.h"
#include "p6sun.h"
#include "ambvol.h"
#include "d3dconst.h"
#include "frames.h"
#include "lgwork.h"
#include "l3work.h"
#include "l4work.h"
#include "log.h"
#include "photon_lightblock.h"
#include "vlg.h"

/* Test only (mock host): 1 the engine checks are skipped, 2 the flip test is skipped */
#ifndef LIGHTS_MUTANT
#define LIGHTS_MUTANT 0
#endif

int photon_lg_on;
static int g_wanted;                       /* photon_lights at load */
static SRWLOCK g_lock = SRWLOCK_INIT;      /* the texture: the draw thread vs the release on main */
static IDirect3DTexture9 *g_tex;
static IDirect3DTexture9 *g_stage;   /* the same image in system memory: dirty rows are written here, UpdateTexture copies them to g_tex */
static int g_broken, g_ready;
static int g_cal_on;                       /* PHOTON_TEST_LIGHTCAL=1, see photon_lg_cal */
/* our texture is on sampler s since our last bind and stock has not touched it: cleared whenever stock's enable byte for s is seen at 1 (only an enabled
 * sampler is ever bound by stock, FUN_180010eb0), at a release and at the first draw of a frame (a once-a-frame GetTexture check), so a draw needs no COM call */
static uint8_t g_bound[16];
static float g_c63[4];
static unsigned long long g_last_frame = ~0ull;
static double g_qpc_us;

/* --- engine facts [confirmed on the shipped engine.dll, analysis/18; tests/ambient/check_engine_patterns.py replays these] ---------------------------- */
typedef void *(*CreateInterfaceFn)(const char *name, int *ret);
enum { EFX_SLOT_ACTIVEDLIGHTS = 6, EFX_SLOT_ELIGHTBYKEY = 8,
       RVA_NONAMBIENT_LIGHTS = 0x103b7c,   /* inside R_SetNonAmbientLightingState FUN_180103b30 */
       RVA_DESC_FROM_WORLDLIGHT = 0x107ea0, LEN_DESC_FROM_WORLDLIGHT = 777,
       RVA_WORLDLIGHT_FROM_DLIGHT = 0x1dcdd0, LEN_WORLDLIGHT_FROM_DLIGHT = 349,
       DL_OFF_FLAG = 6, DL_NEXT_FLAG = 10, DL_OFF_MASK = 18, DL_NEXT_MASK = 22, DL_OFF_ARRAY = 29, DL_NEXT_ARRAY = 33, DL_OFF_ARRAY1 = 58, DL_NEXT_ARRAY1 = 62,
       DL_OFF_MASK1 = 44, DL_NEXT_MASK1 = 48,
       EL_OFF_FLAG = 12, EL_NEXT_FLAG = 17, EL_OFF_SCAN = 24, EL_NEXT_SCAN = 28, EL_OFF_ARRAY = 74, EL_NEXT_ARRAY = 78,
       NA_OFF_STYLES = 3, NA_NEXT_STYLES = 7, NA_OFF_SCALE = 29, NA_NEXT_SCALE = 33, NA_OFF_CALL = 54, NA_NEXT_CALL = 58,
       WD_OFF_TABLE = 0x2b, WD_NEXT_TABLE = 0x2f, WD_TABLE_BIAS = 0x200, WD_OFF_FLOOR = 0xad, WD_NEXT_FLOOR = 0xb1 };
static const uint32_t k_hash_desc = 0xf1c096e0, k_hash_wfd = 0x74dd056a;   /* FNV-1a of the two conversion functions' bytes */
/* CL_GetActiveDLights: xor r8d,r8d; cmp [dlights in use],r8b; je; mov eax,[mask]; test al,1; je; lea rax,[array]; mov r8d,1; mov [rdx],rax;
 * mov eax,[mask]; test al,2; je; mov eax,r8d; lea rcx,[array + 0x48] */
static const char k_pat_dlights[] = "45 33 c0 44 38 05 ?? ?? ?? ?? 0f 84 ?? ?? ?? ?? 8b 05 ?? ?? ?? ?? a8 01 74 16 48 8d 05 ?? ?? ?? ?? 41 b8 01 00 00 00 48 89 02 "
                                    "8b 05 ?? ?? ?? ?? a8 02 74 17 41 8b c0 48 8d 0d ?? ?? ?? ??";
/* GetElightByKey: cmp byte [elights in use],0; ... lea rcx,[array + 0x28]; the key scan (stride 0x48, 0x40 entries); lea rdi,[array] */
static const char k_pat_elights[] = "48 89 5c 24 08 57 48 83 ec 20 80 3d ?? ?? ?? ?? 00 74 1c 33 c0 48 8d 0d ?? ?? ?? ?? 0f 1f 40 00 39 11 74 18 ff c0 48 83 c1 48 "
                                    "83 f8 40 72 f1 33 c0 48 8b 5c 24 30 48 83 c4 20 5f c3 48 8d 0d ?? ?? ?? ?? 48 8d 1c c0 48 8d 3d ?? ?? ?? ?? e8";
/* R_SetNonAmbientLightingState's loop: lea r12,[d_lightstylevalue]; ...; movss xmm7,[1/264]; per light: call FUN_180107ea0, style = [light+0x2c],
 * cmp style,0x7f; ja; movd xmm2,[r12+style*4]; cvtdq2ps; mulss xmm2,xmm7 */
static const char k_pat_styles[] = "4c 8d 25 ?? ?? ?? ?? 0f 29 74 24 30 f3 0f 10 35 ?? ?? ?? ?? 0f 29 7c 24 20 f3 0f 10 3d ?? ?? ?? ?? 0f 1f 00 48 63 06 49 8b 0c de "
                                   "48 6b f8 58 49 03 ff 48 8b d7 e8 ?? ?? ?? ?? 84 c0 74 45 49 8b 04 de 48 63 48 2c 83 f9 7f 77 0f 66 41 0f 6e 14 8c 0f 5b d2 "
                                   "f3 0f 59 d7";

static const volatile uint8_t *g_dl_flag, *g_el_flag;
static const volatile uint32_t *g_dl_mask;
static const volatile LgDlight *g_dl, *g_el;
static const volatile int *g_styles;
static const volatile float *g_floor;     /* [0x1804f13e0], read every frame: 1.0 in the file, 1/256 in game (P5 cal run) */
static int g_engine;                       /* 0 not yet, 1 bound, -1 refused */
static LgParams g_params;

static int hexv(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
static int pat_match(uintptr_t code, const char *pat, uintptr_t lo, uintptr_t hi) {
    size_t n = (strlen(pat) + 1) / 3;
    if (code < lo || code >= hi || hi - code < n) return 0;
    if (LIGHTS_MUTANT == 1) return 1;
    for (size_t i = 0; i < n; i++, pat += 3) {
        if (pat[0] == '?') continue;
        if (*(const uint8_t *)(code + i) != (uint8_t)(hexv(pat[0]) * 16 + hexv(pat[1]))) return 0;
    }
    return 1;
}
static uintptr_t rip(uintptr_t code, int off_rel, int off_next) {
    int32_t rel;
    memcpy(&rel, (const void *)(code + (size_t)off_rel), sizeof rel);
    return code + (size_t)off_next + (uintptr_t)(intptr_t)rel;
}
static uint32_t fnv(uintptr_t p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) h = (h ^ *(const uint8_t *)(p + i)) * 0x01000193u;
    return h;
}
static int in_img(uintptr_t p, size_t n, uintptr_t lo, uintptr_t hi) { return p >= lo && p <= hi && hi - p >= n; }

static int engine_bind(const char **why) {
    HMODULE en = GetModuleHandleA("engine.dll");
    if (!en) { *why = "no engine.dll"; return 0; }
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)en + ((const IMAGE_DOS_HEADER *)en)->e_lfanew);
    uintptr_t lo = (uintptr_t)en, hi = lo + nt->OptionalHeader.SizeOfImage;
    CreateInterfaceFn ci = (CreateInterfaceFn)(void *)GetProcAddress(en, "CreateInterface");
    void *efx = ci ? ci("VEngineEffects001", NULL) : NULL;
    if (!efx) { *why = "no VEngineEffects001"; return -1; }
    void **vt = *(void ***)efx;
    if (!in_img((uintptr_t)vt, 8 * (EFX_SLOT_ELIGHTBYKEY + 1), lo, hi)) { *why = "VEngineEffects001's vtable"; return -1; }
    uintptr_t fd = (uintptr_t)vt[EFX_SLOT_ACTIVEDLIGHTS], fe = (uintptr_t)vt[EFX_SLOT_ELIGHTBYKEY];
    if (!pat_match(fd, k_pat_dlights, lo, hi)) { *why = "VEngineEffects001 slot 6 (CL_GetActiveDLights)"; return -1; }
    if (!pat_match(fe, k_pat_elights, lo, hi)) { *why = "VEngineEffects001 slot 8 (GetElightByKey)"; return -1; }
    uintptr_t dflag = rip(fd, DL_OFF_FLAG, DL_NEXT_FLAG), dmask = rip(fd, DL_OFF_MASK, DL_NEXT_MASK), darr = rip(fd, DL_OFF_ARRAY, DL_NEXT_ARRAY);
    uintptr_t eflag = rip(fe, EL_OFF_FLAG, EL_NEXT_FLAG), escan = rip(fe, EL_OFF_SCAN, EL_NEXT_SCAN), earr = rip(fe, EL_OFF_ARRAY, EL_NEXT_ARRAY);
    if (rip(fd, DL_OFF_ARRAY1, DL_NEXT_ARRAY1) != darr + sizeof(LgDlight) || rip(fd, DL_OFF_MASK1, DL_NEXT_MASK1) != dmask || escan != earr + 0x28 ||
        eflag != dflag + 1 || !in_img(dflag, 2, lo, hi) || !in_img(dmask, 4, lo, hi) || !in_img(darr, LG_NDL * sizeof(LgDlight), lo, hi) ||
        !in_img(earr, LG_NEL * sizeof(LgDlight), lo, hi)) { *why = "the dlight and elight arrays' addresses"; return -1; }
    uintptr_t na = lo + RVA_NONAMBIENT_LIGHTS;
    if (!pat_match(na, k_pat_styles, lo, hi)) { *why = "R_SetNonAmbientLightingState's light loop"; return -1; }
    uintptr_t styles = rip(na, NA_OFF_STYLES, NA_NEXT_STYLES), scale = rip(na, NA_OFF_SCALE, NA_NEXT_SCALE);
    if (!in_img(styles, 128 * sizeof(int), lo, hi) || !in_img(scale, 4, lo, hi) || *(const float *)scale != 0.0037878789f ||
        rip(na, NA_OFF_CALL, NA_NEXT_CALL) != lo + RVA_DESC_FROM_WORLDLIGHT) { *why = "d_lightstylevalue, its scale or the conversion call"; return -1; }
    if (LIGHTS_MUTANT != 1 && (fnv(lo + RVA_DESC_FROM_WORLDLIGHT, LEN_DESC_FROM_WORLDLIGHT) != k_hash_desc ||
                               fnv(lo + RVA_WORLDLIGHT_FROM_DLIGHT, LEN_WORLDLIGHT_FROM_DLIGHT) != k_hash_wfd)) {
        *why = "the light conversions' code (FUN_180107ea0, FUN_1801dcdd0)";
        return -1;
    }
    uintptr_t wd = lo + RVA_WORLDLIGHT_FROM_DLIGHT, tab = rip(wd, WD_OFF_TABLE, WD_NEXT_TABLE) + WD_TABLE_BIAS - 128 * 4, floor_ = rip(wd, WD_OFF_FLOOR, WD_NEXT_FLOOR);
    if (!in_img(tab, 256 * 4, lo, hi) || !in_img(floor_, 4, lo, hi)) { *why = "the dlight conversion's table"; return -1; }
    lg_params_default(&g_params);
    memcpy(g_params.exptab, (const void *)tab, sizeof g_params.exptab);
    g_params.minlight_floor = *(const float *)floor_;
    g_floor = (const volatile float *)floor_;
    g_dl_flag = (const volatile uint8_t *)dflag;
    g_el_flag = (const volatile uint8_t *)eflag;
    g_dl_mask = (const volatile uint32_t *)dmask;
    g_dl = (const volatile LgDlight *)darr;
    g_el = (const volatile LgDlight *)earr;
    g_styles = (const volatile int *)styles;
    float ref[256];
    lg_exptab_default(ref);
    int same = memcmp(ref, g_params.exptab, sizeof ref) == 0;
    photon_log("lights: engine checks passed: dlights at engine+0x%llx (mask +0x%llx), elights +0x%llx, lightstyles +0x%llx; exponent table %s, minlight floor %g",
               (unsigned long long)(darr - lo), (unsigned long long)(dmask - lo), (unsigned long long)(earr - lo), (unsigned long long)(styles - lo),
               same ? "= 2^e/255" : "differs from 2^e/255 (the engine's is used)", g_params.minlight_floor);
    return 1;
}

/* P6 sun (p6sun.h): the live style scale of R_SetNonAmbientLightingState, 1 before the engine checks passed (only then is g_styles known) */
float photon_lg_style_scale(int style) {
    if (!g_styles) return 1.0f;
    return lg_style_scale((const int *)g_styles, style);
}

/* --- setup and the frame ---------------------------------------------------------------------------------------------- */
void photon_lg_setup(void) {
    int k;
    float cutoff;
    if (!photon_vlg_lights_wanted(&k, &cutoff)) return;
    g_wanted = 1;
    photon_lg_on = 1;
    char e[8];
    g_cal_on = GetEnvironmentVariableA("PHOTON_TEST_LIGHTCAL", e, sizeof e) > 0 && e[0] == '1';
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_us = 1e6 / (double)f.QuadPart;
    if (!photon_vlg_ambient_wanted())
        photon_log("lights: photon_lights 1 with photon_ambient 0: the grid needs the volume's draws, so no draw will use it");
    photon_log("lights: photon_lights 1 at load: the light grid starts once engine.dll passes its checks (k %d, cutoff %g)", k, cutoff);
}

static volatile LONG64 g_frames, g_snaps, g_uploads, g_up_fail, g_binds, g_displaced, g_flips, g_weights, g_creates, g_create_fail, g_releases;
static double g_up_us_frame, g_up_us_sum, g_up_us_max;

void photon_lg_frame(void) {
    if (!photon_lg_on) return;
    int in_game = 0;
    char level[MAX_PATH] = "";
    double cam[3] = { 0, 0, 0 };
    const AmbWorkFiles *files = NULL;
    int r = photon_eng_facts(&in_game, level, sizeof level, cam, &files);
    if (r < 0) { photon_log("lights: no engine facts: the light grid stays off"); photon_lg_on = 0; return; }
    if (r == 0) return;
    if (g_engine == 0) {
        const char *why = "?";
        int b = engine_bind(&why);
        if (b == 0) return;
        if (b < 0) { photon_log("lights: %s differs from the validated build: the light grid stays off", why); g_engine = -1; photon_lg_on = 0; return; }
        int k, ks;
        float cutoff;
        photon_vlg_lights_wanted(&k, &cutoff);
        ks = photon_vlg_lights_split();
        g_params.k = k;
        g_params.k_static = ks;
        g_params.cutoff = cutoff;
        photon_log("lights: the split keeps %d static lights per cell (photon_cluster_ks; 0 = no split), the rest folded per draw", ks);
        lg_work_want_sun(photon_p6_wanted());   /* P6: the sun's map with every map load (p6sun.c) */
        if (lg_work_init(files, &g_params) != AMB_OK) { photon_log("lights: lg_work_init failed: off"); g_engine = -1; photon_lg_on = 0; return; }
        g_engine = 1;
    }
    int k = 0;
    int want = in_game && photon_vlg_lights_wanted(&k, NULL);
    InterlockedIncrement64(&g_frames);
    {   /* L3 (analysis/37): the re-bake follows the map and photon_rebake / photon_rebake_res */
        int vox = 0;
        l3_work_frame(files, in_game ? level : "", in_game && photon_vlg_rebake(&vox) ? vox : 0);
        l4_work_frame(files, in_game ? level : "", !in_game ? 0 : photon_ptrt_on() ? 3 : photon_vlg_pathtrace() ? 1 : photon_vlg_toon() ? 2 : 0);   /* L4 (analysis/39) / TOON */
        photon_ptrt_frame(files, in_game ? level : "");   /* PT-RT (analysis/43): the map for the voxel worker */
        {   /* SP 1b: a shader pack's filtered texture copies belong to one map (their originals' references are dropped at the change) */
            static char last[260];
            const char *lv = in_game ? level : "";
            if (strcmp(lv, last)) { snprintf(last, sizeof last, "%s", lv); photon_texf_level_change(); }
        }
        photon_nanite_frame(files, in_game ? level : "");   /* NANITE (analysis/44): the map for the prop scene worker */
    }
    if (in_game && photon_ptrt_on() && g_dl && g_el) {   /* SP layer 2 (analysis/47): the engine's dynamic lights (Lua DynamicLight, muzzle flashes,
                                                          * a pack's eye glow) as PT-RT lamps, this frame */
        static float rec[(LG_NDL + LG_NEL) * VOX_LIGHT_WORDS];
        int n = 0;
        unsigned mask = *g_dl_flag ? *g_dl_mask : 0; int el_on = *g_el_flag != 0;
        for (int which = 0; which < 2; which++)
            for (int i = 0; i < (which ? LG_NEL : LG_NDL); i++) {
                LgDlight dl; memcpy(&dl, (const void *)(which ? &g_el[i] : &g_dl[i]), sizeof dl);
                if (which ? !(el_on && dl.radius > 0.0f) : !(mask >> i & 1)) continue;
                if (dl.flags & 1) continue;   /* DLIGHT_NO_WORLD_ILLUMINATION */
                LgWorldLight w;
                lg_worldlight_from_dlight(&dl, g_params.exptab, *g_floor > 0.0f ? *g_floor : g_params.minlight_floor, &w);
                float *r = rec + (size_t)n++ * VOX_LIGHT_WORDS;
                memset(r, 0, sizeof(float) * VOX_LIGHT_WORDS);
                r[0] = w.origin[0]; r[1] = w.origin[1]; r[2] = w.origin[2]; r[3] = (float)(w.type == LG_EMIT_SPOT ? 2 : 1);
                r[4] = w.intensity[0]; r[5] = w.intensity[1]; r[6] = w.intensity[2]; r[7] = w.radius;
                float c = w.constant_attn, li = w.linear_attn, q = w.quadratic_attn;
                if (c == 0 && li == 0 && q == 0) c = 1;
                r[8] = c; r[9] = li; r[10] = q; r[11] = w.exponent > 0 ? w.exponent : 1;
                r[12] = w.normal[0]; r[13] = w.normal[1]; r[14] = w.normal[2]; r[15] = w.stopdot; r[16] = w.stopdot2;
            }
        photon_ptrt_set_dyn_lamps(rec, n);
    }
    if (!want) { lg_work_frame(0, level, 0, NULL); return; }
    static LgSnapshot s;   /* Present's thread only */
    memcpy(s.cam, cam, sizeof s.cam);
    s.dl_mask = *g_dl_flag ? *g_dl_mask : 0;
    s.el_on = *g_el_flag != 0;
    memcpy(s.dl, (const void *)g_dl, sizeof s.dl);
    memcpy(s.el, (const void *)g_el, sizeof s.el);
    memcpy(s.style, (const void *)g_styles, sizeof s.style);
    s.k = k;
    s.minlight_floor = *g_floor;
    if (g_cal_on) {   /* the raw fields of the first active dlight, a few times */
        static int logged;
        for (int i = 0; i < LG_NDL && logged < 6; i++)
            if (s.dl_mask >> i & 1) {
                const LgDlight *d = &s.dl[i];
                if (photon_frame_count() % 200 == 0 || !logged)
                    photon_log("lights: cal: dlight %d key %d flags %d origin %g %g %g radius %g rgb %u %u %u exp %d minlight %g decay %g style %d die %g"
                               " outer %g", i, d->key, d->flags, d->origin[0], d->origin[1], d->origin[2], d->radius, d->r, d->g, d->b, d->exponent,
                               d->minlight, d->decay, d->style, d->die, d->outer), logged++;
                break;
            }
    }
    InterlockedIncrement64(&g_snaps);
    lg_work_frame(1, level, photon_eng_hdr(), &s);
    if (photon_frame_count() % 600 == 0) photon_lg_dump("periodic");
}

/* --- the texture, on the draw thread ------------------------------------------------------------------------------------ */
static void tex_bind(IDirect3DDevice9 *d, int s) {
    d->lpVtbl->SetTexture(d, (DWORD)s, (IDirect3DBaseTexture9 *)g_tex);
    d->lpVtbl->SetSamplerState(d, (DWORD)s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->lpVtbl->SetSamplerState(d, (DWORD)s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->lpVtbl->SetSamplerState(d, (DWORD)s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->lpVtbl->SetSamplerState(d, (DWORD)s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->lpVtbl->SetSamplerState(d, (DWORD)s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->lpVtbl->SetSamplerState(d, (DWORD)s, D3DSAMP_SRGBTEXTURE, 0);
    InterlockedIncrement64(&g_binds);
    g_bound[s & 15] = 1;
}
static int bound(IDirect3DDevice9 *d, int s) {
    IDirect3DBaseTexture9 *t = NULL;
    d->lpVtbl->GetTexture(d, (DWORD)s, &t);
    if (t) t->lpVtbl->Release(t);
    return t == (IDirect3DBaseTexture9 *)g_tex;
}

/* PHOTON_TEST_LIGHTCAL=1 (acceptance e): every engine LightDesc_t of a draw (vlg.c's SetLight hook) matched by position to a record of the uploaded
 * grid; the record must be lg_record of the engine's own desc (colour ratio 1, the same attenuation and cone), the window radius aside */
static int g_have_recs;
static float g_recs[LG_NREC][4][4];
static volatile LONG64 g_cal_seen, g_cal_matched, g_cal_fields_bad, g_cal_dyn, g_cal_in5;
static float g_cal_min[3] = { 1e30f, 1e30f, 1e30f }, g_cal_max[3] = { -1e30f, -1e30f, -1e30f };
void photon_lg_cal(const void *desc) {
    if (!g_cal_on || !g_have_recs) return;
    const LgDesc *d = desc;
    if (d->type != 1 && d->type != 3) return;
    InterlockedIncrement64(&g_cal_seen);
    for (int p = 0; p < LG_NREC; p++) {
        const float (*r)[4] = g_recs[p];
        if (fabsf(r[0][0] - d->pos[0]) > 0.01f || fabsf(r[0][1] - d->pos[1]) > 0.01f || fabsf(r[0][2] - d->pos[2]) > 0.01f) continue;
        float want[4][4];
        lg_record(d, 1.0f, r[0][3] > 0 ? 1.0f / sqrtf(r[0][3]) : 0.0f, want);
        for (int k = 0; k < 3; k++) {
            if (!(fabsf(d->color[k]) > 1e-9f)) continue;
            float q = r[1][k] / d->color[k];
            if (q < g_cal_min[k]) g_cal_min[k] = q;
            if (q > g_cal_max[k]) g_cal_max[k] = q;
            if (k == 1 && fabsf(q - 1.0f) <= 0.05f) InterlockedIncrement64(&g_cal_in5);
        }
        int bad = 0;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++)
                if (!(a == 1 && b < 3) && !(a == 0 && b == 3) && fabsf(want[a][b] - r[a][b]) > 1e-5f * (1.0f + fabsf(want[a][b]))) bad = 1;
        if (bad && InterlockedIncrement64(&g_cal_fields_bad) <= 8)
            photon_log("lights: cal: record %d differs from the engine's desc beyond its colour: pos %g %g %g att (record) %g %g %g / (engine) %g %g %g;"
                       " colour %g %g %g / %g %g %g; engine range %g falloff %g type %d", p, d->pos[0], d->pos[1], d->pos[2], r[3][0], r[3][1], r[3][2],
                       want[3][0], want[3][1], want[3][2], r[1][0], r[1][1], r[1][2], d->color[0], d->color[1], d->color[2], d->range, d->falloff, d->type);
        if (p >= LG_NSTATIC_REC) InterlockedIncrement64(&g_cal_dyn);
        InterlockedIncrement64(&g_cal_matched);
        return;
    }
}

/* The dirty rows go to a system-memory copy of the texture, in runs, and UpdateTexture copies just those rows (the lock's dirty rects)
 * into the GPU texture as a queued copy. Locking the GPU texture itself (flags 0, DEFAULT pool) made the driver wait until the GPU was
 * done with it, which every queued frame still reads: on the laptop (GTX 1050, native D3D9) a rifle's muzzle-flash lights dirtied rows
 * each shot and single uploads took 190-370 ms (Photon CPU's C1b ARC9 run). */
static int copy_rows(void *ctx, const float *image, const uint8_t rows[LG_TEX_H]) {
    IDirect3DTexture9 *t = ctx;
    if (g_cal_on) {
        for (int r = LG_REC_ROW; r < LG_TEX_H; r++)
            if (rows[r]) memcpy(&g_recs[(r - LG_REC_ROW) * 32][0][0], image + (size_t)r * LG_TEX_W * 4, LG_TEX_W * 4 * sizeof(float));
        g_have_recs = 1;
    }
    int any = 0;
    for (int r = 0; r < LG_TEX_H;) {
        if (!rows[r]) { r++; continue; }
        int e = r;
        while (e < LG_TEX_H && rows[e]) e++;
        RECT rc = { 0, r, LG_TEX_W, e };
        D3DLOCKED_RECT lr;
        if (FAILED(g_stage->lpVtbl->LockRect(g_stage, 0, &lr, &rc, 0))) return 0;   /* also marks rc dirty for UpdateTexture */
        for (int y = r; y < e; y++)
            memcpy((uint8_t *)lr.pBits + (size_t)(y - r) * (size_t)lr.Pitch, image + (size_t)y * LG_TEX_W * 4, LG_TEX_W * 4 * sizeof(float));
        g_stage->lpVtbl->UnlockRect(g_stage, 0);
        any = 1;
        r = e;
    }
    IDirect3DDevice9 *d = photon_d3d_device();
    if (any && (!d || FAILED(d->lpVtbl->UpdateTexture(d, (IDirect3DBaseTexture9 *)g_stage, (IDirect3DBaseTexture9 *)t)))) return 0;
    return 1;
}

void photon_lg_first_draw(void) {
    unsigned long long f = photon_frame_count();
    if (f == g_last_frame || g_engine != 1) return;
    g_last_frame = f;
    AcquireSRWLockExclusive(&g_lock);
    IDirect3DDevice9 *d = photon_d3d_device();
    if (d && !g_tex && !g_broken) {
        HRESULT hr = d->lpVtbl->CreateTexture(d, LG_TEX_W, LG_TEX_H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g_tex, NULL);
        if (SUCCEEDED(hr) && g_tex && !g_stage) hr = d->lpVtbl->CreateTexture(d, LG_TEX_W, LG_TEX_H, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &g_stage, NULL);
        if (FAILED(hr) || !g_tex || !g_stage) {
            if (g_tex) { g_tex->lpVtbl->Release(g_tex); g_tex = NULL; }
            if (g_stage) { g_stage->lpVtbl->Release(g_stage); g_stage = NULL; }
            g_broken = 1;
            InterlockedIncrement64(&g_create_fail);
            photon_log("lights: CreateTexture %dx%d A32B32G32R32F -> hr 0x%lx: the light grid stays off", LG_TEX_W, LG_TEX_H, (unsigned long)hr);
        } else {
            InterlockedIncrement64(&g_creates);
            lg_work_texture_lost();
            photon_log("lights: grid texture created on thread %lu (#%lld)", GetCurrentThreadId(), (long long)g_creates);
        }
    }
    g_ready = 0;
    if (d && g_tex) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        long long before = g_uploads;
        LgWorkStats st;
        lg_work_stats(&st);
        g_ready = lg_work_upload(copy_rows, g_tex, g_c63);
        QueryPerformanceCounter(&b);
        LgWorkStats st2;
        lg_work_stats(&st2);
        if (st2.uploads != st.uploads) {
            double us = (double)(b.QuadPart - a.QuadPart) * g_qpc_us;
            InterlockedIncrement64(&g_uploads);
            g_up_us_frame = us;
            g_up_us_sum += us;
            if (us > g_up_us_max) g_up_us_max = us;
        } else g_up_us_frame = 0;
        if (st2.upload_failures != st.upload_failures) InterlockedIncrement64(&g_up_fail);
        (void)before;
        if (g_ready) for (int s = PHOTON_LB_GRID_SAMPLER; s <= PHOTON_LB_GRID_SAMPLER_BUMP; s++) { g_bound[s] = (uint8_t)bound(d, s); if (!g_bound[s]) tex_bind(d, s); }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* stock's cached enable byte of sampler s, as ambvol.c reads s15's (FUN_180010eb0: api + 0x161c + 0x28 s) */
enum { OFF_API_S15_ENABLE = 0x1874, SAMPLER_STRIDE = 0x28 };
float photon_lg_weight(const void *api_real, int bump, float f) {
    if (!(f > 0.0f)) return 0.0f;
    int s = bump ? PHOTON_LB_GRID_SAMPLER_BUMP : PHOTON_LB_GRID_SAMPLER;
    AcquireSRWLockExclusive(&g_lock);
    float g = 0.0f;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (g_ready && g_tex && d) {
        if (LIGHTS_MUTANT != 2 && *((const volatile char *)api_real + OFF_API_S15_ENABLE - (15 - s) * SAMPLER_STRIDE)) {
            InterlockedIncrement64(&g_flips);
            g_bound[s] = 0;   /* stock has this sampler: it will set NULL (or its texture) inside this RenderPass */
        } else {
            if (!g_bound[s]) { if (!bound(d, s)) InterlockedIncrement64(&g_displaced); tex_bind(d, s); }
            g = f;
            InterlockedIncrement64(&g_weights);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g;
}

void photon_lg_constants(float c63[4]) { memcpy(c63, g_c63, sizeof g_c63); }
void photon_lg_fold(const float origin[3], float out[6][3]) {
    if (g_engine != 1 || !lg_work_fold(origin, out)) memset(out, 0, 18 * sizeof(float));
}

int photon_lg_k(void) {
    int k = 0;
    photon_vlg_lights_wanted(&k, NULL);
    return k;
}

void photon_lg_release(void) {
    if (!g_wanted) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_tex) {
        IDirect3DDevice9 *d = photon_d3d_device();
        for (int s = PHOTON_LB_GRID_SAMPLER; d && s <= PHOTON_LB_GRID_SAMPLER_BUMP; s++) if (bound(d, s)) d->lpVtbl->SetTexture(d, (DWORD)s, NULL);
        g_tex->lpVtbl->Release(g_tex);
        memset(g_bound, 0, sizeof g_bound);
        g_tex = NULL;
        if (g_stage) { g_stage->lpVtbl->Release(g_stage); g_stage = NULL; }
        InterlockedIncrement64(&g_releases);
    }
    g_ready = 0;
    ReleaseSRWLockExclusive(&g_lock);
    photon_log("lights: grid texture released on thread %lu", GetCurrentThreadId());
}

void photon_lg_restore(void) { /* the next first draw recreates it and uploads every row */ }

void photon_lg_shutdown(const char *why) {
    if (!g_wanted) return;
    photon_lg_dump(why);
    if (g_engine == 1) lg_work_shutdown(why);
    photon_lg_release();
}

/* --- reporting ------------------------------------------------------------------------------------------------------------------- */
void photon_lg_csv(FILE *f, int header) {
    static LONG64 prev_flips, prev_weights, prev_disp;
    if (header) {
        fprintf(f, ",lg_build_ms,lg_dyn,lg_pending,lg_overflow_cells,lg_overflow_lights,lg_static_records,lg_dirty_rows,lg_upload_us,lg_weights,lg_flips,"
                   "lg_displaced,lg_builds,lg_skipped,lg_fold_cells,lg_fold_entries,lg_fold_lost,lg_fold_evals,lg_fold_hits");
        return;
    }
    LgWorkStats s;
    memset(&s, 0, sizeof s);
    if (g_engine == 1) lg_work_stats(&s);
    static long prev_builds, prev_skipped;
    LONG64 fl = g_flips, w = g_weights, di = g_displaced;
    static long prev_fe, prev_fh;
    fprintf(f, ",%.4f,%d,%d,%d,%d,%d,%d,%.1f,%lld,%lld,%lld,%ld,%ld,%d,%d,%d,%ld,%ld", s.last.ms, s.last.dyn_lights, s.last.pending, s.last.overflow_cells,
            s.last.overflow_lights, s.last.records_static, s.last.dirty_rows, g_up_us_frame, (long long)(w - prev_weights), (long long)(fl - prev_flips),
            (long long)(di - prev_disp), s.builds - prev_builds, s.snapshots_skipped - prev_skipped, s.last.fold_cells, s.last.fold_entries,
            s.last.fold_lost, s.fold_evals - prev_fe, s.fold_hits - prev_fh);
    prev_flips = fl; prev_weights = w; prev_disp = di; prev_builds = s.builds; prev_skipped = s.snapshots_skipped;
    prev_fe = s.fold_evals; prev_fh = s.fold_hits;
}

void photon_lg_dump(const char *why) {
    if (!g_wanted) return;
    photon_log("lights (%s): %s; frames %lld, snapshots %lld; texture creates %lld (failed %lld), releases %lld; uploads %lld (failed %lld, mean %.0f us, max %.0f us);"
               " binds %lld (displaced %lld); draws with the grid %lld, flips %lld", why ? why : "?",
               photon_lg_on ? (g_engine == 1 ? "on" : "waiting for engine.dll") : "off", (long long)g_frames, (long long)g_snaps, (long long)g_creates,
               (long long)g_create_fail, (long long)g_releases, (long long)g_uploads, (long long)g_up_fail, g_uploads ? g_up_us_sum / (double)g_uploads : 0.0,
               g_up_us_max, (long long)g_binds, (long long)g_displaced, (long long)g_weights, (long long)g_flips);
    if (g_cal_on)
        photon_log("lights cal (%s): engine point/spot lights seen %lld, matched to a grid record %lld (dynamic %lld); colour ratio record / engine r %.6f..%.6f,"
                   " g %.6f..%.6f, b %.6f..%.6f, green within 5%% in %lld; records differing beyond colour and window %lld", why ? why : "?",
                   (long long)g_cal_seen, (long long)g_cal_matched, (long long)g_cal_dyn, g_cal_min[0], g_cal_max[0], g_cal_min[1], g_cal_max[1], g_cal_min[2],
                   g_cal_max[2], (long long)g_cal_in5, (long long)g_cal_fields_bad);
    if (g_engine == 1) lg_work_summary(why);
}
