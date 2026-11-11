/* The lump reader (amb_map_load_reader) and the GMA index (amb_gma_find), against the whole-file loader. Built twice by scripts/run_ambient_test.sh
 * (-O2 for the timings, ASan/UBSan for the hostile files), and once per AMB_GMA_MUTANT, which must fail.
 *   readtest [map.bsp ...]: for each map, from disk and from inside a synthetic GMA written next to the build: the same AmbMap bit for bit as
 *   amb_map_load on the whole file, for LDR and HDR; bytes read, reads, the largest read and the peak of buffers held, against the whole file.
 *   Always: synthetic maps (L4D2 lump order, LZMA); 3000 mutated headers and every truncation of the synthetic map, where both paths must
 *   return the same code and the same map; short reads; cancel; hostile GMA indexes. Every read is checked against the reader's size. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../src/ambient/amb_bake.h"
#include "../../src/ambient/amb_gma.h"
#include "synth_bsp.h"

static long g_fails;
#define CHECK(cond, ...) do { if (!(cond)) { if (g_fails++ < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6;
}

/* --- readers ------------------------------------------------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *mem;       /* or fd */
    int fd;
    long long size;
    long reads, bad, fail_at, cancel_at;   /* fail_at: this read returns short; cancel_at: the flag is set during this read */
    int largest;
    volatile int cancel;
    long reads_after_cancel;
} Rd;

static int rd_read(void *ctx, long long ofs, void *buf, int n) {
    Rd *r = ctx;
    if (r->cancel) r->reads_after_cancel++;
    r->reads++;
    if (n > r->largest) r->largest = n;
    if (ofs < 0 || n <= 0 || n > AMB_READ_CHUNK || ofs > r->size || n > r->size - ofs) { r->bad++; return -1; }
    if (r->cancel_at && r->reads == r->cancel_at) r->cancel = 1;
    if (r->fail_at && r->reads == r->fail_at) return n / 2;
    if (r->mem) { memcpy(buf, r->mem + ofs, (size_t)n); return n; }
    ssize_t got = pread(r->fd, buf, (size_t)n, (off_t)ofs);
    return (int)got;
}
static AmbReader reader_of(Rd *r) { AmbReader a = { r, r->size, rd_read, &r->cancel }; return a; }
static Rd rd_mem(const uint8_t *p, long long n) { Rd r; memset(&r, 0, sizeof r); r.mem = p; r.fd = -1; r.size = n; return r; }

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* --- a GMA, written the way gmad writes one --------------------------------------------------------------------------------------- */
typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put_le(Buf *b, unsigned long long v, int n) { uint8_t t[8]; for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * i)); put(b, t, (size_t)n); }
static void put_s(Buf *b, const char *s) { put(b, s, strlen(s) + 1); }
typedef struct { const char *name; const uint8_t *data; long long size; } GmaFile;
/* The index into idx; returns where each file's bytes start (absolute), the data appended by the caller in order */
static void gma_index(Buf *idx, int version, const GmaFile *f, int n, long long *starts) {
    put(idx, "GMAD", 4);
    put_le(idx, (unsigned)version, 1);
    put_le(idx, 76561197960265728ull, 8);
    put_le(idx, 1462129767ull, 8);
    if (version > 1) { put_s(idx, "base"); put_s(idx, ""); }
    put_s(idx, "photon test");
    put_s(idx, "{\"description\":\"a synthetic addon\",\"type\":\"map\"}");
    put_s(idx, "Author Name");
    put_le(idx, 1, 4);
    for (int i = 0; i < n; i++) { put_le(idx, (unsigned)(i + 1), 4); put_s(idx, f[i].name); put_le(idx, (unsigned long long)f[i].size, 8); put_le(idx, 0x1234, 4); }
    put_le(idx, 0, 4);
    long long at = (long long)idx->n;
    for (int i = 0; i < n; i++) { starts[i] = at; at += f[i].size; }
}

static void compare(const char *what, const AmbMap *want, int rc_want, const AmbMap *got, int rc_got) {
    CHECK(rc_want == rc_got, "%s: the whole file gives %d, the reader %d", what, rc_want, rc_got);
    if (rc_want == AMB_OK && rc_got == AMB_OK) CHECK(amb_map_same(want, got), "%s: the maps differ", what);
}

/* A small bake of each, compared bytewise: the map is the bake's only input, so this is a second look at the same claim */
static void compare_bake(const char *what, const AmbMap *a, const AmbMap *b) {
    AmbMapInfo ia;
    amb_map_info(a, &ia);
    AmbBakeParams p;
    amb_params_default(&p);
    double c[3] = { 0.5 * (ia.world_min[0] + ia.world_max[0]), 0.5 * (ia.world_min[1] + ia.world_max[1]), 0.5 * (ia.world_min[2] + ia.world_max[2]) };
    AmbBox box = { { (int)(c[0] / 32) - 8, (int)(c[1] / 32) - 8, (int)(c[2] / 32) - 4 }, { 16, 16, 8 } };
    size_t n = 16 * 16 * 8 * AMB_VOXEL_FLOATS;
    float *va = malloc(n * sizeof(float)), *vb = malloc(n * sizeof(float));
    int ra = amb_bake_region(a, &p, &box, va, NULL), rb = amb_bake_region(b, &p, &box, vb, NULL);
    CHECK(ra == AMB_OK && rb == AMB_OK && memcmp(va, vb, n * sizeof(float)) == 0, "%s: the bakes differ (%d, %d)", what, ra, rb);
    free(va);
    free(vb);
}

/* --- real maps ---------------------------------------------------------------------------------------------------------------------- */
static void real_map(const char *path, const char *dir) {
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    double t0 = now_ms();
    size_t len = 0;
    uint8_t *whole = slurp(path, &len);   /* the old loader's read: the whole file */
    double t1 = now_ms();
    if (!whole) { CHECK(0, "%s: cannot read", path); return; }
    int fd = open(path, O_RDONLY);
    for (int hdr = 0; hdr < 2; hdr++) {
        AmbMap *m0 = NULL, *m1 = NULL, *m2 = NULL;
        double t2 = now_ms();
        int r0 = amb_map_load(whole, len, hdr, &m0);
        double t3 = now_ms();
        Rd rd;
        memset(&rd, 0, sizeof rd);
        rd.fd = fd;
        rd.size = (long long)len;
        AmbReader ar = reader_of(&rd);
        AmbReadStats st;
        double t4 = now_ms();
        int r1 = amb_map_load_reader(&ar, hdr, &m1, &st);
        double t5 = now_ms();
        char what[300];
        snprintf(what, sizeof what, "%s %s from disk", base, hdr ? "HDR" : "LDR");
        compare(what, m0, r0, m1, r1);
        CHECK(r0 == AMB_OK, "%s: %d", what, r0);
        CHECK(!rd.bad && rd.largest <= AMB_READ_CHUNK && st.bytes_read <= AMB_READ_MAX && st.bytes_read < (long long)len, "%s: %ld bad reads, largest %d, %lld bytes",
              what, rd.bad, rd.largest, st.bytes_read);
        if (r0 == AMB_OK && r1 == AMB_OK) compare_bake(what, m0, m1);
        printf("  %s: whole file %zu bytes, read %.1f ms + parse %.1f ms; lumps %lld bytes (%.2f%%) in %d reads, largest %d, peak held %lld bytes, %.1f ms\n", what, len,
               t1 - t0, t3 - t2, st.bytes_read, 100.0 * (double)st.bytes_read / (double)len, st.reads, rd.largest, st.peak_bytes, t5 - t4);

        /* inside a GMA: two files before it and one after, so an offset that is off by any one entry lands somewhere else */
        static uint8_t pad1[777], pad2[4096], pad3[13];
        memset(pad1, 'a', sizeof pad1);
        memset(pad2, 'b', sizeof pad2);
        memcpy(pad2, "VBSP", 4);   /* a decoy: a BSP magic at the wrong place */
        char inner[300];
        snprintf(inner, sizeof inner, "maps/%s", base);
        GmaFile files[4] = { { "materials/a.vmt", pad1, sizeof pad1 }, { "maps/decoy.bsp", pad2, sizeof pad2 }, { inner, whole, (long long)len }, { "lua/x.lua", pad3, sizeof pad3 } };
        Buf idx = { 0 };
        long long starts[4];
        gma_index(&idx, 3, files, 4, starts);
        char gpath[600];
        snprintf(gpath, sizeof gpath, "%s/readtest_%s.gma", dir, base);
        FILE *g = fopen(gpath, "wb");
        int ok = g && fwrite(idx.p, 1, idx.n, g) == idx.n;
        for (int i = 0; i < 4 && ok; i++) ok = fwrite(files[i].data, 1, (size_t)files[i].size, g) == (size_t)files[i].size;
        ok = ok && fwrite("\0\0\0\0", 1, 4, g) == 4;
        if (g) fclose(g);
        free(idx.p);
        CHECK(ok, "%s: cannot write the GMA", gpath);
        int gfd = open(gpath, O_RDONLY);
        struct stat sb;
        fstat(gfd, &sb);
        Rd gr;
        memset(&gr, 0, sizeof gr);
        gr.fd = gfd;
        gr.size = (long long)sb.st_size;
        AmbReader ga = reader_of(&gr);
        long long o = -1, sz = -1;
        AmbGmaInfo gi;
        /* upper case and backslashes: the filesystem's names are not case-sensitive */
        char upper[300];
        snprintf(upper, sizeof upper, "MAPS\\%s", base);
        for (char *c = upper; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
        int rg = amb_gma_find(&ga, upper, -1, &o, &sz, &gi);
        CHECK(rg == AMB_OK && o == starts[2] && sz == (long long)len, "%s: the BSP's offset in the GMA: rc %d, %lld (wanted %lld), size %lld", what, rg, o, starts[2], sz);
        long long o2, sz2;
        CHECK(amb_gma_find(&ga, inner, starts[2], &o2, &sz2, NULL) == AMB_OK && o2 == starts[2], "%s: the lookup at the engine's offset", what);
        CHECK(amb_gma_find(&ga, inner, starts[2] + 1, &o2, &sz2, NULL) == AMB_GMA_ABSENT, "%s: an offset the index does not have was accepted", what);
        CHECK(amb_gma_find(&ga, "maps/none.bsp", -1, &o2, &sz2, NULL) == AMB_GMA_ABSENT, "%s: a path the index does not have", what);
        AmbSubReader sub;
        AmbReader sa;
        AmbReadStats gs;
        double t6 = now_ms();
        int rs = amb_sub_reader(&sa, &sub, &ga, o, sz);
        if (rs == AMB_OK) rs = amb_map_load_reader(&sa, hdr, &m2, &gs);
        double t7 = now_ms();
        snprintf(what, sizeof what, "%s %s inside a GMA", base, hdr ? "HDR" : "LDR");
        compare(what, m0, r0, m2, rs);
        if (r0 == AMB_OK && rs == AMB_OK) compare_bake(what, m0, m2);
        CHECK(!gr.bad, "%s: %ld reads outside the GMA", what, gr.bad);
        printf("  %s: index of %d entries, %lld bytes read for it, data at %lld..%lld; the map at %lld, its lumps in %.1f ms\n", what, gi.entries, gi.index_bytes_read,
               gi.data_start, gi.data_end, o, t7 - t6);
        close(gfd);
        unlink(gpath);
        amb_map_free(m0);
        amb_map_free(m1);
        amb_map_free(m2);
    }
    close(fd);
    free(whole);
}

/* --- synthetic and hostile ---------------------------------------------------------------------------------------------------------- */
static unsigned g_rng = 12345;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

static void both(const char *what, const uint8_t *b, size_t n, int hdr, long *ok_count) {
    uint8_t *copy = malloc(n ? n : 1);   /* exactly n bytes, so ASan sees any read past them */
    memcpy(copy, b, n);
    AmbMap *m0 = NULL, *m1 = NULL;
    int r0 = amb_map_load(copy, n, hdr, &m0);
    Rd rd = rd_mem(copy, (long long)n);
    AmbReader ar = reader_of(&rd);
    AmbReadStats st;
    int r1 = amb_map_load_reader(&ar, hdr, &m1, &st);
    compare(what, m0, r0, m1, r1);
    CHECK(!rd.bad && st.bytes_read <= AMB_READ_MAX && st.peak_bytes <= AMB_READ_MAX, "%s: %ld bad reads, %lld bytes", what, rd.bad, st.bytes_read);
    if (r1 == AMB_OK && ok_count) (*ok_count)++;
    amb_map_free(m0);
    amb_map_free(m1);
    free(copy);
}

static void synthetic(void) {
    size_t n[3];
    uint8_t *b[3] = { synth_bsp_variant(0, 0, 0, &n[0]), synth_bsp_variant(1, 0, 37, &n[1]), synth_bsp_variant(0, 1, 0, &n[2]) };
    for (int h = 0; h < 2; h++) {
        both("synthetic", b[0], n[0], h, NULL);
        both("synthetic, L4D2 lump order", b[1], n[1], h, NULL);
    }
    AmbMap *m = NULL;
    Rd rd = rd_mem(b[1], (long long)n[1]);
    AmbReader ar = reader_of(&rd);
    CHECK(amb_map_load_reader(&ar, 0, &m, NULL) == AMB_OK, "the L4D2 map through the reader");
    amb_map_free(m);
    rd = rd_mem(b[2], (long long)n[2]);
    ar = reader_of(&rd);
    CHECK(amb_map_load_reader(&ar, 0, &m, NULL) == AMB_ERR_LZMA && !m, "an LZMA lump through the reader");
    both("synthetic, LZMA", b[2], n[2], 0, NULL);

    /* every truncation */
    for (size_t k = 0; k <= n[0]; k++) both("truncated", b[0], k, 0, NULL);
    /* mutated headers: lump fields to boundary values, random bytes, the L4D2 switch */
    long loaded = 0;
    uint8_t *x = malloc(n[0]);
    for (int i = 0; i < 3000; i++) {
        memcpy(x, b[0], n[0]);
        int edits = 1 + (int)(rnd() % 4);
        for (int e = 0; e < edits; e++) {
            static const int lumps[] = { 0, 1, 5, 10, 14, 51, 52, 55, 56 };
            int l = lumps[rnd() % 9], field = (int)(rnd() % 3);
            uint8_t *f = x + 8 + 16 * l + 4 * field;
            static const uint32_t vals[] = { 0, 1, 4, 20, 28, 32, 1000, 1001, 0x7fffffff, 0x80000000u, 0xffffffffu, 1032, 1036 };
            switch (rnd() % 3) {
            case 0: synth_w32(f, vals[rnd() % 13]); break;
            case 1: synth_w32(f, (uint32_t)(rnd() % (unsigned)(n[0] + 64))); break;
            default: x[rnd() % (unsigned)n[0]] ^= (uint8_t)(1u << (rnd() % 8)); break;
            }
        }
        both("mutated", x, n[0], (int)(rnd() % 2), &loaded);
    }
    free(x);
    printf("  synthetic maps: L4D2 order and LZMA as the whole-file loader; %zu truncations and 3000 mutated headers give the same codes and maps (%ld loaded)\n",
           n[0] + 1, loaded);

    /* a short read, wherever it lands; then cancel, set during read k: no read after it */
    rd = rd_mem(b[0], (long long)n[0]);
    ar = reader_of(&rd);
    CHECK(amb_map_load_reader(&ar, 0, &m, NULL) == AMB_OK, "the synthetic map");
    amb_map_free(m);
    long full = rd.reads;
    for (long at = 1; at <= full; at++) {
        rd = rd_mem(b[0], (long long)n[0]);
        rd.fail_at = at;
        ar = reader_of(&rd);
        int rc = amb_map_load_reader(&ar, 0, &m, NULL);
        CHECK(rc == AMB_ERR_READ && !m && rd.reads == at, "a short read at call %ld gave %d after %ld reads", at, rc, rd.reads);
        amb_map_free(m);
        rd = rd_mem(b[0], (long long)n[0]);
        rd.cancel_at = at;
        ar = reader_of(&rd);
        rc = amb_map_load_reader(&ar, 0, &m, NULL);
        if (at < full) CHECK(rc == AMB_ERR_CANCELLED && !m && rd.reads == at && !rd.reads_after_cancel, "cancel at read %ld: %d, %ld reads, %ld after", at, rc, rd.reads, rd.reads_after_cancel);
        else CHECK(rc == AMB_OK, "cancel after the last read: %d", rc);
        amb_map_free(m);
    }
    printf("  a short read at each of the %ld reads fails the load; cancel during each stops it with no further read\n", full);
    for (int i = 0; i < 3; i++) free(b[i]);
}

/* A hostile GMA index: every truncation, mutated bytes, and the specific traps */
static void hostile_gma(void) {
    size_t n = 0;
    uint8_t *bsp = synth_bsp_variant(0, 0, 0, &n);
    GmaFile files[3] = { { "a.txt", (const uint8_t *)"hello", 5 }, { "maps/x.bsp", bsp, (long long)n }, { "b.txt", (const uint8_t *)"xy", 2 } };
    for (int version = 1; version <= 3; version += 2) {
        Buf g = { 0 };
        long long starts[3];
        gma_index(&g, version, files, 3, starts);
        size_t index_n = g.n;
        for (int i = 0; i < 3; i++) put(&g, files[i].data, (size_t)files[i].size);
        long long o, sz;
        Rd rd = rd_mem(g.p, (long long)g.n);
        AmbReader ar = reader_of(&rd);
        CHECK(amb_gma_find(&ar, "maps/x.bsp", -1, &o, &sz, NULL) == AMB_OK && o == starts[1] && sz == (long long)n, "v%d: the offset %lld, wanted %lld", version, o, starts[1]);
        AmbSubReader sub;
        AmbReader sa;
        AmbMap *m = NULL, *m0 = NULL;
        amb_map_load(bsp, n, 0, &m0);
        CHECK(amb_sub_reader(&sa, &sub, &ar, o, sz) == AMB_OK && amb_map_load_reader(&sa, 0, &m, NULL) == AMB_OK && amb_map_same(m, m0), "v%d: the map inside", version);
        amb_map_free(m);
        amb_map_free(m0);
        CHECK(amb_sub_reader(&sa, &sub, &ar, o, (long long)g.n) == AMB_ERR_ARG, "a sub reader past the file");
        /* every truncation: in the index it is a format error; in the data, the entries no longer fit */
        for (size_t k = 0; k < g.n; k++) {
            uint8_t *c = malloc(k ? k : 1);
            memcpy(c, g.p, k);
            Rd t = rd_mem(c, (long long)k);
            AmbReader ta = reader_of(&t);
            int rc = amb_gma_find(&ta, "maps/x.bsp", -1, &o, &sz, NULL);
            CHECK(rc == AMB_ERR_FORMAT && !t.bad, "v%d truncated at %zu (index %zu): %d, %ld bad reads", version, k, index_n, rc, t.bad);
            free(c);
        }
        /* mutated index bytes: an error, or an entry that lies inside the file */
        long accepted = 0;
        for (int i = 0; i < 4000; i++) {
            uint8_t *c = malloc(g.n);
            memcpy(c, g.p, g.n);
            for (int e = 0; e < 1 + (int)(rnd() % 3); e++) {
                size_t at = rnd() % (unsigned)index_n;
                c[at] = rnd() % 4 == 0 ? 0xff : rnd() % 3 == 0 ? 0 : (uint8_t)rnd();
            }
            Rd t = rd_mem(c, (long long)g.n);
            AmbReader ta = reader_of(&t);
            int rc = amb_gma_find(&ta, "maps/x.bsp", -1, &o, &sz, NULL);
            CHECK(!t.bad, "a mutated index read outside the file");
            if (rc == AMB_OK) { accepted++; CHECK(o >= 0 && sz >= 0 && o <= (long long)g.n && sz <= (long long)g.n - o, "an entry outside the file: %lld + %lld", o, sz); }
            else CHECK(rc == AMB_ERR_FORMAT || rc == AMB_GMA_ABSENT, "mutated index: %d", rc);
            free(c);
        }
        printf("  GMA v%d: map at %lld (index %zu bytes): every truncation refused, 4000 mutated indexes in bounds (%ld still found it)\n", version, starts[1], index_n, accepted);
        free(g.p);
    }
    /* traps: a size that overflows the sum, a negative size, a path without its NUL, required content without its end, a huge description */
    struct { const char *what; int version; int trap; } traps[] = { { "a size of 2^63 - 1", 3, 1 }, { "a negative size", 3, 2 }, { "a path with no NUL", 3, 3 },
                                                                    { "required content with no end", 3, 4 }, { "two entries whose sum overflows", 3, 5 }, { "not GMAD", 3, 6 } };
    for (size_t i = 0; i < sizeof traps / sizeof traps[0]; i++) {
        Buf g = { 0 };
        put(&g, traps[i].trap == 6 ? "GMAX" : "GMAD", 4);
        put_le(&g, 3, 1);
        put_le(&g, 0, 8);
        put_le(&g, 0, 8);
        if (traps[i].trap == 4) { for (int k = 0; k < 9000; k++) put_s(&g, "x"); }
        else put_s(&g, "");
        put_s(&g, "n");
        put_s(&g, "d");
        put_s(&g, "a");
        put_le(&g, 1, 4);
        put_le(&g, 1, 4);
        if (traps[i].trap == 3) { for (int k = 0; k < 3000; k++) put(&g, "p", 1); }
        else put_s(&g, "maps/x.bsp");
        put_le(&g, traps[i].trap == 1 || traps[i].trap == 5 ? 0x7fffffffffffffffull : traps[i].trap == 2 ? 0xffffffffffffff00ull : 4, 8);
        put_le(&g, 0, 4);
        if (traps[i].trap == 5) { put_le(&g, 2, 4); put_s(&g, "y"); put_le(&g, 0x7fffffffffffffffull, 8); put_le(&g, 0, 4); }
        put_le(&g, 0, 4);
        put(&g, "VBSP", 4);
        Rd t = rd_mem(g.p, (long long)g.n);
        AmbReader ta = reader_of(&t);
        long long o, sz;
        int rc = amb_gma_find(&ta, "maps/x.bsp", -1, &o, &sz, NULL);
        CHECK(rc == AMB_ERR_FORMAT && !t.bad, "%s: %d", traps[i].what, rc);
        free(g.p);
    }
    printf("  GMA traps refused: overflowing and negative sizes, a path with no NUL, endless required content, not GMAD\n");
    free(bsp);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *dir = getenv("READTEST_DIR") ? getenv("READTEST_DIR") : ".";
    synthetic();
    hostile_gma();
    for (int i = 1; i < argc; i++) real_map(argv[i], dir);
    printf(g_fails ? "READTEST FAIL: %ld\n" : "READTEST OK\n", g_fails);
    return g_fails ? 1 : 0;
}
