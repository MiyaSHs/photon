/* Photon CPU sampler (PHYS diagnosis, 2026-11-11): a sampling profiler of the engine's main thread, off unless PHOTON_CPU_SAMPLE=1.
 * A thread suspends the main thread about every millisecond, unwinds up to 16 frames with the modules' own unwind data
 * (RtlLookupFunctionEntry / RtlVirtualUnwind), and counts each function (its module and start offset): self for the innermost frame,
 * inclusive once per sample for every frame. After PHOTON_CPU_SAMPLE_DELAY seconds (default 75) it samples for PHOTON_CPU_SAMPLE_SECS
 * (default 20) and logs the top functions by inclusive and by self time as module+0xRVA (the function's start), for offline symbols
 * (llvm-symbolizer with the module's PDB). Read-only: it only reads the suspended thread's context and stack.
 * PHOTON_CPU_SAMPLE_THREADS=all also samples every other thread of the process (re-listed every 2 s) into a second table, reported
 * as "other": where worker threads (Jolt's job system, the material system's queue) spend their time. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

enum { MAXF = 16, NB = 8192 };
typedef struct { uintptr_t fn; uint32_t self, incl; } Slot;
static Slot g_tab[NB], g_tab_o[NB];
static uint32_t g_samples, g_lost, g_samples_o;
static Slot *g_cur = g_tab;

static Slot *slot_of(uintptr_t fn) {
    uint32_t h = (uint32_t)((fn >> 4) * 2654435761u) & (NB - 1);
    for (int i = 0; i < NB; i++, h = (h + 1) & (NB - 1)) {
        if (g_cur[h].fn == fn) return &g_cur[h];
        if (!g_cur[h].fn) { g_cur[h].fn = fn; return &g_cur[h]; }
    }
    return NULL;
}

static uintptr_t fn_start(uintptr_t pc) {   /* the function's start, from the unwind data; the pc itself without it (leaf / no data) */
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)pc, &base, NULL);
    return rf ? (uintptr_t)(base + rf->BeginAddress) : pc;
}

static void take(HANDLE th) {
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(th) == (DWORD)-1) return;
    if (!GetThreadContext(th, &c)) { ResumeThread(th); g_lost++; return; }
    uintptr_t seen[MAXF];
    int n = 0;
    for (int d = 0; d < MAXF && c.Rip; d++) {
        uintptr_t f = fn_start((uintptr_t)c.Rip);
        int dup = 0;
        for (int k = 0; k < n; k++) if (seen[k] == f) { dup = 1; break; }
        if (!dup) seen[n++] = f;
        Slot *s = slot_of(f);
        if (s) { if (d == 0) s->self++; if (!dup) s->incl++; }
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (rf) {
            PVOID hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &est, NULL);
        } else {   /* a leaf function: the return address is at the stack pointer */
            if (IsBadReadPtr((void *)c.Rsp, 8)) break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
    }
    ResumeThread(th);
    if (g_cur == g_tab) g_samples++; else g_samples_o++;
}

static int by_incl(const void *a, const void *b) { return (int)((const Slot *)b)->incl - (int)((const Slot *)a)->incl; }
static int by_self(const void *a, const void *b) { return (int)((const Slot *)b)->self - (int)((const Slot *)a)->self; }

static void report_tab(const Slot *tab, uint32_t samples, const char *what) {
    static Slot v[NB];
    int n = 0;
    for (int i = 0; i < NB; i++) if (tab[i].fn) v[n++] = tab[i];
    cpu_log("sampler: %u samples of %s (%u lost), %d functions", samples, what, g_lost, n);
    const char *tag = tab == g_tab ? "" : " other";
    for (int pass = 0; pass < 2; pass++) {
        qsort(v, (size_t)n, sizeof v[0], pass ? by_self : by_incl);
        for (int i = 0; i < n && i < 40; i++) {
            char w[160];
            uint32_t c = pass ? v[i].self : v[i].incl;
            if (!c) break;
            cpu_log("sampler%s %s %5.1f%% %s", tag, pass ? "self" : "incl", 100.0 * c / (samples ? samples : 1),
                    cpu_where((void *)v[i].fn, w, sizeof w));
        }
    }
}

static void report(void) {
    report_tab(g_tab, g_samples, "the main thread");
    if (g_samples_o) report_tab(g_tab_o, g_samples_o, "the other threads");
}

enum { MAXT = 96 };
static HANDLE g_oth[MAXT];
static int g_noth;
static void list_threads(DWORD main_tid) {   /* every other thread of this process, except the sampler itself */
    for (int i = 0; i < g_noth; i++) CloseHandle(g_oth[i]);
    g_noth = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok && g_noth < MAXT; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == main_tid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) g_oth[g_noth++] = h;
    }
    CloseHandle(snap);
}

static DWORD WINAPI run(LPVOID p) {
    (void)p;
    char e[16];
    int delay = GetEnvironmentVariableA("PHOTON_CPU_SAMPLE_DELAY", e, sizeof e) ? atoi(e) : 75;
    int secs = GetEnvironmentVariableA("PHOTON_CPU_SAMPLE_SECS", e, sizeof e) ? atoi(e) : 20;
    Sleep((DWORD)delay * 1000);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, cpu_host()->main_thread);
    if (!th) { cpu_log("sampler: cannot open the main thread"); return 0; }
    timeBeginPeriod(1);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)secs * 1000;
    cpu_log("sampler: sampling the main thread for %d s", secs);
    int all = GetEnvironmentVariableA("PHOTON_CPU_SAMPLE_THREADS", e, sizeof e) > 0 && !strcmp(e, "all");
    ULONGLONG relist = 0;
    while (GetTickCount64() < end) {
        g_cur = g_tab;
        take(th);
        if (all) {
            if (GetTickCount64() >= relist) { list_threads(cpu_host()->main_thread); relist = GetTickCount64() + 2000; }
            g_cur = g_tab_o;
            for (int i = 0; i < g_noth; i++) take(g_oth[i]);
            g_cur = g_tab;
        }
        Sleep(1);
    }
    for (int i = 0; i < g_noth; i++) CloseHandle(g_oth[i]);
    timeEndPeriod(1);
    CloseHandle(th);
    report();
    return 0;
}

void sampler_init(void) {
    char e[8];
    if (GetEnvironmentVariableA("PHOTON_CPU_SAMPLE", e, sizeof e) <= 0 || e[0] != '1') return;
    HANDLE t = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
