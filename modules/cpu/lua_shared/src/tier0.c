/* Photon CPU L3: the tier0.dll functions the original lua_shared imports (Error, Warning, ThreadInMainThread, g_pMemAlloc),
 * resolved by name from the already-loaded tier0.dll (the engine loads it long before lua_shared). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static HMODULE tier0(void) {
    static HMODULE h;
    if (!h) h = GetModuleHandleA("tier0.dll");
    return h;
}
static FARPROC sym(const char *name) { return tier0() ? GetProcAddress(tier0(), name) : NULL; }

void tier0_Error(const char *fmt, ...) {
    char b[4096];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    extern int l3_tracing; extern void l3_trace(const char *, ...);
    if (l3_tracing) l3_trace("FATAL Error: %s", b);
    void (*f)(const char *, ...) = (void (*)(const char *, ...))sym("Error");
    if (f) f("%s", b);
    else { OutputDebugStringA(b); TerminateProcess(GetCurrentProcess(), 1); }
}
void tier0_Warning(const char *fmt, ...) {
    char b[4096];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    void (*f)(const char *, ...) = (void (*)(const char *, ...))sym("Warning");
    if (f) f("%s", b); else OutputDebugStringA(b);
}
int tier0_ThreadInMainThread(void) {
    static int (*f)(void);
    if (!f) f = (int (*)(void))sym("ThreadInMainThread");
    return f ? (unsigned char)f() : 1;
}
/* IMemAlloc*: tier0 exports the variable g_pMemAlloc */
void **photon_memalloc(void) {
    static void ***p;
    if (!p) p = (void ***)sym("g_pMemAlloc");
    return *p;
}

/* PHOTON_L3_TRACE=1: L3 diagnostics, appended to photon_l3.log in the working directory */
int l3_tracing;   /* set from the environment in CreateInterface */
void l3_trace(const char *fmt, ...) {
    static FILE *f;
    if (!f) f = fopen("photon_l3.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fflush(f);
}
void l3_check_entry(void *self, void *L, void *mainL, const char *where) {
    if (!l3_tracing || L == mainL) return;
    l3_trace("%s: L %p is not the main state %p (its luabase %p, this %p)", where, L, mainL, L ? *(void **)((char *)L + 0x78) : NULL, self);
}

/* CHECK=1 builds: LuaJIT's assertion handler, replaced (libluajit's lj_assert.o is then not linked) so a failed API check also logs
 * the C call stack (module+offset) and the Lua traceback of the last state SetState saw, before aborting as LuaJIT does. */
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
void *l3_last_L;
static void trace_backtrace(void) {
    void *fr[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, fr, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fr[i], &m)) {
            GetModuleFileNameA(m, name, sizeof name);
            char *b = strrchr(name, '\\'); if (b) memmove(name, b + 1, strlen(b));
        }
        l3_trace("  c%02u %s+%#llx", i, name, (unsigned long long)((char *)fr[i] - (char *)m));
    }
}
/* TRACE: access violations and other hard faults, logged before the game's handlers see them (LuaJIT's own error exceptions,
 * 0xe24c4a0x, and debugger/C++ exceptions are skipped) */
static void *l3_watch_addr;
static LONG CALLBACK l3_veh(EXCEPTION_POINTERS *e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_SINGLE_STEP && l3_watch_addr) {   /* Wine may leave Dr6 clear, so any single-step trap counts */
        e->ContextRecord->Dr6 = 0;
        l3_trace("WATCH write: %p now %p (rip %p)", l3_watch_addr, *(void **)l3_watch_addr, (void *)e->ContextRecord->Rip);
        trace_backtrace();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_STACK_OVERFLOW || c == 0xc0000409) {
        void *a = e->ExceptionRecord->ExceptionAddress;
        HMODULE m = NULL; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)) {
            GetModuleFileNameA(m, name, sizeof name);
            char *b = strrchr(name, '\\'); if (b) memmove(name, b + 1, strlen(b));
        }
        l3_trace("EXCEPTION %#lx at %s+%#llx (thread %lu, info %#llx %#llx)", c, name, (unsigned long long)((char *)a - (char *)m),
                 GetCurrentThreadId(), (unsigned long long)e->ExceptionRecord->ExceptionInformation[0],
                 (unsigned long long)e->ExceptionRecord->ExceptionInformation[1]);
        trace_backtrace();
        extern void l3_ring_dump(void);
        l3_ring_dump();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
/* PHOTON_L3_WATCH=<module>+<hex rva>: a hardware write watchpoint (DR0, 8 bytes) on the calling thread, set from a helper thread
 * (a thread cannot reliably set its own debug registers); every write is logged with the C stack */
static HANDLE l3_watch_thread;
static DWORD WINAPI l3_watch_setter(void *unused) {
    (void)unused;
    CONTEXT c = { 0 };
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SuspendThread(l3_watch_thread);
    if (GetThreadContext(l3_watch_thread, &c)) {
        c.Dr0 = (DWORD64)l3_watch_addr;
        c.Dr7 = (c.Dr7 & ~0xf0003ull) | 1ull | (1ull << 16) | (3ull << 18);   /* L0, write, 8 bytes */
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!SetThreadContext(l3_watch_thread, &c)) l3_trace("WATCH: SetThreadContext failed %lu", GetLastError());
        else l3_trace("WATCH armed on %p (value %p)", l3_watch_addr, *(void **)l3_watch_addr);
    } else l3_trace("WATCH: GetThreadContext failed %lu", GetLastError());
    ResumeThread(l3_watch_thread);
    return 0;
}
void l3_trace_install(void) {
    AddVectoredExceptionHandler(1, l3_veh);
    const char *w = getenv("PHOTON_L3_WATCH");
    if (!w || !*w) return;
    char mod[64]; unsigned long long rva = 0;
    const char *plus = strchr(w, '+');
    if (!plus || (size_t)(plus - w) >= sizeof mod) return;
    memcpy(mod, w, (size_t)(plus - w)); mod[plus - w] = 0;
    rva = strtoull(plus + 1, NULL, 16);
    HMODULE m = GetModuleHandleA(mod);
    if (!m) { l3_trace("WATCH: %s not loaded", mod); return; }
    l3_watch_addr = (char *)m + rva;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &l3_watch_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE t = CreateThread(NULL, 0, l3_watch_setter, NULL, 0, NULL);
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
}
void lj_assert_fail(void *g, const char *file, int line, const char *func, const char *fmt, ...) {
    (void)g;
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    fprintf(stderr, "LuaJIT ASSERT %s:%d: %s: %s\n", file, line, func, msg);
    l3_trace("LuaJIT ASSERT %s:%d: %s: %s", file, line, func, msg);
    trace_backtrace();
    if (l3_last_L) {
        luaL_traceback((lua_State *)l3_last_L, (lua_State *)l3_last_L, NULL, 0);
        l3_trace("%s", lua_tolstring((lua_State *)l3_last_L, -1, NULL));
    }
    abort();
}
