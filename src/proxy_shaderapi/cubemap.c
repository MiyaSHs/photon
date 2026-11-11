/* P4a: see cubemap.h. */
#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cubemap.h"
#include "p6.h"
#include "ambvol.h"
#include "cubework.h"
#include "d3dconst.h"
#include "frames.h"
#include "photon_lightblock.h"
#include "log.h"
#include "vlg.h"

int photon_cube_on;
int photon_cube_blend_wanted, photon_cube_pick_wanted;
static int g_probe;                     /* PHOTON_TEST_CUBEPROBE=1: the class counts' CSV and log lines */
static volatile LONG g_tab_gen;         /* bumped by every cubemap handle created or deleted */

/* ShaderApi030 slots [confirmed: vtable 0x18007b4b8; FUN_180014410 (143) calls +0x798 = 243 with count 1; FUN_180014620 (243) fills the handle array with
 * Texture_t pointers and names them; 94 = FUN_180015800; 0x201 there calls +0x4d8 = 155 with (sampler, handle)] */
enum { SLOT_EXECCMD = 94, SLOT_CREATE1 = 143, SLOT_DELETE = 144, SLOT_BIND = 155, SLOT_CREATEN = 243 };
static void *g_prev[256];

/* --- the handle table -------------------------------------------------------------------------------------------- */
enum { KIND_SAMPLE = 1, KIND_MAPDEFAULT = 2, KIND_ENGDEFAULT = 3, KIND_DEAD = 0x100 };   /* DEAD: deleted, kept to see what the handle becomes */
enum { NMAPS = 64, HSLOTS = 1 << 14 };
typedef struct { uint64_t h; int kind, map, o[3], hdr; } Rec;   /* h 0: empty, ~0: deleted */
static Rec g_tab[HSLOTS];
static int g_tab_n;
static char g_maps[NMAPS][64];
static int g_nmaps;
static SRWLOCK g_tlock = SRWLOCK_INIT;
static volatile LONG64 g_created, g_created_cube, g_deleted_cube, g_reused_cube, g_reused_other, g_table_full, g_creates_by_slot[2], g_remap_map, g_remap_same;
static volatile LONG g_name_logs;

static uint64_t hslot(uint64_t h) { h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33; return h & (HSLOTS - 1); }
/* under g_tlock */
static Rec *tab_find(uint64_t h) {
    for (uint64_t i = hslot(h), k = 0; k < HSLOTS; k++, i = (i + 1) & (HSLOTS - 1)) {
        if (g_tab[i].h == h) return &g_tab[i];
        if (!g_tab[i].h) return NULL;
    }
    return NULL;
}
static Rec *tab_insert(uint64_t h) {
    Rec *r = tab_find(h);
    if (r) return r;
    for (uint64_t i = hslot(h), k = 0; k < HSLOTS; k++, i = (i + 1) & (HSLOTS - 1))
        if (!g_tab[i].h || g_tab[i].h == ~0ull) {
            if (g_tab_n >= HSLOTS * 3 / 4) return NULL;
            g_tab_n += !g_tab[i].h;
            g_tab[i].h = h;
            return &g_tab[i];
        }
    return NULL;
}
static int map_id(const char *name, size_t n) {
    if (n >= sizeof g_maps[0]) n = sizeof g_maps[0] - 1;
    for (int i = 0; i < g_nmaps; i++) if (strlen(g_maps[i]) == n && memcmp(g_maps[i], name, n) == 0) return i;
    if (g_nmaps == NMAPS) return NMAPS - 1;   /* the last one stands for every further map: still never the current one by accident (checked by name) */
    memcpy(g_maps[g_nmaps], name, n);
    g_maps[g_nmaps][n] = 0;
    return g_nmaps++;
}

static int parse_int(const char **s, int *v) {
    const char *p = *s;
    int neg = *p == '-';
    if (neg) p++;
    if (!isdigit((unsigned char)*p)) return 0;
    long x = 0;
    while (isdigit((unsigned char)*p)) { x = x * 10 + (*p - '0'); if (x > 1 << 24) return 0; p++; }
    *v = (int)(neg ? -x : x);
    *s = p;
    return 1;
}

/* name -> kind (0: not a cubemap), map name range, origin, hdr */
static int classify(const char *raw, const char **map, size_t *mlen, int o[3], int *hdr) {
    char s[256];
    size_t n = 0;
    for (; raw[n] && n < sizeof s - 1; n++) s[n] = raw[n] == '\\' ? '/' : (char)tolower((unsigned char)raw[n]);
    s[n] = 0;
    if (n >= 4 && strcmp(s + n - 4, ".vtf") == 0) s[n -= 4] = 0;
    *hdr = n >= 4 && strcmp(s + n - 4, ".hdr") == 0;
    if (*hdr) s[n -= 4] = 0;
    static __thread char keep[256];
    memcpy(keep, s, n + 1);
    if (strcmp(keep, "engine/defaultcubemap") == 0) { *map = keep; *mlen = 0; return KIND_ENGDEFAULT; }
    if (strncmp(keep, "maps/", 5) != 0) return 0;
    const char *m = keep + 5, *slash = strchr(m, '/');
    if (!slash || slash == m) return 0;
    *map = m;
    *mlen = (size_t)(slash - m);
    const char *t = slash + 1;
    if (strcmp(t, "cubemapdefault") == 0) return KIND_MAPDEFAULT;
    if (*t++ != 'c') return 0;
    if (!parse_int(&t, &o[0]) || *t++ != '_' || !parse_int(&t, &o[1]) || *t++ != '_' || !parse_int(&t, &o[2]) || *t) return 0;
    return KIND_SAMPLE;
}

static void note_created(uint64_t h, const char *name, int slot) {
    if (!h || h == ~0ull) return;
    InterlockedIncrement64(&g_created);
    InterlockedIncrement64(&g_creates_by_slot[slot]);
    const char *map = NULL;
    size_t ml = 0;
    int o[3] = { 0, 0, 0 }, hdr = 0;
    int kind = name ? classify(name, &map, &ml, o, &hdr) : 0;
    AcquireSRWLockExclusive(&g_tlock);
    Rec *r = tab_find(h);
    if (r && r->h == h) {
        /* a recycled handle that named a cubemap (deleted or not): it is something else now, or another cubemap */
        int dead = (r->kind & KIND_DEAD) != 0, was = r->kind & ~KIND_DEAD;
        if (!dead) { if (kind) InterlockedIncrement64(&g_reused_cube); else InterlockedIncrement64(&g_reused_other); }
        int other_map = kind && was != KIND_ENGDEFAULT && kind != KIND_ENGDEFAULT && map && r->map >= 0 &&
                        (strlen(g_maps[r->map]) != ml || memcmp(g_maps[r->map], map, ml) != 0);
        if (other_map) InterlockedIncrement64(&g_remap_map);
        else if (kind) InterlockedIncrement64(&g_remap_same);
        if (InterlockedIncrement(&g_name_logs) <= 40)
            photon_log("cubemap: handle %p %s: was %s %s (%d %d %d), now %s%s", (void *)(uintptr_t)h, dead ? "recycled after a delete" : "reused without a delete",
                       was == KIND_SAMPLE ? "a sample of" : "the default of", was == KIND_ENGDEFAULT ? "the engine" : g_maps[r->map], r->o[0], r->o[1], r->o[2],
                       name ? name : "(no name)", other_map ? " (ANOTHER MAP: remapped)" : "");
        if (!kind) r->h = ~0ull;
        InterlockedIncrement(&g_tab_gen);
    }
    if (kind) {
        InterlockedIncrement(&g_tab_gen);
        Rec *w = tab_insert(h);
        if (w) {
            w->kind = kind;
            w->map = kind == KIND_ENGDEFAULT ? -1 : map_id(map, ml);
            memcpy(w->o, o, sizeof o);
            w->hdr = hdr;
        } else InterlockedIncrement64(&g_table_full);
        InterlockedIncrement64(&g_created_cube);
    }
    ReleaseSRWLockExclusive(&g_tlock);
}

static uintptr_t hk_create1(void *self, uintptr_t width, uintptr_t height, uintptr_t depth, int fmt, int mips, int copies, int flags,
                            const char *name, const char *group) {
    PhotonWrap *w = photon_wrap_of(self);
    uintptr_t h = ((uintptr_t (*)(PhotonWrap *, uintptr_t, uintptr_t, uintptr_t, int, int, int, int, const char *, const char *))g_prev[SLOT_CREATE1])(
        w, width, height, depth, fmt, mips, copies, flags, name, group);
    note_created(h, name, 0);
    return h;
}
static void hk_createn(void *self, uint64_t *handles, uintptr_t count, uintptr_t width, int height, int depth, int fmt, int mips, int copies, int flags,
                       const char *name, const char *group) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SLOT_CREATEN, -1)) {   /* T4.6: stock's CreateTexture (143) calls CreateTextures; counted once, as before */
        ((void (*)(void *, uint64_t *, uintptr_t, uintptr_t, int, int, int, int, int, int, const char *, const char *))w->cls->real_vtbl[SLOT_CREATEN])(
            w->real, handles, count, width, height, depth, fmt, mips, copies, flags, name, group);
        return;
    }
    ((void (*)(PhotonWrap *, uint64_t *, uintptr_t, uintptr_t, int, int, int, int, int, int, const char *, const char *))g_prev[SLOT_CREATEN])(
        w, handles, count, width, height, depth, fmt, mips, copies, flags, name, group);
    int n = (int)count;
    for (int i = 0; handles && i < n && i < 4096; i++) note_created(handles[i], name, 1);
}
static void hk_delete(void *self, uint64_t h) {
    PhotonWrap *w = photon_wrap_of(self);
    AcquireSRWLockExclusive(&g_tlock);
    Rec *r = tab_find(h);
    if (r && !(r->kind & KIND_DEAD)) { r->kind |= KIND_DEAD; InterlockedIncrement64(&g_deleted_cube); InterlockedIncrement(&g_tab_gen); }
    ReleaseSRWLockExclusive(&g_tlock);
    ((void (*)(PhotonWrap *, uint64_t))g_prev[SLOT_DELETE])(w, h);
}

/* --- what the draw bound ---------------------------------------------------------------------------------------- */
enum { PATH_CMDBUF = 1, PATH_DIRECT = 2, PATH_STANDARD = 4 };
static __thread uint64_t t_h[2];      /* s1, s8: the last handle bound since the last RenderPass */
static __thread int t_path[2];
static volatile LONG64 g_cmd_bufs, g_cmd_unknown, g_cmd_deep, g_cmd_binds[16], g_direct_binds[16];
static volatile LONG g_unknown_logs;

static int slot_of(int sampler) { return sampler == 1 ? 0 : sampler == 8 ? 1 : -1; }

/* FUN_180015800's opcodes and sizes (in ints): 0 end (or return from a JSR), 1 jump (pointer at +1), 2 JSR (pointer at +1, 3), 0x100/0x101 constants
 * (3 + 4 n), 0x103..0x106 (2), 0x107 (1), 0x108 (3), 0x200 standard texture (3), 0x201 texture handle (sampler, 64-bit handle: 4), 0x400/0x401 shader
 * indices (2), 0x7d4 (3). Anything else stock would loop on forever, so it never occurs; here it ends the parse. */
static void parse_cmds(const int *p, int depth) {
    for (int guard = 0; guard < 65536; guard++) {
        int op = p[0];
        const int *q;
        switch (op) {
        case 0: return;
        case 1: memcpy(&q, p + 1, sizeof q); if (!q) return; p = q; continue;
        case 2:
            memcpy(&q, p + 1, sizeof q);
            if (q && depth < 8) parse_cmds(q, depth + 1);
            else (++g_cmd_deep);
            p += 3;
            continue;
        case 0x100: case 0x101: {
            int n = p[2];
            if (n < 0 || n > 256) { (++g_cmd_unknown); return; }
            p += 3 + 4 * n;
            continue;
        }
        case 0x103: case 0x104: case 0x105: case 0x106: case 0x400: case 0x401: p += 2; continue;
        case 0x107: p += 1; continue;
        case 0x108: case 0x7d4: p += 3; continue;
        case 0x200: {
            int s = slot_of(p[1]);
            if (s >= 0) { t_h[s] = 0; t_path[s] = PATH_STANDARD; }
            p += 3;
            continue;
        }
        case 0x201: {
            uint64_t h;
            memcpy(&h, p + 2, sizeof h);
            if (p[1] >= 0 && p[1] < 16) (++g_cmd_binds[p[1]]);
            int s = slot_of(p[1]);
            if (s >= 0) { t_h[s] = h; t_path[s] = PATH_CMDBUF; }
            p += 4;
            continue;
        }
        default:
            (++g_cmd_unknown);
            if (InterlockedIncrement(&g_unknown_logs) <= 4) photon_log("cubemap: unknown command buffer opcode 0x%x: parse stopped", op);
            return;
        }
    }
}
/* What was bound since the last draw, in order, resolved only when a draw of ours needs it (photon_cube_draw): most draws are not ours
 * and their command buffers are never walked (the walk was the largest piece of Photon's per-draw time on the render thread with
 * 100 ragdolls, analysis/33). A buffer entry is the command buffer pointer (material memory, alive through the draw); a direct entry
 * is a BindTexture of s1 or s8. A full list is resolved at once, so the result is the eager walk's in every case. */
enum { NOPS = 16 };
typedef struct { const int *buf; int s; uint64_t h; } BindOp;
static __thread BindOp t_ops[NOPS];
static __thread int t_nops;
static __thread unsigned long long t_ops_frame;   /* entries never outlive their frame: a command buffer may be per-frame queue memory */
static void ops_resolve(void) {
    for (int i = 0; i < t_nops; i++) {
        if (t_ops[i].buf) parse_cmds(t_ops[i].buf, 0);
        else { t_h[t_ops[i].s] = t_ops[i].h; t_path[t_ops[i].s] = PATH_DIRECT; }
    }
    t_nops = 0;
}
static void ops_push(const int *buf, int s, uint64_t h) {
    unsigned long long f = photon_frame_count();
    if (f != t_ops_frame) { t_ops_frame = f; t_nops = 0; }
    if (t_nops == NOPS) ops_resolve();
    t_ops[t_nops].buf = buf; t_ops[t_nops].s = s; t_ops[t_nops].h = h;
    t_nops++;
}
static void hk_execcmd(void *self, const int *buf) {
    PhotonWrap *w = photon_wrap_of(self);
    if (PHOTON_INTERNAL(w, SLOT_EXECCMD, -1)) { ((void (*)(void *, const int *))w->cls->real_vtbl[SLOT_EXECCMD])(w->real, buf); return; }
    (++g_cmd_bufs);
    if (buf) ops_push(buf, 0, 0);
    ((void (*)(PhotonWrap *, const int *))g_prev[SLOT_EXECCMD])(w, buf);
}
static void hk_bind(void *self, uintptr_t sampler, uint64_t h) {
    PhotonWrap *w = photon_wrap_of(self);
    /* T4.6: stock's own BindTexture calls (ExecuteCommandBuffer, slot 287) and the BindStandardTexture (slot 54) tail jump stay unseen */
    if (PHOTON_INTERNAL(w, SLOT_BIND, 54)) { ((void (*)(void *, uintptr_t, uint64_t))w->cls->real_vtbl[SLOT_BIND])(w->real, sampler, h); return; }
    int sm = (int)sampler;
    if (sm >= 0 && sm < 16) (++g_direct_binds[sm]);
    int s = slot_of(sm);
    if (s >= 0) ops_push(NULL, s, h);
    ((void (*)(PhotonWrap *, uintptr_t, uint64_t))g_prev[SLOT_BIND])(w, sampler, h);
}

/* --- per-draw accounting (draw thread) ------------------------------------------------------------------------------- */
enum { C_NONE, C_STANDARD, C_UNKNOWN, C_SAMPLE, C_STALE, C_NOINDEX, C_MAPDEFAULT, C_ENGDEFAULT, C_OTHERDEFAULT, NCLASS };
static const char *const k_class[NCLASS] = { "none bound", "standard texture", "unknown handle", "sample", "ANOTHER MAP'S SAMPLE", "sample not in lump 42",
                                             "map default", "engine default", "ANOTHER MAP'S DEFAULT" };
enum { NPAIR = 3, NROUTE = 3 };
static const char *const k_pair[NPAIR] = { "vlg", "bump", "skin" };
static const char *const k_route[NROUTE] = { "override", "engine", "static" };
static long long g_cls[NPAIR][NROUTE][NCLASS], g_path_n[NPAIR][3];
static long long g_tab_hits, g_tab_same, g_tab_hidden_engine, g_tab_differs, g_tab_none;
static long long g_samples_hist[AMB_CUBES_MAX];
static int g_cur_map = -2;              /* map id of the level in game; -2 none */
static char g_cur_level[MAX_PATH];
static SRWLOCK g_wlock = SRWLOCK_INIT;  /* cube_work_* on the draw thread vs shutdown on main */
static int g_work_ok, g_work_down;
static FILE *g_csv;
static long g_csv_bytes, g_csv_rows;
enum { DSLOTS = 1 << 16, CSV_MAX = 24 << 20 };
static struct { float o[3]; uint64_t h; int pair; } g_seen[DSLOTS];
static long g_seen_n, g_seen_full;
static double g_qpc_ms;

static void csv_open(void) {
    char dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    snprintf(path, sizeof path, "%s\\photon_cubeprobe.csv", dir);
    g_csv = fopen(path, "w");
    if (g_csv) fprintf(g_csv, "frame,t_ms,level,pair,route,x,y,z,class,map,sx,sy,sz,sample,hdr,table,ta,tb,tflags,fa,fb,fon\n");
}

/* 1 the first time this (pair, origin, handle, the A and blend bound after P4b) is seen since the last map change */
static int first_sight(const float o[3], uint64_t h, int pair) {
    uint32_t k = 2166136261u;
    const uint8_t *b = (const uint8_t *)o;
    for (int i = 0; i < 12; i++) k = (k ^ b[i]) * 16777619u;
    k ^= (uint32_t)(h >> 4) * 2654435761u ^ (uint32_t)pair;
    for (int probe = 0; probe < 64; probe++) {
        uint32_t i = (k + (uint32_t)probe) & (DSLOTS - 1);
        if (g_seen[i].pair == 0) {
            if (g_seen_n >= DSLOTS / 2) { g_seen_full++; return 0; }
            memcpy(g_seen[i].o, o, 12);
            g_seen[i].h = h;
            g_seen[i].pair = pair + 1;
            g_seen_n++;
            return 1;
        }
        if (g_seen[i].pair == pair + 1 && g_seen[i].h == h && memcmp(g_seen[i].o, o, 12) == 0) return 0;
    }
    g_seen_full++;
    return 0;
}

/* --- P4b: binding the visible A and the second sample B (draw thread) ---------------------------------------------------- */
static uint64_t g_samp_h[AMB_CUBES_MAX];        /* the current map's sample index -> its texture handle */
static LONG g_samp_gen = -1;
static int g_samp_map = -3;
static float g_cube_dev[2][4];                  /* c57, c58 as last written */
static int g_cube_valid;
static int g_last_a = -1, g_last_b = -1, g_last_on;   /* the last draw's result, for the probe's CSV */
static long long g_f_fine, g_f_coarse, g_f_novisible, g_f_p6_nearest;
static PhotonCubeCtx g_cctx;                    /* T4.5 replay: the last suppressed draw's pick context (photon_cube_replay_take) */
static int g_cube_valid_tex;                     /* the learned handle -> texture table is valid (cleared with the device) */
static long long g_f_draws, g_f_outside, g_f_rebind, g_f_kept, g_f_blend, g_f_noblend_unlit, g_f_noblend_nob, g_f_nohandle, g_f_writes, g_f_fail;

/* invalid after a device Reset or a snapshot clear (vlg.c), so the next draw of ours writes c57/c58 again */
void photon_cube_device_lost(void) { g_cube_valid = 0; g_cube_valid_tex = 0; }

static void samp_refresh(void) {
    LONG gen = g_tab_gen;
    if (gen == g_samp_gen && g_samp_map == g_cur_map) return;
    if (!cube_work_map_ready()) return;   /* try again: the indices come from the loaded map */
    memset(g_samp_h, 0, sizeof g_samp_h);
    AcquireSRWLockShared(&g_tlock);
    for (int i = 0; i < HSLOTS; i++) {
        const Rec *r = &g_tab[i];
        if (!r->h || r->h == ~0ull || r->kind != KIND_SAMPLE || r->map != g_cur_map) continue;
        int idx = cube_work_sample(r->o, NULL);
        if (idx >= 0) g_samp_h[idx] = r->h;
    }
    ReleaseSRWLockShared(&g_tlock);
    g_samp_gen = gen;
    g_samp_map = g_cur_map;
}

static void cube_bind(PhotonWrap *w, int sampler, uint64_t h) {
    ((void (*)(PhotonWrap *, uintptr_t, uint64_t))g_prev[SLOT_BIND])(w, (uintptr_t)sampler, h);
}

static void cube_consts(const float plane[4], int on) {
    const float v[2][4] = { { plane[0], plane[1], plane[2], plane[3] }, { on ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f } };
    if (g_cube_valid && (on ? memcmp(v, g_cube_dev, sizeof v) == 0 : g_cube_dev[1][0] == 0.0f)) return;
    if (photon_d3d_set_ps_const(PHOTON_LB_CUBE_PLANE, &v[0][0], 2) == S_OK) {
        memcpy(g_cube_dev, v, sizeof v);
        g_cube_valid = 1;
        g_f_writes++;
    } else { g_cube_valid = 0; g_f_fail++; }
}

/* B's weight as one plane: 0.5 on the bisector of A and B, 0 and 1 at W / 2 on either side (photon_cubeblend_ps.h) */
static void plane_of(const int a[3], const int b[3], float out[4]) {
    double d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    double width = 0.5 * l < 64.0 ? 64.0 : 0.5 * l > 512.0 ? 512.0 : 0.5 * l, m = 0.0;
    for (int k = 0; k < 3; k++) { d[k] /= l; m += 0.5 * (a[k] + b[k]) * d[k]; }
    for (int k = 0; k < 3; k++) out[k] = (float)(d[k] / width);
    out[3] = (float)(0.5 - m / width);
}

/* cls / sample: what the engine bound (the classes below); lit: c32.x is on for this draw (skin's s5 is free only then) */
static void cube_feature(PhotonWrap *w, int pair, int cls, int sample, uint64_t bound, const float origin[3], int lit, int blend, int pick) {
    const int es = pair == 2 ? 8 : 1, bs = pair == 2 ? 5 : 13;
    float plane[4] = { 0, 0, 0, 0 };
    int on = 0;
    uint64_t lha = 0, lhb = 0;   /* the handles bound (or kept) on es / bs by this draw's pick, for photon_cube_learn */
    g_f_draws++;
    g_last_a = cls == C_SAMPLE ? sample : -1;
    g_last_b = -1;
    /* only a cubemap the engine chose for this draw is ours to change: a material's own $envmap texture stays */
    int eligible = cls == C_SAMPLE || cls == C_MAPDEFAULT || cls == C_ENGDEFAULT;
    uint32_t e = 0;
    AcquireSRWLockShared(&g_wlock);
    int lv = eligible && g_work_ok && !g_work_down ? cube_work_lookup(origin, &e) : 0;
    /* no visible sample among the tested ones (A_FALLBACK, e.g. the 3D skybox, which has no env_cubemap of its own): the engine's pick stays */
    int have = lv && !(e & (AMB_PICK_SOLID | AMB_PICK_EMPTY | AMB_PICK_A_FALLBACK));
    if (eligible && !lv) g_f_outside++;
    if (lv && !have) g_f_novisible++;
    if (lv == 1) g_f_fine++; else if (lv == 2) g_f_coarse++;
    /* P6: the engine skipped its BindLocalCubemap, so the bound handle is the previous draw's. Where no sample is visible, bind what the engine
     * would have: A is then the nearest sample by distance (A_FALLBACK, or SOLID), from the voxel's probe point rather than the exact origin. */
    if (!have && lv && pick && photon_p6_suppressed() && !(e & AMB_PICK_EMPTY)) {
        samp_refresh();
        int ta = AMB_PICK_A(e);
        if (ta < AMB_CUBES_MAX && g_samp_h[ta]) {
            if (g_samp_h[ta] != bound) { cube_bind(w, es, g_samp_h[ta]); g_f_p6_nearest++; }
            g_last_a = ta;
            lha = g_samp_h[ta];
        } else g_f_nohandle++;
    }
    if (have) {
        samp_refresh();
        int ta = AMB_PICK_A(e), tb = AMB_PICK_B(e), a = cls == C_SAMPLE ? sample : -1;
        if (pick && ta < AMB_CUBES_MAX && g_samp_h[ta]) {
            if (g_samp_h[ta] != bound) { cube_bind(w, es, g_samp_h[ta]); g_f_rebind++; } else g_f_kept++;
            a = ta;
            lha = g_samp_h[ta];
        } else if (pick) g_f_nohandle++;
        if (blend && a >= 0) {
            int b = a == ta ? tb : ta;
            int ao[3], bo[3];
            if (pair == 2 && !lit) g_f_noblend_unlit++;
            /* B hidden too (B_FALLBACK): blending towards it would bring the wrong room back */
            else if ((e & AMB_PICK_B_FALLBACK) || b >= AMB_CUBES_MAX || b == a || !g_samp_h[b] || !cube_work_origin(a, ao) || !cube_work_origin(b, bo))
                g_f_noblend_nob++;
            else {
                cube_bind(w, bs, g_samp_h[b]);
                lhb = g_samp_h[b];
                plane_of(ao, bo, plane);
                on = 1;
                g_f_blend++;
            }
        }
        g_last_a = a;
        g_last_b = on ? (a == ta ? tb : ta) : -1;
    }
    ReleaseSRWLockShared(&g_wlock);
    g_last_on = on;
    if (photon_cube_blend_wanted) cube_consts(plane, on);
    if (photon_p6_suppressed()) {
        g_cctx.ok = 1; g_cctx.pair = pair; g_cctx.eligible = eligible; g_cctx.blend = blend; g_cctx.pick = pick; g_cctx.lit = lit;
        g_cctx.ha = lha; g_cctx.hb = lhb;
    }
}

/* --- T4.5 stage 2b: the pick of a replayed copy (cubemap.h) --------------------------------------------------------------------------------
 * Handle -> D3D texture, learned from recorded draws (the device's texture on the envmap sampler is the handle bound there). Valid for one
 * g_tab_gen (any DeleteTexture bumps it) and until a device loss. Draw thread only. */
enum { TEXL = 256 };
static struct { uint64_t h; void *tex; } g_texl[TEXL];
static LONG g_texl_gen = -1;
static long long g_rp_calls, g_rp_noctx, g_rp_inel, g_rp_nopick, g_rp_copies, g_rp_unknown, g_rp_nohandle, g_rp_changed, g_rp_learned;
static void texl_check(void) {
    LONG gen = g_tab_gen;
    if (gen != g_texl_gen || !g_cube_valid_tex) { memset(g_texl, 0, sizeof g_texl); g_texl_gen = gen; g_cube_valid_tex = 1; }
}
static size_t texl_slot(uint64_t h) { return (size_t)hslot(h) & (TEXL - 1); }
static void *texl_get(uint64_t h) {
    size_t i = texl_slot(h);
    for (int n = 0; n < 8; n++, i = (i + 1) & (TEXL - 1)) { if (g_texl[i].h == h) return g_texl[i].tex; if (!g_texl[i].h) return NULL; }
    return NULL;
}
static void texl_put(uint64_t h, void *tex) {
    if (!h || !tex) return;
    size_t i = texl_slot(h);
    for (int n = 0; n < 8; n++, i = (i + 1) & (TEXL - 1))
        if (!g_texl[i].h || g_texl[i].h == h) { if (!g_texl[i].h) g_rp_learned++; g_texl[i].h = h; g_texl[i].tex = tex; return; }
}

void photon_cube_replay_take(PhotonCubeCtx *ctx) { *ctx = g_cctx; g_cctx.ok = 0; }
void photon_cube_learn(const PhotonCubeCtx *c, void *const tex[20]) {
    if (!c->ok) return;
    texl_check();
    const int es = c->pair == 2 ? 8 : 1, bs = c->pair == 2 ? 5 : 13;
    texl_put(c->ha, tex[es]);
    texl_put(c->hb, tex[bs]);
}
int photon_cube_replay(const PhotonCubeCtx *c, const float origin[3], int *ntex, int slot[2], void *tex[2], float psc[65][4]) {
    *ntex = 0;
    g_rp_calls++;
    if (!c->ok) { g_rp_noctx++; return 1; }
    if (!c->eligible) { g_rp_inel++; return 1; }   /* the first copy's cubemap is not ours to change: the copies' neither */
    if (!c->pick) { g_rp_nopick++; return 0; }                 /* the engine's own pick: under P6 the previous draw's handle, nothing to redo here */
    g_rp_copies++;
    texl_check();
    const int es = c->pair == 2 ? 8 : 1, bs = c->pair == 2 ? 5 : 13;
    uint32_t e = 0;
    uint64_t ha = 0, hb = 0;
    float plane[4] = { 0, 0, 0, 0 };
    int on = 0, ok = 1;
    AcquireSRWLockShared(&g_wlock);
    int lv = g_work_ok && !g_work_down ? cube_work_lookup(origin, &e) : 0;
    int have = lv && !(e & (AMB_PICK_SOLID | AMB_PICK_EMPTY | AMB_PICK_A_FALLBACK));
    if (lv) samp_refresh();
    int ta = AMB_PICK_A(e), tb = AMB_PICK_B(e);
    if (lv && !(e & AMB_PICK_EMPTY)) {   /* cube_feature: the visible A, or with none visible the nearest (P6) */
        if (ta < AMB_CUBES_MAX && g_samp_h[ta]) ha = g_samp_h[ta];
        else { ok = 0; g_rp_nohandle++; }
    }
    if (ok && have && c->blend) {
        int b = tb;
        int ao[3], bo[3];
        if (!(c->pair == 2 && !c->lit) && !(e & AMB_PICK_B_FALLBACK) && b < AMB_CUBES_MAX && b != ta && g_samp_h[b] && cube_work_origin(ta, ao) &&
            cube_work_origin(b, bo)) {
            hb = g_samp_h[b];
            plane_of(ao, bo, plane);
            on = 1;
        }
    }
    ReleaseSRWLockShared(&g_wlock);
    if (!ok) return 0;
    void *ta_tex = ha ? texl_get(ha) : NULL, *tb_tex = hb ? texl_get(hb) : NULL;
    if ((ha && !ta_tex) || (hb && !tb_tex)) { g_rp_unknown++; return 0; }
    if (ha) { slot[*ntex] = es; tex[(*ntex)++] = ta_tex; }
    if (hb) { slot[*ntex] = bs; tex[(*ntex)++] = tb_tex; }
    if (ha != c->ha || hb != c->hb) g_rp_changed++;
    if (photon_cube_blend_wanted) {
        if (on) memcpy(psc[PHOTON_LB_CUBE_PLANE], plane, sizeof plane);
        psc[PHOTON_LB_CUBE_PLANE + 1][0] = on ? 1.0f : 0.0f;
        psc[PHOTON_LB_CUBE_PLANE + 1][1] = psc[PHOTON_LB_CUBE_PLANE + 1][2] = psc[PHOTON_LB_CUBE_PLANE + 1][3] = 0.0f;
    }
    return 1;
}

void photon_cube_draw(PhotonWrap *w, int snap, int route, int array, const float origin[3], int lit, int blend, int pick) {
    if (!photon_cube_on || !(snap & PHOTON_VLG_SNAP_OURS)) { t_nops = 0; t_h[0] = t_h[1] = 0; t_path[0] = t_path[1] = 0; return; }
    if (t_ops_frame != photon_frame_count()) t_nops = 0;
    ops_resolve();
    int pair = (snap & PHOTON_VLG_SNAP_BUMP) ? 1 : (snap & PHOTON_VLG_SNAP_SKIN) ? 2 : 0;
    int s = pair == 2 ? 1 : 0;
    int rt = route == 0 ? 0 : array ? 2 : 1;
    uint64_t h = t_h[s];
    int path = t_path[s];
    t_h[0] = t_h[1] = 0;
    t_path[0] = t_path[1] = 0;
    if (path & PATH_CMDBUF) g_path_n[pair][0]++;
    else if (path & PATH_DIRECT) g_path_n[pair][1]++;
    else if (path & PATH_STANDARD) g_path_n[pair][2]++;
    int cls = C_NONE, sample = -1, hdr = 0, o[3] = { 0, 0, 0 }, map = -1;
    if (path == PATH_STANDARD) cls = C_STANDARD;
    else if (path) {
        AcquireSRWLockShared(&g_tlock);
        Rec *r = tab_find(h);
        Rec rec = r ? *r : (Rec){ 0, 0, 0, { 0, 0, 0 }, 0 };
        ReleaseSRWLockShared(&g_tlock);
        if (!r || (rec.kind & KIND_DEAD)) cls = C_UNKNOWN;
        else {
            map = rec.map;
            hdr = rec.hdr;
            memcpy(o, rec.o, sizeof o);
            if (rec.kind == KIND_ENGDEFAULT) cls = C_ENGDEFAULT;
            else if (rec.map != g_cur_map) cls = rec.kind == KIND_SAMPLE ? C_STALE : C_OTHERDEFAULT;
            else if (rec.kind == KIND_MAPDEFAULT) cls = C_MAPDEFAULT;
            else {
                AcquireSRWLockShared(&g_wlock);
                int n = 0;
                sample = g_work_ok && !g_work_down ? cube_work_sample(o, &n) : -1;
                ReleaseSRWLockShared(&g_wlock);
                cls = sample >= 0 ? C_SAMPLE : C_NOINDEX;
                if (sample >= 0) g_samples_hist[sample]++;
            }
        }
    }
    g_cls[pair][rt][cls]++;
    if (photon_cube_blend_wanted || photon_cube_pick_wanted) cube_feature(w, pair, cls, sample, h, origin, lit, blend, pick);
    if (!g_probe) return;
    if (cls == C_STALE || cls == C_OTHERDEFAULT) {
        static int logs;
        if (logs++ < 16)
            photon_log("cubemap: ALIASING? frame %llu: a %s draw bound %s's %s (%d %d %d) while %s is loaded", (unsigned long long)photon_frame_count(), k_pair[pair],
                       map >= 0 ? g_maps[map] : "?", cls == C_STALE ? "sample" : "default", o[0], o[1], o[2], g_cur_level);
    }
    if (cls == C_NONE || cls == C_STANDARD) return;
    /* the pick table at the same lighting origin */
    uint32_t e = 0;
    int have = 0;
    AcquireSRWLockShared(&g_wlock);
    if (g_work_ok && !g_work_down) have = cube_work_lookup(origin, &e);
    ReleaseSRWLockShared(&g_wlock);
    if (have && cls == C_SAMPLE) {
        g_tab_hits++;
        if (AMB_PICK_A(e) == sample) g_tab_same++;
        else {
            g_tab_differs++;
            if (e & AMB_PICK_NEAR_HIDDEN) g_tab_hidden_engine++;
        }
    } else if (cls == C_SAMPLE) g_tab_none++;
    if (g_csv && g_csv_bytes < CSV_MAX && first_sight(origin, h ^ ((uint64_t)(g_last_a + 1) << 48) ^ ((uint64_t)(g_last_on) << 60) ^ ((uint64_t)(have) << 62), pair)) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_csv_bytes += fprintf(g_csv, "%llu,%.1f,%s,%s,%s,%.3f,%.3f,%.3f,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,0x%x,%d,%d,%d\n", (unsigned long long)photon_frame_count(),
                               (double)t.QuadPart * g_qpc_ms, g_cur_level, k_pair[pair], k_route[rt], origin[0], origin[1], origin[2], cls,
                               map >= 0 ? g_maps[map] : "-", o[0], o[1], o[2], sample, hdr, have, have ? AMB_PICK_A(e) : -1, have ? AMB_PICK_B(e) : -1,
                               have ? e >> 22 : 0, g_last_a, g_last_b, g_last_on);
        g_csv_rows++;
    }
}

/* --- frames, map changes, shutdown ------------------------------------------------------------------------------------- */
static void log_handles(const char *why) {
    int per[NMAPS + 1][3];
    memset(per, 0, sizeof per);
    AcquireSRWLockShared(&g_tlock);
    for (int i = 0; i < HSLOTS; i++) {
        const Rec *r = &g_tab[i];
        if (!r->h || r->h == ~0ull || (r->kind & KIND_DEAD)) continue;
        int m = r->map < 0 ? NMAPS : r->map;
        per[m][r->kind - 1]++;
    }
    ReleaseSRWLockShared(&g_tlock);
    for (int m = 0; m <= NMAPS; m++) {
        if (!per[m][0] && !per[m][1] && !per[m][2]) continue;
        photon_log("cubemap: handles at %s: %s: %d samples, %d map defaults, %d engine defaults", why, m == NMAPS ? "engine" : g_maps[m], per[m][0], per[m][1],
                   per[m][2]);
    }
}

void photon_cube_frame(void) {
    if (!photon_cube_on) return;
    int in_game = 0;
    char level[MAX_PATH];
    double cam[3];
    const AmbWorkFiles *files = NULL;
    int r = photon_eng_facts(&in_game, level, sizeof level, cam, &files);
    if (r <= 0) return;
    AcquireSRWLockExclusive(&g_wlock);
    if (g_work_down) { ReleaseSRWLockExclusive(&g_wlock); return; }
    if (!g_work_ok) {
        g_work_ok = cube_work_init(files) == AMB_OK;
        photon_log("cubemap: pick table worker %s", g_work_ok ? "ready" : "failed to initialise");
    }
    if (!in_game) level[0] = 0;
    if (strcmp(level, g_cur_level) != 0) {
        char name[64] = "";
        const char *b = strrchr(level, '/');
        b = b ? b + 1 : level;
        size_t n = strcspn(b, ".");
        if (n >= sizeof name) n = sizeof name - 1;
        for (size_t i = 0; i < n; i++) name[i] = (char)tolower((unsigned char)b[i]);
        name[n] = 0;
        photon_log("cubemap: level %s -> %s (frame %llu)", g_cur_level[0] ? g_cur_level : "none", level[0] ? level : "none", (unsigned long long)photon_frame_count());
        log_handles(level[0] ? "the level change" : "leaving the level");
        AcquireSRWLockExclusive(&g_tlock);
        g_cur_map = level[0] ? map_id(name, n) : -2;
        ReleaseSRWLockExclusive(&g_tlock);
        snprintf(g_cur_level, sizeof g_cur_level, "%s", level);
        memset(g_seen, 0, sizeof g_seen);
        g_seen_n = 0;
        if (g_work_ok) cube_work_summary("level change");
    }
    if (g_work_ok) cube_work_frame(in_game && level[0], level, cam);
    ReleaseSRWLockExclusive(&g_wlock);
    if (g_csv && photon_frame_count() % 120 == 0) fflush(g_csv);
}

void photon_cube_shutdown(const char *why) {
    if (!photon_cube_on) return;
    AcquireSRWLockExclusive(&g_wlock);
    if (g_work_ok && !g_work_down) cube_work_shutdown(why);
    g_work_down = 1;
    if (g_csv) fflush(g_csv);
    ReleaseSRWLockExclusive(&g_wlock);
}

void photon_cube_setup(void) {
    char e[8];
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_ms = 1e3 / (double)f.QuadPart;
    g_probe = GetEnvironmentVariableA("PHOTON_TEST_CUBEPROBE", e, sizeof e) > 0 && e[0] == '1';
    photon_vlg_cube_wanted(&photon_cube_blend_wanted, &photon_cube_pick_wanted);
    photon_cube_on = g_probe || photon_cube_blend_wanted || photon_cube_pick_wanted;
    if (g_probe) {
        csv_open();
        photon_log("cubemap: TEST PHOTON_TEST_CUBEPROBE=1: texture names, envmap binds and the pick table are observed (csv %s)", g_csv ? "open" : "off");
    }
    if (photon_cube_blend_wanted || photon_cube_pick_wanted)
        photon_log("cubemap: photon_cubemap_blend %d, photon_cubemap_pick %d at load: the pick table runs; B on s13 (vlg, bump) and s5 (skin)",
                   photon_cube_blend_wanted, photon_cube_pick_wanted);
}

void photon_cube_install(PhotonWrapClass *cls) {
    if (!photon_cube_on || strcmp(cls->name, "ShaderApi030") != 0) return;
    static const int slots[] = { SLOT_EXECCMD, SLOT_CREATE1, SLOT_DELETE, SLOT_BIND, SLOT_CREATEN };
    static void *const fns[] = { (void *)hk_execcmd, (void *)hk_create1, (void *)hk_delete, (void *)hk_bind, (void *)hk_createn };
    for (int i = 0; i < 5; i++) {
        if ((size_t)slots[i] >= cls->nslots) { photon_log("cubemap: ShaderApi030 has %zu slots: probe off", cls->nslots); photon_cube_on = 0; return; }
    }
    for (int i = 0; i < 5; i++) {
        g_prev[slots[i]] = cls->thunk_vtbl[slots[i]];
        photon_wrap_set_hook(cls, (size_t)slots[i], fns[i]);
    }
    photon_log("cubemap: ShaderApi030 slots 94/143/144/155/243 hooked");
}

void photon_cube_dump(const char *why) {
    if (!photon_cube_on) return;
    if (g_rp_calls)
        photon_log("cubemap replay (%s): copies %lld (no pick context %lld, not eligible %lld, pick off %lld), picked %lld, a cubemap other than the first copy's %lld, texture not learned yet %lld, no handle %lld;"
                   " textures learned %lld", why, g_rp_calls, g_rp_noctx, g_rp_inel, g_rp_nopick, g_rp_copies, g_rp_changed, g_rp_unknown, g_rp_nohandle, g_rp_learned);
    photon_log("cubemap summary (%s): textures created %lld (slot 143 %lld, slot 243 %lld), cubemap names %lld, deleted %lld, handles reused from a cubemap"
               " for a cubemap %lld / for something else %lld, deleted cubemap handles named again as another map's cubemap %lld / the same map's %lld, table full %lld; command buffers %lld (unknown opcode %lld, nesting too deep %lld);"
               " CSV rows %ld (%s)", why, (long long)g_created, (long long)g_creates_by_slot[0], (long long)g_creates_by_slot[1], (long long)g_created_cube,
               (long long)g_deleted_cube, (long long)g_reused_cube, (long long)g_reused_other, (long long)g_remap_map, (long long)g_remap_same, (long long)g_table_full, (long long)g_cmd_bufs,
               (long long)g_cmd_unknown, (long long)g_cmd_deep, g_csv_rows, g_seen_full ? "dedupe table full" : "complete");
    char line[512];
    int n = 0;
    for (int s = 0; s < 16; s++) n += snprintf(line + n, sizeof line - (size_t)n, " s%d %lld/%lld", s, (long long)g_cmd_binds[s], (long long)g_direct_binds[s]);
    photon_log("cubemap: texture handle binds by sampler, command buffer / direct (all draws):%s", line);
    for (int p = 0; p < NPAIR; p++) {
        photon_log("cubemap: %s draws: envmap sampler bound through the command buffer %lld, directly %lld, a standard texture %lld", k_pair[p], g_path_n[p][0],
                   g_path_n[p][1], g_path_n[p][2]);
        for (int r = 0; r < NROUTE; r++) {
            long long tot = 0;
            for (int c = 0; c < NCLASS; c++) tot += g_cls[p][r][c];
            if (!tot) continue;
            n = 0;
            for (int c = 0; c < NCLASS; c++)
                if (g_cls[p][r][c]) n += snprintf(line + n, sizeof line - (size_t)n, "%s%s %lld", n ? ", " : "", k_class[c], g_cls[p][r][c]);
            photon_log("cubemap:   %s, %s route, %lld draws: %s", k_pair[p], k_route[r], tot, line);
        }
    }
    n = 0;
    for (int i = 0; i < AMB_CUBES_MAX && n < (int)sizeof line - 24; i++) if (g_samples_hist[i]) n += snprintf(line + n, sizeof line - (size_t)n, " %d:%lld", i, g_samples_hist[i]);
    photon_log("cubemap: draws per sample index:%s", n ? line : " none");
    if (photon_cube_blend_wanted || photon_cube_pick_wanted)
        photon_log("cubemap: P4c: eligible draws (the engine's cubemap) in cascade 0 %lld, in the coarse level %lld, in neither %lld; with no visible"
                   " sample (the engine's pick kept) %lld; P6 draws given the nearest sample there %lld", g_f_fine, g_f_coarse, g_f_outside, g_f_novisible,
                   g_f_p6_nearest);
    if (photon_cube_blend_wanted || photon_cube_pick_wanted)
        photon_log("cubemap: P4b: draws of ours %lld; outside the table %lld; pick: rebound %lld, already visible %lld, no handle %lld; blended %lld, not: skin"
                   " unlit %lld, no B %lld; c57/c58 writes %lld (failed %lld)", g_f_draws, g_f_outside, g_f_rebind, g_f_kept, g_f_nohandle, g_f_blend,
                   g_f_noblend_unlit, g_f_noblend_nob, g_f_writes, g_f_fail);
    photon_log("cubemap: pick table at the draws' lighting origins: in the table %lld (A = the engine's sample %lld, differs %lld, of those with the engine's"
               " nearest hidden %lld), outside the table %lld", g_tab_hits, g_tab_same, g_tab_differs, g_tab_hidden_engine, g_tab_none);
    log_handles(why);
    if (g_work_ok) cube_work_summary(why);
    if (g_csv) fflush(g_csv);
}
