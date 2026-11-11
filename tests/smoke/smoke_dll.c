/* T0.2 smoke DLL: an interface object with an explicit C vtable, returned by CreateInterface.
 * Mirrors how a game interface is exposed (Source tier1 CreateInterfaceFn). */
#include <string.h>

typedef struct Vec3 { float x, y, z; } Vec3; /* 12-byte UDT: tests hidden-return-pointer handling */

typedef struct ISmoke ISmoke;
typedef struct ISmokeVtbl {
    int   (*GetValue)(ISmoke *self);
    /* An MSVC member function returning a UDT: `this` stays in rcx, the hidden return pointer is in rdx,
     * and the return pointer comes back in rax. In C that is exactly this signature. */
    Vec3 *(*GetVec)(ISmoke *self, Vec3 *ret, float scale);
} ISmokeVtbl;
struct ISmoke { const ISmokeVtbl *vtbl; int value; };

static int smoke_GetValue(ISmoke *self) { return self->value; }
static Vec3 *smoke_GetVec(ISmoke *self, Vec3 *ret, float scale) {
    ret->x = 1.0f * scale; ret->y = 2.0f * scale; ret->z = (float)self->value * scale;
    return ret;
}
static const ISmokeVtbl g_vtbl = { smoke_GetValue, smoke_GetVec };
static ISmoke g_smoke = { &g_vtbl, 42 };

__declspec(dllexport) void *CreateInterface(const char *name, int *ret) {
    if (name && strcmp(name, "PhotonSmoke001") == 0) { if (ret) *ret = 0; return &g_smoke; }
    if (ret) *ret = 1;
    return 0;
}
