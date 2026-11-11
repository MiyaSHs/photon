/* The clipmap scheduler (src/ambient/amb_clip.h) against a simulated atlas of voxel ids, written by the real amb_write_toroidal and
 * read with the shader's own arithmetic (src/shaderdll/hlsl/photon_ambvol_ps.h) in float32, including all 8 trilinear taps.
 *
 * The invariant checked after every simulated event: any pixel of a draw whose weight is above 0 fetches, on every tap, the voxel
 * that belongs to its clamped world position, from the current map. Cameras walk, run, fly, jitter around a step boundary, teleport
 * and stop; workers finish late and in any order, uploads stretch over frames with draws in between, devices reset, maps change.
 * When the camera stops, the window must converge and every voxel of it must be right.
 *
 * Usage: cliptest [--seeds N] [--frames N] [--quiet]. Exit 0 if all held. scripts/run_ambient_test.sh also builds this against each
 * AMB_CLIP_MUTANT and requires a failure. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ambient/amb_bake.h"
#include "../../src/ambient/amb_clip.h"
#include "synth_bsp.h"

static long g_fails;
static int g_quiet;
#define CHECK(cond, ...) do { if (!(cond)) { if (g_fails++ < 8) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

/* --- rng ---------------------------------------------------------------------------------------------------------- */
typedef struct { uint64_t s; } Rng;
static uint64_t rnd64(Rng *r) {
    uint64_t z = (r->s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static int rnd(Rng *r, int n) { return (int)(rnd64(r) % (uint64_t)n); }
static double rndf(Rng *r, double lo, double hi) { return lo + (hi - lo) * ((double)(rnd64(r) >> 11) / 9007199254740992.0); }

/* --- the simulated system ---------------------------------------------------------------------------------------- */
enum { ID_X = 1024, ID_Y = 1024, ID_Z = 128, ID_GEN = 256, GENS = 7, XY_LIMIT = 15000, Z_LIMIT = 3000 };

typedef struct {
    int busy, delay, wrote;      /* wrote: its voxels are in the CPU atlas, amb_clip_written still to come */
    AmbClipJob job;
} SimWorker;

typedef struct {
    AmbClip c;
    int atlas[3], X, band;
    size_t rp, sp;
    uint8_t *cpu, *gpu;
    float *vol;
    int nworkers;
    SimWorker w[2];
    int gen;                     /* the map: part of every voxel id, so another map's voxels cannot pass */
    /* the upload in progress */
    unsigned up_id;
    int up_part, up_box, up_n, up_delay;
    AmbTexBox up_boxes[AMB_CLIP_MAX_BOXES];
    int need_full;               /* the device was reset: draws use the cube until the whole atlas is re-uploaded */
    /* stats */
    long draws, pixels, taps, in_box_draws, resets, map_changes, stale_writes, upload_parts, gutter_slabs;
} Sim;

static void sim_free(Sim *s) { free(s->cpu); free(s->gpu); free(s->vol); }

static int sim_init(Sim *s, const int dim[3], const int voxel[3], int step, int nworkers) {
    memset(s, 0, sizeof *s);
    if (amb_clip_init_dims(&s->c, dim, voxel, step)) return 0;
    amb_clip_atlas_dim(&s->c, s->atlas);
    s->X = dim[0];
    s->band = AMB_BAND_TEXELS(dim[0]);
    s->rp = amb_clip_row_bytes(&s->c);
    s->sp = amb_clip_slice_bytes(&s->c);
    s->cpu = calloc(1, amb_clip_atlas_bytes(&s->c));
    s->gpu = calloc(1, amb_clip_atlas_bytes(&s->c));
    s->vol = malloc((size_t)dim[0] * (size_t)dim[1] * (size_t)dim[2] * AMB_VOXEL_FLOATS * sizeof(float));
    s->nworkers = nworkers;
    return s->cpu && s->gpu && s->vol;
}

/* The voxel id of (ax, ay, az) in map gen: exact integers in half floats (11 significant bits) */
static void voxel_rgb(const Sim *s, const int a[3], float rgb[3]) {
    rgb[0] = (float)(a[0] + ID_X);
    rgb[1] = (float)(a[1] + ID_Y);
    rgb[2] = (float)(a[2] + ID_Z + ID_GEN * s->gen);
}

static void write_job_to_cpu(Sim *s, const AmbClipJob *j) {
    const AmbBox *b = &j->box;
    float *o = s->vol;
    for (int k = 0; k < b->dim[2]; k++)
        for (int y = 0; y < b->dim[1]; y++)
            for (int i = 0; i < b->dim[0]; i++) {
                int a[3] = { b->org[0] + i, b->org[1] + y, b->org[2] + k };
                float rgb[3];
                voxel_rgb(s, a, rgb);
                for (int f = 0; f < AMB_FACES; f++, o += 3) memcpy(o, rgb, sizeof rgb);
            }
    int rc = amb_write_toroidal(s->vol, b, s->c.dim, s->cpu, s->rp, s->sp);
    CHECK(rc == AMB_OK, "amb_write_toroidal %d", rc);
}

static void copy_box(Sim *s, const AmbTexBox *b) {
    for (int w = b->w0; w < b->w1; w++)
        for (int v = b->v0; v < b->v1; v++)
            memcpy(s->gpu + (size_t)w * s->sp + (size_t)v * s->rp + (size_t)b->u0 * 8,
                   s->cpu + (size_t)w * s->sp + (size_t)v * s->rp + (size_t)b->u0 * 8, (size_t)(b->u1 - b->u0) * 8);
}

/* --- the invariant --------------------------------------------------------------------------------------------- */
static int wrapi(int a, int d) { int r = a % d; return r < 0 ? r + d : r; }

/* The shader's fetch of one pixel, on one face, with the taps a linear filter reads. Addressing per amb_bake.h; texel positions are
 * what the float32 path really produces, so its rounding is part of what is tested. */
static void check_pixel(Sim *s, const float pixel[3], int face) {
    float a59[4], b60[4], mn[4], mx[4], p[3], t[3];
    amb_clip_constants(&s->c, a59, b60);
    amb_clip_shader_box(&s->c, mn, mx);
    for (int i = 0; i < 3; i++) {
        p[i] = pixel[i] > mn[i] ? pixel[i] : mn[i];
        p[i] = p[i] < mx[i] ? p[i] : mx[i];
        t[i] = p[i] * a59[i];
    }
    float shifted = t[0] - b60[0], fr = shifted - floorf(shifted);
    int period = (int)floorf(shifted);
    float u = (float)face * b60[3] + fr * b60[1] + b60[2];
    double U = (double)u * s->atlas[0] - 0.5, V = (double)t[1] * s->atlas[1] - 0.5, W = (double)t[2] * s->atlas[2] - 0.5;
    int i0 = (int)floor(U), j0 = (int)floor(V), k0 = (int)floor(W), lo = face * s->band;
    double fu = U - i0, fv = V - j0, fw = W - k0;
    for (int bits = 0; bits < 8; bits++) {
        /* float32 leaves a pixel at a texel centre up to 2e-5 texels short of it, on the far side of a band's edge (amb_bake.h's "never leaves" holds
         * to that error), so a tap of no more weight than that is not a read. The limit is well above the error and well below any real margin:
         * an inset just under half a voxel must fail here (AMB_CLIP_MUTANT 6). */
        static const double none = 1.0 / 4096.0;
        if ((bits & 1 ? fu : 1 - fu) < none || (bits & 2 ? fv : 1 - fv) < none || (bits & 4 ? fw : 1 - fw) < none) continue;
        int i = i0 + (bits & 1), j = j0 + (bits >> 1 & 1), k = k0 + (bits >> 2 & 1);
        CHECK(i >= lo && i <= lo + s->X, "tap u %d left band %d", i, face);
        /* the voxel this tap stands for in this pixel's period of the torus, and the voxel it holds */
        int want[3] = { period * s->X + (i - lo), j, k };
        const uint16_t *q = (const uint16_t *)(s->gpu + (size_t)wrapi(k, s->atlas[2]) * s->sp + (size_t)wrapi(j, s->atlas[1]) * s->rp +
                                               (size_t)(i < 0 ? 0 : i >= s->atlas[0] ? s->atlas[0] - 1 : i) * 8);
        float rgb[3], exp[3];
        for (int c = 0; c < 3; c++) rgb[c] = amb_f16_to_f32(q[c]);
        voxel_rgb(s, want, exp);
        CHECK(rgb[0] == exp[0] && rgb[1] == exp[1] && rgb[2] == exp[2],
              "pixel (%.3f %.3f %.3f) clamped to (%.3f %.3f %.3f), face %d, tap %d: wants voxel %d %d %d gen %d, texel holds %g %g %g gen %g;"
              " valid %d..%d %d..%d %d..%d", pixel[0], pixel[1], pixel[2], p[0], p[1], p[2], face, bits, want[0], want[1], want[2], s->gen,
              rgb[0] - ID_X, rgb[1] - ID_Y, fmod(rgb[2], ID_GEN) - ID_Z, floor(rgb[2] / ID_GEN), s->c.vmin[0], s->c.vmax[0], s->c.vmin[1], s->c.vmax[1],
              s->c.vmin[2], s->c.vmax[2]);
        s->taps++;
    }
    s->pixels++;
}

/* Random draws around the camera and the box; a draw with weight > 0 may still paint pixels anywhere, which the clamp must hold */
static void check_draws(Sim *s, Rng *r, const double cam[3]) {
    for (int n = 0; n < 6; n++) {
        float o[3];
        for (int a = 0; a < 3; a++) {
            double base = cam[a], range = 1500;
            if (s->c.valid && (n & 1)) {   /* close to the box, its edges included */
                double lo = s->c.vmin[a] * s->c.voxel[a], hi = s->c.vmax[a] * s->c.voxel[a];
                base = rndf(r, lo - 32, hi + 32);
                range = 1;
            }
            o[a] = (float)(base + rndf(r, -range, range));
        }
        s->draws++;
        float f = amb_clip_weight(&s->c, o);
        /* the weight itself, recomputed */
        float mn[4], mx[4];
        amb_clip_shader_box(&s->c, mn, mx);
        double d = 1e30;
        for (int a = 0; a < 3; a++) {
            if ((double)o[a] - s->c.wmin[a] < d) d = (double)o[a] - s->c.wmin[a];
            if (s->c.wmax[a] - (double)o[a] < d) d = s->c.wmax[a] - (double)o[a];
        }
        double want = !s->c.valid || d <= 0 ? 0 : s->c.wgain * (d >= 256 ? 1 : d / 256);
        CHECK(fabs((double)f - want) < 1e-5, "weight %.6f, wanted %.6f (d %.3f, valid %d)", f, want, d, s->c.valid);
        if (!(f > 0) || s->need_full) continue;
        s->in_box_draws++;
        for (int q = 0; q < 4; q++) {
            float px[3];
            for (int a = 0; a < 3; a++) {
                double spread = q == 0 ? 0 : q == 1 ? 40 : q == 2 ? 600 : 6000;
                px[a] = (float)((double)o[a] + rndf(r, -spread, spread));
            }
            check_pixel(s, px, rnd(r, AMB_FACES));
        }
    }
}

/* --- one frame of the system -------------------------------------------------------------------------------------- */
static int worker_may_start_write(const Sim *s, int self) {
    for (int i = 0; i < s->nworkers; i++) if (i != self && s->w[i].wrote) return 0;   /* the atlas lock */
    return 1;
}

static void workers_step(Sim *s, Rng *r) {
    for (int i = 0; i < s->nworkers; i++) {
        SimWorker *w = &s->w[i];
        if (w->busy && w->wrote) {
            /* holding the atlas lock across a cancel is possible: this no-ops then */
            if (rnd(r, 3) == 0) {
                if (!amb_clip_alive(&s->c, w->job.id)) s->stale_writes++;
                amb_clip_written(&s->c, w->job.id);
                w->busy = w->wrote = 0;
            }
            continue;
        }
        if (w->busy && w->delay > 0) { w->delay--; continue; }
        if (w->busy) {
            if (!amb_clip_alive(&s->c, w->job.id)) { w->busy = 0; continue; }
            if (!amb_clip_may_write(&s->c, w->job.id) || !worker_may_start_write(s, i)) continue;
            write_job_to_cpu(s, &w->job);
            w->wrote = 1;
            continue;
        }
        /* worker 1 only helps when two or more jobs are behind and no whole window is pending, as the proxy's */
        if ((i == 0 || (amb_clip_backlog(&s->c) >= 2 && !amb_clip_whole_pending(&s->c))) && rnd(r, 2) == 0 && amb_clip_take(&s->c, &w->job)) {
            w->busy = 1;
            w->wrote = 0;
            w->delay = w->job.kind == AMB_JOB_WHOLE ? rnd(r, 40) : rnd(r, 16);
        }
    }
}

/* The proxy does one job's part in one go. Here boxes spread over frames, with draws between them, to test what the box promises. */
static void upload_step(Sim *s, Rng *r, const double cam[3]) {
    if (s->need_full) {
        /* A worker writing the atlas has its lock: the re-upload waits. A head already written is taken along, the proxy's rule. */
        for (int i = 0; i < s->nworkers; i++) if (s->w[i].wrote) return;
        AmbClipJob j;
        int head = amb_clip_upload_ready(&s->c, &j);
        if (head) amb_clip_begin_upload(&s->c, j.id);
        AmbTexBox all;
        amb_clip_all_box(&s->c, &all);
        copy_box(s, &all);
        if (head) amb_clip_end_upload(&s->c, j.id);
        s->need_full = 0;
        s->up_id = 0;
        return;
    }
    AmbClipJob j;
    if (!amb_clip_upload_ready(&s->c, &j)) { s->up_id = 0; return; }
    if (j.id != s->up_id) {
        if (rnd(r, 3) == 0) return;   /* the atlas lock was busy, or no draw this frame */
        CHECK(amb_clip_begin_upload(&s->c, j.id), "begin_upload refused job %u", j.id);
        s->up_id = j.id;
        s->up_part = 0;
        s->up_box = 0;
        s->up_n = amb_clip_boxes(&s->c, &j, 0, s->up_boxes);
        check_draws(s, r, cam);
    }
    if (rnd(r, 4) == 0) return;
    if (s->up_box < s->up_n) {
        AmbTexBox b = s->up_boxes[s->up_box++];
        if (rnd(r, 2) == 0 && b.w1 - b.w0 > 1) {   /* a torn box: half of it, draws, the rest */
            AmbTexBox half = b;
            half.w1 = b.w0 + (b.w1 - b.w0) / 2;
            copy_box(s, &half);
            check_draws(s, r, cam);
        }
        copy_box(s, &b);
        s->upload_parts++;
        check_draws(s, r, cam);
    }
    if (s->up_box == s->up_n) {
        if (++s->up_part < amb_clip_parts(&s->c, &j)) {
            s->up_box = 0;
            s->up_n = amb_clip_boxes(&s->c, &j, s->up_part, s->up_boxes);
            return;
        }
        amb_clip_end_upload(&s->c, j.id);
        s->up_id = 0;
    }
}

static void drop_workers_on_map_change(Sim *s) {
    /* Workers of the old map stop within a row; one that has written may still report. New voxels start over. */
    for (int i = 0; i < s->nworkers; i++) if (!s->w[i].wrote) s->w[i].busy = 0;
}

static void frame(Sim *s, Rng *r, const double cam[3]) {
    amb_clip_tick(&s->c, 1.0 / 60.0);
    amb_clip_camera(&s->c, cam);
    check_draws(s, r, cam);
    workers_step(s, r);
    check_draws(s, r, cam);
    upload_step(s, r, cam);
    check_draws(s, r, cam);
}

/* Camera stopped: run until idle, then the window must surround the camera and hold every voxel */
static void converge(Sim *s, Rng *r, const double cam[3]) {
    int frames = 0;
    for (; frames < 4000; frames++) {
        frame(s, r, cam);
        if (!s->c.qn && !s->c.holding && !s->up_id && !s->need_full) {
            int idle = 1;
            for (int i = 0; i < s->nworkers; i++) idle &= !s->w[i].busy;
            if (idle) break;
        }
    }
    CHECK(frames < 4000, "no convergence in 4000 frames: %d jobs queued", s->c.qn);
    CHECK(s->c.valid, "converged with an empty box");
    for (int a = 0; a < 3; a++) {
        CHECK(s->c.vmax[a] - s->c.vmin[a] == s->c.dim[a], "box axis %d spans %d voxels", a, s->c.vmax[a] - s->c.vmin[a]);
        CHECK(s->c.vmin[a] % s->c.step == 0, "box axis %d starts at %d", a, s->c.vmin[a]);
        double off = cam[a] / s->c.voxel[a] - (s->c.vmin[a] + 0.5 * s->c.dim[a]);
        CHECK(fabs(off) <= s->c.hysteresis + 1e-9, "camera %.1f voxels off centre on axis %d", off, a);
    }
    for (int i = 0; i < 90; i++) amb_clip_tick(&s->c, 1.0 / 60.0);   /* the weight box arrives */
    for (int a = 0; a < 3; a++) CHECK(s->c.wmin[a] == s->c.smin[a] && s->c.wmax[a] == s->c.smax[a], "the weight box has not arrived on axis %d", a);
    float o[3] = { (float)cam[0], (float)cam[1], (float)cam[2] };
    CHECK(amb_clip_weight(&s->c, o) > 0, "camera outside its own box");
    /* the real cascade's promise: at the camera the volume is always at full weight */
    if (s->c.dim[0] == 64) CHECK(amb_clip_weight(&s->c, o) == 1.0f, "weight at the camera %g", amb_clip_weight(&s->c, o));
    /* every voxel of the window, every face */
    for (int z = s->c.vmin[2]; z < s->c.vmax[2]; z++)
        for (int y = s->c.vmin[1]; y < s->c.vmax[1]; y++)
            for (int x = s->c.vmin[0]; x < s->c.vmax[0]; x++)
                for (int f = 0; f < AMB_FACES; f++) {
                    int tx = wrapi(x, s->X), want[3] = { x, y, z };
                    float exp[3];
                    voxel_rgb(s, want, exp);
                    const uint16_t *q = (const uint16_t *)(s->gpu + (size_t)wrapi(z, s->atlas[2]) * s->sp + (size_t)wrapi(y, s->atlas[1]) * s->rp +
                                                           (size_t)(f * s->band + tx) * 8);
                    CHECK(amb_f16_to_f32(q[0]) == exp[0] && amb_f16_to_f32(q[1]) == exp[1] && amb_f16_to_f32(q[2]) == exp[2],
                          "converged: voxel %d %d %d face %d is wrong", x, y, z, f);
                    if (tx == 0) {
                        q = (const uint16_t *)(s->gpu + (size_t)wrapi(z, s->atlas[2]) * s->sp + (size_t)wrapi(y, s->atlas[1]) * s->rp +
                                               (size_t)(f * s->band + s->X) * 8);
                        CHECK(amb_f16_to_f32(q[0]) == exp[0], "converged: gutter of voxel %d %d %d face %d is wrong", x, y, z, f);
                    }
                }
}

typedef struct { long frames, stops; } RunStats;

static void run_seed(const int dim[3], const int voxel[3], int step, int nworkers, uint64_t seed, int frames, Sim *total, RunStats *rs) {
    Sim S;
    Rng r = { seed };
    if (!sim_init(&S, dim, voxel, step, nworkers)) { printf("out of memory\n"); exit(2); }
    double span[3] = { (double)dim[0] * voxel[0], (double)dim[1] * voxel[1], (double)dim[2] * voxel[2] };
    double cam[3] = { rndf(&r, -3000, 3000), rndf(&r, -3000, 3000), rndf(&r, -500, 500) }, vel[3] = { 0, 0, 0 };
    int mode = 0, left = 0;
    for (int f = 0; f < frames; f++) {
        if (left-- <= 0) {
            mode = rnd(&r, 10);
            left = 20 + rnd(&r, 200);
            double speed = mode <= 1 ? rndf(&r, 0, 6) : mode <= 3 ? rndf(&r, 4, 12) : mode == 4 ? rndf(&r, 20, 60) : mode == 5 ? rndf(&r, 60, 400) : 0;
            /* speeds relative to the real window's size, so the small test windows see the same situations */
            speed *= span[0] / 2048.0;
            for (int a = 0; a < 3; a++) vel[a] = rndf(&r, -speed, speed);
            if (mode == 2) { vel[0] = speed; vel[1] = vel[2] = 0; }   /* straight along one axis, up to the voxels' boundaries */
            if (mode == 3) vel[2] = (rnd(&r, 2) ? speed : -speed);
            if (mode == 6) {   /* a teleport, any distance */
                for (int a = 0; a < 2; a++) cam[a] += rndf(&r, -span[a] * 3, span[a] * 3);
                cam[2] += rndf(&r, -span[2] * 2, span[2] * 2);
            }
            if (mode == 7) {   /* stop, and everything must settle */
                converge(&S, &r, cam);
                rs->stops++;
            }
            if (rnd(&r, 12) == 0) {   /* a device reset: the texture is gone until everything is re-uploaded */
                memset(S.gpu, 0xff, amb_clip_atlas_bytes(&S.c));
                S.need_full = 1;
                S.resets++;
            }
            if (rnd(&r, 16) == 0) {   /* a map change: nothing of the old map may be fetched again */
                S.gen = (S.gen + 1) % GENS;
                amb_clip_reset(&S.c);
                drop_workers_on_map_change(&S);
                S.up_id = 0;
                S.map_changes++;
            }
        }
        for (int a = 0; a < 3; a++) cam[a] += vel[a];
        if (mode == 8)   /* jitter across a window step: must not thrash */
            cam[f % 3] += sin(f * 0.9) * voxel[f % 3];
        if (mode == 9)   /* back and forth over a few window steps: steps back over jobs not yet taken, and over jobs being baked */
            cam[0] += cos(f * 0.12) * 0.12 * 2.0 * step * voxel[0];
        for (int a = 0; a < 3; a++) {
            double lim = a == 2 ? Z_LIMIT : XY_LIMIT;
            if (cam[a] > lim) { cam[a] = lim; vel[a] = -fabs(vel[a]); }
            if (cam[a] < -lim) { cam[a] = -lim; vel[a] = fabs(vel[a]); }
        }
        frame(&S, &r, cam);
    }
    converge(&S, &r, cam);
    rs->frames += frames;
    rs->stops++;
    total->draws += S.draws; total->pixels += S.pixels; total->taps += S.taps; total->in_box_draws += S.in_box_draws;
    total->resets += S.resets; total->map_changes += S.map_changes; total->stale_writes += S.stale_writes; total->upload_parts += S.upload_parts;
    total->c.jumps += S.c.jumps; total->c.wholes += S.c.wholes; total->c.coalesced += S.c.coalesced;
    for (int a = 0; a < 3; a++) total->c.slabs[a] += S.c.slabs[a];
    sim_free(&S);
}

/* --- direct checks -------------------------------------------------------------------------------------------------- */
static void check_constants(void) {
    AmbClip c;
    amb_clip_init(&c);
    float a[4], b[4];
    amb_clip_constants(&c, a, b);
    CHECK(c.dim[0] == 64 && c.dim[1] == 64 && c.dim[2] == 32 && c.voxel[0] == 32 && c.step == 8, "defaults");
    CHECK(amb_clip_atlas_bytes(&c) == 390u * 64 * 32 * 8, "atlas size");
    CHECK(a[0] == 1.0f / 2048 && a[1] == 1.0f / 2048 && a[2] == 1.0f / 1024 && a[3] == 16.0f, "c59: %g %g %g %g", a[0], a[1], a[2], a[3]);
    CHECK(b[0] == 0.5f / 64 && b[1] == (float)(64.0 / 390.0) && b[2] == (float)(0.5 / 390.0) && b[3] == (float)(1.0 / 6.0), "c60: %g %g %g %g", b[0], b[1], b[2], b[3]);
    static const int bad[3] = { 60, 64, 32 }, vox[3] = { 32, 32, 32 };
    CHECK(amb_clip_init_dims(&c, bad, vox, 8) == AMB_ERR_ARG, "a dim that is not a multiple of step");
    /* no NaN or absurd camera moves anything */
    amb_clip_init(&c);
    double nan_cam[3] = { NAN, 0, 0 }, far_cam[3] = { 0, 1e9, 0 };
    CHECK(amb_clip_camera(&c, nan_cam) == 0 && amb_clip_camera(&c, far_cam) == 0 && !c.have_window, "bad cameras ignored");
    float p[3] = { NAN, 0, 0 };
    CHECK(amb_clip_weight(&c, p) == 0.0f, "weight on an empty box");
    /* stale calls */
    amb_clip_init(&c);
    double cam[3] = { 100, 100, 100 };
    AmbClipJob j;
    amb_clip_camera(&c, cam);
    CHECK(amb_clip_take(&c, &j) && j.kind == AMB_JOB_WHOLE && amb_clip_may_write(&c, j.id), "first use bakes the whole window");
    amb_clip_written(&c, j.id + 1);
    AmbClipJob k;
    CHECK(!amb_clip_upload_ready(&c, &k), "an unknown id is ignored");
    CHECK(!amb_clip_begin_upload(&c, j.id), "not written yet");
    amb_clip_reset(&c);
    amb_clip_written(&c, j.id);
    amb_clip_end_upload(&c, j.id);
    CHECK(!c.valid && !c.qn, "calls for a cancelled job do nothing");
}

/* A real box around (100, 200, 300): the first camera's whole window, written and uploaded, with the weight fully up */
static const double k_settle_cam[3] = { 100, 200, 300 };
static void settle(AmbClip *c) {
    AmbClipJob j;
    amb_clip_init(c);
    amb_clip_camera(c, k_settle_cam);
    CHECK(amb_clip_take(c, &j) && j.kind == AMB_JOB_WHOLE, "settle: a whole window first");
    amb_clip_written(c, j.id);
    CHECK(amb_clip_begin_upload(c, j.id), "settle: begin");
    amb_clip_end_upload(c, j.id);
    for (int i = 0; i < 60; i++) amb_clip_tick(c, 1.0 / 60.0);
}

static void check_nan_on_a_real_box(void) {
    AmbClip c;
    settle(&c);
    float in[3] = { 100, 200, 300 };
    CHECK(amb_clip_weight(&c, in) == 1.0f, "inside the box the weight is 1: %g", amb_clip_weight(&c, in));
    for (int a = 0; a < 3; a++) {
        float p[3] = { in[0], in[1], in[2] };
        p[a] = NAN;
        CHECK(amb_clip_weight(&c, p) == 0.0f, "NaN on axis %d counts as outside", a);
        p[a] = INFINITY;
        CHECK(amb_clip_weight(&c, p) == 0.0f, "inf on axis %d counts as outside", a);
    }
}

/* Past half a window from where the box is, the camera gets a whole window instead of more slabs; exactly half is still slabs. Both axes kinds. */
static void check_half_window_jump(void) {
    for (int axis = 0; axis < 3; axis++) {
        AmbClip c;
        settle(&c);
        double cam[3] = { k_settle_cam[0], k_settle_cam[1], k_settle_cam[2] };
        int half_steps = c.dim[axis] / 2 / c.step;
        for (int s = 1; s <= half_steps; s++) {
            cam[axis] += c.step * c.voxel[axis];
            amb_clip_camera(&c, cam);
            CHECK(c.jumps == 0 && c.qn == s && c.valid, "axis %d, %d steps of %d: no jump yet (jumps %ld, %d jobs)", axis, s, half_steps, c.jumps, c.qn);
        }
        cam[axis] += c.step * c.voxel[axis];
        int fl = amb_clip_camera(&c, cam);
        CHECK(c.jumps == 1 && !c.valid && !c.qn && (fl & AMB_CLIP_CANCELLED), "axis %d, one step past half: a jump (jumps %ld, valid %d, %d jobs)", axis, c.jumps, c.valid, c.qn);
        amb_clip_tick(&c, 0.05);
        for (int i = 0; i < 8; i++) { amb_clip_tick(&c, 0.05); amb_clip_camera(&c, cam); }
        AmbClipJob j;
        CHECK(amb_clip_take(&c, &j) && j.kind == AMB_JOB_WHOLE, "axis %d: the camera stopped, so a whole window follows", axis);
        for (int a = 0; a < 3; a++)
            CHECK(fabs(cam[a] / c.voxel[a] - (j.box.org[a] + 0.5 * c.dim[a])) <= c.step / 2.0 + 1e-9, "axis %d: the whole window is centred on the camera", axis);
    }
}

/* Circles with no worker taking anything: every step is a new job that does not undo the last, inside half a window. The queue is bounded, by a jump. */
static void check_queue_bound(void) {
    AmbClip c;
    settle(&c);
    static const int ax[4] = { 0, 1, 0, 1 }, dir[4] = { 1, 1, -1, -1 };
    int steps = 0, most = 0;
    for (; steps < 3 * AMB_CLIP_MAX_JOBS && !c.jumps; steps++) {
        /* just past the hysteresis from where the queue leaves the window: exactly one step */
        double cam[3];
        for (int a = 0; a < 3; a++) cam[a] = (c.tail[a] + 0.5 * c.dim[a]) * c.voxel[a];
        cam[ax[steps % 4]] += dir[steps % 4] * (c.hysteresis + 1) * c.voxel[ax[steps % 4]];
        amb_clip_camera(&c, cam);
        most = c.qn > most ? c.qn : most;
        CHECK(c.qn <= AMB_CLIP_MAX_JOBS, "%d jobs queued, the bound is %d", c.qn, AMB_CLIP_MAX_JOBS);
    }
    CHECK(c.jumps == 1 && steps == AMB_CLIP_MAX_JOBS + 1 && most == AMB_CLIP_MAX_JOBS, "the bound forces a jump: after %d steps, most %d, jumps %ld", steps, most, c.jumps);
    CHECK(!c.valid && !c.qn, "the jump empties box and queue");
}

/* Walking past props: an instant worker, so every step lands in one frame. No prop's weight may move by more than the easing allows, at any step. */
static void check_fade(void) {
    AmbClip c;
    settle(&c);
    enum { PROPS = 160 };
    float prop[PROPS][3], last[PROPS];
    for (int i = 0; i < PROPS; i++) {
        /* a line across the leading and trailing X edges, and across the Z edges */
        prop[i][0] = i < 80 ? 100.0f - 1400.0f + 35.0f * (float)i : 100.0f;
        prop[i][1] = 200.0f;
        prop[i][2] = i < 80 ? 300.0f : 300.0f - 800.0f + 20.0f * (float)(i - 80);
        last[i] = amb_clip_weight(&c, prop[i]);
    }
    double cam[3] = { k_settle_cam[0], k_settle_cam[1], k_settle_cam[2] }, dt = 1.0 / 60.0, worst = 0;
    int steps = 0;
    /* 150 units per second, and a slow climb, to cross X and Z steps; the props stay put, so they meet both edges */
    for (int f = 0; f < 1200; f++) {
        cam[0] += 150 * dt;
        cam[2] += 40 * dt;
        amb_clip_tick(&c, dt);
        amb_clip_camera(&c, cam);
        AmbClipJob j;
        while (amb_clip_take(&c, &j)) {
            amb_clip_written(&c, j.id);
            amb_clip_begin_upload(&c, j.id);
            amb_clip_end_upload(&c, j.id);
            steps++;
        }
        for (int i = 0; i < PROPS; i++) {
            float w = amb_clip_weight(&c, prop[i]);
            double d = fabs((double)w - last[i]);
            worst = d > worst ? d : worst;
            last[i] = w;
        }
    }
    printf("cliptest: weight easing: %d window steps, largest change of any prop's weight in one frame %.3f\n", steps, worst);
    CHECK(steps >= 8, "only %d steps", steps);
    /* one frame of the base speed: 8 * 32 units in fade_seconds, over the fade width */
    CHECK(worst <= 1.5 * dt / c.fade_seconds, "a weight jumped by %.3f in one frame", worst);
}

/* A camera too fast for one bake and upload gets no job, whatever the time; a slow one, or a stopped one, gets its window soon */
static void check_hold(void) {
    AmbClip c;
    settle(&c);
    double cam[3] = { k_settle_cam[0], k_settle_cam[1], k_settle_cam[2] }, dt = 1.0 / 60.0;
    long wholes0 = c.wholes;
    /* 3000 units per second: 2 s across the map */
    for (int f = 0; f < 600; f++) {
        cam[0] += 3000 * dt;
        if (cam[0] > 12000) cam[0] = -12000;
        amb_clip_tick(&c, dt);
        amb_clip_camera(&c, cam);
    }
    CHECK(c.wholes == wholes0, "flying at 3000 u/s: %ld whole windows queued", c.wholes - wholes0);
    CHECK(c.holding && !c.valid && !c.qn, "flying: box empty and nothing queued");
    int frames = 0;
    while (c.holding && frames < 600) { amb_clip_tick(&c, dt); amb_clip_camera(&c, cam); frames++; }
    CHECK(!c.holding && c.wholes == wholes0 + 1, "the camera stopped: one whole window");
    /* the hold last restarted within the last 307 units of flight, 7 frames */
    CHECK(frames >= (int)(c.hold_seconds / dt) - 8 && frames <= (int)(c.hold_seconds / dt) + 2, "the window comes after the hold: %d frames", frames);
    /* a walk does not hold the window back: 300 units per second, jump onto a far place first */
    settle(&c);
    wholes0 = c.wholes;
    cam[0] = 5000; cam[1] = 5000; cam[2] = 0;
    amb_clip_camera(&c, cam);
    frames = 0;
    while (c.holding && frames < 600) { cam[1] += 300 * dt; amb_clip_tick(&c, dt); amb_clip_camera(&c, cam); frames++; }
    CHECK(frames <= (int)(c.hold_seconds / dt) + 2 && c.wholes == wholes0 + 1, "walking: %d frames to the window", frames);
    /* climbing: half a window high in survive_seconds is the limit on Z, so 1000 units per second waits */
    settle(&c);
    wholes0 = c.wholes;
    cam[0] = 5000; cam[1] = 5000; cam[2] = 0;
    amb_clip_camera(&c, cam);
    for (int f = 0; f < 240; f++) { cam[2] += 1000 * dt; if (cam[2] > 12000) cam[2] = -12000; amb_clip_tick(&c, dt); amb_clip_camera(&c, cam); }
    CHECK(c.wholes == wholes0, "climbing at 1000 u/s: %ld whole windows", c.wholes - wholes0);
}

static void check_boxes(void) {
    AmbClip c;
    amb_clip_init(&c);
    AmbClipJob j;
    memset(&j, 0, sizeof j);
    j.kind = AMB_JOB_SLAB;
    AmbTexBox b[AMB_CLIP_MAX_BOXES];
    long hits_max = 0;
    /* every X slab position: exactly the 8 columns per band, and the gutter with texel 0, no other texel */
    for (int org = -640; org < 640; org += 8) {
        j.axis = 0; j.box.org[0] = org;
        int n = amb_clip_boxes(&c, &j, 0, b), t = ((org % 64) + 64) % 64;
        CHECK(n == (t == 0 ? 7 : 6), "X slab at %d: %d boxes", org, n);
        hits_max = n > hits_max ? n : hits_max;
        static uint8_t cover[390];
        memset(cover, 0, sizeof cover);
        for (int i = 0; i < n; i++) {
            CHECK(b[i].v0 == 0 && b[i].v1 == 64 && b[i].w0 == 0 && b[i].w1 == 32, "X slab rows");
            for (int u = b[i].u0; u < b[i].u1; u++) { CHECK(u >= 0 && u < 390, "u %d", u); cover[u]++; }
        }
        for (int u = 0; u < 390; u++) {
            int inband = u % 65, want = (inband >= t && inband < t + 8) || (t == 0 && inband == 64);
            CHECK(cover[u] == want, "X slab %d: texel %d covered %d times, wanted %d", org, u, cover[u], want);
        }
    }
    CHECK(hits_max == AMB_CLIP_MAX_BOXES, "the box limit is reached");
    for (int org = -640; org < 640; org += 8) {
        j.axis = 1; j.box.org[1] = org;
        int t = ((org % 64) + 64) % 64;
        CHECK(amb_clip_boxes(&c, &j, 0, b) == 1 && b[0].v0 == t && b[0].v1 == t + 8 && b[0].u1 == 390 && b[0].w1 == 32, "Y slab");
        j.axis = 2; j.box.org[2] = org;
        t = ((org % 32) + 32) % 32;
        CHECK(amb_clip_boxes(&c, &j, 0, b) == 1 && b[0].w0 == t && b[0].w1 == t + 8 && b[0].u1 == 390 && b[0].v1 == 64, "Z slab");
    }
    j.kind = AMB_JOB_WHOLE;
    CHECK(amb_clip_parts(&c, &j) == 8 && amb_clip_boxes(&c, &j, 7, b) == 1 && b[0].v0 == 56 && b[0].v1 == 64, "whole window parts");
}

static void check_fold(void) {
    float cube[6][4], a[18], out[6][4];
    for (int f = 0; f < 6; f++) for (int k = 0; k < 4; k++) cube[f][k] = 0.25f + 0.1f * (float)f + 0.01f * (float)k;
    for (int i = 0; i < 18; i++) a[i] = i % 2 ? 0.01f * (float)i : 2.0f;   /* even: more than C / 1.5, so the residual clamps to 0 */
    amb_clip_fold(cube, a, 0.0f, out);
    CHECK(memcmp(out, cube, sizeof cube) == 0, "f = 0 is exactly the cube");
    amb_clip_fold(cube, a, 1.0f, out);
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 3; k++) {
            float R = cube[f][k] - 1.5f * a[f * 3 + k];
            R = R > 0 ? R : 0;
            CHECK(out[f][k] == R, "f = 1 is the residual: face %d channel %d: %g, wanted %g", f, k, out[f][k], R);
        }
        CHECK(out[f][3] == cube[f][3], "w kept");
    }
    amb_clip_fold(cube, a, 0.5f, out);
    for (int k = 0; k < 3; k++) {
        float R = cube[1][k] - 1.5f * a[3 + k];
        R = R > 0 ? R : 0;
        CHECK(fabsf(out[1][k] - 0.5f * (cube[1][k] + R)) < 1e-6f, "f = 0.5 is the lerp");
    }
    float aliased[6][4];
    memcpy(aliased, cube, sizeof cube);
    amb_clip_fold(aliased, a, 0.5f, aliased);
    CHECK(memcmp(aliased, out, sizeof out) == 0, "out may be cube");
}

/* The L4D2 lump order is undone exactly as the engine does (engine.dll FUN_180112730, lump[0].version > 1000) */
static void check_l4d2(void) {
    size_t n1 = 0, n2 = 0, n3 = 0;
    uint8_t *a = synth_bsp(0, 0, &n1), *b = synth_bsp(1, 0, &n2), *z = synth_bsp(1, 1, &n3);
    AmbMap *ma = NULL, *mb = NULL, *mz = NULL;
    int ra = amb_map_load(a, n1, 0, &ma), rb = amb_map_load(b, n2, 0, &mb), rz = amb_map_load(z, n3, 0, &mz);
    CHECK(ra == AMB_OK && rb == AMB_OK, "synthetic BSP: normal order %d, L4D2 order %d", ra, rb);
    CHECK(rz == AMB_ERR_LZMA && !mz, "an LZMA lump behind an L4D2 header: %d", rz);
    if (ma && mb) {
        AmbMapInfo ia, ib;
        amb_map_info(ma, &ia);
        amb_map_info(mb, &ib);
        CHECK(ia.leaf_lump_version == 1 && ib.leaf_lump_version == 1 && ia.nsamples == SYNTH_NSAMPLES && ib.nsamples == SYNTH_NSAMPLES && ia.nleaves == 3 && ib.nleaves == 3,
              "synthetic BSP parsed: %d samples, %d leaves", ib.nsamples, ib.nleaves);
        CHECK(ia.ambient_source == 0 && ib.ambient_source == 0, "indexed ambient");
        double pts[3][3] = { { -100, 0, 0 }, { 100, 2000, 50 }, { 100, 0, 0 } };
        for (int i = 0; i < 3; i++) {
            float x[18], y[18];
            int cx = amb_eval_point(ma, pts[i], 1.0, x), cy = amb_eval_point(mb, pts[i], 1.0, y);
            CHECK(cx == cy && memcmp(x, y, sizeof x) == 0, "L4D2 order evaluates the same at point %d", i);
            CHECK(amb_point_solid(mb, pts[i]) == (i == 2), "solid leaf at point %d", i);
            CHECK(i == 2 || x[0] > 0, "a non-zero sample at point %d", i);
            /* the engine's rule: the same with samples, zero in a leaf without any, where the tool rule follows its reference */
            float e[18], zero[18] = { 0 };
            int ce = amb_eval_engine(mb, pts[i], 1.0, e);
            if (i == 2) CHECK(ce == 0 && memcmp(e, zero, sizeof e) == 0 && cy > 0 && y[0] > 0, "solid leaf: engine zero (%d), reference followed by the tool rule (%d)", ce, cy);
            else CHECK(ce == cy && memcmp(e, y, sizeof e) == 0, "engine rule = tool rule at point %d, which has samples", i);
        }
    }
    amb_map_free(ma);
    amb_map_free(mb);
    free(a); free(b); free(z);
}

int main(int argc, char **argv) {
    int seeds = 60, frames = 1500;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
    }
    check_constants();
    check_boxes();
    check_fold();
    check_l4d2();
    check_nan_on_a_real_box();
    check_half_window_jump();
    check_queue_bound();
    check_fade();
    check_hold();
    static const struct { int dim[3], voxel[3], step; } cfg[] = {
        { { 16, 16, 8 }, { 32, 32, 32 }, 4 },     /* small windows: every situation many times */
        { { 16, 8, 8 }, { 16, 32, 48 }, 4 },
        { { 24, 24, 16 }, { 32, 32, 32 }, 8 },
        { { 64, 64, 32 }, { 32, 32, 32 }, 8 },    /* the real cascade */
    };
    Sim total;
    memset(&total, 0, sizeof total);
    RunStats rs = { 0, 0 };
    long runs = 0;
    for (int s = 0; s < seeds && g_fails < 8; s++)
        for (size_t k = 0; k < sizeof cfg / sizeof cfg[0] && g_fails < 8; k++) {
            /* a window 4^3 the size costs 64 times as much per job: the real one gets fewer, shorter runs */
            int real = cfg[k].dim[0] == 64;
            if (real && s % 6) continue;
            run_seed(cfg[k].dim, cfg[k].voxel, cfg[k].step, 1 + s % 2, 0x1000u + (uint64_t)s * 977u + k, real ? frames / 2 : frames, &total, &rs);
            runs++;
        }
    if (!g_quiet)
        printf("cliptest: %ld runs, %ld frames, %ld stops converged; %ld draws (%ld with weight > 0), %ld pixels, %ld taps; jobs: %ld whole, %ld X + %ld Y + %ld Z slabs, %ld steps back"
               " coalesced, %ld jumps, %ld upload boxes, %ld writes overtaken by a cancel; %ld device resets, %ld map changes\n",
               runs, rs.frames, rs.stops, total.draws, total.in_box_draws, total.pixels, total.taps, total.c.wholes, total.c.slabs[0], total.c.slabs[1], total.c.slabs[2],
               total.c.coalesced, total.c.jumps, total.upload_parts, total.stale_writes, total.resets, total.map_changes);
    /* a test that exercised nothing proves nothing */
    if (!g_fails && seeds >= 10) {
        CHECK(total.taps > 1000000 && total.c.slabs[0] > 50 && total.c.slabs[1] > 50 && total.c.slabs[2] > 50 && total.c.jumps > 10 && total.c.coalesced > 0 &&
              total.resets > 0 && total.map_changes > 0 && total.stale_writes > 0, "coverage too thin");
    }
    printf(g_fails ? "CLIPTEST FAIL: %ld violations\n" : "CLIPTEST OK\n", g_fails);
    return g_fails ? 1 : 0;
}
