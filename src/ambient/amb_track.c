/* P3h: see amb_track.h. */
#include "amb_track.h"
#include <math.h>
#include <string.h>

/* Test-only (scripts/run_ambient_test.sh): each mutant removes one rule, and tests/ambient/tracktest.c must then fail.
 * 1 no velocity prediction, 2 one averaging step per frame instead of per time, 3 no teleport join, 4 no same-frame reuse, 5 dt capped at 0.1 s, 6 a new track
 * starts from zero instead of the current value. */
#ifndef AMB_TRACK_MUTANT
#define AMB_TRACK_MUTANT 0
#endif

static const double k_bound_base = 64.0, k_bound_speed = 4000.0, k_ambiguous = 1.5, k_forget = 1e-4;
static const unsigned long long k_forget_frames = 300;
static const float k_cube_max = 8.0f;   /* FUN_180105bc0's clamp (DAT_18039cf7c) */

void amb_track_reset(AmbTrack *t) {
    AmbTrackStats st = t->st;
    memset(t, 0, sizeof *t);
    t->st = st;
}

static void retop(AmbTrack *t) {
    while (t->top > 0 && !t->e[t->top - 1].used) t->top--;
}

static void join_teleport(AmbTrack *t) {
    /* frame t->frame has ended: one track born in it and one that was drawn the frame before and not in it are the same model */
    int born = -1, lost = -1, nb = 0, nl = 0;
    for (int i = 0; i < t->top; i++) {
        const AmbTrackEntry *e = &t->e[i];
        if (!e->used) continue;
        if (e->born == t->frame && e->frame == t->frame) { born = i; nb++; }
        else if (t->frame > 0 && e->frame == t->frame - 1) { lost = i; nl++; }
    }
    if (AMB_TRACK_MUTANT == 3 || nb != 1 || nl != 1) return;
    AmbTrackEntry *n = &t->e[born], *o = &t->e[lost];
    /* what the engine would have done at that frame: one averaging step from the old history to the new place's value */
    if (t->average && t->rate != 0.0f) {
        double dt = n->stamp - o->stamp;
        if (!(dt > 0.0)) dt = 0.0;
        float k = expf((float)(-dt * t->rate));
        if (!(k >= 0.0f)) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        for (int i = 0; i < AMB_TRACK_FLOATS; i++) {
            float v = n->a[i] - (n->a[i] - o->a[i]) * k;
            n->a[i] = v < 0.0f ? 0.0f : v > k_cube_max ? k_cube_max : v;
        }
    }
    n->born = o->born;
    o->used = 0;
    t->st.joined++;
}

void amb_track_frame(AmbTrack *t, unsigned long long frame, double now, float rate, int average) {
    if (t->have_frame && frame == t->frame) return;
    if (t->have_frame) join_teleport(t);
    for (int i = 0; i < t->top; i++) {
        AmbTrackEntry *e = &t->e[i];
        if (!e->used) continue;
        double gone = now - e->stamp;
        int forgotten = frame > e->frame + k_forget_frames || (average && rate > 0.0f && gone > 0.0 && exp(-gone * rate) < k_forget);
        if (forgotten) { e->used = 0; t->st.dropped++; }
    }
    retop(t);
    t->frame = frame;
    t->now = now;
    t->rate = rate;
    t->average = average;
    t->have_frame = 1;
}

static unsigned hint_of(const float o[3]) {
    unsigned h = 2166136261u, u;
    for (int k = 0; k < 3; k++) { memcpy(&u, &o[k], sizeof u); h = (h ^ u) * 16777619u; }
    return (h ^ (h >> 15)) % AMB_TRACK_HINTS;
}

static void step(AmbTrack *t, AmbTrackEntry *e, const float a[AMB_TRACK_FLOATS]) {
    if (!t->average || t->rate == 0.0f) {   /* the engine returns the current state and leaves the entry alone */
        t->st.unaveraged++;
        return;
    }
    double dt = t->now - e->stamp;
    if (AMB_TRACK_MUTANT == 2) dt = 1.0 / 60.0;
    if (AMB_TRACK_MUTANT == 5 && dt > 0.1) dt = 0.1;
    if (dt > 0.0) e->stamp = t->now;
    else dt = 0.0;
    float k = expf((float)(-dt * t->rate));
    if (!(k >= 0.0f)) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    for (int i = 0; i < AMB_TRACK_FLOATS; i++) {
        float v = a[i] - (a[i] - e->a[i]) * k;
        e->a[i] = v < 0.0f ? 0.0f : v > k_cube_max ? k_cube_max : v;
    }
}

void amb_track_draw(AmbTrack *t, const float origin[3], const float a[AMB_TRACK_FLOATS], float out[AMB_TRACK_FLOATS]) {
    t->st.draws++;
    if (!t->have_frame) { memmove(out, a, sizeof(float) * AMB_TRACK_FLOATS); t->st.unaveraged++; return; }
    unsigned h = hint_of(origin);
    int hinted = t->hint[h] ? t->hint[h] - 1 : -1;
    if (hinted >= 0 && !(t->e[hinted].used && memcmp(t->e[hinted].pos, origin, sizeof origin[0] * 3) == 0)) hinted = -1;
    /* another draw of a model already handled this frame */
    if (AMB_TRACK_MUTANT != 4 && hinted >= 0 && t->e[hinted].frame == t->frame) {
        t->st.reused++;
        memmove(out, (!t->average || t->rate == 0.0f) ? a : t->e[hinted].a, sizeof(float) * AMB_TRACK_FLOATS);
        return;
    }
    int best = -1;
    double d1 = 1e300, d2 = 1e300;
    if (hinted >= 0 && t->e[hinted].frame < t->frame) { best = hinted; d1 = 0.0; }   /* standing where it was: nothing can be nearer */
    for (int i = 0; i < t->top && (hinted < 0 || best != hinted); i++) {
        const AmbTrackEntry *e = &t->e[i];
        if (!e->used || e->frame >= t->frame) continue;
        double since = t->now - e->seen;
        if (!(since > 0.0)) since = 0.0;
        double d = 0.0;
        for (int k = 0; k < 3; k++) {
            double p = e->pos[k] + (AMB_TRACK_MUTANT == 1 ? 0.0 : e->vel[k] * since);
            d += (origin[k] - p) * (origin[k] - p);
        }
        d = sqrt(d);
        if (d > k_bound_base + k_bound_speed * since) continue;
        if (d < d1) { d2 = d1; d1 = d; best = i; }
        else if (d < d2) d2 = d;
    }
    if (best >= 0) {
        AmbTrackEntry *e = &t->e[best];
        if (d2 < 1e300 && d2 <= k_ambiguous * d1 + 1.0) t->st.ambiguous++;
        t->st.matched++;
        step(t, e, a);
        double since = t->now - e->seen;
        if (since > 0.0)
            for (int k = 0; k < 3; k++) e->vel[k] = (float)((origin[k] - e->pos[k]) / since);
        memcpy(e->pos, origin, sizeof e->pos);
        e->seen = t->now;
        e->frame = t->frame;
        t->hint[h] = (unsigned short)(best + 1);
        memmove(out, (!t->average || t->rate == 0.0f) ? a : e->a, sizeof(float) * AMB_TRACK_FLOATS);
        return;
    }
    for (int i = 0; i < AMB_TRACK_MAX; i++) {
        AmbTrackEntry *e = &t->e[i];
        if (e->used) continue;
        memset(e, 0, sizeof *e);
        e->used = 1;
        memcpy(e->pos, origin, sizeof e->pos);
        if (AMB_TRACK_MUTANT == 6) memset(e->a, 0, sizeof e->a);
        else memcpy(e->a, a, sizeof e->a);
        e->stamp = e->seen = t->now;
        e->frame = e->born = t->frame;
        t->hint[h] = (unsigned short)(i + 1);
        if (i + 1 > t->top) t->top = i + 1;
        t->st.created++;
        memmove(out, AMB_TRACK_MUTANT == 6 ? e->a : a, sizeof(float) * AMB_TRACK_FLOATS);
        return;
    }
    t->st.overflow++;
    memmove(out, a, sizeof(float) * AMB_TRACK_FLOATS);
}
