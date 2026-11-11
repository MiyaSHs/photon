/* Mock host for the proxy shaderapidx9.dll (runs under Wine; see scripts/run_proxyhost.sh).
 * argv[1] = directory holding the proxy as shaderapidx9.dll, the stock DLL as shaderapidx9_stock.dll and the
 * other game DLLs. Requests the interfaces in CMaterialSystem::Connect order (FEASIBILITY.md section 8.1) through
 * the proxy and checks each result against what the stock DLL returns directly. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../src/proxy_shaderapi/photon_stats.h"

typedef void *(*CreateInterfaceFn)(const char *, int *);
int vlg_tests(PhotonStats *st, HMODULE hs, HMODULE hp, void *api_w, void *api_r, void *sh_w, void *sh_r, void *hw_r);


/* --- T1.3 fakes: a stand-in for materialsystem's IShaderUtil and its factory -------------------------------- */
static void *g_restore_factory_seen;   /* factory argument RestoreShaderObjects received */
static void *g_restore_api_result;     /* what that factory returned for "ShaderApi030" */
static int g_util_requested;
static long long fake_stub(void *self) { (void)self; return 0; }
static void *fake_config(void *self) { static char config[256]; (void)self; return config; }
static void fake_restore(void *self, void *(*factory)(const char *, int *), int flags) {
    (void)self; (void)flags;
    g_restore_factory_seen = (void *)factory;
    g_restore_api_result = factory ? factory("ShaderApi030", NULL) : NULL;
}
/* 40 slots like IShaderUtil (9-slot IAppSystem + 31), then a NULL so the proxy measures exactly 40. */
static void *fake_util_vtbl[41];
static struct { void **vtbl; } fake_util = { fake_util_vtbl };
void *g_fake_fs;   /* vlgtest.c, PHOTON_TEST_AMB: the fake engine's filesystem */
static void *fake_ms_factory(const char *name, int *ret) {
    if (name && g_fake_fs && strcmp(name, "VFileSystem022") == 0) { if (ret) *ret = 0; return g_fake_fs; }
    if (name && strcmp(name, "VShaderUtil001") == 0) { g_util_requested++; if (ret) *ret = 0; return &fake_util; }
    if (ret) *ret = 1;
    return NULL;
}

/* P0 meter test helpers */
static volatile unsigned g_spin_sink;
static void spin_ms(DWORD ms) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    do { for (int i = 0; i < 10000; i++) g_spin_sink += i; QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000 < (LONGLONG)ms * f.QuadPart);
}
static DWORD WINAPI mq_thread(LPVOID p) {
    PhotonStats *st = p;
    st->frame_end();   /* frame 4: Present on a second thread -> it becomes the render (MatQueue) thread */
    spin_ms(1600);
    st->frame_end();   /* frame 5: render thread busy, main waiting */
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: proxyhost <bindir>\n"); return 2; }
    int fails = 0;
    char path[MAX_PATH];
    SetDllDirectoryA(argv[1]);
    snprintf(path, sizeof path, "%s\\shaderapidx9.dll", argv[1]);
    HMODULE hp = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hp) { printf("FAIL load proxy (%lu)\n", GetLastError()); return 1; }
    CreateInterfaceFn pci = (CreateInterfaceFn)(void *)GetProcAddress(hp, "CreateInterface");
    if (!pci || GetProcAddress(hp, "BuiltDebug")) { printf("FAIL proxy exports\n"); return 1; }

    static const char *names[] = { "ShaderDeviceMgr001", "MaterialSystemHardwareConfig012", "ShaderApi030",
                                   "ShaderDevice001", "ShaderShadow010", "DebugTextureInfo001", "VBAllocTracker001" };
    void *via_proxy[7];
    for (int i = 0; i < 7; i++) {
        int rc = -1;
        via_proxy[i] = pci(names[i], &rc);
        printf("proxy %-32s -> %p rc=%d\n", names[i], via_proxy[i], rc);
        if (!via_proxy[i] || rc != 0) fails++;
    }
    /* Now that the proxy has loaded the stock DLL, ask the stock DLL directly and compare. */
    HMODULE hs = GetModuleHandleA("shaderapidx9_stock.dll");
    CreateInterfaceFn sci = hs ? (CreateInterfaceFn)(void *)GetProcAddress(hs, "CreateInterface") : NULL;
    if (!sci) { printf("FAIL stock not loaded by proxy\n"); return 1; }
    if (hs == hp) { printf("FAIL proxy and stock are the same module\n"); return 1; }
    PhotonStats *st = (PhotonStats *)pci(PHOTON_STATS_VERSION, NULL);
    if (!st || st->version != 3) { printf("FAIL stats interface\n"); return 1; }
    printf("stats: passthrough=%d classes=%zu\n", st->passthrough, st->nclasses);
    void *direct[7];
    for (int i = 0; i < 7; i++) {
        direct[i] = sci(names[i], NULL);
        void *real = st->unwrap(via_proxy[i]);
        int wrapped = real != via_proxy[i];
        printf("  %-32s %s\n", names[i], wrapped ? "wrapped" : "raw");
        if (real != direct[i]) { printf("FAIL %s: unwrap %p != stock %p\n", names[i], real, direct[i]); fails++; }
        if (via_proxy[i] != pci(names[i], NULL)) { printf("FAIL %s: second request gave a different wrapper\n", names[i]); fails++; }
    }
    /* Calls through the ShaderApi030 wrapper must equal direct field reads (trivial getters on build 26229091):
     * slot 4 = mov eax,[rcx+0x3640]; slot 257 = movss xmm0,[rcx+0x1344] (GetFogMaxDensity, a float return). */
    if (!st->passthrough) {
        void *w = via_proxy[2], *r = direct[2];
        PhotonWrapClass *c = NULL;
        for (size_t i = 0; i < st->nclasses; i++) if (strcmp(st->classes[i]->name, "ShaderApi030") == 0) c = st->classes[i];
        if (!c || c->nslots != 289) { printf("FAIL ShaderApi030 class missing or nslots=%zu\n", c ? c->nslots : 0); return 1; }
        *(float *)((char *)r + 0x1344) = 0.625f;          /* give the float a recognisable value */
        unsigned long long c4 = c->counters[4], c257 = c->counters[257];
        int v4 = ((int (*)(void *))(*(void ***)w)[4])(w);
        float f = ((float (*)(void *))(*(void ***)w)[257])(w);
        printf("ShaderApi030 via wrapper: slot4=%d (field %d) slot257=%.3f (field %.3f) counters %llu->%llu, %llu->%llu\n",
               v4, *(int *)((char *)r + 0x3640), f, *(float *)((char *)r + 0x1344),
               c4, (unsigned long long)c->counters[4], c257, (unsigned long long)c->counters[257]);
        if (v4 != *(int *)((char *)r + 0x3640) || f != 0.625f) fails++;
        if (c->counters[4] != c4 + 1 || c->counters[257] != c257 + 1) { printf("FAIL counters\n"); fails++; }
        /* --- T1.3: re-entry factories --- */
        for (int i = 0; i < 40; i++) fake_util_vtbl[i] = (void *)fake_stub;
        fake_util_vtbl[16] = (void *)fake_restore;           /* IShaderUtil::RestoreShaderObjects */
        fake_util_vtbl[9] = (void *)fake_config;             /* GetConfig: stock's HW slot 54 reads the DX level limit from it */
        void *mgr = via_proxy[0];
        void **mvt = *(void ***)mgr;
        HMODULE owner = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)mvt[16], &owner);
        if (owner != hp) { printf("FAIL SetMode slot 16 not hooked by the proxy\n"); fails++; }
        /* Connect (slot 0) with our fake materialsystem factory. The stock Connect stores g_pShaderUtil
         * (stock+0xa6ad8) and then fails (no filesystem in this host); that is expected here. */
        unsigned char ok = ((unsigned char (*)(void *, void *))mvt[0])(mgr, (void *)fake_ms_factory);
        void *stored_util = *(void **)((char *)hs + 0xa6ad8);
        printf("Connect -> %d; VShaderUtil001 requested %d time(s); stock g_pShaderUtil=%p (unwrap %p, fake %p)\n",
               ok, g_util_requested, stored_util, st->unwrap(stored_util), (void *)&fake_util);
        if (g_util_requested < 1 || stored_util == (void *)&fake_util || st->unwrap(stored_util) != (void *)&fake_util) {
            printf("FAIL stock g_pShaderUtil is not our wrapper\n"); fails++;
        }
        /* Simulate the state after a real SetMode: the internal factory FUN_180023f80 returns the globals
         * DAT_1800a6ae0 (ShaderDevice001), DAT_1800a6af0 (ShaderApi030), DAT_1800a6af8 (ShaderShadow010), which
         * are NULL until device init. Seed them with the real objects. */
        *(void **)((char *)hs + 0xa6ae0) = direct[3];
        *(void **)((char *)hs + 0xa6af0) = direct[2];
        *(void **)((char *)hs + 0xa6af8) = direct[4];
        /* Replay ReacquireResources: g_pShaderUtil->RestoreShaderObjects(internal factory FUN_180023f80, 0). */
        void *inner = (char *)hs + 0x23f80;
        ((void (*)(void *, void *, int))(*(void ***)stored_util)[16])(stored_util, inner, 0);
        printf("RestoreShaderObjects: factory seen %p (inner %p); factory(\"ShaderApi030\") -> %p (proxy wrapper %p)\n",
               g_restore_factory_seen, inner, g_restore_api_result, via_proxy[2]);
        if (g_restore_factory_seen == inner) { printf("FAIL internal factory leaked unwrapped\n"); fails++; }
        if (g_restore_api_result != via_proxy[2]) { printf("FAIL re-entry factory did not return our wrapper\n"); fails++; }
        /* QueryInterface (slot 2) on the manager: FUN_1800238e0 returns `this` (the REAL object) for
         * "ShaderDeviceMgr001", so the hook must map it back to our wrapper; HWConfig stays raw. */
        void *qi = ((void *(*)(void *, const char *))mvt[2])(mgr, "ShaderDeviceMgr001");
        void *qh = ((void *(*)(void *, const char *))mvt[2])(mgr, "MaterialSystemHardwareConfig012");
        printf("IShaderDeviceMgr::QueryInterface: ShaderDeviceMgr001 -> %p (wrapper %p); HWConfig -> %p (stock %p)\n",
               qi, mgr, qh, direct[1]);
        if (qi != mgr) { printf("FAIL QueryInterface leaked the raw manager\n"); fails++; }
        if (qh != via_proxy[1] || st->unwrap(qh) != direct[1]) { printf("FAIL QueryInterface HWConfig is not our wrapper\n"); fails++; }
        /* --- T3.2a: HWConfig wrapped (78 slots); slot 73 names photon_shaders.dll iff it sits next to the proxy;
         * other slots still forward (slot 24 MaxNumLights = field +0x54c, slot 34 returns false). --- */
        void *hw = via_proxy[1]; void **hvt = *(void ***)hw;
        int n24 = ((int (*)(void *))hvt[24])(hw);
        unsigned char b34 = ((unsigned char (*)(void *))hvt[34])(hw);
        const char *dllname = ((const char *(*)(void *))hvt[73])(hw);
        char sdpath[MAX_PATH];
        snprintf(sdpath, sizeof sdpath, "%s\\photon_shaders.dll", argv[1]);
        int present = GetFileAttributesA(sdpath) != INVALID_FILE_ATTRIBUTES;
        printf("HWConfig via wrapper: slot24=%d (field %d) slot34=%u slot73=%s (photon_shaders.dll present=%d)\n", n24,
               *(int *)((char *)direct[1] + 0x54c), b34, dllname ? dllname : "(null)", present);
        if (n24 != *(int *)((char *)direct[1] + 0x54c) || b34 != 0) fails++;
        if (present ? !(dllname && strcmp(dllname, "photon_shaders.dll") == 0) : dllname != NULL) { printf("FAIL slot 73\n"); fails++; }
        /* --- T1.4: Present hook + per-frame accounting --- */
        void **dvt = *(void ***)via_proxy[3];
        owner = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)dvt[9], &owner);
        if (owner != hp) { printf("FAIL Present (ShaderDevice001 slot 9) not hooked\n"); fails++; }
        st->frame_end();                                   /* frame 1: everything so far */
        for (int k = 0; k < 7; k++) ((float (*)(void *))(*(void ***)w)[257])(w);
        for (int k = 0; k < 3; k++) ((int (*)(void *))(*(void ***)w)[4])(w);
        st->frame_end();                                   /* frame 2: exactly 10 ShaderApi030 calls */
        st->flush();
        printf("frames=%llu (expect 2; CSV checked by run_proxyhost.sh)\n", st->frames());
        if (st->frames() != 2) fails++;
        for (size_t i = 0; i < st->nclasses; i++) printf("  class %-20s nslots=%zu\n", st->classes[i]->name, st->classes[i]->nslots);
        /* --- T3.6 prerequisites: PHOTON_DIAG=1 hooks (src/proxy_shaderapi/diag.h); run_proxyhost.sh checks the log --- */
        char de[8];
        int diag = GetEnvironmentVariableA("PHOTON_DIAG", de, sizeof de) > 0 && de[0] == '1';
        void **avt = *(void ***)w;
        owner = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)avt[62], &owner);
        printf("diag=%d: ShaderApi030 slot 62 is %s\n", diag, owner == hp ? "hooked by the proxy" : "a plain thunk");
        if ((owner == hp) != diag) { printf("FAIL slot 62 hook state does not follow PHOTON_DIAG\n"); fails++; }
        if (diag) {
            typedef void (*SetIntFn)(void *, int, int);
            typedef int (*GetIntFn)(void *, int);
            ((SetIntFn)avt[62])(w, 13, 4242);
            int g13 = ((GetIntFn)avt[65])(w, 13), raw13 = *(int *)((char *)r + 0x3930 + 13 * 4);
            ((SetIntFn)avt[62])(w, 25, 1);                      /* out of range: stock ignores it, histogram counts it */
            /* order window; the origin equals the stored one, so the stock setter takes its no-change path */
            float o[3]; memcpy(o, (char *)r + 0x1300, sizeof o);
            ((SetIntFn)avt[62])(w, 0, 2);
            ((void (*)(void *, const float *))avt[128])(w, o);
            ((SetIntFn)avt[62])(w, 0, 0);
            st->diag_frame();                                    /* first audit: syncs the shadow from raw */
            *(int *)((char *)r + 0x3930 + 14 * 4) = 99;          /* a writer that bypasses the wrapper */
            st->diag_frame();                                    /* second audit: must report parm 14 */
            st->diag_dump("proxyhost");
            printf("diag: set(13,4242) -> get %d raw %d\n", g13, raw13);
            if (g13 != 4242 || raw13 != 4242) { printf("FAIL diag set/get forwarding\n"); fails++; }
        }
        /* --- T3.4a2: direct c32+ API (src/proxy_shaderapi/d3dconst.h). No device in this host: the global
         * g_pD3DDevice (stock+0xa6b18) is NULL, so every call must fail cleanly; a fake object whose vtable is not in
         * d3d9.dll must be rejected by validation and never called. RenderPass is hooked only with PHOTON_TEST_C32. */
        static const float cv[8] = { 0.2f, 0.6f, 1.0f, 1.0f, 0, 0, 0, 0 };
        void **gdev = (void **)((char *)hs + 0xa6b18);
        void *saved = *gdev;
        long e_null = st->d3d_set_ps_const(32, cv, 1), e_lo = st->d3d_set_ps_const(31, cv, 1),
             e_hi = st->d3d_set_ps_const(223, cv, 2), e_v = st->d3d_set_ps_const(32, NULL, 1);
        static int fake_called;
        static void *fake_vt[120];
        for (int i = 0; i < 120; i++) fake_vt[i] = (void *)fake_stub;
        static struct { void **vtbl; } fake_dev = { fake_vt };
        *gdev = &fake_dev;
        void *got = st->d3d_device();
        long e_fake = st->d3d_set_ps_const(32, cv, 1);
        *gdev = saved;
        owner = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)avt[125], &owner);
        char te[64];
        int c32test = GetEnvironmentVariableA("PHOTON_TEST_C32", te, sizeof te) > 0;
        printf("d3dconst: no device -> 0x%lx, c31 -> 0x%lx, c223+2 -> 0x%lx, NULL v -> 0x%lx; fake device -> %p, set -> "
               "0x%lx (fake called %d); RenderPass slot 125 %s (PHOTON_TEST_C32 %s)\n", (unsigned long)e_null,
               (unsigned long)e_lo, (unsigned long)e_hi, (unsigned long)e_v, got, (unsigned long)e_fake, fake_called,
               owner == hp ? "hooked" : "a plain thunk", c32test ? "set" : "unset");
        if (e_null != (long)E_POINTER || e_lo != (long)E_INVALIDARG || e_hi != (long)E_INVALIDARG ||
            e_v != (long)E_INVALIDARG || got != NULL || e_fake != (long)E_POINTER || fake_called) {
            printf("FAIL d3dconst API guards\n"); fails++;
        }
        /* T3.4c: the lighting module also hooks RenderPass when it is on (never together with PHOTON_TEST_C32) */
        if ((owner == hp) != (c32test || st->vlg->api_hooked)) {
            printf("FAIL RenderPass hook state does not follow PHOTON_TEST_C32 / lighting\n"); fails++;
        }
        /* --- T3.4c: lighting v0 (tests/proxyhost/vlgtest.c), only in the run_proxyhost.sh VLG runs --- */
        char ve[16];
        if (GetEnvironmentVariableA("PHOTON_TEST_VLG", ve, sizeof ve) > 0) fails += vlg_tests(st, hs, hp, w, r, via_proxy[4],
                                                                                              direct[4], direct[1]);
        else if (st->vlg->wanted || st->vlg->shadow_hooked || st->vlg->api_hooked) {
            printf("FAIL lighting is on in the base run (%s)\n", st->vlg->reason); fails++;
        } else printf("vlg: %s\n", st->vlg->reason);
        /* --- P0 CPU meters (frames.h), PHOTON_TEST_METERS=1 in the base run: frame 3 after 1.6 s of spinning on
         * the main thread (the CreateInterface caller), frames 4/5 presented from a second thread that spins 1.6 s
         * between them (queued mode). run_proxyhost.sh checks main_cpu_pct / mq_cpu_pct / queued_mode. --- */
        char me[8];
        if (GetEnvironmentVariableA("PHOTON_TEST_METERS", me, sizeof me) > 0 && me[0] == '1' &&
            GetEnvironmentVariableA("PHOTON_TEST_VLG", ve, sizeof ve) == 0) {
            spin_ms(1600);
            st->frame_end();                                   /* frame 3: sync, main busy */
            HANDLE th = CreateThread(NULL, 0, mq_thread, st, 0, NULL);
            if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); } else fails++;
            st->flush();
            printf("meters: frames=%llu (expect 5)\n", st->frames());
            if (st->frames() != 5) fails++;
        }
    }
    int rc = -1;
    if (pci("NoSuchInterface999", &rc) != NULL || rc != 1) { printf("FAIL unknown name not forwarded as failure\n"); fails++; }
    /* --- S1: unload while the meter thread runs (the in-game hang on quit: the game FreeLibrary's the proxy after
     * materialsystem shut it down; a meter thread still inside the unmapped DLL faulted). PHOTON_TEST_UNLOAD=shutdown:
     * IShaderDeviceMgr::Shutdown (slot 4) through the wrapper first, as materialsystem does at quit, then FreeLibrary:
     * the proxy must be unmapped and the process must survive 1.5 s (3 meter periods). =free: FreeLibrary without
     * Shutdown: the meter thread's own module reference must keep the proxy mapped; process exit then runs its DllMain
     * with reserved != NULL (no work). Last, since nothing of the proxy may be called after it. --- */
    char ue[16];
    if (GetEnvironmentVariableA("PHOTON_TEST_UNLOAD", ue, sizeof ue) > 0 && !st->passthrough) {
        int sd = strcmp(ue, "shutdown") == 0;
        void *mgr = via_proxy[0];
        if (sd) ((void (*)(void *))(*(void ***)mgr)[4])(mgr);
        BOOL fl = FreeLibrary(hp);
        HMODULE after = GetModuleHandleA(path);
        Sleep(1500);
        HMODULE later = GetModuleHandleA(path);
        printf("unload (%s): FreeLibrary %d; proxy %s right after, %s 1.5 s later; no fault\n", ue, fl,
               after ? "mapped" : "unmapped", later ? "mapped" : "unmapped");
        if (!fl || (sd ? (after || later) : (!after || !later))) { printf("FAIL unload (%s)\n", ue); fails++; }
        else printf("UNLOAD OK (%s)\n", ue);
    }
    printf(fails ? "PROXYHOST FAIL (%d)\n" : "PROXYHOST OK\n", fails);
    return fails ? 1 : 0;
}
