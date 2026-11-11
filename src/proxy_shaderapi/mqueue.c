/* P6Q: Photon calls on the material system's queue (analysis/27). See mqueue.h. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "mqueue.h"
#include "log.h"

/* build 26229091 (analysis/27):
 *  IMaterialSystem (VMaterialSystem080) +0x330 GetRenderContext; IMatRenderContext slot 0 / 1 AddRef / Release, +0x488 GetCallQueue
 *  (the queued context's call queue q, NULL on the immediate context). q[0] / q[1] head / tail of the {next, functor} list, q + 2 the
 *  node arena's bump pointer, q[0x200003] the functor arena's bump-pointer cell; each arena is 16 MB past its pointer cell.
 *  The functor: materialsystem's CMemberFunctor2<CMatQueuedRenderContext*, void (*)(unsigned short*, int)> (vtable RVA 0xbb778,
 *  0x30 bytes: vtable, -, fn, object, arg1, arg2), whose call slot (3) is `fn(object, arg1, arg2)` and whose refcount is null. */
enum { MS_GET_RC = 0x330, RC_GET_CALLQUEUE = 0x488, FUNCTOR_VT_RVA = 0xbb778, FUNCTOR_SIZE = 0x30, ARENA = 0x1000001 };
static const uint8_t k_call_slot[] = { 0x44, 0x8b, 0x41, 0x28, 0x48, 0x8b, 0xc1, 0x48, 0x8b, 0x51, 0x20, 0x48, 0x8b, 0x49, 0x18, 0x48, 0xff, 0x60, 0x10 };

static void *g_ms;              /* IMaterialSystem */
static void *g_functor_vt;
static int g_state;             /* 0 not tried, 1 ready, -1 off */
static volatile LONG64 g_queued, g_not_queued, g_full;

static int mq_bind(void) {
    HMODULE m = GetModuleHandleA("materialsystem.dll");
    typedef void *(*CiFn)(const char *, int *);
    CiFn ci = m ? (CiFn)(void *)GetProcAddress(m, "CreateInterface") : NULL;
    void *ms = ci ? ci("VMaterialSystem080", NULL) : NULL;
    if (!ms) { photon_log("mqueue: no VMaterialSystem080: queued calls off"); return -1; }
    void **vt = (void **)((uint8_t *)m + FUNCTOR_VT_RVA);
    const uint8_t *call = (const uint8_t *)vt[3];
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(call, &mi, sizeof mi) || mi.AllocationBase != (void *)m || memcmp(call, k_call_slot, sizeof k_call_slot) != 0) {
        photon_log("mqueue: materialsystem+0x%x is not the expected CMemberFunctor2 vtable (call slot differs): queued calls off", FUNCTOR_VT_RVA);
        return -1;
    }
    g_ms = ms;
    g_functor_vt = vt;
    photon_log("mqueue: ready (VMaterialSystem080 %p, functor vtable materialsystem+0x%x)", ms, FUNCTOR_VT_RVA);
    return 1;
}

int photon_mq_queue(PhotonMqFn fn, void *obj, void *a, int b) {
    if (g_state == 0) g_state = mq_bind();
    if (g_state < 0) return 0;
    void *rc = ((void *(*)(void *))(*(void ***)g_ms)[MS_GET_RC / 8])(g_ms);
    if (!rc) return 0;
    ((void (*)(void *))(*(void ***)rc)[0])(rc);   /* AddRef, as CMatRenderContextPtr */
    uintptr_t *q = ((uintptr_t *(*)(void *))(*(void ***)rc)[RC_GET_CALLQUEUE / 8])(rc);
    int ok = 0;
    if (!q) g_not_queued++;
    else {
        /* the functor first, then the node, each from its cell's cursor after the previous bump: in this build both cells are the same
         * arena (q[0x200003] == q + 2), as the engine's inline code allows */
        uintptr_t *fcell = (uintptr_t *)q[0x200003];
        uintptr_t *f = fcell ? (uintptr_t *)*fcell : NULL;
        if (!f || (uintptr_t)f + FUNCTOR_SIZE - 8 - (uintptr_t)fcell >= ARENA) { g_full++; goto out; }
        *fcell = (uintptr_t)(f + FUNCTOR_SIZE / 8);
        f[0] = (uintptr_t)g_functor_vt; f[1] = 0; f[2] = (uintptr_t)fn; f[3] = (uintptr_t)obj; f[4] = (uintptr_t)a; f[5] = (uint32_t)b;
        uintptr_t *ncell = q + 2;
        uintptr_t *node = (uintptr_t *)*ncell;
        if (!node || (uintptr_t)node + 8 - (uintptr_t)ncell >= ARENA) { g_full++; goto out; }   /* the functor stays unlinked: never run */
        {
            *ncell = (uintptr_t)(node + 2);
            node[0] = 0; node[1] = (uintptr_t)f;
            if (q[1]) *(uintptr_t *)q[1] = (uintptr_t)node; else q[0] = (uintptr_t)node;
            q[1] = (uintptr_t)node;
            if (g_queued < 6)
                photon_log("mqueue: queued #%lld on thread %lu: rc %p q %p fcell %p f %p (next %p) node %p", g_queued + 1, GetCurrentThreadId(), rc, (void *)q,
                           (void *)fcell, (void *)f, (void *)*fcell, (void *)node);
            g_queued++;
            ok = 1;
        }
    }
out:
    ((void (*)(void *))(*(void ***)rc)[1])(rc);   /* Release */
    return ok;
}

/* PHOTON_TEST_MQ=1 (p6.c): a counting call queued around every model draw */
static volatile LONG64 g_t_ran, g_t_other_thread, g_t_order_bad;
static volatile LONG64 g_t_last;
static DWORD g_t_main;
static void test_fn(void *obj, void *a, int b) {
    (void)obj;
    if (g_t_ran < 6) photon_log("mqueue: test call ran on thread %lu (a %p, b %d)", GetCurrentThreadId(), a, b);
    g_t_ran++;
    if (GetCurrentThreadId() != g_t_main) g_t_other_thread++;
    LONG64 n = (LONG64)(uintptr_t)a;
    if (b == 0 && n != g_t_last + 1) g_t_order_bad++;   /* begin calls run in queueing order */
    if (b == 0) g_t_last = n;
}
static volatile LONG64 g_t_seq;
void photon_mq_test_draw(int end) {
    if (!g_t_main) g_t_main = GetCurrentThreadId();
    if (!end) { LONG64 n = ++g_t_seq; if (!photon_mq_queue(test_fn, NULL, (void *)(uintptr_t)n, 0)) g_t_seq--; }
    else photon_mq_queue(test_fn, NULL, NULL, 1);
}

void photon_mq_dump(const char *why) {
    if (!g_state) return;
    photon_log("mqueue (%s): %s; queued %lld, not queued (immediate context) %lld, arena full %lld; test calls run %lld (on another thread %lld,"
               " out of order %lld)", why ? why : "?", g_state > 0 ? "ready" : "off", g_queued, g_not_queued, g_full, g_t_ran, g_t_other_thread, g_t_order_bad);
}
