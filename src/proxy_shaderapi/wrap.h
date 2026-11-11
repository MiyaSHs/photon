/* Generic per-slot vtable wrappers (FEASIBILITY.md section 8.2). Header-independent: no method signatures needed.
 * A wrapper object is { thunk_vtbl, real }. Each thunk does: rcx = real; rax = *real; ++counter[slot];
 * jmp [rax + 8*slot]. It touches only rcx (swapped `this`) and rax, so all argument registers (rdx, r8, r9,
 * xmm0-3) and the stack, including stack arguments past the fourth, reach the real method unchanged. MSVC member
 * functions keep `this` in rcx even when returning a UDT through a hidden pointer (in rdx), so the swap is
 * correct for every slot. */
#ifndef PHOTON_WRAP_H
#define PHOTON_WRAP_H
#include <stdint.h>
#include <stddef.h>

typedef struct PhotonWrapClass {
    const char *name;
    void **real_vtbl;          /* the stock vtable (slot 0) */
    size_t nslots;             /* measured at runtime */
    void **thunk_vtbl;         /* our vtable (slot 0); thunk_vtbl[-1] = the real RTTI locator */
    volatile uint64_t *counters; /* nslots counters, incremented by the thunks */
    int direct;                /* T4.6: objects get a private vtable instead of a wrapper (analysis/20) */
    void **direct_vtbl;        /* that private vtable: stock's functions, hooked slots replaced (slot 0; [-1] = RTTI) */
    uintptr_t text_lo, text_hi; /* the owning module's .text (the internal-call guard) */
} PhotonWrapClass;

typedef struct PhotonWrap {
    void **vtbl;   /* = class->thunk_vtbl; must be first */
    void *real;    /* the stock sub-object pointer (offset 8 is hard-coded in the thunks) */
    PhotonWrapClass *cls;
    int direct;    /* T4.6: `real` itself was handed out, its vptr pointing to cls->direct_vtbl */
} PhotonWrap;

/* Measures the real vtable (entries pointing into [text_lo, text_hi)) and builds thunks.
 * Returns NULL on failure. `expected` = 0 to skip the size check (a mismatch only logs a warning). */
PhotonWrapClass *photon_wrap_class(const char *name, void **real_vtbl, uintptr_t text_lo, uintptr_t text_hi,
                                   size_t expected, int direct);
int photon_wrap_direct_allowed(void);   /* 0 with PHOTON_WRAP=1 or call counting: everything wrapped as before T4.6 */
/* Hooks receive `self`: our wrapper (wrapped classes) or the real object (direct ones). This finds the record for either. */
PhotonWrap *photon_wrap_of_slow(void *self);
/* Inline fast path: the last record found (nearly every hooked call goes to the one ShaderApi030 object). A record is never freed, and a hit is
 * checked against the record itself, so a stale pointer only costs the slow path. */
extern PhotonWrap *volatile g_wrap_of_last;
static inline PhotonWrap *photon_wrap_of(void *self) {
    PhotonWrap *w = g_wrap_of_last;
    if (w && ((void *)w == self || (w->real == self && w->direct))) return w;
    return photon_wrap_of_slow(self);
}
int photon_wrap_known(void *p);   /* a wrapper of ours or a direct object */
/* T4.6 internal-call guard, called from a hook with __builtin_return_address(0): 1 when stock called this slot on its own object
 * (such calls never reached a hook while wrapped; the hook must forward them to stock untouched). `also`: a second slot, or -1. */
int photon_wrap_internal(const PhotonWrap *w, int slot, int also, const void *ret);
#define PHOTON_INTERNAL(w, slot, also) photon_wrap_internal((w), (slot), (also), __builtin_return_address(0))
void photon_wrap_restore_all(void);   /* stock vtables back on every direct object (DLL unload) */
int photon_wrap_is_thunk(uintptr_t addr);   /* inside a wrapper class's thunk pages (the profiler) */
int photon_wrap_counting(void);   /* PHOTON_CALL_COUNTS=1: per-slot call counters in the thunks (off by default, wrap.c) */
/* Returns the single wrapper for `real` (creates it on first use). NULL in -> NULL out. */
void *photon_wrap(PhotonWrapClass *cls, void *real);
/* If p is one of our wrappers, returns the real object, else p. */
void *photon_unwrap(void *p);
/* Replaces slot `slot` of the class's vtable with `hook`. The hook is called with the PhotonWrap* as `this`
 * (first parameter) and must call cls->real_vtbl[slot] with w->real itself. Calls to a hooked slot are not
 * counted by the thunk counters. */
void photon_wrap_set_hook(PhotonWrapClass *cls, size_t slot, void *hook);
/* .text range of the module containing `addr`; returns 0 on failure. */
int photon_module_text(const void *addr, uintptr_t *lo, uintptr_t *hi);
#endif
