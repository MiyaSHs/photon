/* T3.4a2: direct D3D9 pixel-shader constants c32+ (see d3dconst.h for the design, guards and threading). */
#include <windows.h>
#include <initguid.h>   /* defines IID_IDirect3DDevice9 in this translation unit (no dxguid import lib) */
#include <d3d9.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3dconst.h"
#include "log.h"
#include "frames.h"

/* Slot numbers from the mingw-w64 d3d9.h, checked against the stock call sites (d3dconst.h). */
_Static_assert(offsetof(IDirect3DDevice9Vtbl, QueryInterface) == 0x0, "IUnknown::QueryInterface slot 0");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, Release) == 0x10, "IUnknown::Release slot 2");
_Static_assert(offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantF) == 0x368,
               "IDirect3DDevice9::SetPixelShaderConstantF = slot 109 = +0x368, as stock 0x18001efc9 calls it");
enum { D3D9_SLOT_SETPSCONSTF = offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantF) / sizeof(void *) };

/* Build 26229091 (T1.5 guard) RVAs in shaderapidx9_stock.dll. */
enum { RVA_G_D3DDEVICE = 0xa6b18,      /* g_pD3DDevice (DAT_1800a6b18) */
       RVA_SETPSCONST = 0x1ef50,       /* ShaderApi030 slot 31 SetPixelShaderConstant (FUN_18001ef50) */
       RVA_LOAD_SITE = 0x1efb7,        /* mov rcx,[rip+disp32] -> g_pD3DDevice */
       RVA_CALL_SITE = 0x1efc9 };      /* call [r10+0x368] */
enum { SLOT_API_SETPSCONST = 31, SLOT_API_RENDERPASS = 125 };

static uintptr_t g_base;            /* stock module base; 0 = API disabled */
static int g_code_ok;               /* guard (2) passed */
static void *volatile g_good_dev;   /* last device pointer that passed guard (4) */
static void *volatile g_bad_dev;    /* last device pointer that failed it (not re-validated) */
static SRWLOCK g_val_lock = SRWLOCK_INIT;

/* stats */
static volatile LONG64 g_calls, g_ok, g_fail, g_nodev;
static volatile LONG g_last_tid, g_tid_logs;
static volatile LONG g_fail_logs;

/* test mode */
static int g_test = -1;
static int g_test_render_only;      /* PHOTON_TEST_C32_THREAD=render: write only from threads other than g_main_tid */
static DWORD g_main_tid;            /* the thread that loaded the stock DLL (the game's main thread) */
static float g_test_c32[4];
static PhotonWrapClass *g_api_cls;

static int text_range(HMODULE m, uintptr_t *lo, uintptr_t *hi) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)m + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 5) == 0) {
            *lo = (uintptr_t)m + sec->VirtualAddress; *hi = *lo + sec->Misc.VirtualSize; return 1;
        }
    return 0;
}
static int image_range(HMODULE m, uintptr_t *lo, uintptr_t *hi) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)m + dos->e_lfanew);
    *lo = (uintptr_t)m; *hi = *lo + nt->OptionalHeader.SizeOfImage;
    return 1;
}
static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY))) return 0;
    return (uintptr_t)p + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}

void photon_d3d_setup(HMODULE stock, int build_ok_and_wrapping) {
    g_base = 0; g_code_ok = 0;
    g_main_tid = GetCurrentThreadId();
    if (!stock || !build_ok_and_wrapping) {
        photon_log("d3dconst: disabled (%s)", stock ? "pass-through or unknown build" : "no stock module");
        return;
    }
    const uint8_t *b = (const uint8_t *)stock;
    const uint8_t *ld = b + RVA_LOAD_SITE, *cl = b + RVA_CALL_SITE;
    int32_t disp; memcpy(&disp, ld + 3, 4);
    uintptr_t target = (uintptr_t)(ld + 7) + (intptr_t)disp;
    static const uint8_t k_call[] = { 0x41, 0xff, 0x92, 0x68, 0x03, 0x00, 0x00 };
    g_code_ok = ld[0] == 0x48 && ld[1] == 0x8b && ld[2] == 0x0d && target == (uintptr_t)b + RVA_G_D3DDEVICE &&
                memcmp(cl, k_call, sizeof k_call) == 0;
    photon_log("d3dconst: stock SetPixelShaderConstant site +0x%x: %02x %02x %02x -> +0x%llx, +0x%x call [r10+0x368] %s"
               " -> device global stock+0x%x %s", RVA_LOAD_SITE, ld[0], ld[1], ld[2],
               (unsigned long long)(target - (uintptr_t)b), RVA_CALL_SITE,
               memcmp(cl, k_call, sizeof k_call) == 0 ? "ok" : "MISMATCH", RVA_G_D3DDEVICE,
               g_code_ok ? "verified" : "NOT verified (API disabled)");
    if (g_code_ok) g_base = (uintptr_t)stock;
}

static int validate(void *dev) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    uintptr_t ilo, ihi, tlo, thi;
    if (!d3d9 || !image_range(d3d9, &ilo, &ihi) || !text_range(d3d9, &tlo, &thi)) {
        photon_log("d3dconst: device %p rejected: d3d9.dll not loaded", dev); return 0;
    }
    if (!readable(dev, sizeof(void *))) { photon_log("d3dconst: device %p rejected: not readable", dev); return 0; }
    void **vt = *(void ***)dev;
    /* The vtable itself may live outside d3d9.dll: native Windows d3d9.dll builds the device's vtable on the heap (laptop, analysis/19), DXVK keeps
     * it in its image. What matters is checked below: every slot up to SetPixelShaderConstantF points into d3d9.dll's .text, and QueryInterface. */
    if (!readable(vt, (D3D9_SLOT_SETPSCONSTF + 1) * sizeof(void *))) {
        photon_log("d3dconst: device %p rejected: vtable %p not readable", dev, (void *)vt);
        return 0;
    }
    if ((uintptr_t)vt < ilo || (uintptr_t)vt >= ihi)
        photon_log("d3dconst: device %p: vtable %p is outside d3d9.dll [%p, %p) (native d3d9 builds it on the heap); checking its slots",
                   dev, (void *)vt, (void *)ilo, (void *)ihi);
    for (int s = 0; s <= D3D9_SLOT_SETPSCONSTF; s++)
        if ((uintptr_t)vt[s] < tlo || (uintptr_t)vt[s] >= thi) {
            photon_log("d3dconst: device %p rejected: vtable slot %d = %p outside d3d9.dll .text", dev, s, vt[s]);
            return 0;
        }
    IDirect3DDevice9 *d = dev, *q = NULL;
    HRESULT hr = d->lpVtbl->QueryInterface(d, &IID_IDirect3DDevice9, (void **)&q);
    if (q) q->lpVtbl->Release(q);
    if (FAILED(hr) || q != d) {
        photon_log("d3dconst: device %p rejected: QueryInterface(IID_IDirect3DDevice9) hr 0x%lx -> %p", dev,
                   (unsigned long)hr, (void *)q);
        return 0;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(d3d9, path, sizeof path);
    photon_log("d3dconst: device %p validated (vtable %p in %s, SetPixelShaderConstantF slot %d = d3d9+0x%llx) tid %lu"
               " frame %llu", dev, (void *)vt, path, D3D9_SLOT_SETPSCONSTF,
               (unsigned long long)((uintptr_t)vt[D3D9_SLOT_SETPSCONSTF] - ilo), GetCurrentThreadId(),
               (unsigned long long)photon_frame_count());
    return 1;
}

int photon_d3d_api_ready(void) { return g_base != 0; }

void *photon_d3d_device(void) {
    if (!g_base) return NULL;
    void *dev = *(void *volatile *)(g_base + RVA_G_D3DDEVICE);
    if (!dev) return NULL;
    if (dev == g_good_dev) return dev;
    if (dev == g_bad_dev) return NULL;
    AcquireSRWLockExclusive(&g_val_lock);
    void *r = NULL;
    if (dev == g_good_dev) r = dev;
    else if (dev != g_bad_dev) {
        if (validate(dev)) { g_good_dev = dev; r = dev; } else g_bad_dev = dev;
    }
    ReleaseSRWLockExclusive(&g_val_lock);
    return r;
}

static void note_thread(void) {
    LONG tid = (LONG)GetCurrentThreadId();
    LONG last = InterlockedExchange(&g_last_tid, tid);
    if (last != tid && InterlockedIncrement(&g_tid_logs) <= 16)
        photon_log("d3dconst: SetPixelShaderConstantF caller thread %ld -> %ld at frame %llu (call %lld)", (long)last,
                   (long)tid, (unsigned long long)photon_frame_count(), (long long)g_calls);
}

long photon_d3d_set_ps_const(int start, const float *v, int count) {
    InterlockedIncrement64(&g_calls);
    if (!v || count < 1 || start < PHOTON_D3D_PS_FIRST || start > PHOTON_D3D_PS_END - count) {
        InterlockedIncrement64(&g_fail);
        if (InterlockedIncrement(&g_fail_logs) <= 8)
            photon_log("d3dconst: rejected start %d count %d v %p (allowed c%d..c%d, v non-NULL)", start, count, (const void *)v,
                       PHOTON_D3D_PS_FIRST, PHOTON_D3D_PS_END - 1);
        return E_INVALIDARG;
    }
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d) { InterlockedIncrement64(&g_nodev); return E_POINTER; }
    note_thread();
    HRESULT hr = d->lpVtbl->SetPixelShaderConstantF(d, (UINT)start, v, (UINT)count);
    if (SUCCEEDED(hr)) InterlockedIncrement64(&g_ok);
    else {
        InterlockedIncrement64(&g_fail);
        if (InterlockedIncrement(&g_fail_logs) <= 8)
            photon_log("d3dconst: SetPixelShaderConstantF(%d, %d) -> hr 0x%lx", start, count, (unsigned long)hr);
    }
    return hr;
}

/* --- PHOTON_TEST_C32 ------------------------------------------------------------------------------------------- */
static int test_mode(void) {
    if (g_test < 0) {
        char e[128];
        DWORD n = GetEnvironmentVariableA("PHOTON_TEST_C32", e, sizeof e);
        g_test = 0;
        if (n > 0 && n < sizeof e) {
            char *s = e, *end;
            int k = 0;
            for (; k < 4; k++) { g_test_c32[k] = strtof(s, &end); if (end == s) break; s = end; }
            g_test = k == 4;
            char t[16];
            g_test_render_only = g_test && GetEnvironmentVariableA("PHOTON_TEST_C32_THREAD", t, sizeof t) > 0 &&
                                 strcmp(t, "render") == 0;
            if (g_test_render_only)
                photon_log("d3dconst: PHOTON_TEST_C32_THREAD=render: c32 written only from threads other than the main"
                           " thread %lu (queued mode: the MatQueue thread)", g_main_tid);
            photon_log("d3dconst: PHOTON_TEST_C32=\"%s\" -> %s (%.4f %.4f %.4f %.4f)", e,
                       g_test ? "test mode on: c32 written at every RenderPass" : "unparsable, test mode off",
                       g_test_c32[0], g_test_c32[1], g_test_c32[2], g_test_c32[3]);
        }
    }
    return g_test;
}

static volatile LONG64 g_rp, g_rp_written;
/* RenderPass (slot 125, FUN_18001a9d0): only `this` is used by stock; rdx/r8/r9 are forwarded unchanged anyway. */
static void hook_renderpass(void *self, uintptr_t a, uintptr_t b, uintptr_t c) {
    PhotonWrap *w = photon_wrap_of(self);
    InterlockedIncrement64((volatile LONG64 *)&w->cls->counters[SLOT_API_RENDERPASS]);   /* keep frames.csv counts */
    InterlockedIncrement64(&g_rp);
    if ((!g_test_render_only || GetCurrentThreadId() != g_main_tid) && photon_d3d_set_ps_const(32, g_test_c32, 1) == S_OK)
        InterlockedIncrement64(&g_rp_written);
    ((void (*)(void *, uintptr_t, uintptr_t, uintptr_t))w->cls->real_vtbl[SLOT_API_RENDERPASS])(w->real, a, b, c);
}

void photon_d3d_install(PhotonWrapClass *cls) {
    if (strcmp(cls->name, "ShaderApi030") != 0 || cls->nslots <= SLOT_API_RENDERPASS) return;
    g_api_cls = cls;
    if (g_base) {
        uintptr_t s31 = (uintptr_t)cls->real_vtbl[SLOT_API_SETPSCONST];
        if (s31 != g_base + RVA_SETPSCONST) {
            photon_log("d3dconst: ShaderApi030 slot 31 = stock+0x%llx, expected +0x%x: API disabled",
                       (unsigned long long)(s31 - g_base), RVA_SETPSCONST);
            g_base = 0;
        } else photon_log("d3dconst: ShaderApi030 slot 31 = stock+0x%x (SetPixelShaderConstant) as expected; API ready"
                          " (c%d..c%d)", RVA_SETPSCONST, PHOTON_D3D_PS_FIRST, PHOTON_D3D_PS_END - 1);
    }
    if (test_mode()) {
        photon_wrap_set_hook(cls, SLOT_API_RENDERPASS, (void *)hook_renderpass);
        photon_log("d3dconst: TEST ShaderApi030 slot 125 (RenderPass) hooked: c32 = (%.4f %.4f %.4f %.4f)%s",
                   g_test_c32[0], g_test_c32[1], g_test_c32[2], g_test_c32[3], g_base ? "" : " but the API is disabled");
    }
}

void photon_d3d_dump(const char *why) {
    if (g_test <= 0 && !g_calls) return;
    photon_log("d3dconst summary (%s, frame %llu): calls %lld ok %lld fail %lld no-device %lld; RenderPass hooked %lld,"
               " c32 written %lld; device %p; last caller tid %ld", why, (unsigned long long)photon_frame_count(),
               (long long)g_calls, (long long)g_ok, (long long)g_fail, (long long)g_nodev, (long long)g_rp,
               (long long)g_rp_written, g_good_dev, (long)g_last_tid);
}

void photon_d3d_frame(void) {
    if (g_test > 0 && photon_frame_count() % 300 == 0) photon_d3d_dump("periodic");
}
