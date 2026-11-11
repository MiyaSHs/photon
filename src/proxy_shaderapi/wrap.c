#include <windows.h>
#include <string.h>
#include "wrap.h"
#include "log.h"

enum { THUNK_SIZE = 32 };

/* Per-slot call counting (frames.csv call columns, photon_slots.csv, live meshes) costs a locked increment on every forwarded call: on the
 * laptop's busy main thread about 0.1-0.4 ms a frame (analysis/19), and cache-line traffic between main and MatQueue. Off for players;
 * PHOTON_CALL_COUNTS=1 turns it on (run_gmod.sh and the mock host set it). Read once, before the first class is built. */
int photon_wrap_counting(void) {
    static int v = -1;
    if (v < 0) { char e[8]; v = GetEnvironmentVariableA("PHOTON_CALL_COUNTS", e, sizeof e) > 0 && e[0] == '1'; }
    return v;
}

static struct { uintptr_t lo, hi; } g_code[64];   /* thunk pages, for the profiler (frames.c, PHOTON_PROFILE) */
static volatile size_t g_ncode;
int photon_wrap_is_thunk(uintptr_t a) {
    for (size_t i = 0; i < g_ncode; i++) if (a >= g_code[i].lo && a < g_code[i].hi) return 1;
    return 0;
}

/* PHOTON_WRAP=1 (or call counting, which the mock host's checks rely on): every interface is wrapped as before T4.6. Otherwise classes the
 * proxy marks direct (photon_wrap_class's `direct`) hand the engine its own objects (analysis/20). Read once. */
int photon_wrap_direct_allowed(void) {
    static int v = -1;
    if (v < 0) { char e[8]; v = !photon_wrap_counting() && !(GetEnvironmentVariableA("PHOTON_WRAP", e, sizeof e) > 0 && e[0] == '1'); }
    return v;
}

PhotonWrapClass *photon_wrap_class(const char *name, void **real_vtbl, uintptr_t text_lo, uintptr_t text_hi,
                                   size_t expected, int direct) {
    size_t n = 0;
    while (n < 4096 && (uintptr_t)real_vtbl[n] >= text_lo && (uintptr_t)real_vtbl[n] < text_hi) n++;
    if (n == 0) { photon_log("wrap %s: vtable %p has no slots in .text", name, (void *)real_vtbl); return NULL; }
    if (expected && n != expected)
        photon_log("WARNING wrap %s: measured %zu slots, expected %zu (different build?)", name, n, expected);

    /* One allocation keeps the counters and stock's function table within rel32 reach of the thunks:
     * [counters | thunk vtable | stock functions | direct vtable | code]. */
    SYSTEM_INFO si; GetSystemInfo(&si);
    size_t page = si.dwPageSize;
    size_t data = n * sizeof(uint64_t) + 3 * (n + 1) * sizeof(void *);
    size_t data_sz = (data + page - 1) / page * page;
    size_t code_sz = (n * THUNK_SIZE + page - 1) / page * page;
    uint8_t *mem = VirtualAlloc(NULL, data_sz + code_sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    PhotonWrapClass *cls = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *cls);
    if (!mem || !cls) return NULL;
    volatile uint64_t *counters = (volatile uint64_t *)mem;
    void **vt = (void **)(mem + n * sizeof(uint64_t));
    void **orig = vt + (n + 1);
    void **dvt = orig + (n + 1);
    uint8_t *code = mem + data_sz;

    vt[0] = real_vtbl[-1];           /* RTTI complete-object locator, in case anyone runs typeid/dynamic_cast */
    dvt[0] = real_vtbl[-1];
    vt += 1; dvt += 1;
    memcpy(orig, real_vtbl, n * sizeof(void *));   /* stock's functions: the thunks jump here, never through the object's vptr, so a
                                                     * direct object's hooks can call the thunk as "prev" without coming back to themselves */
    memcpy(dvt, real_vtbl, n * sizeof(void *));
    int counting = photon_wrap_counting();
    int dir = direct && photon_wrap_direct_allowed();
    for (size_t i = 0; i < n; i++) {
        uint8_t *t = code + i * THUNK_SIZE, *j;
        memset(t, 0xCC, THUNK_SIZE);
        static const uint8_t pro[] = { 0x48, 0x8B, 0x49, 0x08,   /* mov rcx,[rcx+8]  (real this) */
                                       0x48, 0x8B, 0x01 };       /* mov rax,[rcx]    (real vtable; wrapped classes only) */
        /* Wrapped classes jump through the object's live vtable, as before T4.6: a vtable replaced after creation (the mock host's fake stock)
         * still takes effect. Direct classes jump to stock's saved functions instead: their objects' vptr is our private table, whose hooked
         * slots would lead a hook calling its thunk as "prev" back into itself. */
        size_t k = dir ? 4 : 7;
        memcpy(t, pro, k);
        j = t + k;
        if (counting) {
            /* lock inc qword ptr [rip+disp32] ; disp is relative to the end of this 8-byte instruction */
            j[0] = 0xF0; j[1] = 0x48; j[2] = 0xFF; j[3] = 0x05;
            int32_t d32 = (int32_t)((intptr_t)&counters[i] - (intptr_t)(j + 8));
            memcpy(j + 4, &d32, 4);
            j += 8;
        }
        if (dir) {   /* jmp qword ptr [rip+disp32] -> orig[i] */
            j[0] = 0xFF; j[1] = 0x25;
            int32_t off = (int32_t)((intptr_t)&orig[i] - (intptr_t)(j + 6));
            memcpy(j + 2, &off, 4);
        } else {     /* jmp qword ptr [rax+disp32] */
            j[0] = 0xFF; j[1] = 0xA0;
            int32_t off = (int32_t)(i * 8);
            memcpy(j + 2, &off, 4);
        }
        vt[i] = t;
    }
    DWORD old;
    if (!VirtualProtect(code, code_sz, PAGE_EXECUTE_READ, &old)) return NULL;
    FlushInstructionCache(GetCurrentProcess(), code, code_sz);

    if (g_ncode < sizeof g_code / sizeof g_code[0]) { g_code[g_ncode].lo = (uintptr_t)code; g_code[g_ncode].hi = (uintptr_t)code + code_sz; g_ncode++; }
    cls->name = name; cls->real_vtbl = real_vtbl; cls->nslots = n; cls->thunk_vtbl = vt; cls->counters = counters;
    cls->direct = dir;
    cls->direct_vtbl = cls->direct ? dvt : NULL;
    cls->text_lo = text_lo; cls->text_hi = text_hi;
    photon_log("wrap %s: %zu slots, real vtable %p, thunks %p, call counts %s, %s", name, n, (void *)real_vtbl, (void *)code,
               counting ? "on" : "off (PHOTON_CALL_COUNTS=1 for them)",
               cls->direct ? "direct: the engine gets its own object with only our hooked slots changed (T4.6)" : "wrapped");
    return cls;
}

/* Small registry, one record per real object (a few dozen at most). Written under the lock; read without it (records are published by a
 * release store of the count and never removed), so the per-call lookup in photon_wrap_of costs a short scan. */
static SRWLOCK g_reg_lock = SRWLOCK_INIT;
static PhotonWrap *g_reg[256];
static volatile LONG g_nreg;

static PhotonWrap *find_locked(PhotonWrapClass *cls, void *real) {
    for (LONG i = 0; i < g_nreg; i++) if (g_reg[i]->real == real && g_reg[i]->cls == cls) return g_reg[i];
    return NULL;
}

void *photon_wrap(PhotonWrapClass *cls, void *real) {
    if (!real || !cls) return real;
    AcquireSRWLockExclusive(&g_reg_lock);
    PhotonWrap *w = find_locked(cls, real);
    int fresh = 0;
    if (!w && g_nreg < (LONG)(sizeof g_reg / sizeof g_reg[0]) && (w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w)) != NULL) {
        w->vtbl = cls->thunk_vtbl; w->real = real; w->cls = cls;
        g_reg[g_nreg] = w;
        InterlockedIncrement(&g_nreg);   /* publishes the record to lock-free readers */
        fresh = 1;
    }
    void *out = w ? (void *)w : real;
    if (w && cls->direct) {
        void **vp = *(void ***)real;
        if (vp == cls->real_vtbl) {   /* swap: the private table is complete (stock's functions plus the hooks so far) before this one store */
            InterlockedExchangePointer((PVOID volatile *)real, cls->direct_vtbl);
            w->direct = 1;
        }
        if (w->direct) out = real;
        else if (fresh) photon_log("wrap %s: object %p has vtable %p, not the class's %p: handed out wrapped", cls->name, real, (void *)vp,
                                   (void *)cls->real_vtbl);
    }
    ReleaseSRWLockExclusive(&g_reg_lock);
    if (!w) photon_log("wrap %s: registry full, passing %p through unwrapped", cls->name, real);
    return out;
}

/* Every hook calls this, so it sits on the draw path (laptop profile: the plain scan was about 0.8% of the main thread). A small cache
 * keyed by the pointer's bits holds record pointers only; a hit is checked against the record itself, so a race between two writers can
 * only cost a miss, never a wrong answer. */
static PhotonWrap *volatile g_of_cache[64];
static inline int matches(const PhotonWrap *w, const void *self) { return (const void *)w == self || (w->real == self && w->direct); }
PhotonWrap *volatile g_wrap_of_last;
PhotonWrap *photon_wrap_of_slow(void *self) {
    size_t h = ((uintptr_t)self >> 4 ^ (uintptr_t)self >> 10) & 63;
    PhotonWrap *w = g_of_cache[h];
    if (w && matches(w, self)) { g_wrap_of_last = w; return w; }
    LONG n = g_nreg;
    MemoryBarrier();
    for (LONG i = 0; i < n; i++)
        if (matches(g_reg[i], self)) { g_of_cache[h] = g_reg[i]; g_wrap_of_last = g_reg[i]; return g_reg[i]; }
    return NULL;
}

void *photon_unwrap(void *p) {
    if (!p) return p;
    PhotonWrap *w = photon_wrap_of(p);
    return w ? w->real : p;
}

int photon_wrap_known(void *p) { return p && photon_wrap_of(p) != NULL; }

void photon_wrap_set_hook(PhotonWrapClass *cls, size_t slot, void *hook) {
    if (!cls || slot >= cls->nslots) return;
    cls->thunk_vtbl[slot] = hook;
    if (cls->direct_vtbl) InterlockedExchangePointer((PVOID volatile *)&cls->direct_vtbl[slot], hook);
}

/* T4.6 internal-call guard (analysis/20). A direct object's hooked slots also see stock's own calls through `this` or its globals, which
 * never reached a hook while wrapped. A call is internal when its return address lies in the owning module's .text and the instruction that
 * ends there is `call [reg+disp]` with this slot's own displacement (`also`: one more slot whose tail jump reaches this one, or -1). */
static int is_call_disp(const uint8_t *r, int32_t disp) {
    /* call qword ptr [reg+disp32]: FF 90+r d32 (rm != 4, 5), or FF 94 SIB d32 */
    int32_t d;
    memcpy(&d, r - 4, 4);
    if (d == disp && r[-6] == 0xFF && (r[-5] & 0xF8) == 0x90 && r[-5] != 0x94 && r[-5] != 0x95) return 1;
    if (d == disp && r[-7] == 0xFF && r[-6] == 0x94) return 1;
    /* call qword ptr [reg+disp8]: FF 50+r d8, or FF 54 SIB d8 */
    if (disp >= -128 && disp <= 127) {
        int8_t d8 = (int8_t)r[-1];
        if (d8 == disp && r[-3] == 0xFF && (r[-2] & 0xF8) == 0x50 && r[-2] != 0x54 && r[-2] != 0x55) return 1;
        if (d8 == disp && r[-4] == 0xFF && r[-3] == 0x54) return 1;
    }
    /* call qword ptr [reg] (slot 0): FF 10+r */
    if (disp == 0 && r[-2] == 0xFF && (r[-1] & 0xF8) == 0x10 && r[-1] != 0x14 && r[-1] != 0x15) return 1;
    return 0;
}

int photon_wrap_internal(const PhotonWrap *w, int slot, int also, const void *ret) {
    if (!w || !w->direct) return 0;
    uintptr_t r = (uintptr_t)ret;
    const uint8_t *p = (const uint8_t *)ret;
    if (r >= w->cls->text_lo + 8 && r < w->cls->text_hi && is_call_disp(p, slot * 8)) return 1;
    /* `also`: a slot whose stock code tail-jumps into this one (Api 54 BindStandardTexture -> 155 BindTexture). The return address is then
     * the outer caller's, in any module, right after its `call [reg + also*8]`; this slot's own offset differs, so no direct call matches.
     * Today such calls never reach a hook (stock jumps through the real vtable), so they count as internal wherever they come from. */
    if (also >= 0) {
        HMODULE m = NULL;
        if (is_call_disp(p, also * 8) &&   /* the byte test first: the module lookup only runs on a match (rare) */
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m) && m)
            return 1;
    }
    return 0;
}

/* At FreeLibrary: every direct object gets stock's vtable back, so nothing points into this DLL once it is gone. */
void photon_wrap_restore_all(void) {
    LONG n = g_nreg;
    for (LONG i = 0; i < n; i++) {
        PhotonWrap *w = g_reg[i];
        if (w->direct && *(void ***)w->real == w->cls->direct_vtbl)
            InterlockedExchangePointer((PVOID volatile *)w->real, w->cls->real_vtbl);
    }
}

int photon_module_text(const void *addr, uintptr_t *lo, uintptr_t *hi) {
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m))
        return 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)m + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (memcmp(sec->Name, ".text", 5) == 0) {
            *lo = (uintptr_t)m + sec->VirtualAddress;
            *hi = *lo + sec->Misc.VirtualSize;
            return 1;
        }
    return 0;
}
