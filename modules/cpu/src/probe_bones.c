/* Photon CPU C3 probe (PHOTON_CPU_PROBE_BONES=1): who calls C_BaseAnimating::SetupBones, how often it really recomputes, and on which thread.
 *
 * How: client.dll's SetupBones (renderable slot 16, FUN_1801a46b0 in the validated build) is inherited by every animating class, so its
 * address sits in ~111 vtable slots. Each slot that holds it is swapped for probe_setupbones (one InterlockedExchangePointer per slot,
 * never a whole table: Photon Render patches slot 31 of the same tables). The original runs unchanged; around it the probe takes rdtsc
 * and the three return addresses (RtlCaptureStackBackTrace, about a microsecond: a probe, not a player path). Off unless the env var is set; on any mismatch with the validated build it logs one line and does nothing.
 *
 * Output every PROBE_WINDOW presented frames or PROBE_WINDOW_MS, whichever comes first (the VM draws ~2 frames a second): per call stack (module+rva, 3 deep), calls per frame, slow calls per frame (over
 * PROBE_SLOW_CYCLES: a real recompute, not a cache hit), main-thread share and microseconds per frame.
 * PHOTON_CPU_PROBE_BONES=2 adds the C3b validation (bit-for-bit compare after the CalcAttachments invalidation; see BoneSnap). */
#include <windows.h>
#include <intrin.h>
#include <string.h>
#include <stdio.h>
#include "photon_cpu.h"

enum { RVA_SETUPBONES = 0x1a46b0, PROBE_SLOTS = 128, PROBE_WINDOW = 600, PROBE_WINDOW_MS = 15000, MAX_PATCH = 256 };
static const DWORD k_client_timestamp = 0x6adfbed6;   /* client.dll of the validated build (VM and laptop, sha1 d2b795b6...) */
static const uint32_t k_hash_setupbones = 0x097786b7; /* FNV-1a of the first 64 bytes of FUN_1801a46b0 */
static const unsigned long long PROBE_SLOW_CYCLES = 6000; /* ~2.5 us at 2.3 GHz: a hit returns well below, a recompute far above */

/* The 5th argument (the time) is a double in this build (callers store it with movsd to [rsp+0x20]; analysis/02). x64 passes argument 5
 * in its stack slot whatever its type, so the probe carries it as the raw 8 bytes: never reinterpret it. */
typedef uint64_t (*SetupBonesFn)(void *self, void *bone_to_world, int max_bones, int bone_mask, uint64_t current_time_raw);
static SetupBonesFn g_orig;
static uintptr_t g_client_lo, g_client_hi;
static void **g_patched[MAX_PATCH];
static int g_npatched;
static volatile LONG g_state;   /* 0 not tried, 1 on, -1 off */

enum { PROBE_DEPTH = 3 };
typedef struct {
    volatile LONG64 key;        /* hash of the PROBE_DEPTH return addresses (0 = empty) */
    void *volatile fr[PROBE_DEPTH];
    volatile LONG64 calls, slow, main_calls, cycles, slow_cycles;
} ProbeRec;
static ProbeRec g_rec[PROBE_SLOTS];

/* Miss samples (C3 step 2): the cache state around a slow call, for a few calls per window. Field offsets on the renderable sub-object
 * (this pointer of slot 16), from FUN_1801a46b0 in the validated build: readable bones +0x1734, previous frame's mask +0x1718,
 * accumulated mask +0x171c, entity's model bone counter +0x1710, last setup time (double) +0x1a80. Probe-only reads. */
enum { NSAMP = 12 };
typedef struct { void *self; void *caller; int mask, readable0, prev0, accum0, readable1, ctr0; double t, last0; unsigned dt_us; } MissSample;
static MissSample g_samp[NSAMP];
static volatile LONG g_nsamp_e, g_nsamp_o;   /* engine callers fill slots 0..NSAMP/2-1, the others NSAMP/2.. */
static uintptr_t g_engine_lo, g_engine_hi;
static volatile const int *g_model_counter;   /* DAT_18093ab84 */

/* C3b validation (PHOTON_CPU_PROBE_BONES=2): is the engine's recompute after GMod's CalcAttachments invalidation bit-identical to the
 * bones that were cached before it? CalcAttachments' own SetupBones (return address client+0x197eed) leaves the cached matrices at
 * renderable+0x1a58 (pointer) with the count at +0x1a68 (matrix3x4, 0x30 bytes each; FUN_1801a46b0's copy-out). The probe snapshots them
 * there and compares them after the engine's next recompute of the same entity in the same frame (same model-bone counter). Main thread
 * only: both calls run there (probe stacks). */
enum { RVA_CALCATTACH_RET = 0x197eed, NSNAP = 512, SNAP_MAXBONES = 256 };
typedef struct { void *self; int ctr, n; double t; uint8_t bones[SNAP_MAXBONES * 0x30]; } BoneSnap;
static BoneSnap *g_snap;
static int g_validate;
static LONG64 v_cmp, v_same, v_diff, v_nosnap, v_bones_diff_max, v_cmp_time_diff;
static LONG64 vj_cmp, vj_same, vn_bones_max; static double vn_maxabs;   /* jiggle-model compares; the max over the others */
static double v_maxabs;

/* Models with jiggle bones are left to GMod's recompute (C1b, analysis/02 §10): a jiggle bone (procedural type 5) simulates a spring on
 * every SetupBones call, so stock simulates it twice a frame here and keeping the first compute would change how hair and cloth swing.
 * renderable+0x1ad0 is the entity's CStudioHdr (FUN_1801a46b0: param_1[0x35a]), whose first field is the studiohdr_t: "IDST", version
 * 44-49, bone count +0x9c, bone table +0xa0, 216-byte bones with the procedural type at +164. Cached per studiohdr_t; unreadable = jiggle. */
enum { NJIG = 512 };
static struct { const void *hdr; int jiggle; } g_jig[NJIG];
static int model_has_jiggle(const void *self) {
    const void *cs = *(const void *const *)((const char *)self + 0x1ad0);
    const uint8_t *h = cs ? *(const uint8_t *const *)cs : NULL;
    if (!h) return 1;
    unsigned k = (unsigned)(((uintptr_t)h >> 4) * 2654435761u) % NJIG;
    for (int i = 0; i < NJIG; i++, k = (k + 1) % NJIG) {
        if (g_jig[k].hdr == h) return g_jig[k].jiggle;
        if (!g_jig[k].hdr) {
            int jig = 1;
            int ver = *(const int *)(h + 4), nb = *(const int *)(h + 0x9c), bi = *(const int *)(h + 0xa0);
            if (memcmp(h, "IDST", 4) == 0 && ver >= 44 && ver <= 49 && nb > 0 && nb <= 256 && bi > 0) {
                jig = 0;
                for (int b = 0; b < nb; b++) if (*(const int *)(h + bi + b * 216 + 164) == 5) { jig = 1; break; }
            }
            g_jig[k].hdr = h; g_jig[k].jiggle = jig;   /* main thread only (the fix and the validation run there) */
            return jig;
        }
    }
    return 1;
}

/* C3c fix (on by default; PHOTON_CPU_BONES=0 turns it off; analysis/02 §7). GMod's CalcAttachments invalidates the bone cache after its own SetupBones (writes model-bone
 * counter G-1 and last setup time -FLT_MAX). On the eye path (GetAttachment from the eye code FUN_1801c1680, return client+0x1c1865) that
 * forces DrawModelSetup to recompute bones whose inputs did not change (the eye code writes flex weights only). When CalcAttachments'
 * SetupBones returns on that path, remember the entity's valid G and T; at the entity's next SetupBones in the same frame, if its fields
 * still hold exactly the invalidation values and its bone count is unchanged, write G and T back so the cache hits. Main thread only:
 * the eye path and the draws run there, and the pool's pre-pass has finished by then (OnRenderStart, before any draw). */
enum { RVA_EYE_RET = 0x1c1865, NPEND = 1024 };
static const uint64_t k_neg_flt_max = 0xc7efffffe0000000ull;   /* -FLT_MAX as a double: GMod's invalidated last time */
typedef struct { void *self; int g, n; uint64_t t; } BonePend;
static BonePend g_pend[NPEND];
static int g_fix, g_probe;
static volatile int g_fix_off;   /* runtime switch: garrysmod/data/photon_cpu_bones_off.txt exists (photon_cpu_flag bones_off 1) */
static LONG64 fx_set, fx_restored, fx_skipped, fx_jiggle;

static BonePend *pend_slot(void *self, int create) {
    unsigned h = (unsigned)(((uintptr_t)self >> 4) * 2654435761u) % NPEND;
    for (int i = 0; i < NPEND; i++, h = (h + 1) % NPEND) {
        if (g_pend[h].self == self) return &g_pend[h];
        if (!g_pend[h].self) { if (!create) return NULL; g_pend[h].self = self; g_pend[h].g = -1; return &g_pend[h]; }
    }
    return NULL;   /* full: the fix just does nothing for that entity */
}

/* before the original: restore a cache that only the eye path's CalcAttachments invalidated */
static void fix_before(void *self) {
    BonePend *pd = pend_slot(self, 0);
    if (!pd || pd->g < 0) return;
    char *b = (char *)self;
    int g = *g_model_counter;
    uint64_t last;
    memcpy(&last, b + 0x1a80, sizeof last);
    if (pd->g == g && *(int *)(b + 0x1710) == g - 1 && last == k_neg_flt_max && *(int *)(b + 0x1a68) == pd->n) {
        *(int *)(b + 0x1710) = g;
        memcpy(b + 0x1a80, &pd->t, sizeof pd->t);
        fx_restored++;
    } else fx_skipped++;
    pd->g = -1;   /* one use: the next invalidation must come from the eye path again */
}

/* after the original: CalcAttachments' setup on the eye path succeeded, GMod is about to invalidate */
static void fix_after(void *self, uintptr_t ret0, uint64_t r) {
    if (ret0 != g_client_lo + RVA_CALCATTACH_RET || !(r & 0xff)) return;
    void *f3[3];
    cpu_caller_stack(f3, 3);   /* only for CalcAttachments' calls (~1-2 per character a frame) */
    if ((uintptr_t)f3[0] != ret0 || (uintptr_t)f3[2] != g_client_lo + RVA_EYE_RET) return;
    if (model_has_jiggle(self)) { fx_jiggle++; return; }
    const char *b = (const char *)self;
    BonePend *pd = pend_slot(self, 1);
    if (!pd) return;
    pd->g = *g_model_counter;
    pd->n = *(const int *)(b + 0x1a68);
    memcpy(&pd->t, b + 0x1a80, sizeof pd->t);
    fx_set++;
}

static BoneSnap *snap_for(void *self, int create) {
    unsigned h = (unsigned)(((uintptr_t)self >> 4) * 2654435761u) % NSNAP;
    for (int i = 0; i < NSNAP; i++, h = (h + 1) % NSNAP) {
        if (g_snap[h].self == self) return &g_snap[h];
        if (!g_snap[h].self) { if (!create) return NULL; g_snap[h].self = self; return &g_snap[h]; }
    }
    return NULL;
}
static volatile LONG64 g_overflow;

static ProbeRec *rec_for(void *const *fr) {
    uint64_t hk = 1469598103934665603ull;
    for (int i = 0; i < PROBE_DEPTH; i++) hk = (hk ^ (uint64_t)(uintptr_t)fr[i]) * 1099511628211ull;
    LONG64 key = (LONG64)(hk | 1);
    unsigned h = (unsigned)(hk % PROBE_SLOTS);
    for (int i = 0; i < PROBE_SLOTS; i++, h = (h + 1) % PROBE_SLOTS) {
        LONG64 k = g_rec[h].key;
        if (k == key) return &g_rec[h];
        if (k == 0 && InterlockedCompareExchange64(&g_rec[h].key, key, 0) == 0) {
            for (int d = 0; d < PROBE_DEPTH; d++) g_rec[h].fr[d] = fr[d];
            return &g_rec[h];
        }
        if (g_rec[h].key == key) return &g_rec[h];
    }
    return NULL;
}


static uint64_t probe_full(void *self, void *bone_to_world, int max_bones, int bone_mask, uint64_t current_time_raw);
static uint64_t probe_setupbones(void *self, void *bone_to_world, int max_bones, int bone_mask, uint64_t current_time_raw) {
    if (g_probe) return probe_full(self, bone_to_world, max_bones, bone_mask, current_time_raw);
    uintptr_t ret0 = (uintptr_t)__builtin_return_address(0);
    int main = GetCurrentThreadId() == cpu_host()->main_thread;
    main = main && g_fix && !g_fix_off;
    if (main) fix_before(self);
    uint64_t r = g_orig(self, bone_to_world, max_bones, bone_mask, current_time_raw);
    if (main) fix_after(self, ret0, r);
    return r;
}
/* the probe (PHOTON_CPU_PROBE_BONES=1|2), with the fix applied the same way when both are on */
static uint64_t probe_full(void *self, void *bone_to_world, int max_bones, int bone_mask, uint64_t current_time_raw) {
    void *fr[PROBE_DEPTH];
    cpu_caller_stack(fr, PROBE_DEPTH);   /* fr[0] is the caller's return address */
    const char *b = (const char *)self;
    int readable0 = *(const int *)(b + 0x1734), prev0 = *(const int *)(b + 0x1718), accum0 = *(const int *)(b + 0x171c);
    int ctr0 = *(const int *)(b + 0x1710);
    double last0 = *(const double *)(b + 0x1a80);
    int on_main = GetCurrentThreadId() == cpu_host()->main_thread && !g_fix_off;
    if (g_fix && on_main) fix_before(self);
    unsigned long long t0 = __rdtsc();
    uint64_t r = g_orig(self, bone_to_world, max_bones, bone_mask, current_time_raw);
    unsigned long long dt = __rdtsc() - t0;
    if (g_fix && on_main) fix_after(self, (uintptr_t)fr[0], r);
    if (g_validate && GetCurrentThreadId() == cpu_host()->main_thread) {
        int n = *(const int *)(b + 0x1a68);
        const uint8_t *arr = *(const uint8_t *const *)(b + 0x1a58);
        double tnow;
        memcpy(&tnow, &current_time_raw, sizeof tnow);
        if ((uintptr_t)fr[0] == g_client_lo + RVA_CALCATTACH_RET && arr && n > 0 && n <= SNAP_MAXBONES) {
            BoneSnap *sn = snap_for(self, 1);
            if (sn) { sn->ctr = *g_model_counter; sn->n = n; sn->t = tnow; memcpy(sn->bones, arr, (size_t)n * 0x30); }
        } else if ((uintptr_t)fr[0] >= g_engine_lo && (uintptr_t)fr[0] < g_engine_hi && dt > PROBE_SLOW_CYCLES && arr && n > 0) {
            BoneSnap *sn = snap_for(self, 0);
            if (!sn || sn->ctr != *g_model_counter || sn->n != n) v_nosnap++;
            else {
                int jig = model_has_jiggle(self);
                if (jig) { vj_cmp++; if (memcmp(sn->bones, arr, (size_t)n * 0x30) == 0) vj_same++; }
                v_cmp++;
                if (sn->t != tnow) v_cmp_time_diff++;
                if (memcmp(sn->bones, arr, (size_t)n * 0x30) == 0) v_same++;
                else {
                    v_diff++;
                    LONG64 nb = 0;
                    for (int k = 0; k < n; k++) {
                        const float *x = (const float *)(sn->bones + k * 0x30), *y = (const float *)(arr + k * 0x30);
                        int bd = 0;
                        for (int j = 0; j < 12; j++) {
                            double dd = x[j] > y[j] ? x[j] - y[j] : y[j] - x[j];
                            if (x[j] != y[j]) bd = 1;
                            if (dd > v_maxabs) v_maxabs = dd;
                        }
                        nb += bd;
                    }
                    if (nb > v_bones_diff_max) v_bones_diff_max = nb;
                    if (!jig) {
                        if (nb > vn_bones_max) vn_bones_max = nb;
                        for (int k = 0; k < n * 12; k++) {
                            float x = ((const float *)sn->bones)[k], y = ((const float *)arr)[k];
                            double dd = x > y ? x - y : y - x;
                            if (dd > vn_maxabs) vn_maxabs = dd;
                        }
                    }
                }
            }
        }
    }
    int eng = (uintptr_t)fr[0] >= g_engine_lo && (uintptr_t)fr[0] < g_engine_hi;
    volatile LONG *nq = eng ? &g_nsamp_e : &g_nsamp_o;
    if (dt > PROBE_SLOW_CYCLES && *nq < NSAMP / 2) {
        LONG i = InterlockedIncrement(nq) - 1 + (eng ? 0 : NSAMP / 2);
        if (i < (eng ? NSAMP / 2 : NSAMP)) {
            MissSample *m = &g_samp[i];
            double t;
            memcpy(&t, &current_time_raw, sizeof t);
            m->self = self; m->caller = fr[0]; m->mask = bone_mask; m->readable0 = readable0; m->prev0 = prev0; m->accum0 = accum0;
            m->ctr0 = ctr0; m->t = t; m->last0 = last0; m->readable1 = *(const int *)(b + 0x1734);
            m->dt_us = (unsigned)(dt / cpu_tsc_mhz());
        }
    }
    ProbeRec *p = rec_for(fr);
    if (!p) { InterlockedIncrement64(&g_overflow); return r; }
    InterlockedIncrement64(&p->calls);
    InterlockedAdd64(&p->cycles, (LONG64)dt);
    if (dt > PROBE_SLOW_CYCLES) { InterlockedIncrement64(&p->slow); InterlockedAdd64(&p->slow_cycles, (LONG64)dt); }
    if (GetCurrentThreadId() == cpu_host()->main_thread) InterlockedIncrement64(&p->main_calls);
    return r;
}

static int probe_install(void) {
    HMODULE cl = GetModuleHandleA("client.dll");
    if (!cl) return 0;   /* not loaded yet: try again at the next present */
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const char *)cl + ((const IMAGE_DOS_HEADER *)cl)->e_lfanew);
    g_client_lo = (uintptr_t)cl;
    g_client_hi = g_client_lo + nt->OptionalHeader.SizeOfImage;
    if (nt->FileHeader.TimeDateStamp != k_client_timestamp) {
        cpu_log("probe bones: client.dll timestamp 0x%lx is not the validated build: off", nt->FileHeader.TimeDateStamp);
        return -1;
    }
    uintptr_t fn = g_client_lo + RVA_SETUPBONES;
    if (cpu_fnv((const void *)fn, 64) != k_hash_setupbones) {
        cpu_log("probe bones: SetupBones code differs from the validated build: off");
        return -1;
    }
    g_orig = (SetupBonesFn)fn;
    DeleteFileA("garrysmod\\data\\photon_cpu_bones_off.txt");   /* a run's runtime flag never carries into the next run */
    g_model_counter = (volatile const int *)(g_client_lo + 0x93ab84);
    HMODULE en = GetModuleHandleA("engine.dll");
    if (en) {
        const IMAGE_NT_HEADERS64 *ne = (const IMAGE_NT_HEADERS64 *)((const char *)en + ((const IMAGE_DOS_HEADER *)en)->e_lfanew);
        g_engine_lo = (uintptr_t)en; g_engine_hi = g_engine_lo + ne->OptionalHeader.SizeOfImage;
    }
    /* every 8-byte slot in client.dll's read-only and data sections that holds the function's address */
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (memcmp(s->Name, ".rdata", 6) != 0 && memcmp(s->Name, ".data", 5) != 0) continue;
        void **p = (void **)(g_client_lo + s->VirtualAddress), **end = (void **)(g_client_lo + s->VirtualAddress + (s->Misc.VirtualSize & ~7u));
        for (; p < end && g_npatched < MAX_PATCH; p++) if ((uintptr_t)*p == fn) g_patched[g_npatched++] = p;
    }
    if (g_npatched == 0) { cpu_log("probe bones: no vtable slot holds SetupBones: off"); return -1; }
    for (int i = 0; i < g_npatched; i++) {
        DWORD old;
        if (!VirtualProtect(g_patched[i], sizeof(void *), PAGE_READWRITE, &old)) continue;
        InterlockedCompareExchangePointer(g_patched[i], (void *)probe_setupbones, (void *)fn);
        VirtualProtect(g_patched[i], sizeof(void *), old, &old);
    }
    cpu_log("photon_cpu bones: %d vtable slots wrapped (client.dll at %p): probe %s, fix %s", g_npatched, (void *)cl,
            g_probe ? "on" : "off", g_fix ? "on" : "off");
    return 1;
}

void probe_bones_present(uint64_t frame) {
    static int enabled = -1;
    static uint64_t f0;
    static ULONGLONG t0;
    if (enabled < 0) {
        char e[4];
        g_probe = GetEnvironmentVariableA("PHOTON_CPU_PROBE_BONES", e, sizeof e) > 0 && (e[0] == '1' || e[0] == '2');
        char f[4];   /* the C3c fix is on by default (analysis/02 §10, agreed with Photon Render); PHOTON_CPU_BONES=0 turns it off */
        (void)f;
        g_fix = cpu_setting("photon_cpu_bones", "PHOTON_CPU_BONES", 1) != 0;
        enabled = g_probe || g_fix;
        if (g_probe && e[0] == '2') {
            g_snap = VirtualAlloc(NULL, sizeof(BoneSnap) * NSNAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            g_validate = g_snap != NULL;
        }
    }
    if (!enabled || g_state < 0) return;
    if (g_state == 0) {
        int r = probe_install();
        if (r != 0) { g_state = r; f0 = frame; t0 = GetTickCount64(); }
        return;
    }
    static uint64_t last_active;
    static LONG64 seen_set;
    if (g_fix && fx_set != seen_set) {   /* eye-path work this frame (fx_set grows; the probe's window resets it) */
        seen_set = fx_set;
        if (!last_active || frame - last_active > 300)
            cpu_log("photon_cpu bones: fix active at frame %llu: characters keep their threaded bone setup through GMod's eye-path "
                    "CalcAttachments (jiggle-bone models excluded; PHOTON_CPU_BONES=0 turns it off)", (unsigned long long)frame);
        last_active = frame;
    }
    if (g_fix && frame % 30 == 0) {
        int off = GetFileAttributesA("garrysmod\\data\\photon_cpu_bones_off.txt") != INVALID_FILE_ATTRIBUTES;
        if (off != g_fix_off) { g_fix_off = off; cpu_log("photon_cpu bones: fix %s at frame %llu (runtime flag)", off ? "off" : "on", (unsigned long long)frame); }
    }
    if (frame - f0 < PROBE_WINDOW && GetTickCount64() - t0 < PROBE_WINDOW_MS) return;
    if (g_fix && g_probe) {   /* the per-window counters only with the probe: players get the one activation line */
        double nf = (double)(frame - f0 ? frame - f0 : 1);
        cpu_log("photon_cpu bones: frame %llu: eye-path invalidations %.1f, caches kept %.1f, not kept %.1f, jiggle models left to GMod %.1f per frame",
                (unsigned long long)frame, fx_set / nf, fx_restored / nf, fx_skipped / nf, fx_jiggle / nf);
        fx_set = fx_restored = fx_skipped = fx_jiggle = 0;
        seen_set = 0;
    }
    if (!g_probe) { f0 = frame; t0 = GetTickCount64(); return; }
    if (frame == f0) return;
    double frames = (double)(frame - f0);
    f0 = frame;
    t0 = GetTickCount64();
    double mhz = cpu_tsc_mhz();
    LONG64 tc = 0, ts = 0, tcy = 0;
    for (int i = 0; i < PROBE_SLOTS; i++) { tc += g_rec[i].calls; ts += g_rec[i].slow; tcy += g_rec[i].cycles; }
    cpu_log("probe bones: frame %llu: %.1f calls, %.1f slow per frame, %.3f ms per frame in SetupBones (all threads)%s",
            (unsigned long long)frame, tc / frames, ts / frames, tcy / frames / mhz / 1000.0, g_overflow ? " (caller table full)" : "");
    for (int i = 0; i < PROBE_SLOTS; i++) {
        ProbeRec *p = &g_rec[i];
        LONG64 c = InterlockedExchange64(&p->calls, 0), sl = InterlockedExchange64(&p->slow, 0), m = InterlockedExchange64(&p->main_calls, 0);
        LONG64 cy = InterlockedExchange64(&p->cycles, 0), scy = InterlockedExchange64(&p->slow_cycles, 0);
        if (!c) continue;
        char a[320], b[320], d[320];
        cpu_log("probe bones:   %.2f calls (%.0f%% main), %.2f slow, %.3f ms per frame, slow call %.1f us <- %s <- %s <- %s",
                c / frames, 100.0 * m / c, sl / frames, cy / frames / mhz / 1000.0, sl ? scy / (double)sl / mhz : 0.0,
                cpu_where(p->fr[0], a, sizeof a), cpu_where(p->fr[1], b, sizeof b), cpu_where(p->fr[2], d, sizeof d));
    }
    LONG ne_ = g_nsamp_e < NSAMP / 2 ? g_nsamp_e : NSAMP / 2, no_ = g_nsamp_o < NSAMP / 2 ? g_nsamp_o : NSAMP / 2;
    for (LONG k = 0; k < ne_ + no_; k++) {
        MissSample *m = &g_samp[k < ne_ ? k : NSAMP / 2 + (k - ne_)];
        char a[320];
        cpu_log("probe bones:   miss %p by %s: mask %08x readable %08x -> %08x, prev %08x accum %08x, counter ent %d global %d, "
                "time %.6f last %.6f, %u us", m->self, cpu_where(m->caller, a, sizeof a), (unsigned)m->mask, (unsigned)m->readable0,
                (unsigned)m->readable1, (unsigned)m->prev0, (unsigned)m->accum0, m->ctr0, *g_model_counter, m->t, m->last0, m->dt_us);
    }
    g_nsamp_e = 0; g_nsamp_o = 0;
    if (g_validate) {
        cpu_log("probe bones: validate: %lld compared (%lld with another time), %lld bit-identical, %lld differ (at most %lld bones, "
                "max |delta| %g), %lld engine recomputes without a same-frame snapshot", (long long)v_cmp, (long long)v_cmp_time_diff,
                (long long)v_same, (long long)v_diff, (long long)v_bones_diff_max, v_maxabs, (long long)v_nosnap);
        cpu_log("probe bones: validate: of those, jiggle models %lld compared, %lld bit-identical; models without jiggle bones: %lld compared, "
                "%lld bit-identical, worst %lld bones, max |delta| %g", (long long)vj_cmp, (long long)vj_same, (long long)(v_cmp - vj_cmp),
                (long long)(v_same - vj_same), (long long)vn_bones_max, vn_maxabs);
        v_cmp = v_same = v_diff = v_nosnap = v_bones_diff_max = v_cmp_time_diff = 0;
        vj_cmp = vj_same = vn_bones_max = 0;
        v_maxabs = vn_maxabs = 0;
    }
}
