/* P3g: the clipmap scheduler for one ambient cascade (analysis/15d section 8.3). Pure C11 with no threads, D3D or Windows, so
 * tests/ambient/cliptest.c can drive every interleaving on the host.
 *
 * The window is a world-aligned box of dim voxels, moved in steps of `step` voxels. A voxel always lives at texel (a mod dim), so
 * moving the window overwrites only the slab leaving it (amb_bake.h, amb_write_toroidal).
 *
 * Jobs run strictly in queue order. Each is either the whole window (first use, or the camera more than half a window away from
 * where the valid box is: queued jobs are cancelled and the box goes empty) or one slab of step voxels on one axis.
 *
 *   QUEUED -- take --> BAKING -- written --> WRITTEN -- begin_upload --> UPLOADING -- end_upload --> gone
 *
 * A camera that leaves the window while it is flying gets no job at all: the box goes empty, and a whole window is queued only once the camera has
 * been slow enough for a while that one bake and upload (survive_seconds) would still be of use when they finish. Baking one per jump would burn
 * up to two cores on windows that are cancelled before they are ever seen.
 *
 * A job's voxels may enter the CPU atlas only while it is the head of the queue (may_write): two jobs overlap at the corner where
 * an X and a Y slab meet, in the same texels but for different voxels, so writing job k + 1 before k is uploaded would hand k's
 * upload the wrong voxels there.
 *
 * The valid box [vmin, vmax) is owned by the draw thread. begin_upload shrinks it to exclude the texels the slab is about to
 * overwrite; end_upload grows it only after the new data is in. Everything else in the valid box is correct in the texture.
 *
 * The clamp box (c61, c62) follows the valid box exactly. A draw's weight comes from a second box that eases after it (amb_clip_tick): the box edge moves
 * by the fade width at each step, so a weight taken from the clamp box would jump by up to 1 in one frame. While the two differ a draw can have weight on the
 * wrong side of the clamp box; its pixels are then clamped to the nearest valid voxel, so nothing invalid is ever read.
 *
 * Threads: the proxy serialises all queue calls with a lock. begin_upload, end_upload, camera and reset change the valid box and
 * are called only by the draw thread, so the draw thread reads the box (weight, shader_box) without a lock. */
#ifndef PHOTON_AMB_CLIP_H
#define PHOTON_AMB_CLIP_H
#include "amb_bake.h"

enum { AMB_CLIP_MAX_JOBS = 32, AMB_CLIP_MAX_BOXES = 7 };
enum { AMB_JOB_WHOLE, AMB_JOB_SLAB };
enum { AMB_JOB_QUEUED, AMB_JOB_BAKING, AMB_JOB_WRITTEN, AMB_JOB_UPLOADING };
enum { AMB_CLIP_ADDED = 1, AMB_CLIP_CANCELLED = 2 };   /* amb_clip_camera's result */

typedef struct AmbClipJob {
    unsigned id;        /* never reused, never 0 */
    int kind;           /* AMB_JOB_WHOLE or AMB_JOB_SLAB */
    int axis, dir;      /* slab: axis 0..2, dir +1 or -1 */
    AmbBox box;         /* the voxels to bake */
    int window[3];      /* the window origin once this job is in */
    int state;
} AmbClipJob;

/* Texels [u0, u1) x [v0, v1) x [w0, w1) of the atlas */
typedef struct AmbTexBox { int u0, u1, v0, v1, w0, w1; } AmbTexBox;

typedef struct AmbClip {
    int dim[3];         /* voxels per axis; multiples of step */
    int voxel[3];       /* world units per voxel */
    int step;           /* voxels per move */
    int hysteresis;     /* voxels off the window centre before it moves: above step / 2, so a jump on the spot cannot flip it */
    double inset;       /* voxels between the valid box and the shader's clamp box: half a voxel for the filter, plus a margin */
    double fade;        /* world units over which a draw's weight goes from 0 to 1 */
    double fade_seconds;  /* how long the weight box takes to follow one step, and the weight to come up after a whole window */
    double catchup_seconds;  /* how fast it closes whatever it lags behind beyond one step (a flight) */
    double hold_seconds;  /* a new whole window waits for the camera to have been slow this long */
    double survive_seconds;  /* about how long a whole window takes from bake to upload: what "slow enough" is measured against */
    double normal_offset;

    AmbClipJob q[AMB_CLIP_MAX_JOBS];
    int qhead, qn;
    unsigned next_id;
    int have_window;
    int tail[3];        /* the window after every queued job */
    int base[3];        /* the window before the head job: where the valid box sits at rest, or the whole job's window */

    double clock;       /* seconds of amb_clip_tick */
    int holding;        /* a jump emptied the box, and no whole window is queued yet */
    double hold_cv[3], hold_since;   /* where (voxels) and when the camera last started being slow */

    int valid;          /* the valid box is not empty */
    int vmin[3], vmax[3];
    float smin[4], smax[4];   /* c61, c62 */
    double wmin[3], wmax[3], wgain;   /* the box weights are taken from, and a factor that ramps after a whole window */

    long jumps, slabs[3], wholes, coalesced;
} AmbClip;

/* 64 x 64 x 32 voxels of 32 units, moved by 8 */
void amb_clip_init(AmbClip *c);
/* Other sizes, for tests. AMB_ERR_ARG unless every dim is a positive multiple of a positive step. */
int amb_clip_init_dims(AmbClip *c, const int dim[3], const int voxel[3], int step);
/* A new map: the queue and the valid box are dropped. Job ids stay unique, so a worker's stale job is recognised. */
void amb_clip_reset(AmbClip *c);

/* Atlas geometry: atlas[0] = 6 (X + 1) texels, atlas[1] = Y, atlas[2] = Z; 8 bytes per texel, tightly packed in the CPU copy */
void amb_clip_atlas_dim(const AmbClip *c, int atlas[3]);
size_t amb_clip_row_bytes(const AmbClip *c);
size_t amb_clip_slice_bytes(const AmbClip *c);
size_t amb_clip_atlas_bytes(const AmbClip *c);

/* Draw thread, once per frame: where the main view is. Returns AMB_CLIP_* flags; a non-finite or absurd position is ignored. */
int amb_clip_camera(AmbClip *c, const double pos[3]);
/* Draw thread, once per frame: time passes. dt in seconds, capped so that a hitch does not skip the easing. */
void amb_clip_tick(AmbClip *c, double dt);

/* Worker side */
int amb_clip_take(AmbClip *c, AmbClipJob *out);                /* the oldest QUEUED job, now BAKING; 0 if none */
int amb_clip_backlog(const AmbClip *c);                        /* jobs not yet written */
int amb_clip_whole_pending(const AmbClip *c);                  /* a whole window is still in the queue: slabs behind it may yet be cancelled with it */
int amb_clip_alive(const AmbClip *c, unsigned id);             /* not cancelled */
int amb_clip_may_write(const AmbClip *c, unsigned id);         /* BAKING and the head: its voxels may enter the CPU atlas */
void amb_clip_written(AmbClip *c, unsigned id);                /* they are in; no effect on a cancelled job */

/* Draw thread: the upload. A slab is one part, the whole window dim[1] / step parts (rows), so no frame pays more than one slab. */
int amb_clip_upload_ready(const AmbClip *c, AmbClipJob *out);  /* the head if WRITTEN or UPLOADING */
int amb_clip_begin_upload(AmbClip *c, unsigned id);            /* shrink; idempotent while UPLOADING; 0 if the job is gone */
void amb_clip_end_upload(AmbClip *c, unsigned id);             /* grow and drop the job; no effect on a cancelled job */
int amb_clip_parts(const AmbClip *c, const AmbClipJob *job);
/* The texel boxes of one part: an X slab has one per face band, and one more if it covers texel 0, whose copy in the gutter (the
 * band's last texel) sits next to the following band's texel 0 and joins its box. Returns the count, at most AMB_CLIP_MAX_BOXES. */
int amb_clip_boxes(const AmbClip *c, const AmbClipJob *job, int part, AmbTexBox out[AMB_CLIP_MAX_BOXES]);
void amb_clip_all_box(const AmbClip *c, AmbTexBox *out);       /* the whole atlas, for a re-upload after a device reset */

/* A draw's weight: saturate(distance from p to the edge of the weight box / fade), times the ramp; 0 outside that box, or while the valid box is empty */
float amb_clip_weight(const AmbClip *c, const float p[3]);
/* c61, c62: the valid box inset, so that a clamped fetch's trilinear taps all stay on valid texels. xyz world units, w 0. */
void amb_clip_shader_box(const AmbClip *c, float min[4], float max[4]);
/* c59 (xyz = 1 / (voxels * voxel size), w = offset along the normal) and c60 (x = 0.5 / X, y = X / texels, z = 0.5 / texels, w = 1 / 6) */
void amb_clip_constants(const AmbClip *c, float a[4], float b[4]);
/* The folded lights stay in the cube (15d section 8.2): c33..c38 = lerp(C, max(0, C - 1.5 A), f). C and out are 6 x (r, g, b, w)
 * with w kept, A is amb_eval_engine's 18 floats. out may be cube. */
void amb_clip_fold(const float cube[6][4], const float a[AMB_VOXEL_FLOATS], float f, float out[6][4]);
#endif
