/* Photon CPU P2: the physics governor (on by default; setting photon_cpu_physgov / PHOTON_CPU_PHYSGOV, 0 off). Under overload, physics
 * time slows down instead of the whole game's frame rate (user, 2026-11-09: "id always prefeir physics slowing down than making
 * everything low fps"). AI1 (analysis/16): corpse piles make the server's physics outgrow the tick, and the catch-up ticks spiral to
 * 2-5 fps; phys_timescale 0.5 cut the physics per tick by 40% while players, AI, weapons and Lua kept real time (66 ticks a second).
 * - The measurement: server.dll's PhysFrame calls IPhysicsEnvironment::Simulate (vtable slot 34) at two sites (RVA 0x3b1cf4,
 *   0x3b1d16: call [rax+0x110], hash-checked); each is redirected through a thunk that times the call. Server physics only.
 * - The steps: phys_timescale x 1, 0.75, 0.5, 0.33, 0.25 (times the value the game or the user set: a change made by anyone else
 *   becomes the new base). Down one step when the physics per tick over a 0.5 s window is above 40% of the tick interval; up one
 *   step after 4 s below 20%; an up step that brings it back over 40% within 2 s is undone and up steps wait 30 s, doubling to 240 s.
 * - phys_timescale is written straight into its ConVar in server.dll (found through its name; the float and int fields, XOR-ed with
 *   the parent's address, as Photon Render's cvar.c does it: no ICvar calls). Physics-driven vehicles slow down with everything else
 *   physical while it is engaged; player movement does not use physics and stays real time. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "photon_cpu.h"

#define SITE1_RVA 0x3b1cf4
#define SITE2_RVA 0x3b1d16
#define SITE1_HASH 0x709d6db1u   /* FNV-1a of the 64 bytes from the site - 16, server.dll timestamp 0x6adfbedb */
#define SITE2_HASH 0x47d4541fu
static const float k_scales[] = { 1.0f, 0.75f, 0.5f, 0.33f, 0.25f };
enum { NSCALES = sizeof k_scales / sizeof k_scales[0] };

static int g_on;
void *physgov_target;               /* the Simulate the server was about to call (set by the thunk) */
static uint64_t g_win_ticks, g_win_calls;
static LARGE_INTEGER g_win_start, g_qpf;
static double g_tsc_per_ms;
static int g_level, g_ban_s = 30;
static ULONGLONG g_last_move, g_calm_since, g_up_at, g_banned_until;
static void *g_cv;                  /* phys_timescale's ConVar */
static uint8_t *g_server;           /* server.dll, once patched: its ConVars are built by its static constructors, after the load
                                     * notification, so the ConVar is looked up later (physgov_present) */
static float g_base = 1.0f, g_written = -1.0f;
static uint64_t g_moves;
static volatile double g_last_per_tick_ms, g_last_ticks_per_s;   /* the last window's numbers, for DYN's savings (dyn.c) */
double physgov_ms_per_tick(void) { return g_last_per_tick_ms; }
double physgov_ticks_per_s(void) { return g_last_ticks_per_s; }

/* ---- phys_timescale ---- */
static int readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const uint8_t *)p + n <= (const uint8_t *)mi.BaseAddress + mi.RegionSize;
}
static uint32_t key_of(const void *par) { return (uint32_t)(uintptr_t)par; }
static void *cv_parent(void *cv) { return *(void **)((uint8_t *)cv + 0x38); }
static float cv_get(void *cv) {
    void *par = cv_parent(cv);
    uint32_t fu; memcpy(&fu, (uint8_t *)par + 0x54, 4); fu ^= key_of(par);
    float f; memcpy(&f, &fu, 4); return f;
}
static void cv_set(void *cv, float f) {
    void *par = cv_parent(cv);
    int v = (int)f;
    uint32_t fu, iu; memcpy(&fu, &f, 4); memcpy(&iu, &v, 4);
    fu ^= key_of(par); iu ^= key_of(par);
    memcpy((uint8_t *)par + 0x54, &fu, 4); memcpy((uint8_t *)par + 0x58, &iu, 4);
}
static int layout_ok(void *cv) {   /* the parent's default string parses to its float (checked before any write) */
    void *par = cv_parent(cv);
    if (!readable(par, 0x60)) return 0;
    const char *def = *(const char **)((uint8_t *)par + 0x40);
    if (!readable(def, 2)) return 0;
    return (float)atof(def) == cv_get(cv);
}
static void *find_cvar(HMODULE m, const char *name) {
    const uint8_t *base = (const uint8_t *)m;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t nl = strlen(name) + 1;
    for (int si = 0; si < nt->FileHeader.NumberOfSections; si++) {
        if (!(sec[si].Characteristics & IMAGE_SCN_MEM_READ) || (sec[si].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))) continue;
        const uint8_t *a = base + sec[si].VirtualAddress, *e = a + sec[si].Misc.VirtualSize;
        for (const uint8_t *p = a; p + nl <= e; p++) {
            if (memcmp(p, name, nl) || (p > a && p[-1])) continue;
            uint64_t want = (uint64_t)(uintptr_t)p;
            for (int sj = 0; sj < nt->FileHeader.NumberOfSections; sj++) {
                if (!(sec[sj].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                const uint8_t *b = base + sec[sj].VirtualAddress, *f = b + sec[sj].Misc.VirtualSize;
                for (const uint8_t *q = b + 0x18; q + 8 <= f; q += 8) {
                    uint64_t v; memcpy(&v, q, 8);
                    if (v != want) continue;
                    void *cv = (void *)(q - 0x18);
                    if (layout_ok(cv)) return cv;
                    char line[600]; int n = 0;
                    for (int k = 0; k < 14 && n < (int)sizeof line - 40; k++) { uint64_t w; memcpy(&w, (const uint8_t *)cv + 8 * k, 8); n += snprintf(line + n, sizeof line - (size_t)n, " %016llx", (unsigned long long)w); }
                    cpu_log("physgov: %s candidate %p does not check out:%s", name, cv, line);
                }
            }
        }
    }
    return NULL;
}

static void apply_level(int lv) {
    if (!g_cv) return;
    float cur = cv_get(g_cv);
    if (g_written >= 0 && cur != g_written) g_base = cur;   /* someone else set it: that is the base now */
    else if (g_written < 0) g_base = cur;
    g_level = lv;
    g_written = g_base * k_scales[lv];
    cv_set(g_cv, g_written);
    g_moves++;
    cpu_log("physgov: physics time x%.2f (phys_timescale %.3f, base %.3f)", k_scales[lv], g_written, g_base);
}

/* ---- the timed Simulate ---- */
void physgov_sim(void *env, float dt) {
    uint64_t t0 = __rdtsc();
    ((void (*)(void *, float))physgov_target)(env, dt);
    g_win_ticks += __rdtsc() - t0;
    g_win_calls++;
}
/* call [rax+0x110] -> call physgov_thunk: the target from the same vtable, then the timed call (the return comes back to the site's
 * trailing nop) */
__asm__(".text\n.p2align 4\nphysgov_thunk:\n"
        "  movq 0x110(%rax), %r11\n"
        "  movq %r11, physgov_target(%rip)\n"
        "  jmp physgov_sim\n");
extern void physgov_thunk(void);

static int patch_site(uint8_t *server, uint32_t rva, uint32_t hash, uint8_t *tramp) {
    uint8_t *s = server + rva;
    if (cpu_fnv(s - 16, 64) != hash || memcmp(s, "\xff\x90\x10\x01\x00\x00", 6)) return 0;
    int64_t rel = (int64_t)(tramp - (s + 5));
    if (rel < INT32_MIN || rel > INT32_MAX) return 0;
    uint8_t p[6] = { 0xe8, 0, 0, 0, 0, 0x90 };
    int32_t r = (int32_t)rel; memcpy(p + 1, &r, 4);
    DWORD old;
    if (!VirtualProtect(s, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(s, p, 6);
    VirtualProtect(s, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s, 6);
    return 1;
}
static void install(uint8_t *server) {
    if (cpu_fnv(server + SITE1_RVA - 16, 64) != SITE1_HASH || cpu_fnv(server + SITE2_RVA - 16, 64) != SITE2_HASH) {
        cpu_log("physgov: server.dll bytes differ, off");
        return;
    }
    uint8_t *tr = NULL;   /* a jmp to the thunk within rel32 reach of server.dll */
    for (uintptr_t a = ((uintptr_t)server - 0x10000000) & ~(uintptr_t)0xffff; a < (uintptr_t)server && !tr; a += 0x10000)
        tr = VirtualAlloc((void *)a, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { cpu_log("physgov: no trampoline near server.dll, off"); return; }
    tr[0] = 0xff; tr[1] = 0x25; memset(tr + 2, 0, 4);
    uint64_t t = (uint64_t)(void *)physgov_thunk; memcpy(tr + 6, &t, 8);
    FlushInstructionCache(GetCurrentProcess(), tr, 14);
    int n = patch_site(server, SITE1_RVA, SITE1_HASH, tr) + patch_site(server, SITE2_RVA, SITE2_HASH, tr);
    g_server = server;
    cpu_log("physgov: %d Simulate calls timed (phys_timescale looked up once server.dll's ConVars exist)", n);
}

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UStr;
typedef struct { ULONG Flags; const UStr *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } DllLoaded;
typedef VOID (CALLBACK *DllNotify)(ULONG reason, const DllLoaded *data, PVOID ctx);
typedef LONG (NTAPI *LdrRegister)(ULONG flags, DllNotify fn, PVOID ctx, PVOID *cookie);
static PVOID g_cookie;
static VOID CALLBACK on_dll(ULONG reason, const DllLoaded *d, PVOID ctx) {
    (void)ctx;
    if (reason != 1 || !d->BaseDllName || !d->BaseDllName->Buffer) return;
    if (d->BaseDllName->Length / 2 != 10 || _wcsnicmp(d->BaseDllName->Buffer, L"server.dll", 10)) return;
    install((uint8_t *)d->DllBase);
}

void physgov_init(void) {
    g_on = cpu_setting("photon_cpu_physgov", "PHOTON_CPU_PHYSGOV", 1);
    if (g_on != 1) { cpu_log("physgov: off (setting %d)", g_on); return; }
    QueryPerformanceFrequency(&g_qpf);
    g_tsc_per_ms = cpu_tsc_mhz() * 1000.0;
    uint8_t *s = (uint8_t *)GetModuleHandleA("server.dll");
    if (s) { install(s); return; }
    LdrRegister reg = (LdrRegister)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
    if (reg) reg(0, on_dll, NULL, &g_cookie);
}

/* the decision, once per 0.5 s window, from the presenting thread (reads counters the main thread writes; a torn read only skews one
 * window) */
void physgov_present(uint64_t frame) {
    (void)frame;
    if (g_on != 1 || !g_server) return;
    if (!g_cv) {
        static ULONGLONG next_try;
        ULONGLONG tn = GetTickCount64();
        if (tn < next_try) return;
        next_try = tn + 3000;
        g_cv = find_cvar((HMODULE)g_server, "phys_timescale");
        if (!g_cv) return;
        cpu_log("physgov: phys_timescale found (%.3f)", cv_get(g_cv));
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_win_start.QuadPart) { g_win_start = now; return; }
    double win_ms = (double)(now.QuadPart - g_win_start.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    if (win_ms < 500.0) return;
    uint64_t calls = g_win_calls, ticks = g_win_ticks;
    g_win_calls = g_win_ticks = 0;
    g_win_start = now;
    if (!calls || g_tsc_per_ms <= 0) return;
    double per_tick = (double)ticks / g_tsc_per_ms / (double)calls;
    g_last_per_tick_ms = per_tick;
    g_last_ticks_per_s = (double)calls * 1000.0 / win_ms;
    const double interval_ms = 1000.0 / 66.0;
    double share = per_tick / interval_ms;
    ULONGLONG t = GetTickCount64();
    if (share > 0.40) {
        g_calm_since = 0;
        if (g_up_at && t - g_up_at < 2000) {   /* the last up step brought it back: undo it, wait longer before the next */
            g_banned_until = t + (ULONGLONG)g_ban_s * 1000;
            if (g_ban_s < 240) g_ban_s *= 2;
            g_up_at = 0;
        }
        if (g_level < NSCALES - 1 && t - g_last_move >= 500) {
            apply_level(g_level + 1);
            g_last_move = t;
        }
    } else if (share < 0.20) {
        if (!g_calm_since) g_calm_since = t;
        if (g_level > 0 && t - g_calm_since >= 4000 && t >= g_banned_until && t - g_last_move >= 3000) {
            apply_level(g_level - 1);
            g_last_move = g_up_at = t;
            g_calm_since = t;
        }
    } else {
        g_calm_since = 0;
    }
    static ULONGLONG last_log;
    if (t - last_log >= 10000) {
        last_log = t;
        cpu_log("physgov: server physics %.2f ms a tick (%.0f%% of a tick), physics time x%.2f, %llu moves", per_tick, share * 100.0,
                k_scales[g_level], (unsigned long long)g_moves);
    }
}
