/* PHOTON_DIAG=1 diagnostics (see diag.h). Build-specific RVAs are for build 26229091 (the T1.5 build guard); each one
 * is checked against the live vtables before it is used, and a mismatch disables that part with a log line. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "diag.h"
#include "log.h"
#include "frames.h"

enum { SLOT_SETINT = 62, SLOT_GETINT = 65, SLOT_RENDERPASS = 125, SLOT_SETORIGIN = 128, HW_NUMVS = 22, HW_NUMPS = 23 };
/* shaderapidx9 RVAs (analysis/scratch/05_cshaderapidx8_vt.txt, slots/CHardwareConfig_vs_minimal.txt, decomp). */
enum { RVA_SETINT = 0x1e760, RVA_GETINT = 0x17ce0, RVA_SETORIGIN = 0x1eb50, RVA_HW_NUMVS = 0x4b40,
       RVA_HW_NUMPS = 0x4b30, RVA_G_HWCONFIG = 0x9b008 /* CHardwareConfig* used by FUN_1800184a0 */ };
/* Offsets in the ShaderApi030 sub-object (the `this` of slots 31/62/65; full object = sub-object - 0x378). */
enum { OFF_INTPARMS = 0x3930, OFF_PSCONST_A = 0x14a0, OFF_PSCONST_B = 0x2478 };
/* CHardwareConfig fields: +0x530 NumPixelShaderConstants (slot 23), +0x534 (sizes the 4-byte-per-entry array at
 * full-object +0x1820), +0x53c NumVertexShaderConstants (slot 22), +0x5bc flags (bit 0x10 gates the allocation). */
enum { HW_F_NUMPS = 0x530, HW_F_534 = 0x534, HW_F_NUMVS = 0x53c, HW_F_FLAGS = 0x5bc };
enum { NPARM = 20, NBUCKET = NPARM + 1, NVAL = 8, NCALLER = 6, NTID = 4, NIMMEDIATE = 20 };

static int g_diag = -1;
int photon_diag_enabled(void) {
    if (g_diag < 0) {
        char e[8];
        g_diag = GetEnvironmentVariableA("PHOTON_DIAG", e, sizeof e) > 0 && e[0] == '1';
    }
    return g_diag;
}

static const char *where(const void *addr, char *buf, size_t n) {
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        char *b = strrchr(name, '\\');
        if (b) memmove(name, b + 1, strlen(b));
    }
    snprintf(buf, n, "%s+0x%llx", name, (unsigned long long)((const char *)addr - (const char *)m));
    return buf;
}
static uintptr_t module_base(const void *addr) {
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m)) return 0;
    return (uintptr_t)m;
}

/* --- R1 histogram ------------------------------------------------------------------------------------------- */
typedef struct { int v; uint64_t n; } ValCount;
typedef struct { const void *addr; uint64_t n; } CallerCount;
typedef struct Hist {
    uint64_t n, first_frame, last_frame;
    ValCount vals[NVAL]; uint64_t vals_other;          /* bucket NPARM (out of range) records the index instead */
    CallerCount callers[NCALLER]; uint64_t callers_other;
    DWORD tids[NTID]; uint64_t tid_n[NTID], tids_other;
    unsigned immediate;                                /* immediate log lines written (indices 12..19, out of range) */
} Hist;
static Hist g_set[NBUCKET], g_get[NBUCKET];
static SRWLOCK g_hlock = SRWLOCK_INIT;

static void hist_add(Hist *h, int v, const void *caller) {
    uint64_t f = photon_frame_count();
    DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_hlock);
    if (h->n++ == 0) h->first_frame = f;
    h->last_frame = f;
    int i;
    for (i = 0; i < NVAL && h->vals[i].n && h->vals[i].v != v; i++) {}
    if (i < NVAL) { h->vals[i].v = v; h->vals[i].n++; } else h->vals_other++;
    for (i = 0; i < NCALLER && h->callers[i].n && h->callers[i].addr != caller; i++) {}
    if (i < NCALLER) { h->callers[i].addr = caller; h->callers[i].n++; } else h->callers_other++;
    for (i = 0; i < NTID && h->tid_n[i] && h->tids[i] != tid; i++) {}
    if (i < NTID) { h->tids[i] = tid; h->tid_n[i]++; } else h->tids_other++;
    ReleaseSRWLockExclusive(&g_hlock);
}

/* --- state captured from the wrappers ------------------------------------------------------------------------ */
static PhotonWrap *volatile g_api_w;       /* ShaderApi030 wrapper */
static PhotonWrap *volatile g_hw_w;        /* MaterialSystemHardwareConfig012 wrapper */
static uintptr_t g_stock_base;
static int g_api_layout_ok = -1, g_hw_layout_ok = -1;

void photon_diag_note(const char *name, void *wrapper, void *real) {
    if (!photon_diag_enabled() || !wrapper || wrapper == real) return;
    if (strcmp(name, "ShaderApi030") == 0 && !g_api_w) {
        g_api_w = wrapper;
        void **vt = g_api_w->cls->real_vtbl;
        g_stock_base = module_base(vt[0]);
        uintptr_t b = g_stock_base;
        g_api_layout_ok = b && (uintptr_t)vt[SLOT_SETINT] - b == RVA_SETINT && (uintptr_t)vt[SLOT_GETINT] - b == RVA_GETINT
                          && (uintptr_t)vt[SLOT_SETORIGIN] - b == RVA_SETORIGIN;
        photon_log("DIAG ShaderApi030 real %p: slot62 +0x%llx slot65 +0x%llx slot128 +0x%llx -> layout %s", real,
                   (unsigned long long)((uintptr_t)vt[SLOT_SETINT] - b), (unsigned long long)((uintptr_t)vt[SLOT_GETINT] - b),
                   (unsigned long long)((uintptr_t)vt[SLOT_SETORIGIN] - b),
                   g_api_layout_ok ? "as expected (raw parm audit on)" : "UNEXPECTED (raw reads off)");
    } else if (strcmp(name, "MaterialSystemHardwareConfig012") == 0 && !g_hw_w) {
        g_hw_w = wrapper;
        void **vt = g_hw_w->cls->real_vtbl;
        uintptr_t b = module_base(vt[0]);
        if (!g_stock_base) g_stock_base = b;
        g_hw_layout_ok = b && (uintptr_t)vt[HW_NUMVS] - b == RVA_HW_NUMVS && (uintptr_t)vt[HW_NUMPS] - b == RVA_HW_NUMPS;
        photon_log("DIAG HWConfig012 real %p: slot22 +0x%llx slot23 +0x%llx -> layout %s", real,
                   (unsigned long long)((uintptr_t)vt[HW_NUMVS] - b), (unsigned long long)((uintptr_t)vt[HW_NUMPS] - b),
                   g_hw_layout_ok ? "as expected" : "UNEXPECTED (raw caps reads off)");
    }
}

/* --- order trace -------------------------------------------------------------------------------------------- */
static SRWLOCK g_tlock = SRWLOCK_INIT;
static int g_tr_on, g_tr_win;
static unsigned g_tr_lines;
enum { TR_MAX_WIN = 80, TR_MAX_LINES = 4000 };
static unsigned long long rp_count(PhotonWrap *w) {
    return w->cls->nslots > SLOT_RENDERPASS ? (unsigned long long)w->cls->counters[SLOT_RENDERPASS] : 0;
}

/* --- hooks -------------------------------------------------------------------------------------------------- */
static int g_shadow[NPARM];        /* last value set through our wrapper (or resynced from raw by the audit) */
static volatile LONG g_shadow_valid;

static void hook_setint(void *self, int idx, int val) {
    PhotonWrap *w = photon_wrap_of(self);
    const void *caller = __builtin_return_address(0);
    InterlockedIncrement64((volatile LONG64 *)&w->cls->counters[SLOT_SETINT]);
    if (idx == 0) {                                /* order trace: tokens are parm 0 (render.SetLightingMode) */
        AcquireSRWLockExclusive(&g_tlock);
        if (!g_tr_on && val != 0 && g_tr_win < TR_MAX_WIN) { g_tr_on = 1; g_tr_win++; }
        if (g_tr_on && g_tr_lines < TR_MAX_LINES) {
            g_tr_lines++;
            photon_log("ORDER win=%d frame=%llu tid=%lu rp=%llu setint 0 %d", g_tr_win,
                       (unsigned long long)photon_frame_count(), GetCurrentThreadId(), rp_count(w), val);
        }
        if (g_tr_on && val == 0) g_tr_on = 0;
        ReleaseSRWLockExclusive(&g_tlock);
    } else if (g_tr_on && g_tr_lines < TR_MAX_LINES) {
        AcquireSRWLockExclusive(&g_tlock);
        g_tr_lines++;
        photon_log("ORDER win=%d frame=%llu tid=%lu rp=%llu setint %d %d", g_tr_win,
                   (unsigned long long)photon_frame_count(), GetCurrentThreadId(), rp_count(w), idx, val);
        ReleaseSRWLockExclusive(&g_tlock);
    }
    ((void (*)(void *, int, int))w->cls->real_vtbl[SLOT_SETINT])(w->real, idx, val);
    int b = idx >= 0 && idx < NPARM ? idx : NPARM;
    if (b < NPARM) g_shadow[b] = val;
    hist_add(&g_set[b], b < NPARM ? val : idx, caller);
    if ((b >= 12) && g_set[b].immediate < NIMMEDIATE) {
        g_set[b].immediate++;
        char buf[MAX_PATH + 32];
        photon_log("DIAG R1 SET parm %d = %d from %s tid %lu frame %llu", idx, val, where(caller, buf, sizeof buf),
                   GetCurrentThreadId(), (unsigned long long)photon_frame_count());
    }
}

static int hook_getint(void *self, int idx) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SLOT_GETINT, -1)) return ((int (*)(void *, int))w->cls->real_vtbl[SLOT_GETINT])(w->real, idx);
    const void *caller = __builtin_return_address(0);
    InterlockedIncrement64((volatile LONG64 *)&w->cls->counters[SLOT_GETINT]);
    int v = ((int (*)(void *, int))w->cls->real_vtbl[SLOT_GETINT])(w->real, idx);
    int b = idx >= 0 && idx < NPARM ? idx : NPARM;
    hist_add(&g_get[b], b < NPARM ? v : idx, caller);
    if (b >= 12 && g_get[b].immediate < NIMMEDIATE) {
        g_get[b].immediate++;
        char buf[MAX_PATH + 32];
        photon_log("DIAG R1 GET parm %d -> %d from %s tid %lu frame %llu", idx, v, where(caller, buf, sizeof buf),
                   GetCurrentThreadId(), (unsigned long long)photon_frame_count());
    }
    return v;
}

static void hook_setorigin(void *self, const float *o) {
    PhotonWrap *w = photon_wrap_of(self);
    InterlockedIncrement64((volatile LONG64 *)&w->cls->counters[SLOT_SETORIGIN]);
    if (g_tr_on && o) {
        AcquireSRWLockExclusive(&g_tlock);
        if (g_tr_on && g_tr_lines < TR_MAX_LINES) {
            g_tr_lines++;
            photon_log("ORDER win=%d frame=%llu tid=%lu rp=%llu origin %.3f %.3f %.3f", g_tr_win,
                       (unsigned long long)photon_frame_count(), GetCurrentThreadId(), rp_count(w), o[0], o[1], o[2]);
        }
        ReleaseSRWLockExclusive(&g_tlock);
    }
    ((void (*)(void *, const float *))w->cls->real_vtbl[SLOT_SETORIGIN])(w->real, o);
}

void photon_diag_install(PhotonWrapClass *cls) {
    if (!photon_diag_enabled()) return;
    if (strcmp(cls->name, "ShaderApi030") == 0 && cls->nslots > SLOT_SETORIGIN) {
        photon_wrap_set_hook(cls, SLOT_SETINT, (void *)hook_setint);
        photon_wrap_set_hook(cls, SLOT_GETINT, (void *)hook_getint);
        photon_wrap_set_hook(cls, SLOT_SETORIGIN, (void *)hook_setorigin);
        photon_log("DIAG on: ShaderApi030 slots 62/65/128 hooked (Set/GetIntRenderingParameter, SetLightingOrigin)");
    }
}

/* --- per-frame: R15 caps and the raw parm audit ---------------------------------------------------------------- */
static long long heap_block_size(const void *p) {     /* best effort: the process heap block holding p, or -1 */
    if (!p) return -1;
    HANDLE hs[64];
    DWORD n = GetProcessHeaps(64, hs);
    for (DWORD i = 0; i < n && i < 64; i++)
        if (HeapValidate(hs[i], 0, p)) return (long long)HeapSize(hs[i], 0, p);
    return -1;
}

static uint64_t g_audits, g_mismatch_lo, g_mismatch_hi;
static unsigned g_mismatch_logged;

static void caps_check(void) {
    static int last_vs = -1, last_ps = -1, last_raw = -1, logged;
    static void *last_a, *last_b;
    PhotonWrap *hw = g_hw_w, *api = g_api_w;
    if (!hw) { if (!logged) { logged = 1; photon_log("DIAG R15: no HWConfig012 wrapper yet"); } return; }
    int vs = ((int (*)(void *))hw->vtbl[HW_NUMVS])(hw);     /* through our wrapper (thunk -> stock) */
    int ps = ((int (*)(void *))hw->vtbl[HW_NUMPS])(hw);
    int raw_ps = -1, raw_534 = -1, raw_vs = -1, flags = -1;
    char *g = NULL;
    if (g_hw_layout_ok == 1 && g_stock_base) {
        g = *(char **)(g_stock_base + RVA_G_HWCONFIG);
        if (g) {
            raw_ps = *(int *)(g + HW_F_NUMPS); raw_534 = *(int *)(g + HW_F_534);
            raw_vs = *(int *)(g + HW_F_NUMVS); flags = *(unsigned char *)(g + HW_F_FLAGS);
        }
    }
    void *a = NULL, *b = NULL;
    if (api && g_api_layout_ok == 1) {
        a = *(void **)((char *)api->real + OFF_PSCONST_A);
        b = *(void **)((char *)api->real + OFF_PSCONST_B);
    }
    if (vs == last_vs && ps == last_ps && raw_ps == last_raw && a == last_a && b == last_b) return;
    last_vs = vs; last_ps = ps; last_raw = raw_ps; last_a = a; last_b = b;
    photon_log("DIAG R15 frame %llu: HWConfig NumVertexShaderConstants (slot 22) = %d, NumPixelShaderConstants (slot 23) = %d;"
               " raw [stock+0x%x]=%p (HWConfig real %p, %s): +0x530 = %d, +0x534 = %d, +0x53c = %d, +0x5bc flags = 0x%x"
               " (bit 0x10 %s)", (unsigned long long)photon_frame_count(), vs, ps, RVA_G_HWCONFIG, (void *)g, hw->real,
               g == hw->real ? "same object" : "DIFFERENT object", raw_ps, raw_534, raw_vs, flags,
               flags >= 0 && (flags & 0x10) ? "set: arrays sized by +0x530" : "clear");
    if (a || b)
        photon_log("DIAG R15 PS constant shadow arrays: +0x14a0 = %p (heap block %lld bytes), +0x2478 = %p (heap block %lld"
                   " bytes); 16 * +0x530 = %d bytes", a, heap_block_size(a), b, heap_block_size(b), 16 * raw_ps);
}

static void parm_audit(void) {
    PhotonWrap *api = g_api_w;
    if (!api || g_api_layout_ok != 1) return;
    volatile int *raw = (volatile int *)((char *)api->real + OFF_INTPARMS);
    if (!g_shadow_valid) {
        char line[512]; int n = 0, self_ok = 1;
        for (int i = 0; i < NPARM; i++) {
            int r = raw[i];
            int via_get = ((int (*)(void *, int))api->cls->real_vtbl[SLOT_GETINT])(api->real, i);
            self_ok &= via_get == r;
            g_shadow[i] = r;
            n += snprintf(line + n, sizeof line - (size_t)n, " %d", r);
        }
        g_shadow_valid = 1;
        photon_log("DIAG R1 raw int parms at frame %llu:%s (stock getter %s the raw reads)",
                   (unsigned long long)photon_frame_count(), line, self_ok ? "matches" : "DIFFERS FROM");
        return;
    }
    g_audits++;
    for (int i = 0; i < NPARM; i++) {
        int r = raw[i];
        if (r == g_shadow[i]) continue;
        if (i >= 12) g_mismatch_hi++; else g_mismatch_lo++;
        if (g_mismatch_logged++ < 20)
            photon_log("DIAG R1 AUDIT parm %d raw %d != last set through wrapper %d (frame %llu): a writer bypassed the"
                       " wrapper", i, r, g_shadow[i], (unsigned long long)photon_frame_count());
        g_shadow[i] = r;
    }
}

void photon_diag_frame(void) {
    if (!photon_diag_enabled()) return;
    caps_check();
    parm_audit();
    uint64_t f = photon_frame_count();
    if (f % 120 == 0) photon_diag_dump("periodic");   /* often enough to survive a hang or kill */
}

static void dump_hist(const char *dir, int b, const Hist *h) {
    if (!h->n) return;
    char vals[512], callers[1024], tids[128], buf[MAX_PATH + 32];
    int n = 0;
    for (int i = 0; i < NVAL && h->vals[i].n; i++)
        n += snprintf(vals + n, sizeof vals - (size_t)n, " %d:%llu", h->vals[i].v, (unsigned long long)h->vals[i].n);
    if (h->vals_other) snprintf(vals + n, sizeof vals - (size_t)n, " other:%llu", (unsigned long long)h->vals_other);
    if (!n && !h->vals_other) vals[0] = 0;
    n = 0; callers[0] = 0;
    for (int i = 0; i < NCALLER && h->callers[i].n; i++)
        n += snprintf(callers + n, sizeof callers - (size_t)n, " %s:%llu", where(h->callers[i].addr, buf, sizeof buf),
                      (unsigned long long)h->callers[i].n);
    if (h->callers_other) snprintf(callers + n, sizeof callers - (size_t)n, " other:%llu", (unsigned long long)h->callers_other);
    n = 0; tids[0] = 0;
    for (int i = 0; i < NTID && h->tid_n[i]; i++)
        n += snprintf(tids + n, sizeof tids - (size_t)n, " %lu:%llu", h->tids[i], (unsigned long long)h->tid_n[i]);
    char idx[16];
    if (b < NPARM) snprintf(idx, sizeof idx, "%2d", b); else snprintf(idx, sizeof idx, "out-of-range");
    photon_log("DIAG R1 hist %s parm %s: n=%llu frames %llu..%llu %s[%s ] tids[%s ] callers[%s ]", dir, idx,
               (unsigned long long)h->n, (unsigned long long)h->first_frame, (unsigned long long)h->last_frame,
               b < NPARM ? "values" : "indices", vals, tids, callers);
}

void photon_diag_dump(const char *why) {
    if (!photon_diag_enabled()) return;
    AcquireSRWLockShared(&g_hlock);
    uint64_t hi_set = 0, hi_get = 0;
    photon_log("DIAG R1 histogram (%s, frame %llu):", why, (unsigned long long)photon_frame_count());
    for (int b = 0; b < NBUCKET; b++) {
        dump_hist("set", b, &g_set[b]);
        dump_hist("get", b, &g_get[b]);
        if (b >= 12) { hi_set += g_set[b].n; hi_get += g_get[b].n; }
    }
    photon_log("DIAG R1 summary (%s): parms 12..19 + out-of-range: sets %llu gets %llu; raw audits %llu, mismatches"
               " parms 0..11 %llu, 12..19 %llu; order windows %d", why, (unsigned long long)hi_set,
               (unsigned long long)hi_get, (unsigned long long)g_audits, (unsigned long long)g_mismatch_lo,
               (unsigned long long)g_mismatch_hi, g_tr_win);
    ReleaseSRWLockShared(&g_hlock);
}
