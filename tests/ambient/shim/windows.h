/* The few Win32 calls src/proxy_shaderapi/ambwork.c makes, on pthreads, so the real file runs under ThreadSanitizer and AddressSanitizer
 * on the host (tests/ambient/worktest.c). SRW locks are plain mutexes: stricter than the real thing, since shared holders exclude
 * each other here, but ambwork.c never relies on two shared holders. */
#ifndef PHOTON_SHIM_WINDOWS_H
#define PHOTON_SHIM_WINDOWS_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef int32_t LONG;
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *LPVOID;
typedef const char *LPCSTR;
typedef void *HMODULE;
#define WINAPI
#define TRUE 1
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4u
typedef union { int64_t QuadPart; } LARGE_INTEGER;

typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER

extern long shim_module_refs;   /* defined by the test: GetModuleHandleExA minus FreeLibrary */
extern volatile long shim_thread_calls, shim_thread_fail_from;   /* CreateThread calls so far; from this call on they fail (0: never) */

static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_mutex_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_mutex_unlock(l); }
static inline int TryAcquireSRWLockShared(SRWLOCK *l) { return pthread_mutex_trylock(l) == 0; }
static inline int TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_mutex_trylock(l) == 0; }
static inline void Sleep(DWORD ms) { struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static inline void shim_deadline(struct timespec *t, DWORD ms) {
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) { t->tv_sec++; t->tv_nsec -= 1000000000L; }
}
static inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, unsigned long flags) {
    (void)flags;
    struct timespec t;
    shim_deadline(&t, ms);
    return pthread_cond_timedwait(c, l, &t) == 0;
}
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_broadcast(c); }

typedef struct ShimThread {
    pthread_t t;
    DWORD (*fn)(LPVOID);
    LPVOID arg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done, joined, closed;
} ShimThread, *HANDLE;
static __thread ShimThread *t_shim_self;

static inline void *shim_thread_main(void *p) {
    ShimThread *s = p;
    t_shim_self = s;
    s->fn(s->arg);
    return NULL;
}
static inline HANDLE CreateThread(void *sec, size_t stack, DWORD (*fn)(LPVOID), LPVOID arg, DWORD flags, DWORD *tid) {
    (void)sec; (void)stack; (void)flags; (void)tid;
    long call = __atomic_add_fetch(&shim_thread_calls, 1, __ATOMIC_SEQ_CST);
    if (shim_thread_fail_from && call >= shim_thread_fail_from) return NULL;
    ShimThread *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->fn = fn;
    s->arg = arg;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    if (pthread_create(&s->t, NULL, shim_thread_main, s)) { free(s); return NULL; }
    return s;
}
static inline BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return 1; }
static inline DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    (void)all;
    struct timespec t;
    shim_deadline(&t, ms);
    for (DWORD i = 0; i < n; i++) {
        pthread_mutex_lock(&hs[i]->mu);
        int rc = 0;
        while (!hs[i]->done && rc == 0) rc = pthread_cond_timedwait(&hs[i]->cv, &hs[i]->mu, &t);
        int ok = hs[i]->done;
        pthread_mutex_unlock(&hs[i]->mu);
        if (!ok) return WAIT_TIMEOUT;
        pthread_join(hs[i]->t, NULL);
        hs[i]->joined = 1;
    }
    return WAIT_OBJECT_0;
}
static inline void shim_thread_free(HANDLE h) {
    pthread_mutex_destroy(&h->mu);
    pthread_cond_destroy(&h->cv);
    free(h);
}
/* The thread may outlive its handle (a wait that timed out): then it frees the record on its way out */
static inline BOOL CloseHandle(HANDLE h) {
    if (h->joined) { shim_thread_free(h); return 1; }
    pthread_detach(h->t);
    pthread_mutex_lock(&h->mu);
    int done = h->done;
    h->closed = 1;
    pthread_mutex_unlock(&h->mu);
    if (done) shim_thread_free(h);
    return 1;
}
static inline BOOL GetModuleHandleExA(DWORD flags, LPCSTR p, HMODULE *out) {
    (void)flags;
    __atomic_add_fetch(&shim_module_refs, 1, __ATOMIC_SEQ_CST);
    *out = (HMODULE)p;
    return 1;
}
static inline BOOL FreeLibrary(HMODULE m) {
    (void)m;
    __atomic_sub_fetch(&shim_module_refs, 1, __ATOMIC_SEQ_CST);
    return 1;
}
static inline void FreeLibraryAndExitThread(HMODULE m, DWORD code) {
    (void)code;
    FreeLibrary(m);
    ShimThread *s = t_shim_self;
    pthread_mutex_lock(&s->mu);
    s->done = 1;
    int closed = s->closed;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    if (closed) shim_thread_free(s);
    pthread_exit(NULL);
}
static inline DWORD GetLastError(void) { return (DWORD)errno; }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return 1; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    c->QuadPart = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
    return 1;
}
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c) { __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
#endif
