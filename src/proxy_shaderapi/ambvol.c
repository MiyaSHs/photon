/* P3d, P3g, P3h: see ambvol.h. Facts this relies on are in analysis/15d sections 2 and 8; what is still open is exactly what the
 * counters here measure: does anything clobber s15, which thread downloads textures, is the draw thread parked during a release. */
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ambvol.h"
#include "ambwork.h"
#include "d3dconst.h"
#include "frames.h"
#include "log.h"
#include "vlg.h"
#include "../ambient/amb_bake.h"
#include "../ambient/amb_track.h"
#include "../ambient/amb_gma.h"

/* -Wl: the IID is defined here so no dxguid import lib is needed */
static const GUID k_iid_ex = { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };

/* Test-only (scripts/run_amb_glue_mutants.sh): each removes one rule of this layer, and the mock host's PHOTON_TEST_AMB runs must then fail.
 * 1 a map inside a GMA is read through the filesystem's shared handle, 2 no filesystem stamp check, 3 no engine pattern check, 4 a new texture is not filled, 5 the
 * box's U offset is ignored, 6 the release keeps the texture, 7 mat_fullbright is ignored, 8 r_shader_srgb is ignored, 9 the s15 cache is not checked, 10 no fold,
 * 11 no check of a GMA map's offset against the GMA's index, 12 a GMA map's offset taken from its length field, 13 A is not
 * low-passed per model. */
#ifndef AMBVOL_MUTANT
#define AMBVOL_MUTANT 0
#endif

enum { VOL_SAMPLER = 15, VOX_X = 64, VOX_Y = 64, VOX_Z = 32, SLAB = 8,
       VOL_W = 6 * AMB_BAND_TEXELS(VOX_X), ROW_BYTES = VOL_W * 8, SLICE_BYTES = ROW_BYTES * VOX_Y, ATLAS_BYTES = SLICE_BYTES * VOX_Z };
enum { SLOT_SH_ENABLETEXTURE = 31, SLOT_API_MODIFYTEXTURE = 148, SLOT_API_BINDTEXTURE = 155 };
enum { OFF_API_S15_ENABLE = 0x1874,    /* sub-object: enable byte of stock's sampler 15 cache entry (FUN_18001fd30) */
       OFF_API_S15_HANDLE = 0x1850,    /* its cached texture handle */
       OFF_API_IDENTITY = 0x158 };     /* the identity table's handle, what stock keeps on s15 with sRGB writes off */

int photon_vol_on;
int photon_amb_on;
static int g_amb_wanted;                /* photon_ambient was 1 at load: the release hook and shutdown matter even if the volume went off later */
static IDirect3DVolumeTexture9 *g_vol;
static SRWLOCK g_lock = SRWLOCK_INIT;   /* the volume pointer: draw thread vs the release on main; also the draw thread's amb_work calls vs shutdown */
static uint8_t *g_atlas[2];             /* the two patterns, packed by the real amb_write_toroidal */
static volatile LONG g_broken;          /* creation failed: the probe stays off */
static unsigned long long g_last_frame = ~0ull;
static int g_slab_next;
static double g_qpc_us;

/* counters */
static volatile LONG64 g_creates, g_releases, g_restores, g_create_fail, g_lock_fail, g_uploads, g_binds;
enum { NSTATE = 7 };
static const D3DSAMPLERSTATETYPE k_state[NSTATE] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_MAGFILTER,
                                                     D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
static const DWORD k_value[NSTATE] = { D3DTADDRESS_CLAMP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, 0 };
static volatile LONG64 g_clobber_draw, g_clobber_inside, g_not_at_rest, g_stock_samples_s15, g_enable_seen, g_release_contended, g_predraws;
static volatile LONG64 g_shadow_s15, g_bind_s15;
enum { NTID = 6, NUP = 8192 };
typedef struct { DWORD tid; LONG64 n; } TidCount;
static TidCount g_modify_tids[NTID], g_bind_tids[NTID];
static DWORD g_release_tid, g_create_tid;
static float g_up_us[NUP], g_flip_us[NUP];
static LONG g_nup, g_nflip;
static double g_fill_us;
static int g_trace_on;                  /* PHOTON_TEST_AMBTRACE=1: photon_ambtrace.csv, see photon_amb_trace */
static void *g_sh_prev, *g_modify_prev, *g_bind_prev;
/* Which samplers does stock really use? Per sampler: EnableTexture(s, true) calls, frames with stock's enable byte set,
 * draws whose bound pixel shader declares it, and how many distinct shaders those were. */
static volatile LONG64 g_en_calls[16], g_en_frames[16], g_ps_draws[16];
static long g_ps_shaders[16];
enum { PS_TABLE = 8192 };
static struct { void *ps; unsigned mask; } g_ps_tab[PS_TABLE];
static long g_ps_table_full;
static TidCount g_pre_tids[NTID];
static char g_en_now[16];

static void tid_note(TidCount *t) {
    DWORD id = GetCurrentThreadId();
    for (int i = 0; i < NTID; i++) {
        if (t[i].tid == id) { InterlockedIncrement64(&t[i].n); return; }
        if (!t[i].tid) { t[i].tid = id; InterlockedIncrement64(&t[i].n); return; }
    }
}

static void pattern(float *vol, int which) {
    /* r = x/63 along a band, g = y/63 (pattern 0) or 1 - y/63 (pattern 1), b = (face/5 + z/31) / 2 */
    for (int z = 0; z < VOX_Z; z++) for (int y = 0; y < VOX_Y; y++) for (int x = 0; x < VOX_X; x++)
        for (int f = 0; f < AMB_FACES; f++) {
            float *o = vol + (((size_t)z * VOX_Y + (size_t)y) * VOX_X + (size_t)x) * AMB_VOXEL_FLOATS + f * 3;
            o[0] = (float)x / (VOX_X - 1);
            o[1] = which ? 1.0f - (float)y / (VOX_Y - 1) : (float)y / (VOX_Y - 1);
            o[2] = 0.5f * ((float)f / 5.0f + (float)z / (VOX_Z - 1));
        }
}

void photon_vol_setup(void) {
    char e[8];
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_us = 1e6 / (double)f.QuadPart;
    if (GetEnvironmentVariableA("PHOTON_TEST_VOL", e, sizeof e) <= 0 || e[0] != '1') {
        /* the real volume. Everything else waits for engine.dll, at the first frames (amb_frame). */
        if (photon_vlg_ambient_wanted()) {
            photon_amb_on = g_amb_wanted = 1;
            g_trace_on = GetEnvironmentVariableA("PHOTON_TEST_AMBTRACE", e, sizeof e) > 0 && e[0] == '1';
            photon_log("ambvol: photon_ambient 1: volume %dx%dx%d on s%d, when the engine checks pass", VOL_W, VOX_Y, VOX_Z, VOL_SAMPLER);
        }
        return;
    }
    if (photon_vlg_ambient_wanted()) photon_log("ambvol: PHOTON_TEST_VOL=1 takes s15: photon_ambient ignored");
    float *v = malloc((size_t)VOX_X * VOX_Y * VOX_Z * AMB_VOXEL_FLOATS * sizeof(float));
    for (int i = 0; i < 2 && v; i++) {
        g_atlas[i] = calloc(1, ATLAS_BYTES);
        if (!g_atlas[i]) { free(v); v = NULL; break; }
        pattern(v, i);
        AmbBox box = { { 0, 0, 0 }, { VOX_X, VOX_Y, VOX_Z } };
        int clip[3] = { VOX_X, VOX_Y, VOX_Z };
        amb_write_toroidal(v, &box, clip, g_atlas[i], ROW_BYTES, SLICE_BYTES);
    }
    if (!v) { photon_log("ambvol: TEST PHOTON_TEST_VOL=1: out of memory, probe off"); return; }
    free(v);
    photon_vol_on = 1;
    photon_log("ambvol: TEST PHOTON_TEST_VOL=1: raw volume %dx%dx%d on s%d", VOL_W, VOX_Y, VOX_Z, VOL_SAMPLER);
}

/* --- counters on what could touch s15 or race with a texture upload ------------------------------------------- */
static void hk_enable(void *self, uintptr_t sampler, uintptr_t on, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SLOT_SH_ENABLETEXTURE, -1)) { ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t))w->cls->real_vtbl[SLOT_SH_ENABLETEXTURE])(w->real, sampler, on, c); return; }
    if ((int)sampler == VOL_SAMPLER && (on & 0xff)) InterlockedIncrement64(&g_shadow_s15);
    if ((on & 0xff) && (int)sampler >= 0 && (int)sampler < 16) InterlockedIncrement64(&g_en_calls[(int)sampler]);
    ((void (*)(PhotonWrap *, uintptr_t, uintptr_t, uintptr_t))g_sh_prev)(w, sampler, on, c);
}
static void hk_modify(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    tid_note(g_modify_tids);
    ((void (*)(PhotonWrap *, uintptr_t, uintptr_t, uintptr_t))g_modify_prev)(w, a, b, c);
}
static void hk_bind(void *self, uintptr_t sampler, uintptr_t h, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SLOT_API_BINDTEXTURE, 54)) { ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t))w->cls->real_vtbl[SLOT_API_BINDTEXTURE])(w->real, sampler, h, c); return; }
    if ((int)sampler == VOL_SAMPLER) { InterlockedIncrement64(&g_bind_s15); tid_note(g_bind_tids); }
    ((void (*)(PhotonWrap *, uintptr_t, uintptr_t, uintptr_t))g_bind_prev)(w, sampler, h, c);
}

void photon_vol_install(PhotonWrapClass *cls) {
    if (!photon_vol_on) return;
    if (strcmp(cls->name, "ShaderShadow010") == 0) {
        g_sh_prev = cls->thunk_vtbl[SLOT_SH_ENABLETEXTURE];
        photon_wrap_set_hook(cls, SLOT_SH_ENABLETEXTURE, (void *)hk_enable);
    } else if (strcmp(cls->name, "ShaderApi030") == 0) {
        g_modify_prev = cls->thunk_vtbl[SLOT_API_MODIFYTEXTURE];
        g_bind_prev = cls->thunk_vtbl[SLOT_API_BINDTEXTURE];
        photon_wrap_set_hook(cls, SLOT_API_MODIFYTEXTURE, (void *)hk_modify);
        photon_wrap_set_hook(cls, SLOT_API_BINDTEXTURE, (void *)hk_bind);
    }
}

/* --- the volume ------------------------------------------------------------------------------------------------ */
static void log_caps(IDirect3DDevice9 *d) {
    IUnknown *ex = NULL;
    HRESULT qi = d->lpVtbl->QueryInterface(d, &k_iid_ex, (void **)&ex);
    if (SUCCEEDED(qi) && ex) ex->lpVtbl->Release(ex);
    D3DCAPS9 c;
    memset(&c, 0, sizeof c);
    d->lpVtbl->GetDeviceCaps(d, &c);
    photon_log("ambvol: device %s; caps VolumeMap %d, MaxVolumeExtent %lu, MaxSimultaneousTextures %lu, VolumeFilterCaps 0x%lx,"
               " VolumeAddressCaps 0x%lx", SUCCEEDED(qi) ? "D3D9Ex" : "not Ex", (c.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP) != 0,
               c.MaxVolumeExtent, c.MaxSimultaneousTextures, c.VolumeTextureFilterCaps, c.VolumeTextureAddressCaps);
    IDirect3D9 *d3d = NULL;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (SUCCEEDED(d->lpVtbl->GetDirect3D(d, &d3d)) && d3d && SUCCEEDED(d->lpVtbl->GetCreationParameters(d, &cp))) {
        HRESULT f = d3d->lpVtbl->CheckDeviceFormat(d3d, cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_FILTER,
                                                   D3DRTYPE_VOLUMETEXTURE, D3DFMT_A16B16G16R16F);
        HRESULT w = d3d->lpVtbl->CheckDeviceFormat(d3d, cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_WRAPANDMIP,
                                                   D3DRTYPE_VOLUMETEXTURE, D3DFMT_A16B16G16R16F);
        photon_log("ambvol: CheckDeviceFormat volume A16B16G16R16F: QUERY_FILTER hr 0x%lx, QUERY_WRAPANDMIP hr 0x%lx, behavior 0x%lx",
                   (unsigned long)f, (unsigned long)w, cp.BehaviorFlags);
    }
    if (d3d) d3d->lpVtbl->Release(d3d);
}

static void write_rows(IDirect3DVolumeTexture9 *v, const uint8_t *atlas, int y0, int y1, int full) {
    D3DBOX box = { 0, (UINT)y0, VOL_W, (UINT)y1, 0, VOX_Z };
    D3DLOCKED_BOX lb;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = v->lpVtbl->LockBox(v, 0, &lb, &box, 0);
    if (FAILED(hr)) {
        if (InterlockedIncrement64(&g_lock_fail) <= 4) photon_log("ambvol: LockBox rows %d..%d -> hr 0x%lx", y0, y1, (unsigned long)hr);
        return;
    }
    for (int z = 0; z < VOX_Z; z++)
        for (int y = y0; y < y1; y++)
            memcpy((uint8_t *)lb.pBits + (size_t)z * (size_t)lb.SlicePitch + (size_t)(y - y0) * (size_t)lb.RowPitch,
                   atlas + (size_t)z * SLICE_BYTES + (size_t)y * ROW_BYTES, ROW_BYTES);
    v->lpVtbl->UnlockBox(v, 0);
    QueryPerformanceCounter(&b);
    double us = (double)(b.QuadPart - a.QuadPart) * g_qpc_us;
    if (full) g_fill_us = us;
    else {
        InterlockedIncrement64(&g_uploads);
        if (g_nup < NUP) g_up_us[g_nup++] = (float)us;
    }
}


/* SetTexture and the states stock's sampler cache would not touch again: FUN_18001fd30 writes a state only when it differs from
 * its cache, so what we set stays until a reset, except where stock's cache and ours disagree on a value it then re-applies. */
static void vol_bind(IDirect3DDevice9 *d) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    d->lpVtbl->SetTexture(d, VOL_SAMPLER, (IDirect3DBaseTexture9 *)g_vol);
    for (int i = 0; i < NSTATE; i++) d->lpVtbl->SetSamplerState(d, VOL_SAMPLER, k_state[i], k_value[i]);
    InterlockedIncrement64(&g_binds);
    QueryPerformanceCounter(&b);
    if (g_nflip < NUP) g_flip_us[g_nflip++] = (float)((double)(b.QuadPart - a.QuadPart) * g_qpc_us);
}

static int bound_is_ours(IDirect3DDevice9 *d, const char *where) {
    IDirect3DBaseTexture9 *t = NULL;
    d->lpVtbl->GetTexture(d, VOL_SAMPLER, &t);
    if (t) t->lpVtbl->Release(t);
    int ours = t == (IDirect3DBaseTexture9 *)g_vol;
    static volatile LONG logs;
    if (!ours && InterlockedIncrement(&logs) <= 24)
        photon_log("ambvol: s15 not ours at %s, frame %llu: holds %s %p", where, (unsigned long long)photon_frame_count(),
                   t ? "another texture" : "NULL", (void *)t);
    return ours;
}

static int vol_create(IDirect3DDevice9 *d) {
    static int caps_done;
    if (!caps_done) { caps_done = 1; log_caps(d); }
    HRESULT hr = d->lpVtbl->CreateVolumeTexture(d, VOL_W, VOX_Y, VOX_Z, 1, D3DUSAGE_DYNAMIC, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_vol, NULL);
    if (FAILED(hr) || !g_vol) {
        InterlockedIncrement64(&g_create_fail);
        g_broken = 1;
        g_vol = NULL;
        photon_log("ambvol: CreateVolumeTexture -> hr 0x%lx: volume off", (unsigned long)hr);
        return 0;
    }
    InterlockedIncrement64(&g_creates);
    g_create_tid = GetCurrentThreadId();
    return 1;
}

/* Stock believes s15 holds its identity table and binds only when that belief changes, inside RenderPass, after this hook */
static int s15_at_rest(const void *api_real) {
    const char *rp = (const char *)api_real;
    return *(const volatile char *)(rp + OFF_API_S15_ENABLE) &&
           *(const volatile uint64_t *)(rp + OFF_API_S15_HANDLE) == *(const volatile uint64_t *)(rp + OFF_API_IDENTITY);
}

static int predraw_amb(const void *api_real);

int photon_vol_predraw(const void *api_real, int wants) {
    if (photon_amb_on) return predraw_amb(api_real);
    if (!photon_vol_on || g_broken) return 0;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return 0;
    int usable = 0;
    AcquireSRWLockExclusive(&g_lock);
    InterlockedIncrement64(&g_predraws);
    tid_note(g_pre_tids);
    for (int s = 0; s < 16; s++) g_en_now[s] |= *((const volatile char *)api_real + OFF_API_S15_ENABLE - (VOL_SAMPLER - s) * 0x28);
    if (!g_vol) {
        if (!vol_create(d)) {
            ReleaseSRWLockExclusive(&g_lock);
            return 0;
        }
        write_rows(g_vol, g_atlas[0], 0, VOX_Y, 1);
        g_slab_next = 0;
        photon_log("ambvol: volume created on thread %lu (create #%lld), filled in %.0f us", g_create_tid, (long long)g_creates, g_fill_us);
    }
    /* one slab per frame: the real scheduling. The states are re-asserted once a frame as a backstop: stock's sampler-wide loops. */
    unsigned long long f = photon_frame_count();
    int new_frame = f != g_last_frame;
    if (new_frame) {
        g_last_frame = f;
        write_rows(g_vol, g_atlas[1], g_slab_next * SLAB, g_slab_next * SLAB + SLAB, 0);
        g_slab_next = (g_slab_next + 1) % (VOX_Y / SLAB);
    }
    if (wants) {
        if (!s15_at_rest(api_real)) InterlockedIncrement64(&g_not_at_rest);
        else {
            int ours = bound_is_ours(d, "draw entry");
            if (!ours) InterlockedIncrement64(&g_clobber_draw);
            if (!ours || new_frame) vol_bind(d);
            usable = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return usable;
}

/* --- P3g, P3h: the real volume ------------------------------------------------------------------------------------- */
/* Engine access [confirmed on the shipped engine.dll, analysis/15d sections 8.4, 8.5]. The T1.5 guard does not cover engine.dll, so every call and read below is
 * checked against the code it depends on, once, and a mismatch leaves the volume off. */
typedef void *(*AmbCreateInterfaceFn)(const char *name, int *ret);
enum { ENG_SLOT_INGAME = 26, ENG_SLOT_LEVELNAME = 51, RV_SLOT_SETMAINVIEW = 41, RENDERER_SLOT_SETMAINVIEW = 27 };
static const char k_pat_ingame[] = "83 3d ?? ?? ?? ?? 06 0f 94 c0 c3";   /* cmp [signon], 6; sete al; ret */
static const char k_pat_levelname[] = "80 3d ?? ?? ?? ?? 00 74 08 48 8d 05 ?? ?? ?? ?? c3 83 3d ?? ?? ?? ?? 02 48 8d 05 ?? ?? ?? ?? 7c 07 48 8d 05 ?? ?? ?? ?? c3";
/* IVRenderView::SetMainView: mov rcx,[g_EngineRenderer]; mov rax,[rcx]; jmp [rax+0xd8] */
static const char k_pat_setmainview[] = "48 8b 0d ?? ?? ?? ?? 48 8b 01 48 ff a0 d8 00 00 00";
/* The renderer's side: the only writer of the main view origin, whose three stores give the global */
static const char k_pat_renderer[] = "f3 0f 10 02 4c 8d 0d ?? ?? ?? ?? f3 0f 11 05 ?? ?? ?? ?? 49 8b c8 f3 0f 10 4a 04 4c 8d 05 ?? ?? ?? ?? f3 0f 11 0d ?? ?? ?? ?? "
                                     "f3 0f 10 42 08 48 8d 15 ?? ?? ?? ?? f3 0f 11 05 ?? ?? ?? ??";
enum { REN_OFF_X = 0x0f, REN_NEXT_X = 0x13, REN_OFF_Y = 0x26, REN_NEXT_Y = 0x2a, REN_OFF_Z = 0x3a, REN_NEXT_Z = 0x3e };
/* CMapLoadHelper::Init (FUN_180112730): how the engine itself opens and reads a map: OpenEx at +0x248 with (name, "rb", 0, NULL, NULL), and Read at slot 0 of the
 * IBaseFileSystem part at +8 with (buf, n, handle). Only the validated build has these bytes at this address. */
enum { RVA_MAPLOAD_OPEN = 0x112805 };
static const char k_pat_mapload[] = "48 8b 0d ?? ?? ?? ?? 4c 8d 05 ?? ?? ?? ?? 48 89 5c 24 28 48 8d 54 24 50 45 33 c9 48 89 5c 24 20 48 8b 01 ff 90 48 02 00 00 "
                                    "48 89 05 ?? ?? ?? ?? 4c 8b c8 48 85 c0 75 11 48 8d 54 24 50 48 8d 0d ?? ?? ?? ?? e9 ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? "
                                    "48 8d 15 ?? ?? ?? ?? 48 83 c1 08 41 b8 0c 04 00 00 48 8b 01 ff 10";
/* IBaseFileSystem per the public SDK filesystem.h, with MSVC's reversed overloads: slot 6 is Size(name, path), slot 7 Size(handle). filesystem_stdio's own code:
 * Size(handle) (0x180023890 -> 0x180023a20) gives the exact size for VPK files too (handle+0x20), where Seek(0, TAIL) + Tell gives size - 1. Seek (slot 4,
 * 0x180022bb0) takes (handle, 64-bit position, mode), mode 0 from the start; the GMA reader calls it so (0x18003ef21..0x18003ef32).
 * MAPS INSIDE A GMA [objdump of the shipped filesystem_stdio.dll; internal layout, so the DLL's stamp is pinned]:
 *   CFileHandle (0x80 bytes, OpenEx 0x18001b855..0x18001b98d): +0x50 the GMA file object of a file inside a GMA (tested by Read and Seek, 0x180020607, 0x180022c31),
 *   +0x68 the OS-level file object (FS_fopen's result), +0x78 the magic 0x43464861.
 *   GMA file object (constructor 0x18003ed30): +0 the file's offset in the .gma (entry +0x48), +8 its length (entry +0x40), +0x10 the position, +0x18 the CFileHandle
 *   of the .gma itself, which the mount opened once (0x18004a3e0: Open(path, "rb", "MOD")) and every file of that GMA shares (entry +0x50, 0x180044ca8). Its Read
 *   (0x18003eec0) is Seek(shared, +0 + pos) then Read, with no lock: the race this code avoids by never reading through it.
 *   Live GMA file objects are in a vector under a CRITICAL_SECTION at RVA 0xe2088 (0x18003ed58). Unmounting runs 0x18003ee60 under it, which sets +0x18 of every
 *   object of that .gma to NULL, and only then closes the shared handle (0x18004176a then 0x18004178c; 0x180047c8c then 0x180047ca2). So with that section held,
 *   a non-NULL +0x18 names a .gma handle that stays open until it is released.
 *   FS_fopen (0x180028a30) opens "rb" with filesystem_native 1 (its default) through the Win32 read-only class (0x180028c20: CreateFileA(GENERIC_READ,
 *   FILE_SHARE_READ, OPEN_EXISTING)), a 0x38-byte object with vtable RVA 0xafd08 and its buffered HANDLE at +0x20; otherwise a CRT FILE*, which is refused here.
 *   A GMA's bytes are read raw at that offset (no decompression in 0x18003eec0): Workshop's compressed legacy items are first written out as a plain
 *   cache/workshop/%llu.gma by the filesystem's own addon code (string 0x1800b1358, used at 0x1800401e3). */
enum { FS_OFF_BASE = 8, FS_SLOT_OPENEX = 0x248 / 8, BFS_SLOT_READ = 0, BFS_SLOT_CLOSE = 3, BFS_SLOT_SEEK = 4, BFS_SLOT_SIZE_HANDLE = 7, FH_OFF_PACK = 0x50,
       FH_OFF_FILE = 0x68, FH_OFF_MAGIC = 0x78, FH_MAGIC = 0x43464861, GF_OFF_BASE = 0, GF_OFF_LEN = 8, GF_OFF_SHARED = 0x18, RVA_FS_GMA_LOCK = 0xe2088,
       RVA_FS_WIN32_VTBL = 0xafd08, W32_OFF_HANDLE = 0x20 };
static const DWORD k_fs_stamp = 0x6adfbca3;   /* filesystem_stdio.dll, build 26229091 */
/* materialsystem's mat_fullbright ConVar (its stamp is in the T1.5 guard), read as vlg.c reads stock's r_pixelfog: FUN_1800092d0 constructs it */
enum { RVA_MS_CV_FULLBRIGHT = 0x20f3c50, RVA_MS_CV_VTBL = 0xc09d8, RVA_MS_CV_GETINT = 0xe8a0, RVA_MS_STR_FULLBRIGHT = 0xb6c78, CV_SLOT_GETINT = 13, CV_OFF_NAME = 0x18 };
/* IMaterialSystemHardwareConfig on the stock object: 54 GetHDRType (FUN_180004740), 62 NeedsShaderSRGBConversion (FUN_180004a90, r_shader_srgb) */
enum { HW_SLOT_HDR = 54, HW_SLOT_SRGB = 62, RVA_HW_HDR = 0x4740, RVA_HW_SRGB = 0x4a90 };

/* The engine's light averaging, read only (analysis/15e, third run; amb_track.h has the formula). FUN_180105dc0 reads r_lightaverage, then r_lightinterp, then the
 * client time through FUN_1800aaea0, in that order at its start. GMod stores a ConVar's numbers XORed with the low 32 bits of the ConVar's own address (int +0x58, float
 * bits +0x54), as these readers show. A mismatch does not stop the volume: the defaults (on, rate 5, the QPC clock) are used and it is logged once. */
enum { RVA_LA_READ = 0x105de9, RVA_LI_READ = 0x105e2c, RVA_TIME_CALL = 0x105ed9, CV_OFF_PARENT = 0x38, CV_OFF_FVALUE = 0x54, CV_OFF_NVALUE = 0x58,
       CL_OFF_TICK = 0x1c4, CL_OFF_PAUSED = 0x20b70, CL_OFF_REMAINDER = 0x20b78 };
static const char k_pat_la_read[] = "48 8b 0d ?? ?? ?? ?? 49 8b e8 0f b7 fa 48 8d 15 ?? ?? ?? ?? 48 89 74 24 38 48 3b ca 75 0a 8b 05 ?? ?? ?? ?? 33 c2 eb 06 48 8b 01 ff 50 68";
static const char k_pat_li_read[] = "48 8b 0d ?? ?? ?? ?? 48 8d 15 ?? ?? ?? ?? 48 3b ca 75 1b f3 0f 10 05 ?? ?? ?? ?? f3 0f 11 44 24 30 31 54 24 30 f3 44 0f 10 44 24 30 eb 0a 48 8b 01 ff 50 60";
static const char k_pat_time_call[] = "48 8d 0d ?? ?? ?? ?? 4c 89 ac 24 e0 00 00 00 4c 89 b4 24 d8 00 00 00 4c 89 bc 24 d0 00 00 00 0f 29 b4 24 c0 00 00 00 e8 ?? ?? ?? ?? "
                                      "f3 0f 10 8f 28 01 00 00";
/* cmp byte [rcx+20b70],0; movd xmm0,[rcx+1c4]; cvtdq2pd; mulsd xmm0,[interval]; jne +8; addsd xmm0,[rcx+20b78]; ret */
static const char k_pat_time_fn[] = "80 b9 70 0b 02 00 00 66 0f 6e 81 c4 01 00 00 f3 0f e6 c0 f2 0f 59 05 ?? ?? ?? ?? 75 08 f2 0f 58 81 78 0b 02 00 c3";
static const char *g_cv_la, *g_cv_li;         /* r_lightaverage, r_lightinterp; NULL: the defaults */
static double (*g_time_fn)(const void *cl);
static const void *g_cl;
static AmbTrack g_track;
static unsigned long long g_track_frame = ~0ull;
static int g_track_bad_cv;
static float g_last_as[AMB_VOXEL_FLOATS];     /* the A the last fold used, for the trace */
static int g_last_as_valid;

static void *g_eng_client, *g_fs, *g_cv_fullbright, *g_hw_real;
static uintptr_t g_fs_lo, g_fs_hi;     /* filesystem_stdio.dll's image */
static volatile const float *g_view_origin;
static int g_engine_ok;
static int g_gate_off;                 /* HW slot 62 or mat_fullbright: read per frame, on the draw thread */
static int g_need_full;                /* a new texture: the whole atlas has to go up first */
static unsigned long long g_bind_frame = ~0ull;
static double g_frame_up_us;

void photon_vol_note_iface(const char *name, void *real) {
    if (strcmp(name, "MaterialSystemHardwareConfig012") == 0) g_hw_real = real;
}

static int hexv(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
/* A pattern of hex bytes and ??, against code that must lie inside the image [lo, hi) */
static int pat_match(uintptr_t code, const char *pat, uintptr_t lo, uintptr_t hi) {
    size_t n = (strlen(pat) + 1) / 3;
    if (code < lo || code >= hi || hi - code < n) return 0;
    if (AMBVOL_MUTANT == 3) return 1;
    for (size_t i = 0; i < n; i++, pat += 3) {
        if (pat[0] == '?') continue;
        if (*(const uint8_t *)(code + i) != (uint8_t)(hexv(pat[0]) * 16 + hexv(pat[1]))) return 0;
    }
    return 1;
}
static uintptr_t rip_target(uintptr_t code, int off_rel, int off_next) {
    int32_t rel;
    memcpy(&rel, (const void *)(code + (size_t)off_rel), sizeof rel);
    return code + (size_t)off_next + (uintptr_t)(intptr_t)rel;
}
static int image_of(HMODULE m, uintptr_t *lo, uintptr_t *hi) {
    if (!m) return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    *lo = (uintptr_t)m;
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
    return 1;
}

/* 0: not all loaded and connected yet, try next frame. -1: something differs from the validated build; why says what. 1: all pointers set. */
static int engine_bind(const char **why) {
    uintptr_t lo, hi, slo, shi, mlo, mhi;
    HMODULE en = GetModuleHandleA("engine.dll");
    if (!en) return 0;
    image_of(en, &lo, &hi);
    AmbCreateInterfaceFn ci = (AmbCreateInterfaceFn)(void *)GetProcAddress(en, "CreateInterface");
    void *client = ci ? ci("VEngineClient015", NULL) : NULL, *rv = ci ? ci("VEngineRenderView014", NULL) : NULL;
    if (!client || !rv) { *why = "no VEngineClient015 or VEngineRenderView014 in engine.dll"; return -1; }
    void **vc = *(void ***)client, **vr = *(void ***)rv;
    if (!pat_match((uintptr_t)vc[ENG_SLOT_INGAME], k_pat_ingame, lo, hi)) { *why = "VEngineClient slot 26"; return -1; }
    if (!pat_match((uintptr_t)vc[ENG_SLOT_LEVELNAME], k_pat_levelname, lo, hi)) { *why = "VEngineClient slot 51"; return -1; }
    uintptr_t smv = (uintptr_t)vr[RV_SLOT_SETMAINVIEW];
    if (!pat_match(smv, k_pat_setmainview, lo, hi)) { *why = "VEngineRenderView slot 41"; return -1; }
    uintptr_t ren_p = rip_target(smv, 3, 7);
    if (ren_p < lo || ren_p + sizeof(void *) > hi) { *why = "g_EngineRenderer's address"; return -1; }
    void *ren = *(void **)ren_p;
    if ((uintptr_t)ren < lo || (uintptr_t)ren >= hi) { *why = "the engine renderer object"; return -1; }
    uintptr_t fn = (uintptr_t)(*(void ***)ren)[RENDERER_SLOT_SETMAINVIEW];
    if (!pat_match(fn, k_pat_renderer, lo, hi)) { *why = "the engine renderer's main view setter"; return -1; }
    uintptr_t x = rip_target(fn, REN_OFF_X, REN_NEXT_X), y = rip_target(fn, REN_OFF_Y, REN_NEXT_Y), z = rip_target(fn, REN_OFF_Z, REN_NEXT_Z);
    if (y != x + 4 || z != x + 8 || x < lo || z + 4 > hi) { *why = "the main view origin's address"; return -1; }
    uintptr_t ml = lo + RVA_MAPLOAD_OPEN;
    if (!pat_match(ml, k_pat_mapload, lo, hi)) { *why = "CMapLoadHelper::Init's filesystem calls"; return -1; }
    uintptr_t fs_global = rip_target(ml, 3, 7);
    if (fs_global < lo || fs_global + sizeof(void *) > hi) { *why = "the engine's filesystem global"; return -1; }
    void *fs = photon_ms_factory_query("VFileSystem022");
    if (!fs || !*(void **)fs_global || !g_hw_real) return 0;   /* the engine or materialsystem has not connected yet */
    if (*(void **)fs_global != fs) { *why = "VFileSystem022, not the object the engine reads maps with,"; return -1; }
    HMODULE fm = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*(void **)fs, &fm);
    if (!fm || (AMBVOL_MUTANT != 2 && ((const IMAGE_NT_HEADERS64 *)((const char *)fm + ((const IMAGE_DOS_HEADER *)fm)->e_lfanew))->FileHeader.TimeDateStamp != k_fs_stamp)) {
        *why = "filesystem_stdio.dll's build (its file handle layout)";
        return -1;
    }
    image_of(fm, &g_fs_lo, &g_fs_hi);
    if (!image_of(GetModuleHandleA("shaderapidx9_stock.dll"), &slo, &shi)) { *why = "shaderapidx9_stock.dll"; return -1; }
    void **vh = *(void ***)g_hw_real;
    if ((uintptr_t)vh[HW_SLOT_HDR] != slo + RVA_HW_HDR || (uintptr_t)vh[HW_SLOT_SRGB] != slo + RVA_HW_SRGB) { *why = "hardware config slots 54, 62"; return -1; }
    if (!image_of(GetModuleHandleA("materialsystem.dll"), &mlo, &mhi)) { *why = "materialsystem.dll is not loaded"; return -1; }
    char *cv = (char *)(mlo + RVA_MS_CV_FULLBRIGHT);
    void **vcv = *(void ***)cv;
    const char *name = *(const char **)(cv + CV_OFF_NAME);
    if ((uintptr_t)vcv != mlo + RVA_MS_CV_VTBL || (uintptr_t)vcv[CV_SLOT_GETINT] != mlo + RVA_MS_CV_GETINT || (uintptr_t)name != mlo + RVA_MS_STR_FULLBRIGHT ||
        strcmp(name, "mat_fullbright") != 0) { *why = "materialsystem's mat_fullbright ConVar"; return -1; }
    g_eng_client = client;
    g_fs = fs;
    g_cv_fullbright = cv;
    g_view_origin = (volatile const float *)x;
    return 1;
}

static int eng_in_game(void) { return ((unsigned char (*)(void *))(*(void ***)g_eng_client)[ENG_SLOT_INGAME])(g_eng_client) != 0; }
static const char *eng_level(void) { return ((const char *(*)(void *))(*(void ***)g_eng_client)[ENG_SLOT_LEVELNAME])(g_eng_client); }
static int hw_hdr(void) { return ((int (*)(void *))(*(void ***)g_hw_real)[HW_SLOT_HDR])(g_hw_real) != 0; }
static int hw_srgb(void) { return ((unsigned char (*)(void *))(*(void ***)g_hw_real)[HW_SLOT_SRGB])(g_hw_real) != 0; }
static int fullbright(void) { return ((int (*)(void *))(*(void ***)g_cv_fullbright)[CV_SLOT_GETINT])(g_cv_fullbright) != 0; }

/* The game's filesystem, on worker 0 */
static void *bfs(void) { return (char *)g_fs + FS_OFF_BASE; }
static void **bfs_vt(void) { return *(void ***)bfs(); }
/* A counted reference on the module of the filesystem's code: a call stuck across the game's quit has something to return to */
static void *fs_pin(void) {
    HMODULE m = NULL;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)*(void **)g_fs, &m) ? (void *)m : NULL;
}
static void fs_unpin(void *pin) { FreeLibrary((HMODULE)pin); }
static void *fs_openex(const char *path) {
    return ((void *(*)(void *, const char *, const char *, unsigned, const char *, char **))(*(void ***)g_fs)[FS_SLOT_OPENEX])(g_fs, path, "rb", 0, NULL, NULL);
}
static long long fs_size_of(void *fh) { return ((unsigned (*)(void *, void *))bfs_vt()[BFS_SLOT_SIZE_HANDLE])(bfs(), fh); }
static void fs_close_fh(void *fh) { ((void (*)(void *, void *))bfs_vt()[BFS_SLOT_CLOSE])(bfs(), fh); }

/* What worker 0 reads a map from: the filesystem's own handle of a loose (or VPK) file, or a handle of our own on the .gma of a map inside a GMA, with the
 * BSP's offset in it */
typedef struct AmbFile { void *fh; HANDLE h; long long base, size; } AmbFile;

static int os_read(HANDLE h, long long ofs, void *buf, int n) {
    OVERLAPPED o;   /* a synchronous handle of our own: the offset only positions this read, nobody else uses the handle */
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)(unsigned long long)ofs;
    o.OffsetHigh = (DWORD)((unsigned long long)ofs >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, (DWORD)n, &got, &o) ? (int)got : 0;
}
static int gma_index_read(void *ctx, long long ofs, void *buf, int n) { return os_read((HANDLE)ctx, ofs, buf, n); }
static int in_fs(const void *p, size_t n) { return (uintptr_t)p >= g_fs_lo && (uintptr_t)p <= g_fs_hi && g_fs_hi - (uintptr_t)p >= n; }

/* A read-only handle of our own on the .gma behind gf, the GMA file object of path's CFileHandle, checked three ways: the same file as the engine's handle
 * (volume and file index), the engine's range inside it, and the GMA's own index listing path at exactly that offset and length. NULL and a reason if not. */
static AmbFile *gma_open(const char *path, const void *gf, const volatile int *cancel) {
    long long base = *(const long long *)((const char *)gf + GF_OFF_BASE), len = *(const long long *)((const char *)gf + GF_OFF_LEN);
    if (AMBVOL_MUTANT == 12) base = len;
    CRITICAL_SECTION *lock = (CRITICAL_SECTION *)(g_fs_lo + RVA_FS_GMA_LOCK);
    const void *vt_win32 = (const void *)(g_fs_lo + RVA_FS_WIN32_VTBL);
    const char *why = NULL;
    HANDLE mine = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION ei, mi;
    static WCHAR wpath[32768];   /* worker 0 only */
    int how = 0;
    if (!in_fs(lock, sizeof *lock) || !in_fs(vt_win32, sizeof(void *))) return photon_log("ambvol: %s: the filesystem's GMA lock is outside its image", path), NULL;
    EnterCriticalSection(lock);
    const char *inner = *(char *const *)((const char *)gf + GF_OFF_SHARED);
    const char *w32 = inner ? *(char *const *)(inner + FH_OFF_FILE) : NULL;
    HANDLE eh = w32 ? *(const HANDLE *)(w32 + W32_OFF_HANDLE) : NULL;
    if (!inner) why = "its .gma was unmounted";
    else if (*(const DWORD *)(inner + FH_OFF_MAGIC) != FH_MAGIC || *(void *const *)(inner + FH_OFF_PACK)) why = "the .gma's own handle is not a plain file";
    else if (!w32 || *(const void *const *)w32 != vt_win32) why = "the .gma is not open through the Win32 file class (filesystem_native 0?)";
    else if (!eh || eh == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(eh, &ei)) why = "the .gma's OS handle cannot be queried";
    else if ((mine = ReOpenFile(eh, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0)) != INVALID_HANDLE_VALUE) how = 1;
    else {
        DWORD n = GetFinalPathNameByHandleW(eh, wpath, sizeof wpath / sizeof wpath[0], 0);
        if (n && n < sizeof wpath / sizeof wpath[0]) how = 2;
        else why = "neither ReOpenFile nor GetFinalPathNameByHandleW works on the .gma's handle";
    }
    LeaveCriticalSection(lock);
    if (how == 2) mine = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER fsz = { .QuadPart = 0 };
    if (!why && mine == INVALID_HANDLE_VALUE) why = "the .gma could not be opened again";
    else if (!why && (!GetFileInformationByHandle(mine, &mi) || mi.dwVolumeSerialNumber != ei.dwVolumeSerialNumber || mi.nFileIndexHigh != ei.nFileIndexHigh ||
                      mi.nFileIndexLow != ei.nFileIndexLow)) why = "the handle opened again is not the engine's .gma";
    else if (!why && (!GetFileSizeEx(mine, &fsz) || base < 0 || len < 0 || base > fsz.QuadPart || len > fsz.QuadPart - base)) why = "the engine's range is not inside the .gma";
    long long io = -1, isz = -1;
    AmbGmaInfo gi = { 0 };
    int rc = 0;
    if (!why) {
        AmbReader r = { (void *)mine, fsz.QuadPart, gma_index_read, cancel };
        rc = AMBVOL_MUTANT == 11 ? AMB_OK : amb_gma_find(&r, path, base, &io, &isz, &gi);
        if (AMBVOL_MUTANT == 11) { io = base; isz = len; }
        if (rc != AMB_OK || io != base || isz != len) why = rc == AMB_ERR_CANCELLED ? "cancelled" : "the GMA's own index does not list it at the engine's offset";
    }
    if (why) {
        photon_log("ambvol: %s is inside a GMA at %lld + %lld: %s (index check %d)", path, base, len, why, rc);
        if (mine != INVALID_HANDLE_VALUE) CloseHandle(mine);
        return NULL;
    }
    AmbFile *f = calloc(1, sizeof *f);
    if (!f) { CloseHandle(mine); return NULL; }
    f->h = mine;
    f->base = base;
    f->size = len;
    photon_log("ambvol: %s is inside a GMA of %lld bytes at %lld + %lld, %d entries: read through a handle of its own (%s)", path, (long long)fsz.QuadPart, base, len,
               gi.entries, how == 1 ? "ReOpenFile" : "its path");
    return f;
}

static void *fs_open(const char *path, int *gma, const volatile int *cancel) {
    void *fh = fs_openex(path);
    if (!fh) return NULL;
    const void *gf = *(void *const *)((char *)fh + FH_OFF_PACK);
    *gma = gf != NULL;
    AmbFile *f = NULL;
    if (gf && AMBVOL_MUTANT != 1) {
        f = gma_open(path, gf, cancel);
        fs_close_fh(fh);   /* the per-open object only: nothing is ever read through the shared handle */
        return f;
    }
    f = calloc(1, sizeof *f);
    if (!f) { fs_close_fh(fh); return NULL; }
    f->fh = fh;
    f->size = fs_size_of(fh);
    return f;
}
static long long fs_size(void *file) { return ((AmbFile *)file)->size; }
static int fs_read_at(void *file, long long ofs, void *buf, int n) {
    AmbFile *f = file;
    if (ofs < 0 || n <= 0 || ofs > f->size || n > f->size - ofs) return 0;
    if (f->h) return os_read(f->h, f->base + ofs, buf, n);
    ((void (*)(void *, void *, long long, int))bfs_vt()[BFS_SLOT_SEEK])(bfs(), f->fh, ofs, 0);
    return ((int (*)(void *, void *, int, void *))bfs_vt()[BFS_SLOT_READ])(bfs(), buf, n, f->fh);
}
static void fs_close(void *file) {
    AmbFile *f = file;
    if (f->h) CloseHandle(f->h);
    else fs_close_fh(f->fh);
    free(f);
}
static void worker_named(int index) {
    typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
    HMODULE kb = GetModuleHandleA("kernelbase.dll"), k32 = GetModuleHandleA("kernel32.dll");
    SetThreadDescriptionFn std = (SetThreadDescriptionFn)(void *)GetProcAddress(kb ? kb : k32, "SetThreadDescription");
    if (!std && k32) std = (SetThreadDescriptionFn)(void *)GetProcAddress(k32, "SetThreadDescription");
    if (std) std(GetCurrentThread(), index == 2 ? L"photon-cube" : index ? L"photon-amb1" : L"photon-amb0");   /* 2: the P4a pick table (cubework.c) */
}
static const AmbWorkFiles k_files = { fs_pin, fs_unpin, fs_open, fs_size, fs_read_at, fs_close, worker_named };

static void avg_bind(void) {
    uintptr_t lo = 0, hi = 0;
    if (!image_of(GetModuleHandleA("engine.dll"), &lo, &hi)) return;
    const char *why = NULL;
    uintptr_t la = lo + RVA_LA_READ, li = lo + RVA_LI_READ, tc = lo + RVA_TIME_CALL;
    uintptr_t la_cv = 0, li_cv = 0, fn = 0, cl = 0;
    if (!pat_match(la, k_pat_la_read, lo, hi)) why = "the r_lightaverage reader";
    else if (!pat_match(li, k_pat_li_read, lo, hi)) why = "the r_lightinterp reader";
    else if (!pat_match(tc, k_pat_time_call, lo, hi)) why = "the client time call";
    if (!why) {
        la_cv = rip_target(la, 16, 20); li_cv = rip_target(li, 10, 14);
        fn = rip_target(tc, 40, 44); cl = rip_target(tc, 3, 7);
        if (rip_target(la, 3, 7) != la_cv + CV_OFF_PARENT || rip_target(la, 32, 36) != la_cv + CV_OFF_NVALUE || rip_target(li, 3, 7) != li_cv + CV_OFF_PARENT ||
            rip_target(li, 23, 27) != li_cv + CV_OFF_FVALUE || la_cv < lo || la_cv + 0x60 > hi || li_cv < lo || li_cv + 0x60 > hi || cl < lo || cl + CL_OFF_REMAINDER + 8 > hi)
            why = "the averaging ConVars' fields";
        else if (!pat_match(fn, k_pat_time_fn, lo, hi)) why = "the client time function";
        else {
            const char *n1 = *(const char *const *)(la_cv + CV_OFF_NAME), *n2 = *(const char *const *)(li_cv + CV_OFF_NAME);
            if ((uintptr_t)n1 < lo || (uintptr_t)n1 >= hi || (uintptr_t)n2 < lo || (uintptr_t)n2 >= hi || strcmp(n1, "r_lightaverage") != 0 || strcmp(n2, "r_lightinterp") != 0)
                why = "the averaging ConVars' names";
        }
    }
    if (why) {
        photon_log("ambvol: %s differs from the validated build: A is low-passed with the defaults (r_lightaverage 1, r_lightinterp 5, the process clock)", why);
        return;
    }
    g_cv_la = (const char *)la_cv; g_cv_li = (const char *)li_cv;
    g_time_fn = (double (*)(const void *))fn; g_cl = (const void *)cl;
    photon_log("ambvol: the engine's light averaging is read from engine+0x%llx (r_lightaverage), +0x%llx (r_lightinterp), the client time at +0x%llx",
               (unsigned long long)(la_cv - lo), (unsigned long long)(li_cv - lo), (unsigned long long)(cl - lo));
}

/* The averaging inputs now: read only. A ConVar whose parent is not itself would be read through a virtual: not done, the default instead. */
static void avg_inputs(double *now, float *rate, int *average) {
    *rate = 5.0f; *average = 1;
    if (g_cv_la && g_cv_li && *(void *const volatile *)(g_cv_la + CV_OFF_PARENT) == (const void *)g_cv_la &&
        *(void *const volatile *)(g_cv_li + CV_OFF_PARENT) == (const void *)g_cv_li) {
        uint32_t ka = (uint32_t)(uintptr_t)g_cv_la, ki = (uint32_t)(uintptr_t)g_cv_li;
        uint32_t fb = *(const volatile uint32_t *)(g_cv_li + CV_OFF_FVALUE) ^ ki;
        memcpy(rate, &fb, sizeof *rate);
        *average = (int)(*(const volatile uint32_t *)(g_cv_la + CV_OFF_NVALUE) ^ ka) != 0;
        if (!(*rate >= 0.0f && *rate < 1e6f)) *rate = 5.0f;
    } else if (g_cv_la && !g_track_bad_cv++) photon_log("ambvol: an averaging ConVar has another parent: the defaults are used");
    if (g_time_fn) *now = g_time_fn(g_cl);
    else { LARGE_INTEGER t; QueryPerformanceCounter(&t); *now = (double)t.QuadPart * g_qpc_us * 1e-6; }
}

/* From the Present hook, under g_lock. No device call here: Present still runs while stock is deactivated, and a new DEFAULT texture then would block a non-Ex Reset.
 * Device work waits for a RenderPass, which stock never reaches in that state. */
static void amb_frame(void) {
    if (!g_engine_ok) {
        const char *why = NULL;
        int r = engine_bind(&why);
        if (r == 0) return;
        static const int dim[3] = { VOX_X, VOX_Y, VOX_Z }, voxel[3] = { 32, 32, 32 };
        if (r > 0 && amb_work_init(&k_files, dim, voxel, SLAB) != AMB_OK) { r = -1; why = "the cascade's dimensions"; }
        if (r < 0) {
            photon_log("ambvol: %s differs from the validated build: the volume stays off", why);
            photon_amb_on = 0;
            return;
        }
        g_engine_ok = 1;
        avg_bind();
        photon_log("ambvol: engine checks passed: main view origin at engine+0x%llx", (unsigned long long)((uintptr_t)g_view_origin - (uintptr_t)GetModuleHandleA("engine.dll")));
    }
    g_gate_off = (AMBVOL_MUTANT != 8 && hw_srgb()) || (AMBVOL_MUTANT != 7 && fullbright());
    char level[MAX_PATH] = "";
    int want = photon_vlg_ambient_wanted() && eng_in_game();
    if (want) {
        snprintf(level, sizeof level, "%s", eng_level());
        want = eng_in_game();   /* the name is stable only while in game: read inside that window */
    }
    double cam[3] = { g_view_origin[0], g_view_origin[1], g_view_origin[2] };
    amb_work_frame(want, level, want ? hw_hdr() : 0, want ? cam : NULL);
    static char track_level[MAX_PATH];
    static int track_want;
    if (want != track_want || strcmp(level, track_level) != 0) {   /* another map: no model carries over */
        amb_track_reset(&g_track);
        g_track_frame = ~0ull;
        track_want = want;
        snprintf(track_level, sizeof track_level, "%s", level);
    }
}

/* P4a (cubemap.c, PHOTON_TEST_CUBEPROBE only): the same engine facts for the pick table, from the Present hook. engine_bind is idempotent: it only reads and
 * checks, and sets the same pointers each time, so binding here as well as in amb_frame is harmless. */
/* P5 (lights.c): the hardware config's HDR type now, the engine's own choice of the light lump; 0 until engine_bind passed (which checks slot 54) */
int photon_eng_hdr(void) {
    AcquireSRWLockExclusive(&g_lock);
    int h = g_eng_client && g_hw_real ? hw_hdr() : 0;
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

int photon_eng_facts(int *in_game, char *level, size_t n, double cam[3], const struct AmbWorkFiles **files) {
    static int state;   /* 0 not bound yet, 1 bound, -1 differs from the validated build */
    AcquireSRWLockExclusive(&g_lock);
    if (state == 0) {
        const char *why = NULL;
        int r = g_engine_ok ? 1 : engine_bind(&why);
        if (r < 0) photon_log("ambvol: %s differs from the validated build: no engine facts for the pick table", why);
        if (r) state = r > 0 ? 1 : -1;
    }
    int ok = state;
    if (ok > 0) {
        *in_game = eng_in_game();
        level[0] = 0;
        if (*in_game) {
            snprintf(level, n, "%s", eng_level());
            *in_game = eng_in_game();   /* the name is stable only while in game */
        }
        for (int i = 0; i < 3; i++) cam[i] = g_view_origin[i];
        *files = &k_files;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

static int up_copy(void *ctx, const uint8_t *atlas, size_t rp, size_t sp, const AmbTexBox *b) {
    IDirect3DVolumeTexture9 *v = ctx;
    D3DBOX box = { (UINT)b->u0, (UINT)b->v0, (UINT)b->u1, (UINT)b->v1, (UINT)b->w0, (UINT)b->w1 };
    D3DLOCKED_BOX lb;
    HRESULT hr = v->lpVtbl->LockBox(v, 0, &lb, &box, 0);
    if (FAILED(hr)) {
        if (InterlockedIncrement64(&g_lock_fail) <= 4)
            photon_log("ambvol: LockBox u %d..%d v %d..%d w %d..%d -> hr 0x%lx", b->u0, b->u1, b->v0, b->v1, b->w0, b->w1, (unsigned long)hr);
        return 0;
    }
    size_t row = (size_t)(b->u1 - b->u0) * 8;
    for (int w = b->w0; w < b->w1; w++)
        for (int y = b->v0; y < b->v1; y++)
            memcpy((uint8_t *)lb.pBits + (size_t)(w - b->w0) * (size_t)lb.SlicePitch + (size_t)(y - b->v0) * (size_t)lb.RowPitch,
                   atlas + (size_t)w * sp + (size_t)y * rp + (AMBVOL_MUTANT == 5 ? 0 : (size_t)b->u0 * 8), row);
    v->lpVtbl->UnlockBox(v, 0);
    return 1;
}

/* Draw thread, under g_lock: 1 if the texture exists and matches the valid box. It is created only once there is data for it. */
static int amb_vol_ready(IDirect3DDevice9 *d) {
    if (!g_vol) {
        if (g_broken || !g_engine_ok || !amb_work_has_data()) return 0;
        if (!vol_create(d)) return 0;
        g_need_full = AMBVOL_MUTANT != 4;
        photon_log("ambvol: volume created on thread %lu (create #%lld)", g_create_tid, (long long)g_creates);
    }
    if (g_need_full) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        if (!amb_work_full_upload(up_copy, g_vol)) return 0;
        QueryPerformanceCounter(&b);
        g_fill_us = (double)(b.QuadPart - a.QuadPart) * g_qpc_us;
        g_need_full = 0;
    }
    return 1;
}

void photon_amb_first_draw(void) {
    unsigned long long f = photon_frame_count();
    if (f == g_last_frame) return;
    AcquireSRWLockExclusive(&g_lock);
    if (photon_amb_on && g_engine_ok) {
        g_last_frame = f;
        IDirect3DDevice9 *d = photon_d3d_device();
        if (d && amb_vol_ready(d)) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            int did = amb_work_upload(up_copy, g_vol);
            QueryPerformanceCounter(&b);
            if (did) {
                float us = (float)((double)(b.QuadPart - a.QuadPart) * g_qpc_us);
                InterlockedIncrement64(&g_uploads);
                if (g_nup < NUP) g_up_us[g_nup++] = us;
                g_frame_up_us += us;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static int predraw_amb(const void *api_real) {
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return 0;
    int usable = 0;
    AcquireSRWLockExclusive(&g_lock);
    InterlockedIncrement64(&g_predraws);
    if (photon_amb_on && amb_vol_ready(d)) {
        if (AMBVOL_MUTANT != 9 && !s15_at_rest(api_real)) InterlockedIncrement64(&g_not_at_rest);
        else {
            unsigned long long f = photon_frame_count();
            int ours = bound_is_ours(d, "draw entry");
            if (!ours) InterlockedIncrement64(&g_clobber_draw);
            if (!ours || f != g_bind_frame) { vol_bind(d); g_bind_frame = f; }
            usable = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return usable;
}

float photon_amb_weight(const void *api_real, const float origin[3], int *why) {
    AcquireSRWLockExclusive(&g_lock);
    int on = photon_amb_on && g_engine_ok, gate = g_gate_off;
    float f = on && !gate ? amb_work_weight(origin) : 0.0f;
    ReleaseSRWLockExclusive(&g_lock);
    if (!on || gate) { *why = PHOTON_AMB_OFF_GATE; return 0.0f; }
    if (!(f > 0.0f)) { *why = PHOTON_AMB_OFF_RANGE; return 0.0f; }
    if (!photon_vol_predraw(api_real, 1)) { *why = PHOTON_AMB_OFF_REST; return 0.0f; }
    return f;
}

/* P6 (p6.c): the point is fully inside the volume (weight 1), so a draw there needs nothing from the engine's ambient cube */
int photon_amb_covers(const float origin[3]) {
    AcquireSRWLockExclusive(&g_lock);
    int on = photon_amb_on && g_engine_ok && !g_gate_off;
    float f = on ? amb_work_weight(origin) : 0.0f;
    ReleaseSRWLockExclusive(&g_lock);
    return f >= 1.0f;
}

void photon_amb_constants(float c[4][4]) {
    AcquireSRWLockExclusive(&g_lock);
    amb_work_constants(c);
    ReleaseSRWLockExclusive(&g_lock);
}

/* The fold with A low-passed per model (amb_track.h): the draw thread, once per draw with weight. The leaf ambient is evaluated only when the origin changes. */
void photon_amb_fold(const float origin[3], float f, float cube[6][4]) {
    static float last_o[3], last_a[AMB_VOXEL_FLOATS];
    static int last_valid;
    AcquireSRWLockExclusive(&g_lock);
    if (photon_amb_on && g_engine_ok && AMBVOL_MUTANT != 10) {
        if (g_track_frame == ~0ull) last_valid = 0;   /* a new map or a Reset since: the cached A may be another map's */
        if (!last_valid || memcmp(origin, last_o, sizeof last_o) != 0) {
            memset(last_a, 0, sizeof last_a);
            amb_work_trace_a(origin, last_a);   /* the engine's leaf rule at the origin; zeros without a map */
            memcpy(last_o, origin, sizeof last_o);
            last_valid = 1;
        }
        unsigned long long fr = photon_frame_count();
        if (fr != g_track_frame) {
            double now = 0.0;
            float rate;
            int average;
            avg_inputs(&now, &rate, &average);
            amb_track_frame(&g_track, fr, now, rate, average);
            g_track_frame = fr;
        }
        float a[AMB_VOXEL_FLOATS];
        if (AMBVOL_MUTANT == 13) memcpy(a, last_a, sizeof a);
        else amb_track_draw(&g_track, origin, last_a, a);
        amb_clip_fold((const float (*)[4])cube, a, f, cube);
        memcpy(g_last_as, a, sizeof a);
        g_last_as_valid = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* --- PHOTON_TEST_AMBTRACE=1 (P3h acceptance, tests only): what each lit engine-route draw got, on the draw thread. Rows go to photon_ambtrace.csv in the log directory, one per
 * origin and only when its weight or cube changed, plus a state row whenever the window changed. Faces are luma, +X -X +Y -Y +Z -Z. tools/p3h/ reads it. */
enum { TRACE_SLOTS = 4096, TRACE_MAX_BYTES = 12 << 20 };
static FILE *g_trace;
static long g_trace_bytes;
static struct { float o[3], f, c; unsigned char used; } g_trace_tab[TRACE_SLOTS];
int photon_amb_trace_on(void) { return g_trace_on; }

static float tr_luma(const float *v) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; }

void photon_amb_trace(const float origin[3], float f, int why, const float before[6][4], const float after[6][4]) {
    static int tried;
    static char last_state[256];
    if (!tried) {
        char dir[MAX_PATH], path[MAX_PATH + 32];
        tried = 1;
        DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
        if (n && n < sizeof dir) {
            snprintf(path, sizeof path, "%s\\photon_ambtrace.csv", dir);
            g_trace = fopen(path, "w");
        }
        if (g_trace) fprintf(g_trace, "D,frame,t_ms,x,y,z,f,why,C+x,C-x,C+y,C-y,C+z,C-z,A+x,A-x,A+y,A-y,A+z,A-z,R+x,R-x,R+y,R-y,R+z,R-z\nS,frame,t_ms,state\n");
    }
    if (!g_trace || g_trace_bytes > TRACE_MAX_BYTES) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double ms = (double)t.QuadPart * g_qpc_us * 1e-3;
    unsigned long long frame = photon_frame_count();
    char st[256];
    AcquireSRWLockExclusive(&g_lock);
    amb_work_trace_state(st, sizeof st);
    ReleaseSRWLockExclusive(&g_lock);
    if (strcmp(st, last_state) != 0) {
        memcpy(last_state, st, sizeof st);
        g_trace_bytes += fprintf(g_trace, "S,%llu,%.1f,%s\n", frame, ms, st);
    }
    float c = 0.0f;
    for (int i = 0; i < 6; i++) c += tr_luma(before[i]);
    unsigned h = 2166136261u;
    for (int i = 0; i < 3; i++) { unsigned u; memcpy(&u, &origin[i], 4); h = (h ^ u) * 16777619u; }
    for (int probe = 0; probe < 32; probe++) {
        unsigned k = (h + (unsigned)probe) % TRACE_SLOTS;
        if (!g_trace_tab[k].used) {
            g_trace_tab[k].used = 1;
            memcpy(g_trace_tab[k].o, origin, sizeof g_trace_tab[k].o);
            g_trace_tab[k].f = -1.0f;
        }
        if (memcmp(g_trace_tab[k].o, origin, sizeof g_trace_tab[k].o) != 0) continue;
        float df = f - g_trace_tab[k].f, dc = c - g_trace_tab[k].c;
        if (df < 0.002f && df > -0.002f && dc < 0.005f * c && dc > -0.005f * c) return;
        g_trace_tab[k].f = f;
        g_trace_tab[k].c = c;
        break;
    }
    float a[AMB_VOXEL_FLOATS] = { 0 };
    AcquireSRWLockExclusive(&g_lock);
    if (g_last_as_valid && f > 0.0f) memcpy(a, g_last_as, sizeof a);
    else amb_work_trace_a(origin, a);
    g_last_as_valid = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_trace_bytes += fprintf(g_trace, "D,%llu,%.1f,%.2f,%.2f,%.2f,%.4f,%d", frame, ms, origin[0], origin[1], origin[2], f, why);
    for (int i = 0; i < 6; i++) g_trace_bytes += fprintf(g_trace, ",%.5f", tr_luma(before[i]));
    for (int i = 0; i < 6; i++) g_trace_bytes += fprintf(g_trace, ",%.5f", tr_luma(&a[i * 3]));
    for (int i = 0; i < 6; i++) g_trace_bytes += fprintf(g_trace, ",%.5f", tr_luma(after[i]));
    fputc('\n', g_trace);
    if (frame % 16 == 0) fflush(g_trace);
}

void photon_amb_shutdown(const char *why) {
    if (!g_amb_wanted) return;
    AcquireSRWLockExclusive(&g_lock);
    photon_amb_on = 0;
    if (g_engine_ok) amb_work_shutdown(why);
    if (g_trace) fflush(g_trace);
    ReleaseSRWLockExclusive(&g_lock);
}

void photon_amb_csv(FILE *f, int header) {
    if (header) { fprintf(f, ",amb_upload_us,amb_backlog"); return; }
    fprintf(f, ",%.0f,%d", g_frame_up_us, photon_amb_on && g_engine_ok ? amb_work_backlog() : 0);
    g_frame_up_us = 0;
}

/* Samplers declared by a pixel shader's bytecode (dcl tokens; same walk as tools) */
static unsigned ps_samplers(const DWORD *w, UINT n) {
    unsigned mask = 0;
    for (UINT i = 1; i < n;) {
        DWORD t = w[i];
        if (t == 0xFFFF) break;
        if ((t & 0xFFFF) == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        if ((t & 0xFFFF) == 0x1F && i + 2 < n) {
            DWORD d = w[i + 2];
            if ((((d >> 28) & 7) | ((d >> 8) & 0x18)) == 10 && (d & 0x7FF) < 16) mask |= 1u << (d & 0x7FF);
        }
        i += 1 + ((t >> 24) & 0xF);
    }
    return mask;
}

static void note_shader(IDirect3DDevice9 *d, int wants) {
    IDirect3DPixelShader9 *ps = NULL;
    if (FAILED(d->lpVtbl->GetPixelShader(d, &ps)) || !ps) return;
    size_t h = ((uintptr_t)ps >> 4) % PS_TABLE;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) % PS_TABLE) {
        if (g_ps_tab[h].ps == ps) break;
        if (!g_ps_tab[h].ps) {
            UINT sz = 0;
            unsigned mask = 0;
            if (SUCCEEDED(ps->lpVtbl->GetFunction(ps, NULL, &sz)) && sz) {
                DWORD *fn = malloc(sz);
                if (fn && SUCCEEDED(ps->lpVtbl->GetFunction(ps, fn, &sz))) mask = ps_samplers(fn, sz / 4);
                free(fn);
            }
            g_ps_tab[h].ps = ps;
            g_ps_tab[h].mask = mask;
            for (int s = 0; s < 16; s++) if (mask >> s & 1) g_ps_shaders[s]++;
            break;
        }
        if (probe == 63) { g_ps_table_full++; h = PS_TABLE; break; }
    }
    if (h < PS_TABLE) {
        for (int s = 0; s < 16; s++) if (g_ps_tab[h].mask >> s & 1) InterlockedIncrement64(&g_ps_draws[s]);
        /* with the volume left bound, a stock draw sampling s15 would read it: this must stay 0 */
        if (!wants && (g_ps_tab[h].mask >> VOL_SAMPLER & 1)) InterlockedIncrement64(&g_stock_samples_s15);
    }
    ps->lpVtbl->Release(ps);
}

/* A replacement inside stock's RenderPass after the at-rest check means that draw did not see the volume: this must stay 0 */
void photon_vol_postdraw(int wants) {
    if ((!photon_vol_on && !photon_amb_on) || g_broken) return;
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) return;
    AcquireSRWLockExclusive(&g_lock);
    if (wants && g_vol && !bound_is_ours(d, "draw exit")) InterlockedIncrement64(&g_clobber_inside);
    if (photon_vol_on) note_shader(d, wants);
    ReleaseSRWLockExclusive(&g_lock);
}

void photon_vol_frame(void) {
    if (photon_amb_on) {
        AcquireSRWLockExclusive(&g_lock);
        if (photon_amb_on) amb_frame();
        ReleaseSRWLockExclusive(&g_lock);
        if (photon_amb_on && photon_frame_count() % 600 == 0) photon_vol_dump("periodic");
        return;
    }
    if (!photon_vol_on) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int s = 0; s < 16; s++) { if (g_en_now[s]) InterlockedIncrement64(&g_en_frames[s]); g_en_now[s] = 0; }
    ReleaseSRWLockExclusive(&g_lock);
}

void photon_vol_release(void) {
    if (!photon_vol_on && !g_amb_wanted) return;
    InterlockedIncrement64(&g_releases);
    g_release_tid = GetCurrentThreadId();
    if (!TryAcquireSRWLockExclusive(&g_lock)) {   /* the draw thread was inside predraw: it is not parked */
        InterlockedIncrement64(&g_release_contended);
        AcquireSRWLockExclusive(&g_lock);
    }
    amb_track_reset(&g_track);   /* a device Reset: start every model over, as after a map change */
    g_track_frame = ~0ull;
    if (g_vol && AMBVOL_MUTANT != 6) {
        IDirect3DDevice9 *d = photon_d3d_device();
        if (d) d->lpVtbl->SetTexture(d, VOL_SAMPLER, NULL);   /* a non-Ex Reset fails while a DEFAULT resource lives */
        g_vol->lpVtbl->Release(g_vol);
        g_vol = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
    photon_log("ambvol: released on thread %lu (draw thread created on %lu)", g_release_tid, g_create_tid);
}

void photon_vol_restore(void) {
    if (photon_vol_on || g_amb_wanted) InterlockedIncrement64(&g_restores);   /* the next draw recreates and refills it */
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static void amb_dump(const char *why) {
    static float s[NUP];
    int n = g_nup;
    float p50 = 0, p95 = 0, mx = 0;
    if (n) {
        memcpy(s, g_up_us, (size_t)n * sizeof(float));
        qsort(s, (size_t)n, sizeof(float), cmpf);
        p50 = s[n / 2]; p95 = s[n * 95 / 100]; mx = s[n - 1];
    }
    photon_log("ambvol summary (%s): %s; creates %lld (fail %lld), releases %lld, restores %lld; volume draws %lld, binds %lld; deferred because stock was about to bind s15 %lld; replaced before"
               " a volume draw %lld, inside one %lld; release while the draw thread was inside the hook %lld; lock failures %lld; upload parts %lld: p50 %.0f us, p95 %.0f us, max %.0f us; last full"
               " upload %.0f us", why, photon_amb_on ? (g_engine_ok ? "on" : "waiting for engine.dll") : "off", (long long)g_creates, (long long)g_create_fail, (long long)g_releases,
               (long long)g_restores, (long long)g_predraws, (long long)g_binds, (long long)g_not_at_rest, (long long)g_clobber_draw, (long long)g_clobber_inside, (long long)g_release_contended,
               (long long)g_lock_fail, (long long)g_uploads, p50, p95, mx, g_fill_us);
    if (g_engine_ok) amb_work_summary(why);
    const AmbTrackStats *t = &g_track.st;
    if (g_engine_ok) photon_log("ambvol tracks (%s): draws %lld: same model this frame %lld, matched %lld (ambiguous %lld), new %lld, teleports joined %lld, dropped %lld,"
                                " table full %lld, averaging off %lld; engine inputs %s", why, t->draws, t->reused, t->matched, t->ambiguous, t->created, t->joined, t->dropped,
                                t->overflow, t->unaveraged, g_time_fn ? "read" : "defaults");
}

void photon_vol_dump(const char *why) {
    if (g_amb_wanted) { amb_dump(why); return; }
    if (!photon_vol_on) return;
    int n = g_nup, nf = g_nflip;
    float p50 = 0, p95 = 0, mx = 0, fp50 = 0, fp95 = 0;
    static float s[NUP];
    if (n) {
        memcpy(s, g_up_us, (size_t)n * sizeof(float));
        qsort(s, (size_t)n, sizeof(float), cmpf);
        p50 = s[n / 2]; p95 = s[n * 95 / 100]; mx = s[n - 1];
    }
    if (nf) {
        memcpy(s, g_flip_us, (size_t)nf * sizeof(float));
        qsort(s, (size_t)nf, sizeof(float), cmpf);
        fp50 = s[nf / 2]; fp95 = s[nf * 95 / 100];
    }
    photon_log("ambvol summary (%s): creates %lld (fail %lld), releases %lld, restores %lld; draws %lld; binds %lld (p50 %.2f us, p95 %.2f us);"
               " volume draws deferred because stock was about to bind s15 %lld; replaced before a volume draw %lld, inside one %lld; stock draws"
               " sampling s15 %lld; EnableTexture(15) %lld, BindTexture(15) %lld; release while the draw thread was inside the hook %lld; lock failures %lld;"
               " slab uploads %lld: p50 %.0f us, p95 %.0f us, max %.0f us; full fill %.0f us", why,
               (long long)g_creates, (long long)g_create_fail, (long long)g_releases, (long long)g_restores, (long long)g_predraws,
               (long long)g_binds, fp50, fp95, (long long)g_not_at_rest, (long long)g_clobber_draw, (long long)g_clobber_inside,
               (long long)g_stock_samples_s15, (long long)g_shadow_s15, (long long)g_bind_s15, (long long)g_release_contended,
               (long long)g_lock_fail, (long long)g_uploads, p50, p95, mx, g_fill_us);
    for (int i = 0; i < NTID && g_modify_tids[i].tid; i++)
        photon_log("ambvol: ModifyTexture from thread %lu: %lld", g_modify_tids[i].tid, (long long)g_modify_tids[i].n);
    for (int i = 0; i < NTID && g_pre_tids[i].tid; i++)
        photon_log("ambvol: draws (predraw) from thread %lu: %lld", g_pre_tids[i].tid, (long long)g_pre_tids[i].n);
    for (int s = 0; s < 16; s++)
        photon_log("ambvol: s%d: EnableTexture(on) %lld, frames enabled in stock's cache %lld, draws whose pixel shader declares it %lld"
                   " (%ld shaders)", s, (long long)g_en_calls[s], (long long)g_en_frames[s], (long long)g_ps_draws[s], g_ps_shaders[s]);
    if (g_ps_table_full) photon_log("ambvol: shader table full %ld times", g_ps_table_full);
    for (int i = 0; i < NTID && g_bind_tids[i].tid; i++)
        photon_log("ambvol: BindTexture(15) from thread %lu: %lld", g_bind_tids[i].tid, (long long)g_bind_tids[i].n);
}
