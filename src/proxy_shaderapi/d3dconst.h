/* T3.4a2: pixel-shader constants c32+ written straight to stock shaderapidx9's IDirect3DDevice9.
 *
 * Why: shaderapi's PS constant state holds exactly 32 registers (CHardwareConfig +0x530 = 32; the two shadow arrays
 * are 16*32 bytes) and its SetPixelShaderConstant (ShaderApi030 slot 31, 0x18001ef50) has no bounds check, so any
 * write at c32+ through IShaderDynamicAPI corrupts the heap (analysis/13 R15). Stock never touches c32+ (CTAB max
 * c31, reset path uploads c0..c31 only), so those registers belong to us alone.
 *
 * Device pointer: choice (a) of T3.4a2 (option (b), hooking Direct3DCreate9(Ex) and CreateDevice(Ex), was not
 * needed): stock's own device global g_pD3DDevice = [stock+0xa6b18], which CShaderDeviceDx8 sets right after IDirect3D9(Ex)::CreateDevice (FUN_180025780) and clears on release
 * (FUN_180027900). Every stock D3D call loads it the same way, e.g. SetPixelShaderConstant at 0x18001efb7:
 *   48 8b 0d <disp32 -> 0x1800a6b18>   mov rcx, [g_pD3DDevice]
 *   41 ff 92 68 03 00 00               call [r10 + 0x368]   (IDirect3DDevice9::SetPixelShaderConstantF, slot 109)
 * Guards: (1) the T1.5 build guard (stock SHA1 + materialsystem stamp) and wrapping on; (2) those exact instruction
 * bytes at stock+0x1efb7 / +0x1efc9, the displacement resolving to stock+0xa6b18; (3) ShaderApi030's real slot 31 =
 * stock+0x1ef50 (checked when the class is created); (4) per device pointer: readable memory, its vtable inside
 * d3d9.dll's image, vtable slot 109 inside d3d9.dll's .text, and QueryInterface(IID_IDirect3DDevice9) returning the
 * same object. Any failed guard disables the API (calls return an error, nothing is written) with one log line.
 * The slot offset 0x368 comes from the mingw d3d9.h (offsetof(IDirect3DDevice9Vtbl, SetPixelShaderConstantF), a
 * static assert) and matches the stock call site above.
 *
 * THREADING: call only from the thread that issues stock's draws, i.e. from inside a ShaderApi030 hook that stock
 * itself calls there (RenderPass, slot 125, is the intended place). In queued mode (mat_queue_mode 2) that is the
 * MatQueue render thread; in sync mode it is the main thread. Calling from the main thread in queued mode races
 * with stock's own device calls and writes out of order with the queued draws. The proxy logs the calling thread
 * whenever it changes (bounded), so a run log shows where the writes came from.
 *
 * Values written here persist on the device until overwritten or until a device reset; callers re-set them per draw.
 *
 * Test mode: PHOTON_TEST_C32="r g b a" hooks RenderPass (slot 125) and writes c32 = (r,g,b,a) at the entry of every
 * RenderPass, before stock binds the snapshot and draws (Photon_Debug32 then outputs it). Unset: no hook at all.
 * PHOTON_TEST_C32_THREAD=render additionally restricts the writes to threads other than the main thread (the one that
 * loaded the stock DLL). In queued mode the readback (render.CapturePixels) forces a sync frame on the main thread,
 * which then writes nothing, so a correct pixel there proves that the render thread's writes reached the device
 * (the value persists); in sync mode nothing is written at all (a negative control). */
#ifndef PHOTON_D3DCONST_H
#define PHOTON_D3DCONST_H
#include <windows.h>
#include "wrap.h"

enum { PHOTON_D3D_PS_FIRST = 32,      /* first register we may write: shaderapi owns c0..c31 */
       PHOTON_D3D_PS_END = 224 };     /* one past the last ps_3_0 float constant (D3D9 SM3, DXVK's limit) */

/* From load_stock(): the loaded stock module and whether wrapping is on with a matching build. */
void photon_d3d_setup(HMODULE stock, int build_ok_and_wrapping);
/* From install_hooks(): validates ShaderApi030's slot 31 and installs the PHOTON_TEST_C32 RenderPass hook. */
void photon_d3d_install(PhotonWrapClass *cls);
/* 1 if the API is usable once a device exists (guards (1)-(3) passed); the lighting module (vlg.h) requires it. */
int photon_d3d_api_ready(void);
/* Stock's IDirect3DDevice9*, validated; NULL if unavailable (no device yet, a guard failed, pass-through). */
void *photon_d3d_device(void);
/* IDirect3DDevice9::SetPixelShaderConstantF(start, v, count) on stock's device. Requires start >= 32 and
 * start + count <= 224. Returns the HRESULT (S_OK = 0); E_INVALIDARG for a bad range, E_POINTER without a device.
 * Render thread only (see above). */
long photon_d3d_set_ps_const(int start, const float *v, int count);
/* Per-frame hook (from Present): periodic summary in test mode. */
void photon_d3d_frame(void);
void photon_d3d_dump(const char *why);
#endif
