/* P4a: the cubemap pick table. Contract in amb_pick.h. Formats: the public SDK bspfile.h (dcubemapsample_t: int origin[3], int size; 16 bytes,
 * LUMP_CUBEMAPS = 42). */
#include "amb_pick.h"
#include <stdlib.h>
#include <string.h>

/* Test-only (scripts/run_ambient_test.sh): each removes one rule, and tests/ambient/picktest.c must then fail. 1 no line of sight, 2 no nudge of a
 * solid voxel centre, 3 distance ties go to the later sample. */
#ifndef AMB_PICK_MUTANT
#define AMB_PICK_MUTANT 0
#endif

enum { LUMP_CUBEMAPS = 42, HEADER_BYTES = 8 + 64 * 16, SAMPLE_BYTES = 16 };

static int32_t rd_s32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

/* The lump's range from a header (amb_bake.c's rule: lump 0's "version" above 1000 means the L4D2 field order) */
static int lump_range(const uint8_t *head, long long size, long long *ofs, long long *len) {
    int l4d2 = rd_s32(head + 8 + 8) > 1000;
    const uint8_t *h = head + 8 + 16 * LUMP_CUBEMAPS;
    int32_t o = rd_s32(l4d2 ? h + 4 : h), n = rd_s32(l4d2 ? h + 8 : h + 4);
    if (o < 0 || n < 0 || o > size || n > size - o) return AMB_ERR_FORMAT;
    if (n % SAMPLE_BYTES || n / SAMPLE_BYTES > AMB_CUBES_MAX) return AMB_ERR_FORMAT;
    *ofs = o;
    *len = n;
    return AMB_OK;
}

static void parse(const uint8_t *p, int n, AmbCubes *out) {
    out->n = n;
    for (int i = 0; i < n; i++, p += SAMPLE_BYTES) {
        for (int k = 0; k < 3; k++) out->origin[i][k] = rd_s32(p + 4 * k);
        out->size[i] = rd_s32(p + 12);
    }
}

int amb_cubes_load(const void *bsp, size_t len, AmbCubes *out) {
    if (!out) return AMB_ERR_ARG;
    out->n = 0;
    const uint8_t *b = bsp;
    if (!b || len < HEADER_BYTES || memcmp(b, "VBSP", 4) != 0) return AMB_ERR_FORMAT;
    long long ofs, n;
    int rc = lump_range(b, (long long)len, &ofs, &n);
    if (rc) return rc;
    if (n >= 4 && memcmp(b + ofs, "LZMA", 4) == 0) return AMB_ERR_LZMA;
    parse(b + ofs, (int)(n / SAMPLE_BYTES), out);
    return AMB_OK;
}

static int reader_cancelled(const AmbReader *r) { return r->cancel && *r->cancel; }
static int rd(const AmbReader *r, long long ofs, void *buf, int n, AmbReadStats *st) {
    if (reader_cancelled(r)) return AMB_ERR_CANCELLED;
    int got = r->read(r->ctx, ofs, buf, n);
    st->reads++;
    if (got != n) return AMB_ERR_READ;
    st->bytes_read += n;
    return AMB_OK;
}

int amb_cubes_load_reader(const AmbReader *r, AmbCubes *out, AmbReadStats *stats) {
    AmbReadStats st = { 0, 0, 0 };
    if (stats) *stats = st;
    if (!out) return AMB_ERR_ARG;
    out->n = 0;
    if (!r || !r->read || r->size < 0) return AMB_ERR_ARG;
    if (r->size < HEADER_BYTES) return AMB_ERR_FORMAT;
    uint8_t head[HEADER_BYTES];
    static uint8_t empty;
    uint8_t *buf = NULL;
    long long ofs = 0, n = 0;
    int rc = rd(r, 0, head, HEADER_BYTES, &st);
    if (rc == AMB_OK) rc = memcmp(head, "VBSP", 4) != 0 ? AMB_ERR_FORMAT : lump_range(head, r->size, &ofs, &n);
    if (rc == AMB_OK && n) {
        /* at most 16 KiB (AMB_CUBES_MAX samples), one read */
        buf = malloc((size_t)n);
        st.peak_bytes = n;
        rc = buf ? rd(r, ofs, buf, (int)n, &st) : AMB_ERR_NOMEM;
        if (rc == AMB_OK && n >= 4 && memcmp(buf, "LZMA", 4) == 0) rc = AMB_ERR_LZMA;
    }
    if (rc == AMB_OK) parse(buf ? buf : &empty, (int)(n / SAMPLE_BYTES), out);
    free(buf);
    if (stats) *stats = st;
    return rc;
}

int amb_cube_nearest(const AmbCubes *c, const float p[3]) {
    /* FUN_1801d9d30: d = (y - oy)^2 + (x - ox)^2 + (z - oz)^2 in float, the running minimum replaced only by a strictly smaller value */
    if (c->n < 1) return -1;
    int best = 0;
    float bd = 0.0f;
    for (int i = 0; i < c->n; i++) {
        float dx = p[0] - (float)c->origin[i][0], dy = p[1] - (float)c->origin[i][1], dz = p[2] - (float)c->origin[i][2];
        float d = dy * dy + dx * dx;
        d = d + dz * dz;
        if (i == 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

void amb_pick_params_default(AmbPickParams *p) {
    memset(p, 0, sizeof *p);
    p->voxel = 32.0;
    p->max_tests = 16;
}

/* The first open point of p, p +- d along x, then y, then z; 0 if all are solid (*q = p then) */
static int open_point(const AmbMap *m, const double p[3], double d, double q[3]) {
    memcpy(q, p, sizeof(double) * 3);
    if (!amb_point_solid(m, q)) return 1;
    for (int ax = 0; ax < 3; ax++)
        for (int s = -1; s <= 1; s += 2) {
            memcpy(q, p, sizeof(double) * 3);
            q[ax] += s * d;
            if (!amb_point_solid(m, q)) return 2;
        }
    memcpy(q, p, sizeof(double) * 3);
    return 0;
}

void amb_pick_targets(const AmbMap *m, const AmbCubes *c, AmbPickTargets *t) {
    t->n = c->n;
    for (int i = 0; i < c->n; i++) {
        double o[3] = { c->origin[i][0], c->origin[i][1], c->origin[i][2] };
        t->solid[i] = open_point(m, o, 8.0, t->p[i]) == 0;
    }
}

static double dist2(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

enum { MAX_TESTS = 64 };

uint32_t amb_pick_voxel(const AmbMap *m, const AmbCubes *c, const AmbPickTargets *t, const AmbPickParams *p, const int a[3]) {
    if (c->n < 1) return (uint32_t)AMB_PICK_NONE | (uint32_t)AMB_PICK_NONE << 11 | AMB_PICK_EMPTY;
    double centre[3], q[3];
    for (int k = 0; k < 3; k++) centre[k] = ((double)a[k] + 0.5) * p->voxel;
    int op = AMB_PICK_MUTANT == 2 ? (memcpy(q, centre, sizeof q), 1) : open_point(m, centre, 12.0, q);
    uint32_t flags = op == 0 ? AMB_PICK_SOLID : op == 2 ? AMB_PICK_MOVED : 0;
    /* the k nearest by distance from q, by insertion (ties keep lump order, as the engine) */
    int k = p->max_tests < 2 ? 2 : p->max_tests > MAX_TESTS ? MAX_TESTS : p->max_tests;
    if (k > c->n) k = c->n;
    int idx[MAX_TESTS];
    double dd[MAX_TESTS];
    int have = 0;
    for (int i = 0; i < c->n; i++) {
        double o[3] = { c->origin[i][0], c->origin[i][1], c->origin[i][2] };
        double d = dist2(q, o);
        if (have == k && d >= dd[k - 1]) continue;
        int j = have < k ? have++ : k - 1;
        while (j > 0 && (AMB_PICK_MUTANT == 3 ? dd[j - 1] >= d : dd[j - 1] > d)) { dd[j] = dd[j - 1]; idx[j] = idx[j - 1]; j--; }
        dd[j] = d;
        idx[j] = i;
    }
    int pick[2] = { AMB_PICK_NONE, AMB_PICK_NONE }, npick = 0;
    if (op) {
        for (int i = 0; i < have && npick < 2; i++) {
            int vis = AMB_PICK_MUTANT == 1 || !amb_segment_solid(m, q, t->p[idx[i]]);
            if (vis) pick[npick++] = idx[i];
            else if (i == 0) flags |= AMB_PICK_NEAR_HIDDEN;
        }
    }
    if (npick == 0) flags |= AMB_PICK_A_FALLBACK;
    if (npick < 2 && have > 1) flags |= AMB_PICK_B_FALLBACK;
    /* fill what is missing with the nearest by distance that is not already a pick */
    for (int i = 0; i < have && npick < 2; i++)
        if (idx[i] != pick[0]) pick[npick++] = idx[i];
    if (op == 0) flags &= ~(uint32_t)(AMB_PICK_A_FALLBACK | AMB_PICK_B_FALLBACK);   /* SOLID says it */
    return (uint32_t)pick[0] | (uint32_t)pick[1] << 11 | flags;
}

typedef struct {
    const AmbMap *m;
    const AmbCubes *c;
    const AmbPickTargets *t;
    const AmbPickParams *p;
    const AmbBox *box;
    uint32_t *out;
} Job;

static int job_cancelled(const AmbPickParams *p) { return p->cancel && *p->cancel; }

static void task_row(void *ctx, int r) {
    Job *j = ctx;
    if (job_cancelled(j->p)) return;   /* the flag stays set until the caller's next job, so the end check below sees it too */
    int y = r % j->box->dim[1], z = r / j->box->dim[1];
    uint32_t *o = j->out + (size_t)r * (size_t)j->box->dim[0];
    for (int x = 0; x < j->box->dim[0]; x++) {
        int a[3] = { j->box->org[0] + x, j->box->org[1] + y, j->box->org[2] + z };
        o[x] = amb_pick_voxel(j->m, j->c, j->t, j->p, a);
    }
}

int amb_pick_box(const AmbMap *m, const AmbCubes *c, const AmbPickTargets *t, const AmbPickParams *p, const AmbBox *box, uint32_t *out) {
    if (!m || !c || !t || !p || !box || !out || !(p->voxel > 0) || t->n != c->n) return AMB_ERR_ARG;
    for (int k = 0; k < 3; k++)
        if (box->dim[k] < 1 || box->dim[k] > 65536) return AMB_ERR_ARG;
    if ((long long)box->dim[1] * box->dim[2] > 1 << 30) return AMB_ERR_ARG;
    Job j = { m, c, t, p, box, out };
    int rows = box->dim[1] * box->dim[2];
    if (p->pfor) p->pfor(p->pool, rows, task_row, &j);
    else for (int r = 0; r < rows; r++) task_row(&j, r);
    return job_cancelled(p) ? AMB_ERR_CANCELLED : AMB_OK;
}

static int wrap_index(int a, int d) { int r = a % d; return r < 0 ? r + d : r; }

int amb_pick_write_toroidal(const uint32_t *src, const AmbBox *box, const int clip_dim[3], uint32_t *table) {
    for (int k = 0; k < 3; k++)
        if (box->dim[k] < 1 || box->dim[k] > clip_dim[k]) return AMB_ERR_ARG;
    for (int z = 0; z < box->dim[2]; z++) {
        int tz = wrap_index(box->org[2] + z, clip_dim[2]);
        for (int y = 0; y < box->dim[1]; y++) {
            int ty = wrap_index(box->org[1] + y, clip_dim[1]);
            const uint32_t *s = src + ((size_t)z * (size_t)box->dim[1] + (size_t)y) * (size_t)box->dim[0];
            uint32_t *row = table + ((size_t)tz * (size_t)clip_dim[1] + (size_t)ty) * (size_t)clip_dim[0];
            for (int x = 0; x < box->dim[0]; x++) row[wrap_index(box->org[0] + x, clip_dim[0])] = s[x];
        }
    }
    return AMB_OK;
}
