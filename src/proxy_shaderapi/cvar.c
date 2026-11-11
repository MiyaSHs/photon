/* The engine's console variables from the proxy (NANITE: turning the engine's static prop drawing off while Photon draws them).
 * No ICvar calls (its vtable order in GMod's x64 build is not known, and a wrong slot could register garbage): the ConVar is a static object
 * in the module that defines it, found through its name string and a pointer to that string. Values are written into the variable's fields (no virtual SetValue, whose overload order is the same question), and only after
 * the layout checks out on the variable itself: ConCommandBase { vtable, next, registered, name +0x18, help, flags +0x28 }, then ConVar's
 * IConVar vtable +0x30, parent +0x38, default string +0x40, string +0x48, string length +0x50, float +0x54, int +0x58 (x64; float and int
 * XOR-ed with the parent's address, see key_of). The check: the parent's default string parses to its float and int ("1", 1.0, 1). FCVAR_CHEAT is not consulted: Photon's
 * own setting is the authority for the variables it drives. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cvar.h"
#include "log.h"

static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const uint8_t *)p + n <= (const uint8_t *)mi.BaseAddress + mi.RegionSize;
}
/* GMod x64 keeps a ConVar's float and int XOR-ed with the low 32 bits of the (parent) ConVar's own address, as CS:GO's engine does
 * (seen on r_drawstaticprops: int field 0xee063741 at 0x...ee063740 = 1; float field 0xd1863740 = 0x3f800000 ^ 0xee063740 = 1.0f) */
static uint32_t key_of(const void *par) { return (uint32_t)(uintptr_t)par; }
/* p lies in one of the module's read-only, non-executable sections (where MSVC puts vtables) */
static int in_rdata(const uint8_t *base, const IMAGE_NT_HEADERS64 *nt, uint64_t p) {
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int si = 0; si < nt->FileHeader.NumberOfSections; si++) {
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_READ) || (sec[si].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))) continue;
        uint64_t a = (uint64_t)(uintptr_t)(base + sec[si].VirtualAddress);
        if (p >= a && p < a + sec[si].Misc.VirtualSize) return 1;
    }
    return 0;
}
/* a ConVar, not another class that happens to hold the name at +0x18 (photon-64 found such an object first for mat_viewportscale in
 * client.dll): both vtable pointers (+0x00 ConCommandBase, +0x30 IConVar) in the module's read-only data, and the parent (+0x38) the
 * object itself or another object naming the same string; then the value untouched since start */
static int layout_ok(void *cv, const uint8_t *base, const IMAGE_NT_HEADERS64 *nt) {
    uint64_t v0, v1; memcpy(&v0, cv, 8); memcpy(&v1, (uint8_t *)cv + 0x30, 8);
    if (!in_rdata(base, nt, v0) || !in_rdata(base, nt, v1) || v0 == v1) return 0;
    void *par = *(void **)((uint8_t *)cv + 0x38);
    if (!readable(par, 0x60)) return 0;
    if (par != cv && memcmp((uint8_t *)par + 0x18, (uint8_t *)cv + 0x18, 8) != 0) return 0;
    const char *def = *(const char **)((uint8_t *)par + 0x40);
    if (!readable(def, 2)) return 0;
    uint32_t fu, iu;
    memcpy(&fu, (uint8_t *)par + 0x54, 4); memcpy(&iu, (uint8_t *)par + 0x58, 4);
    fu ^= key_of(par); iu ^= key_of(par);
    float f; int i;
    memcpy(&f, &fu, 4); memcpy(&i, &iu, 4);
    return (float)atof(def) == f && atoi(def) == i;   /* untouched since start: value == default (checked before any write) */
}

/* the ConVar named `name` defined in `module` (a static object in its data): no calls into the engine at all. The name string is found
 * in the module's image, then a pointer-aligned qword equal to its address; the object starts 0x18 before it. */
void *photon_cvar_find_in(const char *module, const char *name) {
    HMODULE m = GetModuleHandleA(module);
    if (!m) return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const uint8_t *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    const uint8_t *base = (const uint8_t *)m;
    size_t img = nt->OptionalHeader.SizeOfImage, nl = strlen(name) + 1;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int si = 0; si < nt->FileHeader.NumberOfSections; si++) {   /* the name in a read-only section, exactly (bounded by NULs) */
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_READ) || (sec[si].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))) continue;
        const uint8_t *a = base + sec[si].VirtualAddress, *e = a + sec[si].Misc.VirtualSize;
        for (const uint8_t *p = a; p + nl <= e; p++) {
            if (memcmp(p, name, nl) != 0 || (p > a && p[-1] != 0)) continue;
            uint64_t want = (uint64_t)(uintptr_t)p;
            for (int sj = 0; sj < nt->FileHeader.NumberOfSections; sj++) {   /* the object: in a writable data section */
                if (!(sec[sj].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                const uint8_t *b = base + sec[sj].VirtualAddress, *f = b + sec[sj].Misc.VirtualSize;
                for (const uint8_t *q = b + 0x18; q + 8 <= f; q += 8) {
                    uint64_t v; memcpy(&v, q, 8);
                    if (v != want) continue;
                    void *cv = (void *)(q - 0x18);
                    if (layout_ok(cv, base, nt)) return cv;
                    /* not the expected layout: the object's first 0x70 bytes, for working out GMod's (logged once per name) */
                    char line[700]; int n = 0;
                    for (int k = 0; k < 14 && n < (int)sizeof line - 40; k++) { uint64_t w; memcpy(&w, (const uint8_t *)cv + 8 * k, 8); n += snprintf(line + n, sizeof line - (size_t)n, " %016llx", (unsigned long long)w); }
                    photon_log("cvar: %s: candidate %p in %s does not check out:%s", name, cv, module, line);
                }
            }
        }
    }
    (void)img;
    return NULL;
}
void *photon_cvar_find(const char *name) {   /* engine.dll, then client.dll */
    void *cv = photon_cvar_find_in("engine.dll", name);
    if (!cv) cv = photon_cvar_find_in("client.dll", name);
    photon_log("cvar: %s %s%s", name, cv ? "found" : "not found (or its layout did not check out): left alone", cv ? "" : "");
    return cv;
}
int photon_cvar_get_int(void *cv) {
    void *par = *(void **)((uint8_t *)cv + 0x38);
    uint32_t iu; memcpy(&iu, (uint8_t *)par + 0x58, 4);
    iu ^= key_of(par);
    int i; memcpy(&i, &iu, 4);
    return i;
}
int photon_cvar_set_int(void *cv, int v) {
    void *par = *(void **)((uint8_t *)cv + 0x38);
    float f = (float)v;
    uint32_t fu, iu;
    memcpy(&fu, &f, 4); memcpy(&iu, &v, 4);
    fu ^= key_of(par); iu ^= key_of(par);
    memcpy((uint8_t *)par + 0x54, &fu, 4); memcpy((uint8_t *)par + 0x58, &iu, 4);
    char *str = *(char **)((uint8_t *)par + 0x48);
    int len; memcpy(&len, (uint8_t *)par + 0x50, 4);
    if (str && len >= 2 && v >= 0 && v <= 9) { str[0] = (char)('0' + v); str[1] = 0; }   /* GetString agrees for one-digit values */
    return 1;
}
float photon_cvar_get_float(void *cv) {
    void *par = *(void **)((uint8_t *)cv + 0x38);
    uint32_t fu; memcpy(&fu, (uint8_t *)par + 0x54, 4);
    fu ^= key_of(par);
    float f; memcpy(&f, &fu, 4);
    return f;
}
int photon_cvar_set_float(void *cv, float f) {   /* the float, the int (truncated) and, when it fits, the string */
    void *par = *(void **)((uint8_t *)cv + 0x38);
    int v = (int)f;
    uint32_t fu, iu;
    memcpy(&fu, &f, 4); memcpy(&iu, &v, 4);
    fu ^= key_of(par); iu ^= key_of(par);
    memcpy((uint8_t *)par + 0x54, &fu, 4); memcpy((uint8_t *)par + 0x58, &iu, 4);
    char *str = *(char **)((uint8_t *)par + 0x48);
    int len; memcpy(&len, (uint8_t *)par + 0x50, 4);
    char b[16]; int n = snprintf(b, sizeof b, "%g", f);
    if (str && n < len) memcpy(str, b, (size_t)n + 1);
    return 1;
}
