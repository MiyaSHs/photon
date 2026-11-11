/* P3g: see amb_clip.h. */
#include "amb_clip.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Test-only (scripts/run_ambient_test.sh): each mutant removes one rule, and tests/ambient/cliptest.c must then fail.
 * 1 no shrink before the texels are overwritten, 2 no inset, 3 the box grows before the data is in, 4 a jump keeps the box,
 * 5 X slabs miss the gutter, 6 an inset just under half a voxel, 7 the weight's test lets NaN through, 8 no jump on a full queue,
 * 9 no jump past half a window, 10 the weight comes straight from the clamp box, 11 a jump queues a whole window at once. */
#ifndef AMB_CLIP_MUTANT
#define AMB_CLIP_MUTANT 0
#endif

enum { TEXEL_BYTES = 8, BANDS = 6 };
/* Half a voxel keeps a filter's taps inside the box. The margin is for float error in the shader (3e-5 voxel at +-16384, 15d). */
static const double k_inset = 0.5 + 1.0 / 16.0;
static const double k_max_coord = 65536.0;
static const double k_max_tick = 0.1;

static void shader_box_update(AmbClip *c) {
    double in = AMB_CLIP_MUTANT == 2 ? 0.0 : AMB_CLIP_MUTANT == 6 ? 0.5 - 1.0 / 1024.0 : c->inset;
    for (int a = 0; a < 3; a++) {
        c->smin[a] = c->valid ? (float)(((double)c->vmin[a] + in) * c->voxel[a]) : 0.0f;
        c->smax[a] = c->valid ? (float)(((double)c->vmax[a] - in) * c->voxel[a]) : 0.0f;
    }
    c->smin[3] = c->smax[3] = 0.0f;
}

static void set_empty(AmbClip *c) {
    c->valid = 0;
    c->wgain = 0.0;
    memset(c->vmin, 0, sizeof c->vmin);
    memset(c->vmax, 0, sizeof c->vmax);
    shader_box_update(c);
}

int amb_clip_init_dims(AmbClip *c, const int dim[3], const int voxel[3], int step) {
    if (step <= 0) return AMB_ERR_ARG;
    for (int a = 0; a < 3; a++)
        if (dim[a] <= 0 || dim[a] % step || voxel[a] <= 0) return AMB_ERR_ARG;
    memset(c, 0, sizeof *c);
    for (int a = 0; a < 3; a++) { c->dim[a] = dim[a]; c->voxel[a] = voxel[a]; }
    c->step = step;
    c->hysteresis = step / 2 + (step >= 4 ? 1 : 0);
    c->inset = k_inset;
    c->fade = 256.0;
    c->fade_seconds = 0.3;
    c->catchup_seconds = 0.12;
    c->hold_seconds = 0.3;
    c->survive_seconds = 1.0;
    c->normal_offset = 0.5 * voxel[0];
    c->next_id = 1;
    set_empty(c);
    return AMB_OK;
}

void amb_clip_init(AmbClip *c) {
    static const int dim[3] = { 64, 64, 32 }, voxel[3] = { 32, 32, 32 };
    amb_clip_init_dims(c, dim, voxel, 8);
}

void amb_clip_reset(AmbClip *c) {
    c->qhead = c->qn = 0;
    c->have_window = 0;
    c->holding = 0;
    set_empty(c);
}

void amb_clip_atlas_dim(const AmbClip *c, int atlas[3]) {
    atlas[0] = BANDS * AMB_BAND_TEXELS(c->dim[0]);
    atlas[1] = c->dim[1];
    atlas[2] = c->dim[2];
}
size_t amb_clip_row_bytes(const AmbClip *c) { return (size_t)BANDS * (size_t)AMB_BAND_TEXELS(c->dim[0]) * TEXEL_BYTES; }
size_t amb_clip_slice_bytes(const AmbClip *c) { return amb_clip_row_bytes(c) * (size_t)c->dim[1]; }
size_t amb_clip_atlas_bytes(const AmbClip *c) { return amb_clip_slice_bytes(c) * (size_t)c->dim[2]; }

static AmbClipJob *job_at(AmbClip *c, int i) { return &c->q[(c->qhead + i) % AMB_CLIP_MAX_JOBS]; }
static const AmbClipJob *job_at_c(const AmbClip *c, int i) { return &c->q[(c->qhead + i) % AMB_CLIP_MAX_JOBS]; }
static AmbClipJob *job_find(AmbClip *c, unsigned id) {
    for (int i = 0; i < c->qn; i++) if (job_at(c, i)->id == id) return job_at(c, i);
    return NULL;
}

static AmbClipJob *push(AmbClip *c, int kind, int axis, int dir) {
    AmbClipJob *j = job_at(c, c->qn++);
    memset(j, 0, sizeof *j);
    j->id = c->next_id++;
    if (!j->id) j->id = c->next_id++;
    j->kind = kind;
    j->axis = axis;
    j->dir = dir;
    j->state = AMB_JOB_QUEUED;
    return j;
}

static void push_whole(AmbClip *c) {
    AmbClipJob *j = push(c, AMB_JOB_WHOLE, 0, 0);
    for (int a = 0; a < 3; a++) { j->box.org[a] = c->tail[a]; j->box.dim[a] = c->dim[a]; j->window[a] = c->tail[a]; }
    c->wholes++;
}

/* The slab entering when the window at tail moves one step: its cross-section is the window's, so the grown box stays a box */
static void push_slab(AmbClip *c, int axis, int dir) {
    AmbClipJob *j = push(c, AMB_JOB_SLAB, axis, dir);
    for (int a = 0; a < 3; a++) { j->box.org[a] = c->tail[a]; j->box.dim[a] = c->dim[a]; }
    j->box.dim[axis] = c->step;
    j->box.org[axis] = dir > 0 ? c->tail[axis] + c->dim[axis] : c->tail[axis] - c->step;
    c->tail[axis] += dir * c->step;
    for (int a = 0; a < 3; a++) j->window[a] = c->tail[a];
    c->slabs[axis]++;
}

/* One step towards the target. A step back over a job no worker has taken just removes that job. */
static void step_window(AmbClip *c, int axis, int dir) {
    if (c->qn) {
        const AmbClipJob *last = job_at_c(c, c->qn - 1);
        if (last->kind == AMB_JOB_SLAB && last->axis == axis && last->dir == -dir && last->state == AMB_JOB_QUEUED) {
            c->qn--;
            c->tail[axis] += dir * c->step;
            c->coalesced++;
            return;
        }
    }
    push_slab(c, axis, dir);
}

/* Still holding: 1 until the camera has moved less, over hold_seconds, than it could afford in a whole window's survive_seconds and half a window */
static int hold_continues(AmbClip *c, const double pos[3]) {
    int moved = 0;
    for (int a = 0; a < 3; a++) {
        double allow = 0.5 * c->dim[a] * c->hold_seconds / c->survive_seconds;
        if (fabs(pos[a] / c->voxel[a] - c->hold_cv[a]) > allow) moved = 1;
    }
    if (moved) {
        for (int a = 0; a < 3; a++) c->hold_cv[a] = pos[a] / c->voxel[a];
        c->hold_since = c->clock;
        return 1;
    }
    return c->clock - c->hold_since < c->hold_seconds;
}

int amb_clip_camera(AmbClip *c, const double pos[3]) {
    for (int a = 0; a < 3; a++) if (!(fabs(pos[a]) <= k_max_coord)) return 0;
    if (c->holding) {
        if (hold_continues(c, pos)) return 0;
        c->holding = 0;
    }
    int target[3], jump = !c->have_window, cancelled = c->qn != 0;
    long moves = 0;
    for (int a = 0; a < 3; a++) {
        double cv = pos[a] / c->voxel[a], half = 0.5 * c->dim[a];
        if (!c->have_window) {
            target[a] = c->step * (int)floor((cv - half) / c->step + 0.5);
            continue;
        }
        /* relative to the tail, because that is where the queued jobs will leave the window */
        double d = cv - (c->tail[a] + half);
        target[a] = c->tail[a];
        if (d > c->hysteresis) target[a] += c->step * (int)ceil((d - c->hysteresis) / c->step);
        else if (d < -c->hysteresis) target[a] -= c->step * (int)ceil((-d - c->hysteresis) / c->step);
#if AMB_CLIP_MUTANT != 9
        if (abs(target[a] - c->base[a]) * 2 > c->dim[a]) jump = 1;
#endif
        moves += labs((long)(target[a] - c->tail[a])) / c->step;
    }
#if AMB_CLIP_MUTANT != 8
    if (c->qn + moves > AMB_CLIP_MAX_JOBS) jump = 1;
#endif
    if (jump) {
        int was_scrolling = c->have_window;
        c->qhead = c->qn = 0;
        c->jumps += was_scrolling;
#if AMB_CLIP_MUTANT != 4
        set_empty(c);
#endif
#if AMB_CLIP_MUTANT != 11
        if (was_scrolling) {
            c->have_window = 0;
            c->holding = 1;
            c->hold_since = c->clock;
            for (int a = 0; a < 3; a++) c->hold_cv[a] = pos[a] / c->voxel[a];
            return cancelled ? AMB_CLIP_CANCELLED : 0;
        }
#endif
        c->have_window = 1;
        for (int a = 0; a < 3; a++) c->base[a] = c->tail[a] = target[a];
        push_whole(c);
        return AMB_CLIP_ADDED | (cancelled ? AMB_CLIP_CANCELLED : 0);
    }
    int added = 0;
    for (int a = 0; a < 3; a++)
        while (c->tail[a] != target[a]) {
            step_window(c, a, target[a] > c->tail[a] ? 1 : -1);
            added = 1;
        }
    return added ? AMB_CLIP_ADDED : 0;
}

void amb_clip_tick(AmbClip *c, double dt) {
    if (!(dt > 0.0)) return;
    if (dt > k_max_tick) dt = k_max_tick;
    c->clock += dt;
    if (!c->valid) return;
    c->wgain += dt / c->fade_seconds;
    if (c->wgain > 1.0) c->wgain = 1.0;
    for (int a = 0; a < 3; a++) {
        double one_step = (double)c->step * c->voxel[a], speed = one_step / c->fade_seconds;
        double *edge[2] = { &c->wmin[a], &c->wmax[a] };
        const double target[2] = { c->smin[a], c->smax[a] };
        for (int e = 0; e < 2; e++) {
            double gap = fabs(target[e] - *edge[e]), v = speed + (gap > one_step ? (gap - one_step) / c->catchup_seconds : 0.0);
            double by = v * dt;
            *edge[e] = by >= gap ? target[e] : *edge[e] + (target[e] > *edge[e] ? by : -by);
        }
    }
}

int amb_clip_take(AmbClip *c, AmbClipJob *out) {
    for (int i = 0; i < c->qn; i++) {
        AmbClipJob *j = job_at(c, i);
        if (j->state == AMB_JOB_QUEUED) {
            j->state = AMB_JOB_BAKING;
            *out = *j;
            return 1;
        }
    }
    return 0;
}

int amb_clip_backlog(const AmbClip *c) {
    int n = 0;
    for (int i = 0; i < c->qn; i++) {
        int s = job_at_c(c, i)->state;
        n += s == AMB_JOB_QUEUED || s == AMB_JOB_BAKING;
    }
    return n;
}

int amb_clip_whole_pending(const AmbClip *c) {
    for (int i = 0; i < c->qn; i++) if (job_at_c(c, i)->kind == AMB_JOB_WHOLE) return 1;
    return 0;
}

int amb_clip_alive(const AmbClip *c, unsigned id) {
    for (int i = 0; i < c->qn; i++) if (job_at_c(c, i)->id == id) return 1;
    return 0;
}

int amb_clip_may_write(const AmbClip *c, unsigned id) {
    return c->qn && job_at_c(c, 0)->id == id && job_at_c(c, 0)->state == AMB_JOB_BAKING;
}

void amb_clip_written(AmbClip *c, unsigned id) {
    AmbClipJob *j = job_find(c, id);
    if (j && j->state == AMB_JOB_BAKING) j->state = AMB_JOB_WRITTEN;
}

int amb_clip_upload_ready(const AmbClip *c, AmbClipJob *out) {
    if (!c->qn) return 0;
    const AmbClipJob *j = job_at_c(c, 0);
    if (j->state != AMB_JOB_WRITTEN && j->state != AMB_JOB_UPLOADING) return 0;
    *out = *j;
    return 1;
}

int amb_clip_begin_upload(AmbClip *c, unsigned id) {
    if (!c->qn || job_at(c, 0)->id != id) return 0;
    AmbClipJob *j = job_at(c, 0);
    if (j->state == AMB_JOB_UPLOADING) return 1;
    if (j->state != AMB_JOB_WRITTEN) return 0;
    j->state = AMB_JOB_UPLOADING;
    if (j->kind == AMB_JOB_SLAB) {
#if AMB_CLIP_MUTANT == 3
        if (j->dir > 0) { c->vmin[j->axis] += c->step; c->vmax[j->axis] += c->step; }
        else { c->vmax[j->axis] -= c->step; c->vmin[j->axis] -= c->step; }
#elif AMB_CLIP_MUTANT != 1
        if (j->dir > 0) c->vmin[j->axis] += c->step;
        else c->vmax[j->axis] -= c->step;
#endif
        shader_box_update(c);
    }
    return 1;
}

void amb_clip_end_upload(AmbClip *c, unsigned id) {
    if (!c->qn || job_at(c, 0)->id != id || job_at(c, 0)->state != AMB_JOB_UPLOADING) return;
    const AmbClipJob *j = job_at(c, 0);
    if (j->kind == AMB_JOB_WHOLE) {
        for (int a = 0; a < 3; a++) { c->vmin[a] = j->box.org[a]; c->vmax[a] = j->box.org[a] + j->box.dim[a]; }
        c->valid = 1;
        shader_box_update(c);
        for (int a = 0; a < 3; a++) { c->wmin[a] = c->smin[a]; c->wmax[a] = c->smax[a]; }
        c->wgain = 0.0;
    } else {
#if AMB_CLIP_MUTANT == 1
        if (j->dir > 0) { c->vmin[j->axis] += c->step; c->vmax[j->axis] += c->step; }
        else { c->vmax[j->axis] -= c->step; c->vmin[j->axis] -= c->step; }
#elif AMB_CLIP_MUTANT != 3
        if (j->dir > 0) c->vmax[j->axis] += c->step;
        else c->vmin[j->axis] -= c->step;
#endif
    }
    for (int a = 0; a < 3; a++) c->base[a] = j->window[a];
    c->qhead = (c->qhead + 1) % AMB_CLIP_MAX_JOBS;
    c->qn--;
    shader_box_update(c);
}

int amb_clip_parts(const AmbClip *c, const AmbClipJob *job) {
    return job->kind == AMB_JOB_WHOLE ? c->dim[1] / c->step : 1;
}

void amb_clip_all_box(const AmbClip *c, AmbTexBox *out) {
    int atlas[3];
    amb_clip_atlas_dim(c, atlas);
    AmbTexBox b = { 0, atlas[0], 0, atlas[1], 0, atlas[2] };
    *out = b;
}

/* The window starts on a multiple of step and every dim is one too, so a slab never wraps inside the atlas */
static int texel_of(int a, int d) { int r = a % d; return r < 0 ? r + d : r; }

int amb_clip_boxes(const AmbClip *c, const AmbClipJob *job, int part, AmbTexBox out[AMB_CLIP_MAX_BOXES]) {
    AmbTexBox all;
    amb_clip_all_box(c, &all);
    if (job->kind == AMB_JOB_WHOLE) {
        out[0] = all;
        out[0].v0 = part * c->step;
        out[0].v1 = (part + 1) * c->step;
        return 1;
    }
    int t = texel_of(job->box.org[job->axis], c->dim[job->axis]);
    if (job->axis == 1) { out[0] = all; out[0].v0 = t; out[0].v1 = t + c->step; return 1; }
    if (job->axis == 2) { out[0] = all; out[0].w0 = t; out[0].w1 = t + c->step; return 1; }
    int X = c->dim[0], band = AMB_BAND_TEXELS(X), n = 0;
    for (int f = 0; f < BANDS; f++) {
        out[n] = all;
        out[n].u0 = f * band + t;
        out[n].u1 = f * band + t + c->step;
#if AMB_CLIP_MUTANT != 5
        if (t == 0 && f > 0) out[n].u0--;   /* the previous band's gutter is the texel just before this band */
#endif
        n++;
    }
#if AMB_CLIP_MUTANT != 5
    if (t == 0) { out[n] = all; out[n].u0 = BANDS * band - 1; out[n].u1 = BANDS * band; n++; }
#endif
    return n;
}

float amb_clip_weight(const AmbClip *c, const float p[3]) {
    if (!c->valid) return 0.0f;
    double d = c->fade;
    for (int a = 0; a < 3; a++) {
#if AMB_CLIP_MUTANT == 10
        double lo = (double)p[a] - c->smin[a], hi = (double)c->smax[a] - p[a];
#else
        double lo = (double)p[a] - c->wmin[a], hi = c->wmax[a] - (double)p[a];
#endif
#if AMB_CLIP_MUTANT == 7
        if (lo <= 0.0 || hi <= 0.0) return 0.0f;
#else
        if (!(lo > 0.0) || !(hi > 0.0)) return 0.0f;   /* written so that NaN counts as outside */
#endif
        if (lo < d) d = lo;
        if (hi < d) d = hi;
    }
    return (float)(d / c->fade * (AMB_CLIP_MUTANT == 10 ? 1.0 : c->wgain));
}

void amb_clip_shader_box(const AmbClip *c, float min[4], float max[4]) {
    memcpy(min, c->smin, sizeof c->smin);
    memcpy(max, c->smax, sizeof c->smax);
}

void amb_clip_constants(const AmbClip *c, float a[4], float b[4]) {
    double X = c->dim[0], texels = (double)BANDS * (X + 1.0);
    for (int i = 0; i < 3; i++) a[i] = (float)(1.0 / ((double)c->dim[i] * c->voxel[i]));
    a[3] = (float)c->normal_offset;
    b[0] = (float)(0.5 / X);
    b[1] = (float)(X / texels);
    b[2] = (float)(0.5 / texels);
    b[3] = (float)(1.0 / BANDS);
}

void amb_clip_fold(const float cube[6][4], const float a[AMB_VOXEL_FLOATS], float f, float out[6][4]) {
    for (int face = 0; face < AMB_FACES; face++) {
        for (int k = 0; k < 3; k++) {
            float C = cube[face][k], R = C - 1.5f * a[face * 3 + k];
            if (!(R > 0.0f)) R = 0.0f;
            out[face][k] = C + (R - C) * f;
        }
        out[face][3] = cube[face][3];
    }
}
