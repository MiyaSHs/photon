/* P3h: the leaf ambient A, low-passed per model the way the engine low-passes its cube C (analysis/15e, third run), so that the fold's residual
 * R = max(0, C - 1.5 A) compares two values with the same lag. Pure C11, no threads or Windows: tests/ambient/tracktest.c drives it on the host.
 *
 * The engine's averaging [confirmed, engine.dll decomp and disassembly; 15e]: FUN_180105dc0, per model instance (a 0x148-byte cache entry):
 *   - off when r_lightaverage is 0 or r_lightinterp is 0: the current state, unaveraged, and the entry is not touched;
 *   - first use of an entry (its time stamp is the sentinel -999999, set when the instance is created, FUN_1800fb810): copy the current state, no averaging;
 *   - otherwise dt = now - stamp, with now = the client's time (FUN_1800aaea0: tick count * tick interval, plus the remainder unless paused); if dt > 0 the stamp
 *     becomes now, else dt = 0; k = expf(-dt * r_lightinterp) (FUN_180364bd0 is expf; the argument is negated by xor with 0x80000000), clamped to [0, 1];
 *     each of the 18 cube floats becomes cur - (cur - old) * k, clamped to [0, 8] (FUN_180105bc0). So per time, not per frame, with no cap on dt.
 *   - the local lights are averaged the same way, matched by pointer; not mirrored here (the volume does not replace them).
 * There is no reset on a teleport: an instance keeps averaging wherever it goes.
 *
 * The proxy has no instance identity, only each draw's lighting origin (slot 128). Tracks stand in for instances:
 *   - a draw whose origin equals one already handled this frame (several meshes or passes of one model, or bone-merged parts) gets that track's value;
 *   - otherwise the nearest track not yet handled this frame, from where its velocity predicts it, within 64 u + 4000 u/s times the time since it was seen;
 *     a second candidate within 1.5 times the distance counts as ambiguous (the nearest is still taken);
 *   - otherwise a new track that starts at the current value, as a new instance does;
 *   - at the next frame, if exactly one track was born and exactly one went unseen, they are one model that jumped (a teleport): the new one takes the old one's
 *     history from then on (its first frame was unaveraged);
 *   - a track at exactly its last origin (most models, standing) is found through a hash of the origin, without the scan;
 *   - a track unseen for long enough that the engine's average would have forgotten it (k below 1e-4) or for 300 frames is dropped. */
#ifndef PHOTON_AMB_TRACK_H
#define PHOTON_AMB_TRACK_H

enum { AMB_TRACK_MAX = 256, AMB_TRACK_FLOATS = 18, AMB_TRACK_HINTS = 1024 };

typedef struct AmbTrackEntry {
    int used;
    float pos[3], vel[3];          /* last origin; units per second */
    float a[AMB_TRACK_FLOATS];     /* the averaged value */
    double stamp;                  /* engine time of the last averaging step (the instance's +0x128) */
    double seen;                   /* engine time it was last drawn */
    unsigned long long frame;      /* last frame it was drawn */
    unsigned long long born;       /* frame it was created */
} AmbTrackEntry;

typedef struct AmbTrackStats {
    long long draws, reused, matched, created, ambiguous, joined, dropped, overflow, unaveraged;
} AmbTrackStats;

typedef struct AmbTrack {
    AmbTrackEntry e[AMB_TRACK_MAX];
    unsigned long long frame;
    double now;
    float rate;                    /* r_lightinterp */
    int average;                   /* r_lightaverage != 0 */
    int have_frame;
    int top;                       /* one past the highest index in use: the scans stop there */
    unsigned short hint[AMB_TRACK_HINTS];   /* origin hash -> track index + 1: a model where it was last frame is found without a scan (a hint, checked) */
    AmbTrackStats st;
} AmbTrack;

void amb_track_reset(AmbTrack *t);   /* a new map, a device Reset: every track goes (stats stay) */
/* The start of each frame's draws: its number, the engine's time (seconds), r_lightinterp and r_lightaverage. Calling it again with the same frame does nothing. */
void amb_track_frame(AmbTrack *t, unsigned long long frame, double now, float rate, int average);
/* One draw: the current leaf ambient a at origin, the averaged value out (may be a). */
void amb_track_draw(AmbTrack *t, const float origin[3], const float a[AMB_TRACK_FLOATS], float out[AMB_TRACK_FLOATS]);
#endif
