/* d3d9tri: minimal D3D9 render + readback check (T0.3 test bed).
 *
 * Creates a 256x256 window and device, clears to CLEAR_RGB, draws one
 * XYZRHW|DIFFUSE triangle in TRI_RGB, then copies the back buffer to a
 * system-memory surface (GetRenderTargetData), locks it and checks the
 * pixel at a corner (clear colour) and at the triangle centroid.
 * It reads back both before and after Present (D3DSWAPEFFECT_COPY keeps the
 * back buffer), and both must match.
 * Prints "D3D9TRI OK ..." and exits 0, or "D3D9TRI FAIL ..." and exits 1.
 * Run it under DXVK via scripts/run_d3d9tri.sh.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define W 256
#define H 256
#define CLEAR_RGB 0x2040C0u /* R=0x20 G=0x40 B=0xC0 */
#define TRI_RGB   0xE08010u /* R=0xE0 G=0x80 B=0x10 */

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;
#define VTX_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static const Vtx tri[3] = {
    { 64.0f, 192.0f, 0.5f, 1.0f, 0xFF000000u | TRI_RGB},
    {128.0f,  64.0f, 0.5f, 1.0f, 0xFF000000u | TRI_RGB},
    {192.0f, 192.0f, 0.5f, 1.0f, 0xFF000000u | TRI_RGB},
};
/* Centroid = (128, 149.33); corner = (2, 2). */
#define CX 128
#define CY 149
#define KX 2
#define KY 2

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

#define CHECK(expr, what) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    printf("D3D9TRI FAIL %s hr=0x%08lx\n", what, (unsigned long)hr_); return 1; } } while (0)

/* Copy the render target to sysmem, return the two probe pixels (RGB only). */
static int readback(IDirect3DDevice9 *dev, IDirect3DSurface9 *sys, DWORD *corner, DWORD *centre)
{
    IDirect3DSurface9 *bb = NULL;
    D3DLOCKED_RECT lr;
    CHECK(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb), "GetBackBuffer");
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
    IDirect3DSurface9_Release(bb);
    CHECK(hr, "GetRenderTargetData");
    CHECK(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY), "LockRect");
    const BYTE *p = (const BYTE *)lr.pBits;
    *corner = *(const DWORD *)(p + KY * lr.Pitch + KX * 4) & 0xFFFFFFu;
    *centre = *(const DWORD *)(p + CY * lr.Pitch + CX * 4) & 0xFFFFFFu;
    IDirect3DSurface9_UnlockRect(sys);
    return 0;
}

int main(void)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = "d3d9tri";
    RegisterClassA(&wc);
    RECT rc = {0, 0, W, H};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("d3d9tri", "d3d9tri", WS_OVERLAPPEDWINDOW, 0, 0,
                              rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!hwnd) { printf("D3D9TRI FAIL CreateWindow\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    pump();

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("D3D9TRI FAIL Direct3DCreate9\n"); return 1; }

    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id)))
        printf("adapter: %s (driver %s, vendor 0x%04lx device 0x%04lx)\n", id.Description,
               id.Driver, (unsigned long)id.VendorId, (unsigned long)id.DeviceId);

    D3DCAPS9 caps;
    DWORD vp = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    if (SUCCEEDED(IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps)) &&
        (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT))
        vp = D3DCREATE_HARDWARE_VERTEXPROCESSING;

    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, vp, &pp, &dev);
    if (FAILED(hr) && vp == D3DCREATE_HARDWARE_VERTEXPROCESSING) {
        vp = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, vp, &pp, &dev);
    }
    CHECK(hr, "CreateDevice");
    printf("device: HAL, %s vertex processing\n",
           vp == D3DCREATE_HARDWARE_VERTEXPROCESSING ? "hardware" : "software");

    IDirect3DSurface9 *sys = NULL;
    CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &sys, NULL),
          "CreateOffscreenPlainSurface");

    CHECK(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000u | CLEAR_RGB, 1.0f, 0), "Clear");
    CHECK(IDirect3DDevice9_BeginScene(dev), "BeginScene");
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    CHECK(IDirect3DDevice9_SetFVF(dev, VTX_FVF), "SetFVF");
    CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(Vtx)), "DrawPrimitiveUP");
    CHECK(IDirect3DDevice9_EndScene(dev), "EndScene");

    DWORD k0, c0, k1, c1;
    if (readback(dev, sys, &k0, &c0)) return 1;
    CHECK(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL), "Present");
    pump();
    if (readback(dev, sys, &k1, &c1)) return 1;

    int ok = k0 == CLEAR_RGB && c0 == TRI_RGB && k1 == CLEAR_RGB && c1 == TRI_RGB;
    printf("D3D9TRI %s corner(%d,%d)=0x%06lx (want 0x%06x) centroid(%d,%d)=0x%06lx (want 0x%06x)"
           " after-present corner=0x%06lx centroid=0x%06lx\n",
           ok ? "OK" : "FAIL", KX, KY, (unsigned long)k0, CLEAR_RGB, CX, CY, (unsigned long)c0,
           TRI_RGB, (unsigned long)k1, (unsigned long)c1);

    IDirect3DSurface9_Release(sys);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
    return ok ? 0 : 1;
}
