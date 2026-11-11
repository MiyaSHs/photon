/* Photon CPU, GORE: GMod's IsPhysicsObjectValid in O(1). Every Lua PhysObj method first checks that its physics object still exists:
 * client.dll FUN_18007f980 and server.dll's copy (RVA 0xf4ce0) ask the physics environment for its whole object list
 * (IPhysicsEnvironment::GetObjectList, vtable slot 47) and scan it. VPhysics-Jolt builds that list anew on each call (every body ID, then
 * every body), so with ragdolls and gibs piling up the check dominates the frame: 14 ms a frame with Fedhoria's 24 active ragdolls,
 * 118 ms with the gore set (analysis/12).
 *   on by default (since 2026-11-09, after 3.68 million checked answers with 0 mismatches: analysis/12): the answer from a hash set
 *   PHOTON_CPU_PHYSVALID=0  off
 *   PHOTON_CPU_PHYSVALID=2  check: compute both answers, return the original's, log every difference (the exactness gate)
 * Exactness: the set equals the list GetObjectList would return now, because
 *   - the list changes only inside IPhysicsEnvironment calls (create, destroy, simulate, cleanup, transfer, restore, unserialize, the
 *     destructor; VPhysics-Jolt and stock vphysics alike). Every slot of the environment's vtable except the read-only ones (getters,
 *     GetObjectList, IsInSimulation, the traces, stats, serialize-size) is wrapped by a thunk that bumps a global generation counter on
 *     entry. One counter for all environments, since a transfer changes two;
 *   - a call changes the list only outside the simulation (Simulate's own deletions run before it sets "simulating" and after it clears
 *     it; destroys during the simulation are queued), so a set built while IsInSimulation() was true is rebuilt once it is false again;
 *   - a set is used only when the generation and the simulating state both match the ones it was built with.
 * Main thread only; other threads scan as before. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

typedef const void **(*GetObjectList_t)(void *env, int *count);
typedef char (*IsInSimulation_t)(void *env);

static int g_mode;
static volatile LONG64 g_gen = 1;
static struct { uint64_t calls, rebuilds, mismatch, scans; } g_st;

/* ---- the environment vtables: generation thunks ---- */
#define NSLOTS 67
static const uint8_t k_pure[NSLOTS] = { [2] = 1, [4] = 1, [6] = 1, [35] = 1, [36] = 1, [38] = 1, [40] = 1, [45] = 1, [46] = 1, [47] = 1,
                                        [55] = 1, [56] = 1, [57] = 1, [58] = 1, [60] = 1, [62] = 1, [63] = 1 };
typedef struct { void **vt; void *orig[NSLOTS]; } Wrapped;
static Wrapped g_wrapped[4];
static int g_nwrapped;

static Wrapped *wrapped_for(void **vt) {
    for (int i = 0; i < g_nwrapped; i++) if (g_wrapped[i].vt == vt) return &g_wrapped[i];
    if (g_nwrapped == 4) return NULL;
    uint8_t *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) return NULL;
    Wrapped *w = &g_wrapped[g_nwrapped];
    w->vt = vt;
    for (int s = 0; s < NSLOTS; s++) w->orig[s] = vt[s];
    DWORD old;
    if (!VirtualProtect(vt, sizeof(void *) * NSLOTS, PAGE_READWRITE, &old)) { VirtualFree(page, 0, MEM_RELEASE); return NULL; }
    for (int s = 0; s < NSLOTS; s++) {
        if (k_pure[s]) continue;
        uint8_t *t = page + 32 * s;
        uint64_t gen = (uint64_t)(uintptr_t)&g_gen, to = (uint64_t)(uintptr_t)w->orig[s];
        t[0] = 0x48; t[1] = 0xb8; memcpy(t + 2, &gen, 8);            /* mov rax, &g_gen */
        t[10] = 0xf0; t[11] = 0x48; t[12] = 0xff; t[13] = 0x00;      /* lock inc qword [rax] */
        t[14] = 0xff; t[15] = 0x25; memset(t + 16, 0, 4);            /* jmp [rip+0] */
        memcpy(t + 20, &to, 8);
    }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    for (int s = 0; s < NSLOTS; s++) if (!k_pure[s]) InterlockedExchangePointer(&vt[s], page + 32 * s);
    VirtualProtect(vt, sizeof(void *) * NSLOTS, old, &old);
    g_nwrapped++;
    InterlockedIncrement64(&g_gen);
    char where[64];
    cpu_log("physvalid: environment vtable %s wrapped (%d slots counted, %d read-only)", cpu_where(vt, where, sizeof where),
            NSLOTS - 17, 17);
    return w;
}

/* ---- per-environment sets ---- */
typedef struct { void *env; LONG64 gen; char in_sim; int valid; const void **slots; uint32_t cap; } Set;
static Set g_sets[4];

static int scan(void *env, GetObjectList_t gol, const void *obj) {
    int n = 0;
    const void **list = gol(env, &n);
    for (int i = 0; i < n; i++) if (list[i] == obj) return 1;
    return 0;
}

static uint32_t hptr(const void *p) { uint64_t x = (uint64_t)(uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (uint32_t)x; }

static int rebuild(Set *s, void *env, GetObjectList_t gol, LONG64 gen, char in_sim) {
    int n = 0;
    const void **list = gol(env, &n);
    uint32_t cap = 64;
    while (cap < (uint32_t)n * 2 + 2) cap <<= 1;
    if (cap > s->cap) {
        const void **ns = realloc((void *)s->slots, sizeof(void *) * cap);
        if (!ns) { s->valid = 0; return 0; }
        s->slots = ns; s->cap = cap;
    }
    memset((void *)s->slots, 0, sizeof(void *) * s->cap);
    for (int i = 0; i < n; i++) {
        const void *p = list[i];
        if (!p) continue;
        for (uint32_t h = hptr(p) & (s->cap - 1);; h = (h + 1) & (s->cap - 1)) {
            if (!s->slots[h]) { s->slots[h] = p; break; }
            if (s->slots[h] == p) break;
        }
    }
    s->env = env; s->gen = gen; s->in_sim = in_sim; s->valid = 1;
    g_st.rebuilds++;
    return 1;
}

static int contains(const Set *s, const void *p) {
    if (!p) return 0;
    for (uint32_t h = hptr(p) & (s->cap - 1);; h = (h + 1) & (s->cap - 1)) {
        if (!s->slots[h]) return 0;
        if (s->slots[h] == p) return 1;
    }
}

static int valid(void **env_global, const void *obj) {
    void *env = *env_global;
    if (!env) return 0;
    void **vt = *(void ***)env;
    g_st.calls++;
    if (GetCurrentThreadId() != cpu_host()->main_thread) { g_st.scans++; return scan(env, (GetObjectList_t)vt[47], obj); }
    Wrapped *w = wrapped_for(vt);
    if (!w) { g_st.scans++; return scan(env, (GetObjectList_t)vt[47], obj); }
    GetObjectList_t gol = (GetObjectList_t)w->orig[47];
    char in_sim = ((IsInSimulation_t)w->orig[35])(env) != 0;
    Set *s = NULL;
    for (int i = 0; i < 4; i++) if (g_sets[i].env == env || !g_sets[i].env) { s = &g_sets[i]; break; }
    if (!s) { g_st.scans++; return scan(env, gol, obj); }
    LONG64 gen = g_gen;
    if (!s->valid || s->env != env || s->gen != gen || s->in_sim != in_sim)
        if (!rebuild(s, env, gol, gen, in_sim)) { g_st.scans++; return scan(env, gol, obj); }
    int r = contains(s, obj);
    if (g_mode == 2) {
        int o = scan(env, gol, obj);
        if (o != r) {
            g_st.mismatch++;
            if (g_st.mismatch <= 20) cpu_log("physvalid: MISMATCH obj %p: set says %d, list says %d (gen %lld, in_sim %d)", obj, r, o,
                                             (long long)gen, in_sim);
        }
        return o;
    }
    return r;
}

/* ---- the patched functions: one entry per module, each with its own environment global ---- */
static void **g_env_client, **g_env_server;
static int valid_client(const void *obj) { return valid(g_env_client, obj); }
static int valid_server(const void *obj) { return valid(g_env_server, obj); }

typedef struct { const char *dll; uint32_t rva, hash; int (*fn)(const void *); void ***env; int done; } Site;
static Site g_sites[] = {
    { "client.dll", 0x7f980, 0x502e8821u, valid_client, &g_env_client, 0 },   /* client.dll timestamp 0x6adfbed6 */
    { "server.dll", 0xf4ce0, 0xea77290bu, valid_server, &g_env_server, 0 },   /* server.dll timestamp 0x6adfbedb */
};

static void patch_site(Site *s, uint8_t *base) {
    uint8_t *f = base + s->rva;
    uint32_t h = cpu_fnv(f, 64);
    /* mov rcx, [rip+rel32] at +0x47 loads the environment global, before the call [rax+0x178] at +0x5c */
    if (h != s->hash || f[0x47] != 0x48 || f[0x48] != 0x8b || f[0x49] != 0x0d || memcmp(f + 0x5c, "\xff\x90\x78\x01\x00\x00", 6)) {
        cpu_log("physvalid: %s IsPhysicsObjectValid bytes differ (hash %08x), not patched", s->dll, h);
        return;
    }
    int32_t rel;
    memcpy(&rel, f + 0x4a, 4);
    *s->env = (void **)(f + 0x4e + rel);
    uint8_t patch[12] = { 0x48, 0xb8 };                                 /* mov rax, imm64; jmp rax */
    uint64_t to = (uint64_t)(uintptr_t)s->fn;
    memcpy(patch + 2, &to, 8);
    patch[10] = 0xff; patch[11] = 0xe0;
    DWORD old;
    if (!VirtualProtect(f, 12, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(f, patch, 12);
    VirtualProtect(f, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 12);
    s->done = 1;
    cpu_log("physvalid: %s IsPhysicsObjectValid replaced (%s mode)", s->dll, g_mode == 1 ? "serve" : "check");
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UStr;
typedef struct { ULONG Flags; const UStr *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } DllLoaded;
typedef VOID (CALLBACK *DllNotify)(ULONG reason, const DllLoaded *data, PVOID ctx);
typedef LONG (NTAPI *LdrRegister)(ULONG flags, DllNotify fn, PVOID ctx, PVOID *cookie);
static PVOID g_cookie;

static VOID CALLBACK on_dll(ULONG reason, const DllLoaded *d, PVOID ctx) {
    (void)ctx;
    if (reason != 1 || !d->BaseDllName || !d->BaseDllName->Buffer) return;   /* 1 = loaded */
    char name[64];
    int n = WideCharToMultiByte(CP_ACP, 0, d->BaseDllName->Buffer, d->BaseDllName->Length / 2, name, sizeof name - 1, NULL, NULL);
    if (n <= 0) return;
    name[n] = 0;
    for (size_t i = 0; i < sizeof g_sites / sizeof g_sites[0]; i++)
        if (!_stricmp(name, g_sites[i].dll)) { g_sites[i].done = 0; patch_site(&g_sites[i], d->DllBase); }   /* a reload patches again */
}

void physvalid_present(uint64_t frame) {
    (void)frame;
    if (!g_mode) return;
    static ULONGLONG last;
    static uint64_t last_calls;
    ULONGLONG now = GetTickCount64();
    if (now - last < 15000 || g_st.calls == last_calls) return;
    last = now; last_calls = g_st.calls;
    cpu_log("physvalid: %llu checks, %llu set rebuilds, %llu scans (other threads), %llu mismatches; generation %lld",
            (unsigned long long)g_st.calls, (unsigned long long)g_st.rebuilds, (unsigned long long)g_st.scans,
            (unsigned long long)g_st.mismatch, (long long)g_gen);
}

void physvalid_init(void) {
    int m = cpu_setting("photon_cpu_physvalid", "PHOTON_CPU_PHYSVALID", 1);
    if (m != 1 && m != 2) { cpu_log("physvalid: off (setting %d)", m); return; }
    const char *e = m == 1 ? "1" : "2";
    g_mode = *e - '0';
    for (size_t i = 0; i < sizeof g_sites / sizeof g_sites[0]; i++) {
        uint8_t *b = (uint8_t *)GetModuleHandleA(g_sites[i].dll);
        if (b) patch_site(&g_sites[i], b);
    }
    LdrRegister reg = (LdrRegister)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
    if (reg) reg(0, on_dll, NULL, &g_cookie);
}
