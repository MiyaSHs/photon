/* P4a: the pick table core (src/ambient/amb_pick.c) and the line of sight it uses (amb_segment_solid), on a synthetic map with an exact
 * reference (synth_pick.h) and optionally on real maps. Built by scripts/run_ambient_test.sh at -O2, under ASan/UBSan, and once per
 * AMB_PICK_MUTANT, which must fail.
 *   picktest [map.bsp ...]
 * Synthetic: the line of sight against the analytic rule on 200k random segments; every voxel of a box against a reference pick built from that
 * rule; the box against single voxels and against itself on 1, 3 and 8 threads (bit-identical); the flags; cancel; the toroidal write; the engine
 * nearest rule against brute force; lump 42 from memory and through a reader (L4D2 order, LZMA, a funny size, too many samples, truncation, short
 * reads, cancel). Real maps: a whole cascade-0 window at three places, threads against one thread, and every entry against amb_pick_voxel. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/ambient/amb_pick.h"
#include "synth_pick.h"

static long g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { if (g_fails++ < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }
static uint64_t g_rng = 88172645463325252ull;
static double urand(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (double)(g_rng >> 11) / 9007199254740992.0; }
static double uni(double lo, double hi) { return lo + (hi - lo) * urand(); }

/* --- a thread pool for AmbParallelFor --------------------------------------------------------------------------------------- */
typedef struct { int n, next; AmbTaskFn fn; void *ctx; pthread_mutex_t mu; } Pool;
static void *pool_main(void *p) {
    Pool *pl = p;
    for (;;) {
        pthread_mutex_lock(&pl->mu);
        int i = pl->next++;
        pthread_mutex_unlock(&pl->mu);
        if (i >= pl->n) return NULL;
        pl->fn(pl->ctx, i);
    }
}
static void pfor(void *pool, int n, AmbTaskFn fn, void *ctx) {
    int k = *(int *)pool;
    Pool pl = { n, 0, fn, ctx, PTHREAD_MUTEX_INITIALIZER };
    pthread_t t[16];
    for (int i = 0; i < k; i++) pthread_create(&t[i], NULL, pool_main, &pl);
    for (int i = 0; i < k; i++) pthread_join(t[i], NULL);
}

/* --- the reference pick on the synthetic map: the contract of amb_pick.h, written again from the analytic line of sight ------- */
static int ref_solid(const double p[3]) { return p[0] >= SP_WALL_LO && p[0] < SP_WALL && (p[1] < SP_DOOR_LO || p[1] >= SP_DOOR_HI); }
static int ref_open_point(const double p[3], double d, double q[3]) {
    memcpy(q, p, 24);
    if (!ref_solid(q)) return 1;
    for (int ax = 0; ax < 3; ax++)
        for (int s = -1; s <= 1; s += 2) {
            memcpy(q, p, 24);
            q[ax] += s * d;
            if (!ref_solid(q)) return 2;
        }
    memcpy(q, p, 24);
    return 0;
}
static uint32_t ref_pick(const AmbCubes *c, double voxel, int tests, const int a[3]) {
    if (c->n < 1) return AMB_PICK_NONE | AMB_PICK_NONE << 11 | AMB_PICK_EMPTY;
    double ctr[3], q[3], tgt[AMB_CUBES_MAX][3];
    for (int k = 0; k < 3; k++) ctr[k] = (a[k] + 0.5) * voxel;
    for (int i = 0; i < c->n; i++) {
        double o[3] = { c->origin[i][0], c->origin[i][1], c->origin[i][2] };
        ref_open_point(o, 8.0, tgt[i]);
    }
    int op = ref_open_point(ctr, 12.0, q);
    uint32_t fl = op == 0 ? AMB_PICK_SOLID : op == 2 ? AMB_PICK_MOVED : 0;
    /* all samples sorted by (distance, index): selection sort, n is small */
    int ord[AMB_CUBES_MAX], n = c->n;
    double d[AMB_CUBES_MAX];
    for (int i = 0; i < n; i++) {
        ord[i] = i;
        double dx = q[0] - c->origin[i][0], dy = q[1] - c->origin[i][1], dz = q[2] - c->origin[i][2];
        d[i] = dx * dx + dy * dy + dz * dz;
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (d[ord[j]] < d[ord[i]] || (d[ord[j]] == d[ord[i]] && ord[j] < ord[i])) { int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    int k = tests < n ? tests : n, pk[2] = { AMB_PICK_NONE, AMB_PICK_NONE }, np = 0;
    if (op)
        for (int i = 0; i < k && np < 2; i++) {
            if (!synth_pick_blocked(q, tgt[ord[i]])) pk[np++] = ord[i];
            else if (i == 0) fl |= AMB_PICK_NEAR_HIDDEN;
        }
    if (op) {
        if (np == 0) fl |= AMB_PICK_A_FALLBACK;
        if (np < 2 && k > 1) fl |= AMB_PICK_B_FALLBACK;
    }
    for (int i = 0; i < k && np < 2; i++) if (ord[i] != pk[0]) pk[np++] = ord[i];
    return (uint32_t)pk[0] | (uint32_t)pk[1] << 11 | fl;
}

static AmbMap *load_map(const uint8_t *b, size_t len, AmbCubes *c) {
    AmbMap *m = NULL;
    int rc = amb_map_load(b, len, 1, &m);
    CHECK(rc == AMB_OK, "synthetic map load %d", rc);
    rc = amb_cubes_load(b, len, c);
    CHECK(rc == AMB_OK, "synthetic lump 42 load %d", rc);
    return m;
}

static void test_segments(const AmbMap *m) {
    long blocked = 0, n = 200000;
    for (long i = 0; i < n; i++) {
        double a[3], b[3];
        for (int k = 0; k < 3; k++) { a[k] = uni(-300, 300); b[k] = uni(-300, 300); }
        if (i % 4 == 0) { a[1] = uni(-20, 90); b[1] = uni(-20, 90); }   /* many near the doorway */
        if (i % 16 == 1) { b[0] = a[0]; }                                 /* parallel to the wall */
        int want = ref_solid(a) || ref_solid(b) || synth_pick_blocked(a, b);
        int got = amb_segment_solid(m, a, b);
        blocked += got;
        CHECK(got == want, "segment (%.3f %.3f %.3f)-(%.3f %.3f %.3f): C %d, reference %d", a[0], a[1], a[2], b[0], b[1], b[2], got, want);
    }
    printf("segments: %ld random, %ld blocked, all as the analytic rule\n", n, blocked);
}

static void test_table(const AmbMap *m, const AmbCubes *c, const char *tag) {
    AmbPickTargets t;
    amb_pick_targets(m, c, &t);
    AmbPickParams p;
    amb_pick_params_default(&p);
    AmbBox box = { { -24, -24, -6 }, { 48, 48, 12 } };
    size_t nv = 48 * 48 * 12;
    uint32_t *e1 = malloc(nv * 4), *e3 = malloc(nv * 4), *e8 = malloc(nv * 4);
    int rc = amb_pick_box(m, c, &t, &p, &box, e1);
    CHECK(rc == AMB_OK, "%s: amb_pick_box %d", tag, rc);
    long flags[6] = { 0 }, bad = 0;
    for (int z = 0; z < box.dim[2]; z++)
        for (int y = 0; y < box.dim[1]; y++)
            for (int x = 0; x < box.dim[0]; x++) {
                int a[3] = { box.org[0] + x, box.org[1] + y, box.org[2] + z };
                uint32_t got = e1[((size_t)z * box.dim[1] + y) * box.dim[0] + x], want = ref_pick(c, 32.0, p.max_tests, a);
                if (got != want && bad++ < 6) printf("FAIL: %s voxel %d %d %d: 0x%08x, reference 0x%08x\n", tag, a[0], a[1], a[2], got, want);
                CHECK(got == amb_pick_voxel(m, c, &t, &p, a), "%s: box entry differs from amb_pick_voxel", tag);
                for (int f = 0; f < 6; f++) flags[f] += (got >> (22 + f)) & 1;
                if (!(got & (AMB_PICK_SOLID | AMB_PICK_EMPTY)) && c->n >= 2)
                    CHECK(AMB_PICK_A(got) != AMB_PICK_B(got) && AMB_PICK_A(got) < c->n && AMB_PICK_B(got) < c->n, "%s: picks %d %d", tag, AMB_PICK_A(got), AMB_PICK_B(got));
            }
    g_fails += bad;
    int k3 = 3, k8 = 8;
    p.pfor = pfor;
    p.pool = &k3;
    amb_pick_box(m, c, &t, &p, &box, e3);
    p.pool = &k8;
    amb_pick_box(m, c, &t, &p, &box, e8);
    CHECK(memcmp(e1, e3, nv * 4) == 0 && memcmp(e1, e8, nv * 4) == 0, "%s: threads change the table", tag);
    /* two tests per voxel: the fallbacks */
    AmbPickParams p2 = p;
    p2.max_tests = 2;
    long fb[2] = { 0, 0 };
    for (int z = 0; z < box.dim[2]; z += 3)
        for (int y = 0; y < box.dim[1]; y++)
            for (int x = 0; x < box.dim[0]; x++) {
                int a[3] = { box.org[0] + x, box.org[1] + y, box.org[2] + z };
                uint32_t got = amb_pick_voxel(m, c, &t, &p2, a), want = ref_pick(c, 32.0, 2, a);
                CHECK(got == want, "%s, 2 tests, voxel %d %d %d: 0x%08x, reference 0x%08x", tag, a[0], a[1], a[2], got, want);
                fb[0] += !!(got & AMB_PICK_A_FALLBACK);
                fb[1] += !!(got & AMB_PICK_B_FALLBACK);
            }
    if (c->n >= 2) CHECK(fb[0] > 10 && fb[1] > 10, "%s: 2 tests: fallbacks A %ld B %ld", tag, fb[0], fb[1]);
    printf("table %s: %zu voxels as the reference (%ld differ); solid %ld, moved %ld, A fallback %ld, B fallback %ld, nearest hidden %ld, empty %ld\n", tag, nv,
           bad, flags[0], flags[1], flags[2], flags[3], flags[4], flags[5]);
    if (c->n >= 2) CHECK(flags[4] > 100 && flags[1] > 10 && flags[0] > 10, "%s: the flags are not exercised", tag);
    /* cancel: set before the call, every row stops */
    volatile int cancel = 1;
    p.cancel = &cancel;
    CHECK(amb_pick_box(m, c, &t, &p, &box, e3) == AMB_ERR_CANCELLED, "%s: cancel ignored", tag);
    p.pfor = NULL;
    CHECK(amb_pick_box(m, c, &t, &p, &box, e3) == AMB_ERR_CANCELLED, "%s: cancel ignored inline", tag);
    AmbBox bad_box = { { 0, 0, 0 }, { 0, 4, 4 } };
    p.cancel = NULL;
    CHECK(amb_pick_box(m, c, &t, &p, &bad_box, e3) == AMB_ERR_ARG, "%s: an empty box accepted", tag);
    /* toroidal: a box wrapped into a 16 x 16 x 8 table, read back by the rule */
    int clip[3] = { 16, 16, 8 };
    AmbBox tb = { { -21, 5, -3 }, { 16, 9, 8 } };
    uint32_t *src = malloc(16 * 9 * 8 * 4), table[16 * 16 * 8];
    memset(table, 0xee, sizeof table);
    amb_pick_box(m, c, &t, &p, &tb, src);
    CHECK(amb_pick_write_toroidal(src, &tb, clip, table) == AMB_OK, "%s: toroidal write", tag);
    for (int z = 0; z < tb.dim[2]; z++)
        for (int y = 0; y < tb.dim[1]; y++)
            for (int x = 0; x < tb.dim[0]; x++) {
                int a[3] = { tb.org[0] + x, tb.org[1] + y, tb.org[2] + z }, w[3];
                for (int i = 0; i < 3; i++) w[i] = ((a[i] % clip[i]) + clip[i]) % clip[i];
                CHECK(table[(w[2] * 16 + w[1]) * 16 + w[0]] == src[(z * 9 + y) * 16 + x], "%s: toroidal entry", tag);
            }
    AmbBox big = { { 0, 0, 0 }, { 17, 1, 1 } };
    CHECK(amb_pick_write_toroidal(src, &big, clip, table) == AMB_ERR_ARG, "%s: oversized toroidal box", tag);
    free(src);
    free(e1); free(e3); free(e8);
}

static void test_nearest(const AmbCubes *c) {
    for (int i = 0; i < 20000; i++) {
        float p[3] = { (float)uni(-1500, 1500), (float)uni(-1500, 1500), (float)uni(-500, 500) };
        if (i % 10 == 0) for (int k = 0; k < 3; k++) p[k] = (float)c->origin[i % c->n][k];   /* on a sample */
        int best = -1;
        double bd = 0;
        for (int s = 0; s < c->n; s++) {
            double dx = p[0] - c->origin[s][0], dy = p[1] - c->origin[s][1], dz = p[2] - c->origin[s][2], d = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d < bd) { bd = d; best = s; }
        }
        int got = amb_cube_nearest(c, p);
        /* float vs double can only differ on a near-tie */
        if (got != best) {
            double dx = p[0] - c->origin[got][0], dy = p[1] - c->origin[got][1], dz = p[2] - c->origin[got][2];
            CHECK(fabs(sqrt(dx * dx + dy * dy + dz * dz) - sqrt(bd)) < 1e-2, "nearest %d, brute force %d", got, best);
        }
    }
    /* ties: the first in lump order */
    AmbCubes t;
    memset(&t, 0, sizeof t);
    t.n = 3;
    int o[3][3] = { { 10, 0, 0 }, { -10, 0, 0 }, { 0, 10, 0 } };
    memcpy(t.origin, o, sizeof o);
    float z[3] = { 0, 0, 0 };
    CHECK(amb_cube_nearest(&t, z) == 0, "tie not resolved to the first");
    t.n = 0;
    CHECK(amb_cube_nearest(&t, z) == -1, "no samples");
}

/* --- lump 42 through readers -------------------------------------------------------------------------------------------------- */
typedef struct { const uint8_t *mem; long long size; long reads, fail_at, cancel_at; volatile int cancel; } Rd;
static int rd_read(void *ctx, long long ofs, void *buf, int n) {
    Rd *r = ctx;
    r->reads++;
    if (ofs < 0 || n <= 0 || ofs > r->size || n > r->size - ofs) return -1;
    if (r->cancel_at && r->reads == r->cancel_at) r->cancel = 1;
    if (r->fail_at && r->reads == r->fail_at) return n / 2;
    memcpy(buf, r->mem + ofs, (size_t)n);
    return n;
}
static int load_both(const uint8_t *b, size_t len, AmbCubes *c, long fail_at, long cancel_at) {
    static AmbCubes m;
    int rm = amb_cubes_load(b, len, &m);
    Rd r = { b, (long long)len, 0, fail_at, cancel_at, 0 };
    AmbReader ar = { &r, (long long)len, rd_read, &r.cancel };
    AmbReadStats st;
    int rr = amb_cubes_load_reader(&ar, c, &st);
    if (!fail_at && !cancel_at) {
        CHECK(rr == rm, "reader %d, memory %d", rr, rm);
        if (rr == AMB_OK) CHECK(c->n == m.n && memcmp(c->origin, m.origin, sizeof(int) * 3 * (size_t)m.n) == 0, "reader and memory samples differ");
        CHECK(st.bytes_read <= 1032 + 16 * AMB_CUBES_MAX, "reader read %lld bytes", st.bytes_read);
    }
    return rr;
}

static void test_lump(void) {
    size_t len;
    AmbCubes c;
    int ref[SP_MAX_CUBES][3], nref = synth_pick_cubes(0, ref);
    for (int l4d2 = 0; l4d2 < 2; l4d2++) {
        uint8_t *b = synth_pick_bsp(0, l4d2, 0, 0, &len);
        CHECK(load_both(b, len, &c, 0, 0) == AMB_OK && c.n == nref && memcmp(c.origin, ref, sizeof(int) * 3 * (size_t)nref) == 0, "lump 42 (l4d2 %d)", l4d2);
        CHECK(c.size[3] == 3, "size field");
        AmbMap *m = NULL;
        CHECK(amb_map_load(b, len, 1, &m) == AMB_OK, "map (l4d2 %d)", l4d2);
        amb_map_free(m);
        /* every truncation: both paths agree on the code */
        for (size_t cut = 0; cut < len; cut += 7) load_both(b, cut, &c, 0, 0);
        CHECK(load_both(b, len, &c, 2, 0) == AMB_ERR_READ, "short read");
        CHECK(load_both(b, len, &c, 0, 1) == AMB_ERR_CANCELLED, "cancel");
        free(b);
    }
    uint8_t *b = synth_pick_bsp(0, 0, 1, 0, &len);
    CHECK(load_both(b, len, &c, 0, 0) == AMB_ERR_LZMA, "LZMA lump");
    free(b);
    b = synth_pick_bsp(0, 0, 0, -1, &len);
    CHECK(load_both(b, len, &c, 0, 0) == AMB_ERR_FORMAT, "funny lump size");
    free(b);
    b = synth_pick_bsp(0, 0, 0, AMB_CUBES_MAX + 1, &len);
    CHECK(load_both(b, len, &c, 0, 0) == AMB_ERR_FORMAT, "too many samples");
    free(b);
    b = synth_pick_bsp(0, 0, 0, AMB_CUBES_MAX, &len);
    CHECK(load_both(b, len, &c, 0, 0) == AMB_OK && c.n == AMB_CUBES_MAX, "the most samples");
    free(b);
    /* hostile headers: random lump 42 ranges */
    b = synth_pick_bsp(0, 0, 0, 0, &len);
    for (int i = 0; i < 3000; i++) {
        uint8_t *h = b + 8 + 16 * 42;
        uint32_t ofs = (uint32_t)(urand() * 4e9), n = (uint32_t)(urand() < 0.5 ? urand() * 4e9 : (double)(16 * (int)(urand() * 40)));
        if (i % 3 == 0) ofs = (uint32_t)(urand() * (double)len);
        sp_w32(h, ofs); sp_w32(h + 4, n);
        load_both(b, len, &c, 0, 0);
    }
    free(b);
    printf("lump 42: L4D2 order, truncations, short read, cancel, LZMA, funny size, %d and %d samples, 3000 hostile ranges\n", AMB_CUBES_MAX, AMB_CUBES_MAX + 1);
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void test_real(const char *path) {
    size_t len;
    uint8_t *b = slurp(path, &len);
    if (!b) { printf("skip %s (not readable)\n", path); return; }
    static AmbCubes c;
    static AmbPickTargets t;
    AmbMap *m = NULL;
    CHECK(amb_map_load(b, len, 1, &m) == AMB_OK && amb_cubes_load(b, len, &c) == AMB_OK, "%s: load", path);
    if (!m) { free(b); return; }
    amb_pick_targets(m, &c, &t);
    AmbMapInfo mi;
    amb_map_info(m, &mi);
    AmbPickParams p;
    amb_pick_params_default(&p);
    size_t nv = 64 * 64 * 32;
    uint32_t *e1 = malloc(nv * 4), *e8 = malloc(nv * 4);
    for (int w = 0; w < 3; w++) {
        /* windows centred on samples 0, n/2 and n-1 */
        const int *o = c.origin[w == 0 ? 0 : w == 1 ? c.n / 2 : c.n - 1];
        AmbBox box = { { (int)floor(o[0] / 32.0) - 32, (int)floor(o[1] / 32.0) - 32, (int)floor(o[2] / 32.0) - 16 }, { 64, 64, 32 } };
        double t0 = now_ms();
        p.pfor = NULL;
        CHECK(amb_pick_box(m, &c, &t, &p, &box, e1) == AMB_OK, "%s: box", path);
        double t1 = now_ms();
        int k8 = 8;
        p.pfor = pfor;
        p.pool = &k8;
        amb_pick_box(m, &c, &t, &p, &box, e8);
        double t2 = now_ms();
        CHECK(memcmp(e1, e8, nv * 4) == 0, "%s: threads change the table", path);
        long hidden = 0, open = 0;
        for (size_t i = 0; i < nv; i += 1) {
            if (!(e1[i] & AMB_PICK_SOLID)) { open++; hidden += !!(e1[i] & AMB_PICK_NEAR_HIDDEN); }
            if (i % 97 == 0) {
                int a[3] = { box.org[0] + (int)(i % 64), box.org[1] + (int)(i / 64 % 64), box.org[2] + (int)(i / 4096) };
                p.pfor = NULL;
                CHECK(amb_pick_voxel(m, &c, &t, &p, a) == e1[i], "%s: entry differs from amb_pick_voxel", path);
            }
        }
        printf("%s window at sample (%d %d %d): %.0f ms one thread, %.0f ms on 8; open %ld, nearest hidden %ld\n", path, o[0], o[1], o[2], t1 - t0, t2 - t1,
               open, hidden);
    }
    free(e1); free(e8);
    amb_map_free(m);
    free(b);
}

int main(int argc, char **argv) {
    size_t len;
    static AmbCubes c;
    uint8_t *b = synth_pick_bsp(0, 0, 0, 0, &len);
    AmbMap *m = load_map(b, len, &c);
    if (!m) return 1;
    test_segments(m);
    test_table(m, &c, "variant 0");
    test_nearest(&c);
    static AmbCubes c1, c2;
    uint8_t *b1 = synth_pick_bsp(1, 0, 0, 0, &len);
    AmbMap *m1 = load_map(b1, len, &c1);
    test_table(m1, &c1, "variant 1");
    uint8_t *b2 = synth_pick_bsp(2, 0, 0, 0, &len);
    AmbMap *m2 = load_map(b2, len, &c2);
    test_table(m2, &c2, "one sample");
    c2.n = 0;
    test_table(m2, &c2, "no samples");
    amb_map_free(m); amb_map_free(m1); amb_map_free(m2);
    free(b); free(b1); free(b2);
    test_lump();
    for (int i = 1; i < argc; i++) test_real(argv[i]);
    if (g_fails) { printf("PICKTEST FAIL (%ld)\n", g_fails); return 1; }
    printf("PICKTEST OK\n");
    return 0;
}
