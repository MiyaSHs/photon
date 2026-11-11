/* DYN dynamic quality: the controller. See dynq.h for the rules. Runs on the presenting thread. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dynq.h"
#include "log.h"
#include "vlg.h"
#include "nanite.h"
#include "ptrt.h"

enum { RING = 2048, MAXSTEP = 64, GPU = 0, CPU = 1, BOTH = 2 };
static const char *k_side[3] = { "gpu", "cpu", "both" };

typedef struct { char name[32]; int side, cpu_index; } Step;   /* cpu_index: Photon CPU's step number, -1 for the proxy's own */
static Step g_steps[MAXSTEP]; static int g_nsteps;
static int g_ord[MAXSTEP], g_nord;                    /* the order in effect: indices into g_steps */
static char g_order_str[512]; static int g_order_dirty = 1;
static unsigned char g_eng[MAXSTEP];
static int g_stack[MAXSTEP], g_top;                   /* engaged steps, in the order they were engaged */
static double g_bar_until[MAXSTEP], g_bar_len[MAXSTEP];   /* a release that made a dip: barred */
static double g_hold_until[2], g_hold_len[2] = { 10, 10 };  /* a side whose step did not help */

static void (*g_cpu_set)(int, int); static double (*g_cpu_busy)(void); static int (*g_cpu_avail)(int); static double (*g_cpu_saving)(int);
static float g_corr[MAXSTEP];   /* per step: measured saving / expected, learned from its checks (1 until measured) */
static double g_meas[MAXSTEP], g_meas_t[MAXSTEP];   /* per step: the median saving its last check measured, and when */
static double g_now;
static volatile int g_shadow_level;

static LARGE_INTEGER g_freq, g_begin, g_last_end;
static double g_ms[RING], g_wait[RING], g_busy[RING], g_t[RING];
static int g_n, g_i, g_was_on;
static double g_t_move = -1e9;
static int g_check = -1; static double g_check_until, g_check_pre, g_check_pre99, g_check_exp = -1;   /* the step engaged last, being measured */
static int g_prob = -1; static double g_prob_until;                  /* the step released last, on probation */
static long long g_engages, g_releases, g_useless, g_reverts;

static void add_step(const char *name, int side, int cpu_index) {
    if (g_nsteps >= MAXSTEP) return;
    Step *s = &g_steps[g_nsteps++];
    snprintf(s->name, sizeof s->name, "%s", name); s->side = side; s->cpu_index = cpu_index;
    g_order_dirty = 1;
}
static void own_steps(void) {
    static int done;
    if (done) return;
    done = 1;
    char b[16];
    for (int k = 1; k <= 5; k++) { snprintf(b, sizeof b, "shadows%d", k); add_step(b, GPU, -1); }
    add_step("nanitelod", GPU, -1);   /* Nanite: the models' lower LODs at half the distance */
    add_step("ptcheck", GPU, -1);     /* PT-RT: half the pixels traced per frame (checkerboard), the rest from history */
}
static volatile int g_nanitelod, g_ptcheck;

static void apply(int s, int on) {
    g_eng[s] = (unsigned char)on;
    const Step *S = &g_steps[s];
    if (S->cpu_index >= 0) { if (g_cpu_set) g_cpu_set(S->cpu_index, on); return; }
    if (!strcmp(S->name, "nanitelod")) { g_nanitelod = on; return; }
    if (!strcmp(S->name, "ptcheck")) { g_ptcheck = on; return; }
    int lvl = 0;   /* the proxy's: shadowsN -> the highest engaged N */
    for (int i = 0; i < g_nsteps; i++)
        if (g_eng[i] && g_steps[i].cpu_index < 0 && !strncmp(g_steps[i].name, "shadows", 7)) { int k = g_steps[i].name[7] - '0'; if (k > lvl) lvl = k; }
    g_shadow_level = lvl;
}
static void release_all(void) {
    for (int s = 0; s < g_nsteps; s++) if (g_eng[s]) apply(s, 0);
    g_top = 0; g_check = g_prob = -1;
}
static int available(int s) {
    if (g_steps[s].cpu_index >= 0) return g_cpu_set && (!g_cpu_avail || g_cpu_avail(g_steps[s].cpu_index));   /* something to act on now */
    if (!strcmp(g_steps[s].name, "nanitelod")) return photon_nanite_on();
    if (!strcmp(g_steps[s].name, "ptcheck")) return photon_ptrt_on();
    return photon_vlg_shadows_wanted();   /* shadowsN: only while Photon draws the shadows */
}

/* the ms a frame engaging step s is expected to save now (Photon CPU's live estimate x what this step's checks have learned); < 0: not
 * measured (the user's order decides) */
static double expected_saving(int s) {
    double v = g_steps[s].cpu_index >= 0 && g_cpu_saving ? g_cpu_saving(g_steps[s].cpu_index) : -1;
    if (v >= 0) return v * (g_corr[s] > 0 ? g_corr[s] : 1.0f);
    /* no live estimate: what its last check measured, within a minute (photon-64: a remembered measured saving can learn where an
     * estimate cannot) */
    if (g_meas_t[s] > 0 && g_now - g_meas_t[s] < 60.0) return g_meas[s] > 0 ? g_meas[s] : 0;
    return -1;
}

/* the order: photon_dyn_order's names that exist, or (empty) Photon CPU's steps then the proxy's */
static void resolve_order(void) {
    char cur[512];
    photon_vlg_dyn_order(cur, sizeof cur);
    if (!g_order_dirty && !strcmp(cur, g_order_str)) return;
    if (g_top) release_all();
    snprintf(g_order_str, sizeof g_order_str, "%s", cur);
    g_order_dirty = 0; g_nord = 0;
    if (cur[0]) {
        for (char *tok = strtok(cur, ", "); tok; tok = strtok(NULL, ", "))
            for (int s = 0; s < g_nsteps; s++)
                if (!_stricmp(g_steps[s].name, tok)) { int dup = 0; for (int k = 0; k < g_nord; k++) dup |= g_ord[k] == s; if (!dup) g_ord[g_nord++] = s; }
    } else {
        for (int pass = 0; pass < 2; pass++)
            for (int s = 0; s < g_nsteps; s++) if ((g_steps[s].cpu_index >= 0) == (pass == 0)) g_ord[g_nord++] = s;
    }
    char line[1024]; int n = 0;
    for (int k = 0; k < g_nord && n < (int)sizeof line - 40; k++) n += snprintf(line + n, sizeof line - (size_t)n, "%s%s(%s)", k ? " " : "", g_steps[g_ord[k]].name, k_side[g_steps[g_ord[k]].side]);
    photon_log("dynq: order%s: %s", g_order_str[0] ? "" : " (default)", n ? line : "(no steps)");
}

static double select_k(double *v, int k, int at) {   /* the at-th smallest (partial quickselect, reorders v) */
    int lo = 0, hi = k - 1;
    while (lo < hi) {
        double piv = v[(lo + hi) / 2]; int a = lo, b = hi;
        while (a <= b) { while (v[a] < piv) a++; while (v[b] > piv) b--; if (a <= b) { double x = v[a]; v[a] = v[b]; v[b] = x; a++; b--; } }
        if (at <= b) hi = b; else if (at >= a) lo = a; else break;
    }
    return v[at];
}
/* the samples taken in (max(from, the last move), to]: 99th percentile, means of interval, Present wait, main-thread busy (-1: unknown) */
static double g_median;   /* the last range()'s median interval */
static int range(double from, double to, double *p99, double *mean, double *wait, double *busy) {
    static double v[RING];
    int k = 0, kb = 0;
    double sm = 0, sw = 0, sb = 0;
    if (from < g_t_move) from = g_t_move;
    for (int j = 0; j < g_n; j++) {
        int i = (g_i - 1 - j + RING) % RING;
        if (g_t[i] <= from) break;
        if (g_t[i] > to) continue;
        v[k++] = g_ms[i]; sm += g_ms[i]; sw += g_wait[i];
        if (g_busy[i] > 0) { sb += g_busy[i]; kb++; }   /* 0 or less: not measured */
    }
    if (!k) return 0;
    *p99 = select_k(v, k, (int)(0.99 * (k - 1) + 0.5));
    g_median = select_k(v, k, (k - 1) / 2); *mean = sm / k; *wait = sw / k; *busy = kb ? sb / kb : -1;
    return k;
}
static int window(double t, double w, double *p99, double *mean, double *wait, double *busy) { return range(t - w, t, p99, mean, wait, busy); }

static void move(int s, int on, double t, const char *why, double p99, double mean, double wait, double busy) {
    apply(s, on);
    g_t_move = t;
    char b[32] = "";
    if (busy >= 0) snprintf(b, sizeof b, ", main busy %.2f", busy);
    photon_log("dynq: %s %s (%s; %s; p99 %.2f ms, mean %.2f, present wait %.0f%%%s)", on ? "engage" : "release", g_steps[s].name,
               k_side[g_steps[s].side], why, p99, mean, mean > 0 ? 100.0 * wait / mean : 0.0, b);
}

void photon_dynq_present_begin(void) {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_begin);
}

void photon_dynq_present_end(void) {
    LARGE_INTEGER e;
    QueryPerformanceCounter(&e);
    double ms = g_last_end.QuadPart ? (double)(e.QuadPart - g_last_end.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 0;
    double wait = (double)(e.QuadPart - g_begin.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
    double t = (double)e.QuadPart / (double)g_freq.QuadPart;
    g_last_end = e;
    g_now = t;
    int fps = photon_vlg_target_fps();
    if (fps <= 0) {   /* off: back to the user's settings */
        if (g_was_on) {
            release_all(); g_was_on = 0;
            photon_log("dynq: off (engaged %lld, released %lld, useless %lld, re-engaged after a dip %lld)", g_engages, g_releases, g_useless, g_reverts);
        }
        return;
    }
    own_steps();
    resolve_order();
    if (!g_was_on) { g_was_on = 1; g_t_move = t + 2.5; photon_log("dynq: on, target %d fps, %d steps in the order", fps, g_nord); }
    /* a hitch (a load, streaming, a pause: over 250 ms or 6 budgets) is not something a step can fix: left out, and 2 s to settle after */
    if (ms <= 0 || ms > 250 || ms > 6000.0 / fps) { if (g_t_move < t + 1.5) g_t_move = t + 1.5; return; }
    g_ms[g_i] = ms; g_wait[g_i] = wait; g_busy[g_i] = g_cpu_busy ? g_cpu_busy() : -1; g_t[g_i] = t;
    g_i = (g_i + 1) % RING; if (g_n < RING) g_n++;
    if (t - g_t_move < 0.5) return;
    { double a, b, c, d; if (window(t, 1.5, &a, &b, &c, &d) < 20) return; }   /* too few frames to judge */
    double budget = 1000.0 / fps, p99, mean, w, busy;
    /* the step released last, on probation: a dip engages it again and bars its release */
    if (g_prob >= 0) {
        int s = g_prob;
        if (window(t, 2.0, &p99, &mean, &w, &busy) && p99 > 0.9 * budget) {
            g_bar_len[s] = g_bar_len[s] > 0 ? (g_bar_len[s] * 2 < 240 ? g_bar_len[s] * 2 : 240) : 30;
            g_bar_until[s] = t + g_bar_len[s];
            if (g_top < MAXSTEP) g_stack[g_top++] = s;
            g_reverts++; g_prob = -1;
            move(s, 1, t, "its release made a dip: back on, release barred", p99, mean, w, busy);
            return;
        }
        if (t >= g_prob_until) g_prob = -1;
    }
    /* the step engaged last: no faster means the other side bounds the frame */
    if (g_check >= 0 && t < g_check_until) return;   /* the last step is still being measured: nothing else moves meanwhile */
    if (g_check >= 0) {
        int s = g_check, side = g_steps[s].side;
        g_check = -1;
        /* helped: the median >= 2% faster, or the 1% lows >= 10% better (a step that removes spikes, such as corpses, may not move the median) */
        int have = window(t, 1.5, &p99, &mean, &w, &busy);
        /* remembered only when conclusive (photon-64): a check that started far below the budget, or whose median rose by more than
         * half (the scene changed under it, as a battle ramping up), says nothing about the step */
        double budget_ms = fps > 0 ? 1000.0 / fps : 0;
        int conclusive = have && g_check_pre >= 0.5 * budget_ms && g_median <= 1.5 * g_check_pre;
        if (conclusive) { g_meas[s] = g_check_pre - g_median; g_meas_t[s] = t; }
        if (have && g_check_exp > 0.05) {   /* learn: measured median saving / expected (smoothed, 0.2..2) */
            float r = (float)((g_check_pre - g_median) / g_check_exp);
            r = r < 0.2f ? 0.2f : r > 2.0f ? 2.0f : r;
            g_corr[s] = g_corr[s] > 0 ? 0.5f * g_corr[s] + 0.5f * r : r;
        }
        if (have && g_median > g_check_pre * 0.98 && p99 > g_check_pre99 * 0.9) {
            g_hold_until[side] = t + g_hold_len[side];
            photon_log("dynq: %s did not help (median %.2f -> %.2f ms, p99 %.2f -> %.2f): %s side held %.0f s", g_steps[s].name, g_check_pre, g_median,
                       g_check_pre99, p99, k_side[side], g_hold_len[side]);
            if (g_hold_len[side] < 160) g_hold_len[side] *= 2;
            if (g_top > 0 && g_stack[g_top - 1] == s) g_top--;
            g_useless++;
            move(s, 0, t, "undo", p99, mean, w, busy);
            return;
        }
        g_hold_len[side] = 10;
    }
    /* engage before the budget is crossed (user: "prevent more than fix"): the 1% lows over 90% of the budget, or rising so fast (the
     * last 0.75 s against the 0.75 s before) that they would pass the budget within a second */
    int early = 0;
    if (t - g_t_move >= 1.5) {
        double a99, am, aw, ab, b99, bm, bw, bb;
        if (range(t - 1.5, t - 0.75, &a99, &am, &aw, &ab) >= 8 && range(t - 0.75, t, &b99, &bm, &bw, &bb) >= 8)
            early = b99 > 0.75 * budget && b99 + (b99 - a99) / 0.75 > budget;
    }
    if (window(t, 1.5, &p99, &mean, &w, &busy) && (p99 > 0.9 * budget || early)) {
        int gpu = w > 0.15 * mean, cpu = busy >= 0 ? busy > 0.85 * budget : !gpu;
        int bound = gpu && cpu ? BOTH : gpu ? GPU : CPU, pick = -1;
        /* the other side's steps only when the bound is not measured (no busy time): a clearly CPU-bound frame gains nothing from GPU steps */
        int passes = busy >= 0 ? 1 : 2;
        /* cost first (user: "take into account what actually is using a lot"): among the free steps of the bound side, the largest
         * expected saving above 3% of the budget; steps within 20% of it go by the user's order (which also vetoes: a step left out is
         * never a candidate); a step with no estimate falls back to the order; one expected to save nothing is skipped */
        double best = -1;
        for (int pass = 0; pass < passes && pick < 0; pass++) {
            for (int k = 0; k < g_nord; k++) {
                int s = g_ord[k], side = g_steps[s].side;
                if (g_eng[s] || !available(s) || t < g_hold_until[side]) continue;
                if (pass == 0 && bound != BOTH && side != bound) continue;
                double e = expected_saving(s);
                if (e > best) best = e;
            }
            for (int k = 0; k < g_nord && pick < 0; k++) {
                int s = g_ord[k], side = g_steps[s].side;
                if (g_eng[s] || !available(s) || t < g_hold_until[side]) continue;
                if (pass == 0 && bound != BOTH && side != bound) continue;
                double e = expected_saving(s);
                if (best >= 0.03 * budget) { if (e >= 0.8 * best) pick = s; }   /* the costliest (near-ties by order) */
                else if (e < 0) pick = s;                                         /* nothing measured worth it: the order's unmeasured */
            }
        }
        if (pick < 0) return;
        if (g_top < MAXSTEP) g_stack[g_top++] = pick;
        g_check = pick; g_check_pre = g_median; g_check_pre99 = p99; g_check_until = t + 2.0;
        g_engages++; g_prob = -1;
        char why[120]; double ex = expected_saving(pick);
        snprintf(why, sizeof why, "1%% lows %s, %s-bound", early && p99 <= 0.9 * budget ? "rising toward the budget" : "over 90% of the budget", k_side[bound]);
        if (ex >= 0) { size_t l = strlen(why); snprintf(why + l, sizeof why - l, ", expected saving %.2f ms", ex); }
        g_check_exp = ex;
        move(pick, 1, t, why, p99, mean, w, busy);
        return;
    }
    /* release: the last step engaged, after 4 s with the 1% lows under 75% of the budget (a wide band below the 90% that engages, so a
     * release never climbs straight back into what had to be left) */
    if (g_top > 0 && t - g_t_move >= 4.0 && window(t, 4.0, &p99, &mean, &w, &busy) && p99 < 0.75 * budget) {
        int s = g_stack[g_top - 1];
        if (t < g_bar_until[s]) return;
        g_top--; g_releases++;
        g_prob = s; g_prob_until = t + 2.0; g_check = -1;
        move(s, 0, t, "4 s under 75% of the budget", p99, mean, w, busy);
    }
}

int photon_dynq_shadow_level(void) { return g_was_on ? g_shadow_level : 0; }
int photon_dynq_nanite_lod_bias(void) { return g_was_on && g_nanitelod; }
int photon_dynq_ptrt_checker(void) { return g_was_on && g_ptcheck; }

void photon_dynq_set_cpu_steps(int n, const char *const *names, void (*set)(int step, int engaged), double (*busy_ms)(void), int (*avail)(int step),
                               double (*saving)(int step)) {
    g_cpu_set = set; g_cpu_busy = busy_ms; g_cpu_avail = avail; g_cpu_saving = saving;
    /* a name "gpu:<name>" is a step the module carries out that saves GPU time (e.g. the render scale, which only the console path
     * reaches: mat_viewportscale set in memory is ignored) */
    for (int i = 0; i < n; i++) if (names && names[i]) {
        if (!strncmp(names[i], "gpu:", 4)) add_step(names[i] + 4, GPU, i); else add_step(names[i], CPU, i);
    }
    photon_log("dynq: %d steps from Photon CPU%s%s", n, busy_ms ? ", with the main thread's busy time" : "", avail ? ", and whether each has something to act on" : "");
}
