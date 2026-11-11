/* Photon CPU L3: MSVC-layout strings allocated through tier0's g_pMemAlloc (msstr.h) */
#include <string.h>
#include <stdint.h>
#include "msstr.h"

void **photon_memalloc(void);   /* tier0.dll's IMemAlloc* (tier0.c) */
#define MEMALLOC_ALLOC 0x08  /* the slots GMod's lua_shared uses for malloc and free */
#define MEMALLOC_FREE 0x28

static void *ma_alloc(size_t n) { void **m = photon_memalloc(); return ((void *(*)(void *, size_t))(*(void ***)m)[MEMALLOC_ALLOC / 8])(m, n); }
static void ma_free(void *p) { void **m = photon_memalloc(); ((void (*)(void *, void *))(*(void ***)m)[MEMALLOC_FREE / 8])(m, p); }

/* MSVC's _Allocate: blocks of 4 KB and more are 32-byte aligned with the raw pointer stored 8 bytes before */
char *ms_allocate(size_t n) {
    if (n < 0x1000) return (char *)ma_alloc(n);
    uint8_t *raw = (uint8_t *)ma_alloc(n + 0x27);
    if (!raw) return NULL;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + 0x27) & ~(uintptr_t)0x1f);
    ((void **)p)[-1] = raw;
    return (char *)p;
}
void ms_deallocate(char *p, size_t n) {
    if (n >= 0x1000) p = (char *)((void **)p)[-1];
    ma_free(p);
}

void msstr_init(MsStr *s) { memset(s, 0, sizeof *s); s->cap = 15; }
void msstr_free(MsStr *s) {
    if (s->cap > 15) ms_deallocate(s->u.ptr, s->cap + 1);
    msstr_init(s);
}
void msstr_assign(MsStr *s, const char *p, size_t n) {
    if (n <= s->cap) {
        char *d = s->cap > 15 ? s->u.ptr : s->u.buf;
        memmove(d, p, n);
        d[n] = 0;
        s->size = n;
        return;
    }
    size_t cap = n | 15;   /* MSVC's growth: at least the request, rounded up to 16k-1 */
    if (cap < s->cap + s->cap / 2) cap = s->cap + s->cap / 2;
    char *d = ms_allocate(cap + 1);
    memcpy(d, p, n);
    d[n] = 0;
    if (s->cap > 15) ms_deallocate(s->u.ptr, s->cap + 1);
    s->u.ptr = d;
    s->size = n;
    s->cap = cap;
}
