/* See amb_gma.h. */
#include "amb_gma.h"
#include <stdlib.h>
#include <string.h>

/* Test-only (scripts/run_ambient_test.sh): 1 offsets start before the list's end marker, 2 an entry's offset leaves out the entry before it.
 * tests/ambient/readtest.c must fail on each. */
#ifndef AMB_GMA_MUTANT
#define AMB_GMA_MUTANT 0
#endif

enum { BUF = 64 << 10 };
typedef struct {
    const AmbReader *r;
    uint8_t *buf;
    long long at, buf_ofs;   /* at: the next byte's file offset; buf holds [buf_ofs, buf_ofs + n) */
    int n, rc;
    long long read_bytes;
} Stream;

static int stream_cancelled(const AmbReader *r) { return r->cancel && *r->cancel; }   /* an intended race, as in amb_bake.c */
static int refill(Stream *s) {
    if (s->rc) return 0;
    if (s->at >= s->r->size || s->at >= AMB_GMA_MAX_INDEX) { s->rc = AMB_ERR_FORMAT; return 0; }   /* the index runs past the file, or is too long */
    if (stream_cancelled(s->r)) { s->rc = AMB_ERR_CANCELLED; return 0; }
    long long left = s->r->size - s->at;
    int k = left < BUF ? (int)left : BUF;
    if (s->r->read(s->r->ctx, s->at, s->buf, k) != k) { s->rc = AMB_ERR_READ; return 0; }
    s->read_bytes += k;
    s->buf_ofs = s->at;
    s->n = k;
    return 1;
}
static int get(Stream *s, uint8_t *b) {
    if (s->at < s->buf_ofs || s->at >= s->buf_ofs + s->n) { if (!refill(s)) return 0; }
    *b = s->buf[s->at - s->buf_ofs];
    s->at++;
    return 1;
}
static int get_n(Stream *s, uint8_t *dst, int n) {
    for (int i = 0; i < n; i++) if (!get(s, dst + i)) return 0;
    return 1;
}
static unsigned long long le(const uint8_t *p, int n) {
    unsigned long long v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
/* A c-string of at most max bytes before its NUL; with out, it is kept (out holds max + 1). Returns its length, -1 on failure. */
static long long cstr(Stream *s, char *out, long long max) {
    for (long long i = 0;; i++) {
        uint8_t c;
        if (!get(s, &c)) return -1;
        if (!c) { if (out) out[i] = 0; return i; }
        if (i >= max) { s->rc = AMB_ERR_FORMAT; return -1; }
        if (out) out[i] = (char)c;
    }
}
static int path_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

enum { MAX_HITS = 16 };   /* the same path listed more than this many times: the later ones are not candidates */

int amb_gma_find(const AmbReader *r, const char *path, long long want_ofs, long long *ofs, long long *size, AmbGmaInfo *info) {
    if (info) memset(info, 0, sizeof *info);
    if (!r || !r->read || r->size < 0 || !path || !ofs || !size) return AMB_ERR_ARG;
    *ofs = *size = -1;
    Stream s = { .r = r };
    s.buf = malloc(BUF);
    char *name = malloc(AMB_GMA_MAX_PATH + 1);
    struct { long long rel, size; } hit[MAX_HITS];   /* rel: offset from the data start, which is known only at the list's end */
    int rc = AMB_ERR_NOMEM, entries = 0, version = 0, nhit = 0;
    long long data_start = 0, total = 0;
    if (!s.buf || !name) goto out;
    uint8_t h[21];
    rc = AMB_ERR_FORMAT;
    if (!get_n(&s, h, 21)) goto stream;
    if (memcmp(h, "GMAD", 4) != 0) goto out;
    version = h[4];
    if (version > 1) {
        int n = 0;
        long long len;
        while ((len = cstr(&s, NULL, AMB_GMA_MAX_STRING)) > 0)
            if (++n > AMB_GMA_MAX_REQUIRED) goto out;
        if (len < 0) goto stream;
    }
    for (int i = 0; i < 3; i++)   /* name, description, author */
        if (cstr(&s, NULL, AMB_GMA_MAX_STRING) < 0) goto stream;
    uint8_t v[4];
    if (!get_n(&s, v, 4)) goto stream;
    for (;;) {
        uint8_t num[4];
        if (!get_n(&s, num, 4)) goto stream;
        if (le(num, 4) == 0) { data_start = AMB_GMA_MUTANT == 1 ? s.at - 4 : s.at; break; }
        if (++entries > AMB_GMA_MAX_ENTRIES) goto out;
        if (cstr(&s, name, AMB_GMA_MAX_PATH) < 0) goto stream;
        uint8_t f[12];
        if (!get_n(&s, f, 12)) goto stream;
        long long sz = (long long)le(f, 8);
        if (sz < 0 || sz > r->size || total > r->size - sz) goto out;   /* inside the file, and no overflow in the sum */
        if (nhit < MAX_HITS && path_eq(name, path)) { hit[nhit].rel = total; hit[nhit++].size = sz; }
        if (!(AMB_GMA_MUTANT == 2 && entries == 1)) total += sz;
    }
    if (total > r->size - data_start) goto out;
    if (info) {
        info->version = version;
        info->entries = entries;
        info->data_start = data_start;
        info->data_end = data_start + total;
    }
    rc = AMB_GMA_ABSENT;
    for (int i = 0; i < nhit; i++)
        if (want_ofs < 0 || data_start + hit[i].rel == want_ofs) {
            *ofs = data_start + hit[i].rel;
            *size = hit[i].size;
            rc = AMB_OK;
            break;
        }
    goto out;
stream:
    rc = s.rc ? s.rc : AMB_ERR_FORMAT;
out:
    if (info) info->index_bytes_read = s.read_bytes;
    free(s.buf);
    free(name);
    return rc;
}

static int sub_read(void *ctx, long long ofs, void *buf, int n) {
    const AmbSubReader *c = ctx;
    return c->parent->read(c->parent->ctx, c->base + ofs, buf, n);
}

int amb_sub_reader(AmbReader *out, AmbSubReader *ctx, const AmbReader *parent, long long base, long long size) {
    if (!out || !ctx || !parent || base < 0 || size < 0 || parent->size < 0 || base > parent->size || size > parent->size - base) return AMB_ERR_ARG;
    ctx->parent = parent;
    ctx->base = base;
    out->ctx = ctx;
    out->size = size;
    out->read = sub_read;
    out->cancel = parent->cancel;
    return AMB_OK;
}
