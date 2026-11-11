/* T0.2 smoke test (runs under Wine).
 * 1) Load our mingw DLL and call through its C vtable.
 * 2) Optional ABI interop: argv[1] = directory with the stock GMod bin/win64 DLLs.
 *    Load stock shaderapidx9.dll (MSVC), get MaterialSystemHardwareConfig012 and call a getter through
 *    its vtable, which checks that mingw-compiled callers interoperate with MSVC-compiled callees. */
#include <windows.h>
#include <stdio.h>

typedef void *(*CreateInterfaceFn)(const char *, int *);
typedef struct Vec3 { float x, y, z; } Vec3;
typedef struct ISmoke { const struct { int (*GetValue)(void *); Vec3 *(*GetVec)(void *, Vec3 *, float); } *vtbl; } ISmoke;

int main(int argc, char **argv) {
    int fails = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE h = LoadLibraryA("smoke_dll.dll");
    if (!h) { printf("FAIL load smoke_dll (%lu)\n", GetLastError()); return 1; }
    CreateInterfaceFn ci = (CreateInterfaceFn)(void *)GetProcAddress(h, "CreateInterface");
    int rc = -1;
    ISmoke *s = ci ? (ISmoke *)ci("PhotonSmoke001", &rc) : NULL;
    if (!s || rc != 0) { printf("FAIL CreateInterface\n"); return 1; }
    int v = s->vtbl->GetValue(s);
    Vec3 out = {0}, *p = s->vtbl->GetVec(s, &out, 2.0f);
    printf("smoke GetValue=%d GetVec=(%.1f,%.1f,%.1f) retptr_ok=%d\n", v, out.x, out.y, out.z, p == &out);
    if (v != 42 || p != &out || out.z != 84.0f) fails++;
    if (ci("Nope000", &rc) != NULL || rc != 1) { printf("FAIL unknown name\n"); fails++; }

    if (argc > 1) {
        SetDllDirectoryA(argv[1]);
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\shaderapidx9.dll", argv[1]);
        HMODULE hs = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hs) { printf("FAIL load stock shaderapidx9 (%lu)\n", GetLastError()); return 1; }
        CreateInterfaceFn sci = (CreateInterfaceFn)(void *)GetProcAddress(hs, "CreateInterface");
        static const char *names[] = { "ShaderDeviceMgr001", "MaterialSystemHardwareConfig012",
                                       "ShaderApi030", "ShaderDevice001", "ShaderShadow010" };
        void *objs[5];
        for (int i = 0; i < 5; i++) {
            rc = -1; objs[i] = sci(names[i], &rc);
            printf("stock %-32s -> %p rc=%d vtbl=%p\n", names[i], objs[i], rc, objs[i] ? *(void **)objs[i] : NULL);
            if (!objs[i]) fails++;
        }
        /* CHardwareConfig (MaterialSystemHardwareConfig012) slot 24 = MaxNumLights is `mov eax,[rcx+0x54c]; ret`
         * and slot 34 is `xor al,al; ret` (bytes dumped from vtable 0x1800788c0). Calling them through the MSVC
         * vtable must return exactly the field value / false, which checks this-in-rcx interop without device init.
         * (Slot 32 GetDXSupportLevel dereferences config state that is NULL before init, so it would crash.) */
        if (objs[1]) {
            void **vt = *(void ***)objs[1];
            int n = ((int (*)(void *))vt[24])(objs[1]);
            int field = *(int *)((char *)objs[1] + 0x54c);
            unsigned char b = ((unsigned char (*)(void *))vt[34])(objs[1]);
            printf("stock HWConfig slot24=%d field+0x54c=%d slot34=%u\n", n, field, b);
            if (n != field || b != 0) fails++;
        }
    }
    printf(fails ? "SMOKE FAIL (%d)\n" : "SMOKE OK\n", fails);
    return fails ? 1 : 0;
}
