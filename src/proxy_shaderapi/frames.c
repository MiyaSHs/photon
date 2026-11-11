#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "frames.h"
#include "wrap.h"
#include "photon_stats.h"
#include "slot_names.h"
#include "log.h"
#include "vlg.h"

extern PhotonStats g_photon_stats;   /* proxy.c */

/* CSV columns are fixed at the first frame so rows stay aligned even if classes appear later. */
static const char *const k_ifaces[] = { "ShaderApi030", "ShaderShadow010", "ShaderDevice001", "ShaderDeviceMgr001",
                                         "VShaderUtil001" };
enum { NIFACES = sizeof k_ifaces / sizeof k_ifaces[0] };
static const char *const k_headline[] = {   /* IShaderAPI slots reported per frame, looked up by name */
    "RenderPass", "BeginPass", "Bind", "BindTexture", "BindVertexShader", "BindPixelShader",
    "SetVertexShaderConstant", "SetPixelShaderConstant", "SetBooleanVertexShaderConstant", "SetIntegerVertexShaderConstant",
    "SetLight", "SetAmbientLightCube", "CommitPixelShaderLighting", "SetPixelShaderStateAmbientLightCube",
    "ClearBuffers", "ClearBuffersObeyStencil", "MatrixMode", "LoadMatrix", "GetDynamicMesh", "FlushBufferedPrimitives",
};
enum { NHEAD = sizeof k_headline / sizeof k_headline[0] };

static FILE *g_csv;
static int g_csv_tried;
static uint64_t g_frame;
static LARGE_INTEGER g_freq, g_t0, g_tprev;
static long g_head_slot[NHEAD];                 /* resolved slot per headline name, -1 if absent */
static uint64_t *g_prev[PHOTON_MAX_CLASSES];    /* last-frame counter snapshot per class */
static PhotonWrapClass *g_prev_cls[PHOTON_MAX_CLASSES];

static const PhotonSlotNames *table_for(const char *iface) {
    for (const PhotonSlotNames *t = photon_slot_names; t->iface; t++) if (strcmp(t->iface, iface) == 0) return t;
    return NULL;
}

const char *photon_slot_name(const char *iface, size_t slot, char *buf, size_t buflen) {
    const PhotonSlotNames *t = table_for(iface);
    if (t && slot < t->count) return t->names[slot];
    snprintf(buf, buflen, "internal_%zu", slot);
    return buf;
}

static int log_path(const char *file, char *out, size_t outlen) {
    char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) return 0;
    snprintf(out, outlen, "%s\\%s", dir, file);
    return 1;
}

static PhotonWrapClass *find_class(const char *name) {
    for (size_t i = 0; i < g_photon_stats.nclasses; i++)
        if (strcmp(g_photon_stats.classes[i]->name, name) == 0) return g_photon_stats.classes[i];
    return NULL;
}

static uint64_t *prev_for(PhotonWrapClass *c) {
    for (size_t i = 0; i < PHOTON_MAX_CLASSES; i++) {
        if (g_prev_cls[i] == c) return g_prev[i];
        if (!g_prev_cls[i]) {
            g_prev[i] = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, c->nslots * sizeof(uint64_t));
            if (!g_prev[i]) return NULL;
            g_prev_cls[i] = c;
            return g_prev[i];
        }
    }
    return NULL;
}

/* --- P0 CPU meters (frames.h) ----------------------------------------------------------------------------------- */
enum { R_MAIN, R_MQ, R_DXVK, R_DXVK_CS, R_OTHER, R_SELF, NROLES };   /* R_SELF = the meter thread (excluded) */
/* RING-1 intervals = a 1 s rolling window. 500 ms: a sample costs ~12 ms of meter-thread time in game under Wine
 * (100 threads, NtQuerySystemInformation; run-20261103-044942), so 2 Hz keeps the meter near 2% of one core. */
enum { METER_PERIOD_MS = 500, RING = 3, MAX_THREADS = 256 };
typedef struct { DWORD tid; HANDLE h; int cls; int gen, idx, retries; uint64_t last; } MThread;
typedef struct { LARGE_INTEGER qpc; uint64_t cum[NROLES]; uint64_t proc; } MSample;
typedef struct {   /* published by the meter thread, read by photon_frame_end */
    int valid;                         /* a window of >= 1 interval exists */
    double window_ms, pct[NROLES], proc_pct;
    int has_mq, has_cs, ndxvk;
} MPub;
typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR *);
typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
typedef LONG (NTAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
typedef LONG (NTAPI *NtQueryTimerResolutionFn)(PULONG, PULONG, PULONG);

static volatile DWORD g_main_tid, g_present_tid;   /* main = the thread that loaded us (first CreateInterface) */
static volatile LONG g_perf_on;                    /* the CSV is open: meters and the RenderPass timers run */
static volatile LONG64 g_mq_photon_ticks, g_mq_rp_ticks, g_mq_ours_ticks;   /* QPC ticks, read-and-reset per frame */
static SRWLOCK g_pub_lock = SRWLOCK_INIT;
static MPub g_pub;
static MThread g_thr[MAX_THREADS];
static int g_nthr, g_gen;
static MSample g_ring[RING];
static int g_ring_n, g_ring_head;
static uint64_t g_cum[NROLES];
static DWORD g_mq_tid;
static uint64_t g_step_gcd, g_step_min = ~0ull;    /* GetThreadTimes granularity, from the observed nonzero deltas */
static double g_sample_us_sum, g_sample_us_max, g_period_ms_sum;
static long long g_nsamples;
static GetThreadDescriptionFn g_gtd;
static NtQueryInformationThreadFn g_nqit;
/* S1 (hang on quit): the meter thread holds its own reference to this DLL (taken in meter_start, released by
 * FreeLibraryAndExitThread), so the game's FreeLibrary at quit can never unmap the code under it; it is stopped from
 * IShaderDeviceMgr::Shutdown/Disconnect (photon_frames_shutdown), outside the loader lock, never from DllMain. */
static HANDLE volatile g_meter_thread;   /* the running meter thread (NULL: none, or stopped) */
static HANDLE g_meter_stop;              /* manual-reset event: set = stop */
static volatile LONG g_meter_pinned;     /* 1 while the meter thread holds its module reference */

void photon_frames_note_main(unsigned long tid) { if (!g_main_tid) g_main_tid = tid; }
int photon_perf_on(void) { return g_perf_on != 0; }
void photon_mq_photon_add(int64_t photon_ticks, int64_t renderpass_ticks, int ours) {
    InterlockedExchangeAdd64(&g_mq_photon_ticks, photon_ticks);
    if (ours) InterlockedExchangeAdd64(&g_mq_ours_ticks, photon_ticks);
    InterlockedExchangeAdd64(&g_mq_rp_ticks, renderpass_ticks);
}

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static uint64_t gcd64(uint64_t a, uint64_t b) { while (b) { uint64_t t = a % b; a = b; b = t; } return a; }

static const char *role_name(int r) {
    static const char *const n[] = { "main", "mq", "dxvk", "dxvk-cs", "other", "self" };
    return r >= 0 && r < NROLES ? n[r] : "?";
}

/* DXVK threads: named "dxvk-*" through SetThreadDescription (strings in Proton-GE 11's d3d9.dll: dxvk-cs, dxvk-submit,
 * dxvk-queue, dxvk-shader-N, ...); without a name, a start address inside a DXVK module counts (no dxvk-cs split). */
static int classify(HANDLE h, DWORD tid, char *name, size_t nlen, char *mod, size_t mlen) {
    name[0] = mod[0] = 0;
    if (g_gtd && h) {
        PWSTR w = NULL;
        if (SUCCEEDED(g_gtd(h, &w)) && w) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, name, (int)nlen, NULL, NULL);
            name[nlen - 1] = 0;
            LocalFree(w);
        }
    }
    void *start = NULL;
    if (g_nqit && h && g_nqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) == 0 && start) {
        HMODULE m = NULL;
        char path[MAX_PATH];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)start, &m) && GetModuleFileNameA(m, path, sizeof path)) {
            const char *b = strrchr(path, '\\');
            snprintf(mod, mlen, "%s+0x%llx", b ? b + 1 : path, (unsigned long long)((uintptr_t)start - (uintptr_t)m));
        }
    }
    (void)tid;
    if (strcmp(name, "photon-meter") == 0) return R_SELF;
    if (strcmp(name, "dxvk-cs") == 0) return R_DXVK_CS;
    if (strncmp(name, "dxvk-", 5) == 0) return R_DXVK;
    if (!name[0] && (_strnicmp(mod, "d3d9.dll+", 9) == 0 || _strnicmp(mod, "dxgi.dll+", 9) == 0 ||
                     _strnicmp(mod, "d3d11.dll+", 10) == 0)) return R_DXVK;
    return R_OTHER;
}

/* The process's threads and their CPU times. Primary: one NtQuerySystemInformation(SystemProcessInformation) call,
 * which carries per-thread kernel/user times (Wine fills them from the same /proc source as GetThreadTimes; in game
 * the main thread read 740 ms by this call and 770 ms by GetThreadTimes a moment later). Measured under Wine 11.20
 * with 21 threads: about 0.85 ms per call, against ~2 ms for CreateToolhelp32Snapshot plus ~10 ms for the Thread32Next
 * walk plus ~0.12 ms per GetThreadTimes. Fallback (the call fails): Toolhelp32 + GetThreadTimes. */
typedef LONG (NTAPI *NtQuerySystemInformationFn)(int, PVOID, ULONG, PULONG);
typedef struct {   /* SYSTEM_THREAD_INFORMATION (x64, 80 bytes) */
    LARGE_INTEGER kernel, user, create; ULONG wait; PVOID start; HANDLE pid, tid; LONG prio, base; ULONG cs, state, why;
} SysThread;
static NtQuerySystemInformationFn g_nqsi;
static unsigned char *g_spi;
static ULONG g_spi_len;
static DWORD g_ltid[MAX_THREADS];
static uint64_t g_ltime[MAX_THREADS];
static int g_lhave_times, g_lmode_logged;
static int list_threads(DWORD pid) {
    int n = 0;
    for (int tries = 0; g_nqsi && tries < 4; tries++) {
        if (!g_spi) { g_spi_len = g_spi_len ? g_spi_len : 1u << 18; g_spi = HeapAlloc(GetProcessHeap(), 0, g_spi_len); }
        if (!g_spi) break;
        ULONG need = 0;
        LONG st = g_nqsi(5 /* SystemProcessInformation */, g_spi, g_spi_len, &need);
        if (st == (LONG)0xC0000004L) {   /* STATUS_INFO_LENGTH_MISMATCH: grow */
            HeapFree(GetProcessHeap(), 0, g_spi);
            g_spi = NULL;
            g_spi_len = need > g_spi_len ? need + 65536 : g_spi_len * 2;
            continue;
        }
        if (st != 0) break;
        for (unsigned char *q = g_spi;;) {
            ULONG next = *(ULONG *)q, nth = *(ULONG *)(q + 4);
            HANDLE upid = *(HANDLE *)(q + 0x50);   /* UniqueProcessId */
            if ((DWORD)(uintptr_t)upid == pid) {
                const SysThread *t = (const SysThread *)(q + 0x100);
                for (ULONG i = 0; i < nth && n < MAX_THREADS; i++, n++) {
                    g_ltid[n] = (DWORD)(uintptr_t)t[i].tid;
                    g_ltime[n] = (uint64_t)t[i].kernel.QuadPart + (uint64_t)t[i].user.QuadPart;
                }
                break;
            }
            if (!next) break;
            q += next;
        }
        if (n) {
            g_lhave_times = 1;
            if (!g_lmode_logged++) photon_log("meters: thread list and times from NtQuerySystemInformation (%d threads)", n);
            return n;
        }
        break;
    }
    g_lhave_times = 0;
    if (!g_lmode_logged++) photon_log("meters: NtQuerySystemInformation unusable: Toolhelp32 + GetThreadTimes");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = { .dwSize = sizeof te };
    for (BOOL ok = Thread32First(snap, &te); ok && n < MAX_THREADS; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid) g_ltid[n++] = te.th32ThreadID;
    CloseHandle(snap);
    return n;
}
static uint64_t thread_time(const MThread *e, int j) {
    if (g_lhave_times) return g_ltime[j];
    FILETIME c, x, k, u;
    return e->h && GetThreadTimes(e->h, &c, &x, &k, &u) ? ft64(k) + ft64(u) : 0;
}

static void meter_sample(void) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId(), main_tid = g_main_tid, pres = g_present_tid;
    int first = g_nsamples == 0;
    g_gen++;
    /* the render thread: the Present thread once it differs from main (queued mode); it stays the MatQueue thread
     * through short sync fallbacks (screenshots). A new one restarts the window (thread roles changed). */
    if (pres && main_tid && pres != main_tid && pres != g_mq_tid) {
        photon_log("meters: render (Present) thread %lu differs from main %lu: MatQueue thread%s", pres, main_tid,
                   g_mq_tid ? " changed, window restarted" : "");
        g_mq_tid = pres;
        g_ring_n = 0;
    }
    int n = list_threads(pid);
    for (int j = 0; j < n; j++) {
        DWORD tid = g_ltid[j];
        int i = 0;
        while (i < g_nthr && g_thr[i].tid != tid) i++;
        if (i == g_nthr) {
            if (g_nthr == MAX_THREADS) continue;
            MThread *e = &g_thr[g_nthr++];
            e->tid = tid;
            e->h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            char name[64], mod[MAX_PATH + 24];
            e->cls = tid == self ? R_SELF : classify(e->h, tid, name, sizeof name, mod, sizeof mod);
            if (tid == self) { strcpy(name, "photon-meter"); mod[0] = 0; }
            /* a thread found after the first sample was created since the previous one: all its time is new */
            e->last = first ? thread_time(e, j) : 0;
            e->retries = name[0] ? 0 : 8;   /* DXVK names its threads from inside them: look again a few times */
            static int nlogged;
            if (nlogged++ < 96)
                photon_log("meters: thread %lu name \"%s\" start %s -> %s%s", tid, name, mod[0] ? mod : "?",
                           role_name(tid == main_tid ? R_MAIN : e->cls), e->h ? "" : " (OpenThread failed)");
        } else if (g_thr[i].retries > 0 && g_thr[i].cls != R_SELF) {
            MThread *e = &g_thr[i];
            char name[64], mod[MAX_PATH + 24];
            int cls = classify(e->h, tid, name, sizeof name, mod, sizeof mod);
            e->retries = name[0] ? 0 : e->retries - 1;
            if (name[0]) photon_log("meters: thread %lu named later \"%s\" -> %s", tid, name,
                                    role_name(tid == main_tid ? R_MAIN : cls));
            e->cls = cls;
        }
        g_thr[i].gen = g_gen;
        g_thr[i].idx = j;
    }
    for (int i = 0; i < g_nthr; i++) {
        MThread *e = &g_thr[i];
        if (e->gen != g_gen) {   /* exited (Wine reports 0 times for a dead thread): drop it, its tid may be reused */
            if (e->h) CloseHandle(e->h);
            g_thr[i--] = g_thr[--g_nthr];
            continue;
        }
        uint64_t t = thread_time(e, e->idx), d = t > e->last ? t - e->last : 0;
        e->last = t > e->last ? t : e->last;
        if (d) { g_step_gcd = gcd64(g_step_gcd, d); if (d < g_step_min) g_step_min = d; }
        int r = e->tid == main_tid ? R_MAIN : e->tid == g_mq_tid ? R_MQ : e->cls;
        g_cum[r] += d;
    }
    if (first && g_lhave_times)   /* both sources read the same Wine /proc counters; show it once */
        for (int i = 0; i < g_nthr; i++)
            if (g_thr[i].tid == main_tid && g_thr[i].h) {
                FILETIME c, x, k, u;
                if (GetThreadTimes(g_thr[i].h, &c, &x, &k, &u))
                    photon_log("meters: main thread CPU %.1f ms by NtQuerySystemInformation, %.1f ms by GetThreadTimes",
                               g_thr[i].last / 1e4, (ft64(k) + ft64(u)) / 1e4);
            }
    FILETIME c, x, k, u;
    uint64_t proc = GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u) ? ft64(k) + ft64(u) : 0;
    MSample *s = &g_ring[g_ring_head];
    s->qpc = t0;
    memcpy(s->cum, g_cum, sizeof g_cum);
    s->proc = proc;
    const MSample *o = g_ring_n ? &g_ring[(g_ring_head + RING - (g_ring_n < RING ? g_ring_n : RING - 1)) % RING] : NULL;
    g_ring_head = (g_ring_head + 1) % RING;
    if (g_ring_n < RING) g_ring_n++;
    MPub p = { 0 };
    if (o && s->qpc.QuadPart > o->qpc.QuadPart) {
        double wall = (double)(s->qpc.QuadPart - o->qpc.QuadPart) * 1e7 / (double)g_freq.QuadPart;   /* 100 ns */
        p.valid = 1;
        p.window_ms = wall / 1e4;
        for (int r = 0; r < NROLES; r++) p.pct[r] = (double)(s->cum[r] - o->cum[r]) * 100.0 / wall;
        p.proc_pct = s->proc >= o->proc ? (double)(s->proc - o->proc) * 100.0 / wall : 0;
    }
    p.has_mq = g_mq_tid != 0;
    for (int i = 0; i < g_nthr; i++) {
        if (g_thr[i].cls == R_DXVK || g_thr[i].cls == R_DXVK_CS) p.ndxvk++;
        if (g_thr[i].cls == R_DXVK_CS) p.has_cs = 1;
    }
    AcquireSRWLockExclusive(&g_pub_lock);
    g_pub = p;
    ReleaseSRWLockExclusive(&g_pub_lock);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)g_freq.QuadPart;
    g_sample_us_sum += us;
    if (us > g_sample_us_max) g_sample_us_max = us;
    g_nsamples++;
}

static void meter_summary(const char *why) {
    if (!g_nsamples) return;
    photon_log("meters summary (%s): %lld samples, sample cost mean %.1f us max %.1f us, period mean %.1f ms (wait %d);"
               " %d threads tracked, main %lu, MatQueue %lu; GetThreadTimes step gcd %llu x100ns (%.3f ms), min nonzero"
               " delta %.3f ms", why, g_nsamples, g_sample_us_sum / (double)g_nsamples, g_sample_us_max,
               g_nsamples > 1 ? g_period_ms_sum / (double)(g_nsamples - 1) : 0.0, METER_PERIOD_MS, g_nthr, g_main_tid,
               g_mq_tid, (unsigned long long)g_step_gcd, (double)g_step_gcd / 1e4,
               g_step_min == ~0ull ? 0.0 : (double)g_step_min / 1e4);
}

/* S1b (hang while loading): with PHOTON_STALL_DUMP=<seconds> (tests only; run_gmod.sh sets it), frames stopping for that long
 * logs the main and Present threads' stacks, again at twice that, from this thread, which stays alive through the hang. Nothing
 * else can see them: winedbg does not list gmod.exe. A suspended thread may hold the loader lock, and the unwinder and the module
 * lookup may want it. So the raw scan runs first, touching only the stack and VirtualQuery while suspended, and is logged after the
 * resume; only then the unwind, which is what could deadlock. Frames from the scan include stale values; the unwind may stop early. */
enum { STALL_ADDRS = 48 };
static ULONGLONG g_stall_ms;

static void log_addrs(const char *what, DWORD tid, const DWORD64 *a, int n) {
    for (int i = 0; i < n; i++) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)a[i], &m) && m) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(m, path, sizeof path)) {
                const char *b = strrchr(path, '\\');
                snprintf(name, sizeof name, "%s", b ? b + 1 : path);
            }
        }
        photon_log("stall: %s %lu #%d %s+0x%llx", what, tid, i, name,
                   (unsigned long long)(a[i] - (DWORD64)(uintptr_t)m));
    }
}

static int is_code(DWORD64 v) {
    MEMORY_BASIC_INFORMATION mi;
    if (v < 0x10000 || v >= 0x7fffffffffffull || !VirtualQuery((LPCVOID)(uintptr_t)v, &mi, sizeof mi)) return 0;
    return mi.State == MEM_COMMIT && mi.Type == MEM_IMAGE
        && (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static void stall_dump_thread(const char *role, DWORD tid) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { photon_log("stall: %s %lu: OpenThread failed (%lu)", role, tid, GetLastError()); return; }
    DWORD64 a[STALL_ADDRS], rip = 0, rsp = 0;
    int n = 0, got = 0;
    CONTEXT c;
    if (SuspendThread(h) != (DWORD)-1) {
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
            got = 1; rip = c.Rip; rsp = c.Rsp;
            MEMORY_BASIC_INFORMATION mi;
            if ((rsp & 7) == 0 && VirtualQuery((LPCVOID)(uintptr_t)rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                const DWORD64 *p = (const DWORD64 *)(uintptr_t)rsp;
                const DWORD64 *end = (const DWORD64 *)(uintptr_t)((uintptr_t)mi.BaseAddress + mi.RegionSize);
                for (; p < end && n < STALL_ADDRS; p++) if (is_code(*p)) a[n++] = *p;
            }
        }
        ResumeThread(h);
    }
    if (!got) { photon_log("stall: %s %lu: no context (%lu)", role, tid, GetLastError()); CloseHandle(h); return; }
    photon_log("stall: %s %lu rip 0x%llx rsp 0x%llx, stack scan (%d values):", role, tid, (unsigned long long)rip,
               (unsigned long long)rsp, n);
    log_addrs("scan", tid, &rip, 1);
    log_addrs("scan", tid, a, n);
    n = 0;
    if (SuspendThread(h) != (DWORD)-1) {
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &c)) {
            while (n < STALL_ADDRS && c.Rip) {
                a[n++] = c.Rip;
                DWORD64 base = 0, frame = 0;
                PVOID hd = NULL;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                if (!f) break;   /* leaf frames do not occur past the first, and Wine's unix side has no entries */
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &frame, NULL);
            }
        }
        ResumeThread(h);
    }
    photon_log("stall: %s %lu unwind (%d frames):", role, tid, n);
    log_addrs("unwind", tid, a, n);
    CloseHandle(h);
}

static void stall_check(void) {
    static uint64_t seen;
    static ULONGLONG since;
    static int dumps;
    if (!g_stall_ms) return;
    ULONGLONG now = GetTickCount64();
    if (g_frame != seen || !since) { seen = g_frame; since = now; dumps = 0; return; }
    if (!seen || dumps >= 2 || now - since < g_stall_ms * (ULONGLONG)(dumps + 1)) return;
    dumps++;
    DWORD m = g_main_tid, p = g_present_tid;
    photon_log("stall: no frame for %llu s at frame %llu (dump %d of 2)", (now - since) / 1000, (unsigned long long)seen, dumps);
    if (m) stall_dump_thread("main", m);
    if (p && p != m) stall_dump_thread("present", p);
    /* S1b's main thread waits inside DXVK's Present, so what it waits for is one of DXVK's own threads; the shader compilers are idle */
    int n = 0;
    for (int i = 0; i < g_nthr && n < 8; i++) {
        char name[64], mod[MAX_PATH];
        if (g_thr[i].cls != R_DXVK && g_thr[i].cls != R_DXVK_CS) continue;
        classify(g_thr[i].h, g_thr[i].tid, name, sizeof name, mod, sizeof mod);
        if (strncmp(name, "dxvk-shader", 11) == 0) continue;
        stall_dump_thread(name[0] ? name : "dxvk", g_thr[i].tid);
        n++;
    }
}

static DWORD WINAPI meter_thread(LPVOID arg) {   /* arg = this DLL, referenced for the thread (meter_start) */
    HMODULE kb = GetModuleHandleA("kernelbase.dll"), k32 = GetModuleHandleA("kernel32.dll");
    SetThreadDescriptionFn std = (SetThreadDescriptionFn)(void *)GetProcAddress(kb ? kb : k32, "SetThreadDescription");
    if (!std && k32) std = (SetThreadDescriptionFn)(void *)GetProcAddress(k32, "SetThreadDescription");
    if (std) std(GetCurrentThread(), L"photon-meter");
    /* timer facts for the log (Wine/Proton): QPC frequency and call cost, the NT timer resolution */
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 1000; i++) QueryPerformanceCounter(&b);
    QueryPerformanceCounter(&b);
    ULONG tmin = 0, tmax = 0, tcur = 0;
    NtQueryTimerResolutionFn nqtr = (NtQueryTimerResolutionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                                                    "NtQueryTimerResolution");
    LONG st = nqtr ? nqtr(&tmin, &tmax, &tcur) : -1;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    photon_log("meters: on (thread %lu, every %d ms, %d-sample window); QPC %lld Hz, %.1f ns/call; NtQueryTimerResolution"
               " 0x%lx min %.4f ms max %.4f ms cur %.4f ms; %lu logical CPUs; GetThreadDescription %s; main thread %lu",
               GetCurrentThreadId(), METER_PERIOD_MS, RING, (long long)g_freq.QuadPart,
               (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)g_freq.QuadPart / 1001.0, (unsigned long)st,
               tmin / 1e4, tmax / 1e4, tcur / 1e4, si.dwNumberOfProcessors, g_gtd ? "available" : "missing", g_main_tid);
    char stall[16];
    if (GetEnvironmentVariableA("PHOTON_STALL_DUMP", stall, sizeof stall) > 0) g_stall_ms = (ULONGLONG)atoi(stall) * 1000;
    LARGE_INTEGER prev = { 0 };
    do {
        meter_sample();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (prev.QuadPart) g_period_ms_sum += (double)(now.QuadPart - prev.QuadPart) * 1e3 / (double)g_freq.QuadPart;
        prev = now;
        /* after 2 s, then about once a minute (never from DllMain: a thread killed at exit may hold the log lock) */
        if (g_nsamples == 4 || g_nsamples % 120 == 0) meter_summary("periodic");
        stall_check();
    } while (WaitForSingleObject(g_meter_stop, METER_PERIOD_MS) == WAIT_TIMEOUT);
    meter_summary("stop");
    InterlockedExchange(&g_meter_pinned, 0);
    /* drop our module reference and exit without ever returning into this DLL's code */
    FreeLibraryAndExitThread((HMODULE)arg, 0);
    return 0;
}

/* PHOTON_PROFILE=<seconds>: a sampling profiler for the main thread (laptop, analysis/19). From that many seconds after the meters start,
 * the main thread's RIP is read about every millisecond (SuspendThread, GetThreadContext, ResumeThread); at the meters' stop the share per
 * module is logged ("prof:"), and the hottest 64-byte blocks of this DLL and of the stock shader API. Off unless set. */
enum { PROF_MAX = 1 << 19 };
static DWORD64 *g_prof_rip;
static uint32_t *g_prof_frm;
/* PHOTON_PROFILE_STACK=1: up to PROF_STK callers per sample (prof_stacks.txt), unwound on a copy of the top of the main thread's stack
 * after it is resumed, so the sampler never calls into the loader while the main thread is suspended */
enum { PROF_STK = 20, PROF_STKCOPY = 65536 };
static DWORD64 *g_prof_stk;   /* the frame each sample fell in (prof_frames.txt, one line per sample: tools/prof_spikes.py) */
static volatile LONG g_prof_n;
static DWORD g_prof_delay_ms;

static void prof_report(void) {
    LONG n = g_prof_n;
    if (!g_prof_rip || n <= 0) { photon_log("prof: no samples"); return; }
    struct { HMODULE m; long c; } mods[96] = { { 0 } };
    int nm = 0;
    HMODULE self = NULL, stock = GetModuleHandleA("shaderapidx9_stock.dll");
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)prof_report, &self);
    enum { HOT = 4096 };
    static struct { uintptr_t key; long c; } hot[HOT];
    for (LONG i = 0; i < n; i++) {
        HMODULE m = NULL;
        if (photon_wrap_is_thunk((uintptr_t)g_prof_rip[i])) m = (HMODULE)1;   /* our wrapper thunks: reported as their own row */
        else GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_prof_rip[i], &m);
        int k = 0;
        while (k < nm && mods[k].m != m) k++;
        if (k == nm) { if (nm == 96) continue; mods[nm].m = m; nm++; }
        mods[k].c++;
        if (m && (m == self || m == stock)) {   /* hot blocks: (which module, RVA / 64) in an open-addressed table */
            uintptr_t key = ((uintptr_t)(m == stock) << 63) | (((uintptr_t)g_prof_rip[i] - (uintptr_t)m) >> 6);
            size_t h = (key * 0x9E3779B97F4A7C15ull) >> 52;
            for (int t = 0; t < HOT; t++, h = (h + 1) & (HOT - 1))
                if (!hot[h].c || hot[h].key == key) { hot[h].key = key; hot[h].c++; break; }
        }
    }
    for (int a = 0; a < nm; a++) for (int b = a + 1; b < nm; b++) if (mods[b].c > mods[a].c) { __typeof__(mods[0]) t = mods[a]; mods[a] = mods[b]; mods[b] = t; }
    photon_log("prof: %ld samples of the main thread (%lu), from %lu s after the meters started", (long)n, (unsigned long)g_main_tid,
               (unsigned long)(g_prof_delay_ms / 1000));
    for (int a = 0; a < nm && a < 20; a++) {
        char name[MAX_PATH] = "(no module: JIT, heap or thunk)";
        if (mods[a].m == (HMODULE)1) strcpy(name, "(Photon wrapper thunks)");
        else if (mods[a].m) GetModuleFileNameA(mods[a].m, name, sizeof name);
        const char *base = strrchr(name, '\\');
        photon_log("prof:   %5.1f%%  %s", 100.0 * mods[a].c / n, base ? base + 1 : name);
    }
    for (int r = 0; r < 12; r++) {
        int best = -1;
        for (int t = 0; t < HOT; t++) if (hot[t].c && (best < 0 || hot[t].c > hot[best].c)) best = t;
        if (best < 0) break;
        photon_log("prof:   hot %s+0x%llx  %.2f%%", (hot[best].key >> 63) ? "shaderapidx9_stock" : "shaderapidx9(proxy)",
                   (unsigned long long)((hot[best].key & ~(1ull << 63)) << 6), 100.0 * hot[best].c / n);
        hot[best].c = 0;
    }
}

/* The nearest exported name at or below an address in module m (ntdll's hot spots by function, laptop analysis/19). */
static const char *export_below(HMODULE m, uintptr_t a, uintptr_t *off) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)m + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return NULL;
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)((uint8_t *)m + dd.VirtualAddress);
    DWORD *names = (DWORD *)((uint8_t *)m + ed->AddressOfNames), *funcs = (DWORD *)((uint8_t *)m + ed->AddressOfFunctions);
    WORD *ords = (WORD *)((uint8_t *)m + ed->AddressOfNameOrdinals);
    uintptr_t rva = a - (uintptr_t)m, best = 0; const char *bn = NULL;
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        uintptr_t f = funcs[ords[i]];
        if (f <= rva && f >= best) { best = f; bn = (const char *)m + names[i]; }
    }
    *off = rva - best;
    return bn;
}

/* Every sample inside this DLL as an RVA, to prof_proxy.txt in the log directory: tools/prof_proxy.py maps them to functions with nm */
static void prof_proxy_dump(void) {
    HMODULE self = NULL;
    char path[MAX_PATH + 32];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)prof_proxy_dump, &self) ||
        !log_path("prof_proxy.txt", path, sizeof path)) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    LONG n = g_prof_n;
    uintptr_t lo = (uintptr_t)self;
    for (LONG i = 0; i < n; i++) {
        HMODULE m = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_prof_rip[i], &m) && m == self)
            fprintf(f, "%llx\n", (unsigned long long)(g_prof_rip[i] - lo));
    }
    fprintf(f, "total %ld\n", (long)n);
    fclose(f);
}

/* Every sample as "module rva" (all modules), to prof_all.txt: tools/prof_all.py maps engine, client and the other game DLLs to the
 * ghidra/out function lists */
static void prof_all_dump(void) {
    char path[MAX_PATH + 32];
    if (!log_path("prof_all.txt", path, sizeof path)) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    LONG n = g_prof_n;
    HMODULE last = NULL;
    char name[MAX_PATH] = "?";
    for (LONG i = 0; i < n; i++) {
        HMODULE m = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_prof_rip[i], &m)) {
            fprintf(f, "- 0\n");
            continue;
        }
        if (m != last) {
            char full[MAX_PATH];
            DWORD k = GetModuleFileNameA(m, full, sizeof full);
            const char *b = full;
            for (DWORD j = 0; j < k; j++) if (full[j] == '\\' || full[j] == '/') b = full + j + 1;
            snprintf(name, sizeof name, "%s", k ? b : "?");
            last = m;
        }
        fprintf(f, "%s %llx\n", name, (unsigned long long)(g_prof_rip[i] - (uintptr_t)m));
    }
    fclose(f);
    if (g_prof_frm && log_path("prof_frames.txt", path, sizeof path) && (f = fopen(path, "w"))) {
        for (LONG i = 0; i < n; i++) fprintf(f, "%lu\n", (unsigned long)g_prof_frm[i]);
        fclose(f);
    }
    if (!g_prof_stk || !log_path("prof_stacks.txt", path, sizeof path) || !(f = fopen(path, "w"))) return;
    for (LONG i = 0; i < n; i++) {   /* one line per sample: "module rva" per caller, ";"-separated, innermost first */
        const DWORD64 *st = g_prof_stk + (size_t)i * PROF_STK;
        for (int d = 0; d < PROF_STK && st[d]; d++) {
            HMODULE m = NULL;
            char full[MAX_PATH]; const char *b = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)st[d], &m)) {
                DWORD k = GetModuleFileNameA(m, full, sizeof full);
                b = full;
                for (DWORD j = 0; j < k; j++) if (full[j] == '\\' || full[j] == '/') b = full + j + 1;
            }
            fprintf(f, "%s%s %llx", d ? ";" : "", b, (unsigned long long)(st[d] - (uintptr_t)m));
        }
        fputc('\n', f);
    }
    fclose(f);
}

static void prof_ntdll(void) {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    LONG n = g_prof_n;
    if (!nt || n <= 0) return;
    enum { NF = 512 };
    static struct { const char *name; long c; } fn[NF];
    int nf = 0; long tot = 0;
    for (LONG i = 0; i < n; i++) {
        HMODULE m = NULL;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)g_prof_rip[i], &m) || m != nt) continue;
        uintptr_t off; const char *name = export_below(nt, (uintptr_t)g_prof_rip[i], &off);
        if (!name) name = "?";
        int k = 0;
        while (k < nf && fn[k].name != name) k++;
        if (k == nf) { if (nf == NF) continue; fn[nf].name = name; fn[nf].c = 0; nf++; }
        fn[k].c++; tot++;
    }
    for (int r = 0; r < 12 && tot; r++) {
        int best = -1;
        for (int k = 0; k < nf; k++) if (fn[k].c && (best < 0 || fn[k].c > fn[best].c)) best = k;
        if (best < 0) break;
        photon_log("prof:   ntdll %-40s %5.2f%% of the main thread", fn[best].name, 100.0 * fn[best].c / n);
        fn[best].c = 0;
    }
}

static DWORD g_prof_dur_ms;   /* 0: until exit */
static int g_prof_mq;   /* PHOTON_PROFILE_MQ=1: sample the MatQueue (render) thread instead of main, once the meters know it */
static DWORD WINAPI prof_thread(LPVOID arg) {
    HANDLE h = g_prof_mq ? NULL : OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
    if (g_prof_mq && WaitForSingleObject(g_meter_stop, g_prof_delay_ms) == WAIT_TIMEOUT) {
        g_prof_delay_ms = 0;
        if (g_mq_tid) h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mq_tid);
        photon_log("prof: sampling the MatQueue thread %lu%s", (unsigned long)g_mq_tid, h ? "" : " (not known: no samples)");
    }
    if (h && WaitForSingleObject(g_meter_stop, g_prof_delay_ms) == WAIT_TIMEOUT) {
        DWORD t0 = GetTickCount();
        do {
            if (g_prof_n >= PROF_MAX) break;
            if (g_prof_dur_ms && GetTickCount() - t0 >= g_prof_dur_ms) break;   /* PHOTON_PROFILE=start,seconds: a window */
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
                static uint8_t copy[PROF_STKCOPY];
                size_t ncopy = 0;
                if (g_prof_stk) c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                int got = GetThreadContext(h, &c) != 0;
                if (got && g_prof_stk) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery((LPCVOID)c.Rsp, &mbi, sizeof mbi)) {
                        size_t avail = (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)c.Rsp);
                        ncopy = avail < sizeof copy ? avail : sizeof copy;
                        memcpy(copy, (const void *)c.Rsp, ncopy);
                    }
                }
                ResumeThread(h);
                if (got) {
                    LONG i = g_prof_n;
                    if (g_prof_frm) g_prof_frm[i] = (uint32_t)g_frame;
                    g_prof_rip[i] = c.Rip;
                    if (g_prof_stk) {
                        DWORD64 *out = g_prof_stk + (size_t)i * PROF_STK;
                        memset(out, 0, PROF_STK * sizeof *out);
                        /* the copy stands in for the stack: Rsp is moved into it, so Rsp-relative frames unwind from the copy */
                        CONTEXT u = c;
                        DWORD64 delta = (DWORD64)(uintptr_t)copy - c.Rsp;
                        u.Rsp += delta;
                        if (c.Rbp >= c.Rsp && c.Rbp < c.Rsp + ncopy) u.Rbp += delta;   /* frame-pointer functions unwind through Rbp */
                        for (int d = 0; d < PROF_STK && ncopy; d++) {
                            DWORD64 base = 0, pc = u.Rip;
                            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &base, NULL);
                            if (rf) {
                                PVOID hd; DWORD64 est;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, rf, &u, &hd, &est, NULL);
                            } else {
                                if (u.Rsp < (DWORD64)(uintptr_t)copy || u.Rsp + 8 > (DWORD64)(uintptr_t)copy + ncopy) break;
                                u.Rip = *(DWORD64 *)(uintptr_t)u.Rsp; u.Rsp += 8;
                            }
                            if (u.Rsp < (DWORD64)(uintptr_t)copy || u.Rsp > (DWORD64)(uintptr_t)copy + ncopy || !u.Rip) break;
                            out[d] = u.Rip;
                        }
                    }
                    g_prof_n = i + 1;
                }
            }
        } while (WaitForSingleObject(g_meter_stop, 1) == WAIT_TIMEOUT);
    }
    if (h) CloseHandle(h);
    prof_report();
    prof_ntdll();
    prof_proxy_dump();
    prof_all_dump();
    FreeLibraryAndExitThread((HMODULE)arg, 0);
    return 0;
}

static void prof_start(void) {
    char e[16];
    if (GetEnvironmentVariableA("PHOTON_PROFILE", e, sizeof e) <= 0 || !g_main_tid) return;
    g_prof_delay_ms = (DWORD)atoi(e) * 1000;
    { char z[4]; g_prof_mq = GetEnvironmentVariableA("PHOTON_PROFILE_MQ", z, sizeof z) > 0 && z[0] == '1'; }
    const char *comma = strchr(e, ',');
    g_prof_dur_ms = comma ? (DWORD)atoi(comma + 1) * 1000 : 0;
    g_prof_rip = VirtualAlloc(NULL, PROF_MAX * sizeof *g_prof_rip, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_prof_frm = VirtualAlloc(NULL, PROF_MAX * sizeof *g_prof_frm, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    { char z[4]; if (GetEnvironmentVariableA("PHOTON_PROFILE_STACK", z, sizeof z) > 0 && z[0] == '1')
        g_prof_stk = VirtualAlloc(NULL, (size_t)PROF_MAX * PROF_STK * sizeof *g_prof_stk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
    HMODULE self = NULL;
    if (!g_prof_rip || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)prof_thread, &self)) return;
    HANDLE h = CreateThread(NULL, 0, prof_thread, self, 0, NULL);
    if (!h) { FreeLibrary(self); return; }
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(h);
    photon_log("prof: on, main thread %lu, from %lu s, about 1 kHz", (unsigned long)g_main_tid, (unsigned long)(g_prof_delay_ms / 1000));
}

static void meter_start(void) {
    HMODULE kb = GetModuleHandleA("kernelbase.dll"), k32 = GetModuleHandleA("kernel32.dll");
    g_gtd = (GetThreadDescriptionFn)(void *)GetProcAddress(kb ? kb : k32, "GetThreadDescription");
    if (!g_gtd && k32) g_gtd = (GetThreadDescriptionFn)(void *)GetProcAddress(k32, "GetThreadDescription");
    g_nqit = (NtQueryInformationThreadFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    g_nqsi = (NtQuerySystemInformationFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    char e[8];   /* PHOTON_METER_TOOLHELP=1: force the Toolhelp32 + GetThreadTimes path (tests) */
    if (GetEnvironmentVariableA("PHOTON_METER_TOOLHELP", e, sizeof e) > 0 && e[0] == '1') g_nqsi = NULL;
    g_meter_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    HMODULE self = NULL;   /* a counted reference for the thread (no _UNCHANGED_REFCOUNT flag) */
    if (!g_meter_stop || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)meter_thread, &self)) {
        photon_log("meters: no stop event or module reference (%lu): CPU columns stay empty", GetLastError());
        return;
    }
    InterlockedExchange(&g_meter_pinned, 1);
    HANDLE h = CreateThread(NULL, 0, meter_thread, self, 0, NULL);
    if (!h) {
        photon_log("meters: CreateThread failed (%lu): CPU columns stay empty", GetLastError());
        InterlockedExchange(&g_meter_pinned, 0);
        FreeLibrary(self);
        return;
    }
    SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    g_meter_thread = h;
    prof_start();
}

void photon_frames_shutdown(const char *why) {
    HANDLE h = InterlockedExchangePointer((PVOID volatile *)&g_meter_thread, NULL);
    if (!h) return;
    SetEvent(g_meter_stop);
    /* a sample takes ~10 ms (max ~70 ms in game); the bound only matters if the thread is stuck, and then it keeps
     * its module reference, so the DLL stays mapped and nothing returns into unmapped code */
    DWORD w = WaitForSingleObject(h, 5000);
    CloseHandle(h);
    AcquireSRWLockExclusive(&g_pub_lock);
    g_pub.valid = 0;   /* the CPU columns go empty instead of repeating the last window */
    ReleaseSRWLockExclusive(&g_pub_lock);
    photon_log("meters: stopped at %s (%s)", why ? why : "?",
               w == WAIT_OBJECT_0 ? "thread exited, module reference released"
                                  : "wait timed out: the thread keeps its module reference");
}

int photon_frames_meter_pinned(void) { return g_meter_pinned != 0; }

static void csv_meters(void) {
    MPub p;
    AcquireSRWLockShared(&g_pub_lock);
    p = g_pub;
    ReleaseSRWLockShared(&g_pub_lock);
    double ph = (double)InterlockedExchange64(&g_mq_photon_ticks, 0) * 1e6 / (double)g_freq.QuadPart;
    double rp = (double)InterlockedExchange64(&g_mq_rp_ticks, 0) * 1e6 / (double)g_freq.QuadPart;
    double po = (double)InterlockedExchange64(&g_mq_ours_ticks, 0) * 1e6 / (double)g_freq.QuadPart;
    DWORD tid = GetCurrentThreadId();
    fprintf(g_csv, ",%d", g_main_tid && tid != g_main_tid);
    if (p.valid) {
        fprintf(g_csv, ",%.1f,%.1f", p.window_ms, p.pct[R_MAIN]);
        if (p.has_mq) fprintf(g_csv, ",%.1f", p.pct[R_MQ]); else fputc(',', g_csv);
        fprintf(g_csv, ",%.1f", p.pct[R_DXVK] + p.pct[R_DXVK_CS]);
        if (p.has_cs) fprintf(g_csv, ",%.1f", p.pct[R_DXVK_CS]); else fputc(',', g_csv);
        fprintf(g_csv, ",%.1f,%.1f,%d", p.pct[R_OTHER], p.proc_pct, p.ndxvk);
    } else fprintf(g_csv, ",0,,,,,,,%d", p.ndxvk);
    fprintf(g_csv, ",%.1f,%.1f,%.1f", ph, rp, po);
}

static void open_csv(void) {
    g_csv_tried = 1;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_tprev = g_t0;
    const PhotonSlotNames *api = table_for("ShaderApi030");
    for (int h = 0; h < NHEAD; h++) {
        g_head_slot[h] = -1;
        for (size_t s = 0; api && s < api->count; s++)
            if (strcmp(api->names[s], k_headline[h]) == 0) { g_head_slot[h] = (long)s; break; }
    }
    char path[MAX_PATH + 32];
    if (!log_path("photon_frames.csv", path, sizeof path)) return;
    g_csv = fopen(path, "w");
    if (!g_csv) return;
    fprintf(g_csv, "frame,t_ms,dt_ms,present_tid,ws_mb,commit_mb,handles,live_static_meshes");
    fprintf(g_csv, ",queued_mode,cpu_window_ms,main_cpu_pct,mq_cpu_pct,dxvk_cpu_pct,dxvk_cs_cpu_pct,other_cpu_pct,"
                   "proc_cpu_pct,dxvk_threads,mq_photon_us,mq_renderpass_us,mq_photon_ours_us");
    for (int i = 0; i < NIFACES; i++) fprintf(g_csv, ",%s_calls", k_ifaces[i]);
    for (int h = 0; h < NHEAD; h++) if (g_head_slot[h] >= 0) fprintf(g_csv, ",%s", k_headline[h]);
    photon_vlg_csv(g_csv, 1);   /* T3.4c lighting counters, only while lighting is wanted */
    fputc('\n', g_csv);
    photon_log("frames: writing %s", path);
    g_perf_on = 1;
    meter_start();
}

void photon_frame_end(void) {
    if (!g_csv_tried) open_csv();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t_ms = (double)(now.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
    double dt_ms = (double)(now.QuadPart - g_tprev.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
    g_tprev = now;
    g_frame++;
    /* T5.0 leak tracking: process memory (commit = PrivateUsage, or PagefileUsage where Wine leaves it 0;
     * GetProcessHandleCount is 0 under Wine/Proton), handle count, and live static meshes (IShaderDevice CreateStaticMesh
     * slot 22 minus DestroyStaticMesh slot 23, cumulative). Steady growth across map reloads indicates a leak. */
    PROCESS_MEMORY_COUNTERS_EX pmc = { .cb = sizeof pmc };
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    long long live_meshes = 0;
    PhotonWrapClass *dev = find_class("ShaderDevice001");
    if (dev && dev->nslots > 23) live_meshes = (long long)dev->counters[22] - (long long)dev->counters[23];
    if (g_csv) fprintf(g_csv, "%llu,%.3f,%.3f,%lu,%.1f,%.1f,%lu,%lld", (unsigned long long)g_frame, t_ms, dt_ms,
                       GetCurrentThreadId(), pmc.WorkingSetSize / 1048576.0,
                       (pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage) / 1048576.0, handles,
                       live_meshes);
    g_present_tid = GetCurrentThreadId();
    if (g_csv) csv_meters();
    uint64_t api_delta[300] = {0};
    for (int i = 0; i < NIFACES; i++) {
        PhotonWrapClass *c = find_class(k_ifaces[i]);
        uint64_t total = 0, *prev = c ? prev_for(c) : NULL;
        if (c && prev)
            for (size_t s = 0; s < c->nslots; s++) {
                uint64_t cur = c->counters[s], d = cur - prev[s];
                prev[s] = cur;
                total += d;
                if (i == 0 && s < 300) api_delta[s] = d;
            }
        if (g_csv) fprintf(g_csv, ",%llu", (unsigned long long)total);
    }
    if (g_csv) {
        for (int h = 0; h < NHEAD; h++)
            if (g_head_slot[h] >= 0) fprintf(g_csv, ",%llu", (unsigned long long)api_delta[g_head_slot[h]]);
        photon_vlg_csv(g_csv, 0);
        fputc('\n', g_csv);
        if ((g_frame & 63) == 0) fflush(g_csv);
    }
    if (g_frame % 600 == 0) photon_frames_flush();
}

void photon_frames_flush(void) {
    if (g_csv) fflush(g_csv);
    char path[MAX_PATH + 32], tmp[MAX_PATH + 40], buf[32];
    if (!log_path("photon_slots.csv", path, sizeof path)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "iface,slot,name,calls,frames\n");
    for (size_t i = 0; i < g_photon_stats.nclasses; i++) {
        PhotonWrapClass *c = g_photon_stats.classes[i];
        for (size_t s = 0; s < c->nslots; s++)
            if (c->counters[s])
                fprintf(f, "%s,%zu,%s,%llu,%llu\n", c->name, s, photon_slot_name(c->name, s, buf, sizeof buf),
                        (unsigned long long)c->counters[s], (unsigned long long)g_frame);
    }
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

uint64_t photon_frame_count(void) { return g_frame; }
