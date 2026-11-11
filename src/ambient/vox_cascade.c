/* PT-RT voxel cascades (analysis/43): the voxel world as camera-centred levels, so memory is fixed by the window, not by the map.
 * Level l has voxels of 8 << l units in a window of VOXC_WIN^3 bricks of 8^3 voxels, placed (brick-aligned) around the camera; level l
 * covers twice the distance of level l-1 and contains its window. A level is rebuilt when the camera moves it by a brick: every triangle
 * whose box touches the window is clipped to the window's box and sampled at half a voxel (huge ground triangles cost only their part in
 * the window). A ray walks level 0 first, then from its exit point on through level 1, and so on; past the last level it escapes. */
#include "vox_world.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { NV = VOX_BRICK * VOX_BRICK * VOX_BRICK, NB = VOXC_WIN * VOXC_WIN * VOXC_WIN };
typedef struct { float alb[NV][3], n[NV][3]; uint16_t cnt[NV]; } CBrick;
typedef struct { float v[3][3], col[3], ref[3], lo[3], hi[3]; } Tri;
typedef struct {
    double vs, bu;          /* voxel size, brick size in units */
    int org[3];             /* the window's first brick (in this level's bricks) */
    int built;
    CBrick *b[NB];          /* NULL: empty brick */
    long bricks, voxels;
} Level;
typedef struct { Tri *tri; long ntri; } IModel;   /* a model's triangles in its own space (lo / hi: model space) */
typedef struct { int model; float X[3][4], lo[3], hi[3]; } Inst;   /* a placement: world = X . local; lo / hi its world bounds */
struct VoxCascades {
    Tri *tri; long ntri;
    IModel *mod; int nmod;
    Inst *inst; long ninst;
    Level lv[VOXC_LEVELS];
    long faces, disps, rebuilds;
};

static void collect(void *ctx, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]) {
    VoxCascades *vc = ctx;
    if ((vc->ntri & (vc->ntri - 1)) == 0) {   /* grow at powers of two */
        Tri *t = realloc(vc->tri, sizeof *t * (size_t)(vc->ntri ? vc->ntri * 2 : 1024));
        if (!t) return;
        vc->tri = t;
    }
    Tri *t = &vc->tri[vc->ntri++];
    for (int k = 0; k < 3; k++) {
        t->v[0][k] = (float)a[k]; t->v[1][k] = (float)b[k]; t->v[2][k] = (float)c[k];
        t->col[k] = col[k]; t->ref[k] = ref[k];
        t->lo[k] = fminf(t->v[0][k], fminf(t->v[1][k], t->v[2][k]));
        t->hi[k] = fmaxf(t->v[0][k], fmaxf(t->v[1][k], t->v[2][k]));
    }
}

void vox_cascades_add(VoxCascades *vc, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]) {
    collect(vc, a, b, c, col, ref);
}

int vox_cascades_add_model(VoxCascades *vc, const float (*v)[3][3], const float (*col)[3], const float (*ref)[3], long n) {
    IModel *m = realloc(vc->mod, sizeof *m * (size_t)(vc->nmod + 1));
    if (!m) return -1;
    vc->mod = m;
    IModel *M = &vc->mod[vc->nmod];
    M->tri = malloc(sizeof *M->tri * (size_t)(n ? n : 1)); M->ntri = n;
    if (!M->tri) return -1;
    for (long t = 0; t < n; t++) {
        Tri *T = &M->tri[t];
        memcpy(T->v, v[t], sizeof T->v); memcpy(T->col, col[t], sizeof T->col); memcpy(T->ref, ref[t], sizeof T->ref);
    }
    return vc->nmod++;
}
int vox_cascades_add_instance(VoxCascades *vc, int model, const double X[3][4]) {
    if (model < 0 || model >= vc->nmod) return 0;
    if ((vc->ninst & (vc->ninst - 1)) == 0) {
        Inst *a = realloc(vc->inst, sizeof *a * (size_t)(vc->ninst ? vc->ninst * 2 : 256));
        if (!a) return 0;
        vc->inst = a;
    }
    Inst *I = &vc->inst[vc->ninst++];
    I->model = model;
    for (int a = 0; a < 3; a++) for (int b = 0; b < 4; b++) I->X[a][b] = (float)X[a][b];
    const IModel *M = &vc->mod[model];
    for (int a = 0; a < 3; a++) { I->lo[a] = 1e30f; I->hi[a] = -1e30f; }
    for (long t = 0; t < M->ntri; t++)
        for (int c = 0; c < 3; c++)
            for (int a = 0; a < 3; a++) {
                float w = I->X[a][0] * M->tri[t].v[c][0] + I->X[a][1] * M->tri[t].v[c][1] + I->X[a][2] * M->tri[t].v[c][2] + I->X[a][3];
                if (w < I->lo[a]) I->lo[a] = w;
                if (w > I->hi[a]) I->hi[a] = w;
            }
    return 1;
}

int vox_cascades_new(const uint8_t *bsp, size_t size, const SunMap *sm, const float (*colour)[3], int ncolour, VoxCascades **out) {
    *out = NULL;
    VoxCascades *vc = calloc(1, sizeof *vc);
    if (!vc) return 2;
    int rc = vox_triangles(bsp, size, sm, colour, ncolour, collect, vc, &vc->faces, &vc->disps);
    if (rc) { vox_cascades_free(vc); return rc; }
    for (int l = 0; l < VOXC_LEVELS; l++) { vc->lv[l].vs = (double)(VOX_SIZE << l); vc->lv[l].bu = vc->lv[l].vs * VOX_BRICK; }
    *out = vc;
    return 0;
}

static void level_clear(Level *L) {
    for (int i = 0; i < NB; i++) { free(L->b[i]); L->b[i] = NULL; }
    L->bricks = L->voxels = 0; L->built = 0;
}
void vox_cascades_free(VoxCascades *vc) {
    if (!vc) return;
    for (int l = 0; l < VOXC_LEVELS; l++) level_clear(&vc->lv[l]);
    for (int m = 0; m < vc->nmod; m++) free(vc->mod[m].tri);
    free(vc->mod); free(vc->inst);
    free(vc->tri);
    free(vc);
}

static void level_add(Level *L, const double p[3], const double n[3], const float c[3]) {
    int v[3], k[3];
    for (int a = 0; a < 3; a++) {
        v[a] = (int)floor((p[a] - n[a] * L->vs * 0.5) / L->vs);   /* half a voxel into the surface, as the uniform world */
        k[a] = (int)floor((double)v[a] / VOX_BRICK) - L->org[a];
        if (k[a] < 0 || k[a] >= VOXC_WIN) return;
    }
    int bi = (k[2] * VOXC_WIN + k[1]) * VOXC_WIN + k[0];
    CBrick *b = L->b[bi];
    if (!b) { b = L->b[bi] = calloc(1, sizeof *b); if (!b) return; L->bricks++; }
    int i = ((v[2] - (k[2] + L->org[2]) * VOX_BRICK) * VOX_BRICK + (v[1] - (k[1] + L->org[1]) * VOX_BRICK)) * VOX_BRICK + (v[0] - (k[0] + L->org[0]) * VOX_BRICK);
    for (int a = 0; a < 3; a++) { b->alb[i][a] += c[a]; b->n[i][a] += (float)n[a]; }
    if (!b->cnt[i]) L->voxels++;
    if (b->cnt[i] < 65535) b->cnt[i]++;
}

/* Sutherland-Hodgman: the polygon p[n] clipped to x[axis] >= lo (sign 1) or <= hi (sign -1) */
static int clip_plane(double (*p)[3], int n, double (*q)[3], int axis, double bound, int sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const double *a = p[i], *b = p[(i + 1) % n];
        double da = sign * (a[axis] - bound), db = sign * (b[axis] - bound);
        if (da >= 0) { memcpy(q[m++], a, sizeof q[0]); }
        if ((da >= 0) != (db >= 0)) {
            double t = da / (da - db);
            for (int k = 0; k < 3; k++) q[m][k] = a[k] + (b[k] - a[k]) * t;
            m++;
        }
    }
    return m;
}

/* one world-space triangle into a level: clipped to the window (grown by a voxel), sampled on a grid of half a voxel */
static void sample_tri(Level *L, const float v[3][3], const float col[3], const float ref[3], const double lo[3], const double hi[3]) {
    double step = L->vs * 0.5;
    double e1[3], e2[3], n[3];
    for (int k = 0; k < 3; k++) { e1[k] = v[1][k] - v[0][k]; e2[k] = v[2][k] - v[0][k]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    double nl = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl < 1e-9) return;
    for (int k = 0; k < 3; k++) n[k] /= nl;
    if (n[0] * ref[0] + n[1] * ref[1] + n[2] * ref[2] < 0) for (int k = 0; k < 3; k++) n[k] = -n[k];
    double P[16][3], Q[16][3];
    int np = 3;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) P[i][k] = v[i][k];
    for (int a = 0; a < 3 && np >= 3; a++) {
        np = clip_plane(P, np, Q, a, lo[a] - L->vs, 1);
        np = clip_plane(Q, np, P, a, hi[a] + L->vs, -1);
    }
    if (np < 3) return;
    for (int k = 1; k + 1 < np; k++) {   /* the clipped polygon as a fan */
        double a0[3], f1[3], f2[3];
        for (int c = 0; c < 3; c++) { a0[c] = P[0][c]; f1[c] = P[k][c] - P[0][c]; f2[c] = P[k + 1][c] - P[0][c]; }
        double l1 = sqrt(f1[0] * f1[0] + f1[1] * f1[1] + f1[2] * f1[2]), l2 = sqrt(f2[0] * f2[0] + f2[1] * f2[1] + f2[2] * f2[2]);
        int s1 = (int)ceil(l1 / step) + 1, s2 = (int)ceil(l2 / step) + 1;
        for (int i = 0; i < s1; i++)
            for (int j = 0; j < s2; j++) {
                double u = (i + 0.5) / s1, w = (j + 0.5) / s2;
                if (u + w > 1) continue;
                double p[3] = { a0[0] + f1[0] * u + f2[0] * w, a0[1] + f1[1] * u + f2[1] * w, a0[2] + f1[2] * u + f2[2] * w };
                level_add(L, p, n, col);
            }
    }
}

static void level_build(VoxCascades *vc, Level *L) {
    level_clear(L);
    double lo[3], hi[3];
    for (int a = 0; a < 3; a++) { lo[a] = L->org[a] * L->bu; hi[a] = lo[a] + VOXC_WIN * L->bu; }
    for (long t = 0; t < vc->ntri; t++) {
        const Tri *T = &vc->tri[t];
        if (T->hi[0] < lo[0] || T->lo[0] > hi[0] || T->hi[1] < lo[1] || T->lo[1] > hi[1] || T->hi[2] < lo[2] || T->lo[2] > hi[2]) continue;
        sample_tri(L, T->v, T->col, T->ref, lo, hi);
    }
    for (long q = 0; q < vc->ninst; q++) {   /* placed models: their triangles transformed on the way */
        const Inst *I = &vc->inst[q];
        if (I->hi[0] < lo[0] || I->lo[0] > hi[0] || I->hi[1] < lo[1] || I->lo[1] > hi[1] || I->hi[2] < lo[2] || I->lo[2] > hi[2]) continue;
        const IModel *M = &vc->mod[I->model];
        for (long t = 0; t < M->ntri; t++) {
            const Tri *T = &M->tri[t];
            float w[3][3], r[3];
            for (int c = 0; c < 3; c++)
                for (int a = 0; a < 3; a++) w[c][a] = I->X[a][0] * T->v[c][0] + I->X[a][1] * T->v[c][1] + I->X[a][2] * T->v[c][2] + I->X[a][3];
            for (int a = 0; a < 3; a++) r[a] = I->X[a][0] * T->ref[0] + I->X[a][1] * T->ref[1] + I->X[a][2] * T->ref[2];
            sample_tri(L, (const float (*)[3])w, T->col, r, lo, hi);
        }
    }
    for (int bi = 0; bi < NB; bi++) {   /* averages, unit normals */
        CBrick *b = L->b[bi];
        if (!b) continue;
        for (int i = 0; i < NV; i++) {
            if (!b->cnt[i]) continue;
            float inv = 1.0f / b->cnt[i];
            for (int a = 0; a < 3; a++) b->alb[i][a] *= inv;
            float l = sqrtf(b->n[i][0] * b->n[i][0] + b->n[i][1] * b->n[i][1] + b->n[i][2] * b->n[i][2]);
            for (int a = 0; a < 3; a++) b->n[i][a] = l > 1e-6f ? b->n[i][a] / l : 0;
        }
    }
    L->built = 1;
    vc->rebuilds++;
}

void vox_cascades_update(VoxCascades *vc, const double cam[3]) {
    for (int l = 0; l < VOXC_LEVELS; l++) {
        Level *L = &vc->lv[l];
        int org[3];
        for (int a = 0; a < 3; a++) org[a] = (int)floor(cam[a] / L->bu) - VOXC_WIN / 2;
        if (L->built && !memcmp(org, L->org, sizeof org)) continue;
        memcpy(L->org, org, sizeof org);
        level_build(vc, L);   /* v0: the whole level; later only the slab the camera moved into */
    }
}

void vox_cascades_info(const VoxCascades *vc, VoxCascadeInfo *info) {
    memset(info, 0, sizeof *info);
    info->triangles = vc->ntri;
    for (long q = 0; q < vc->ninst; q++) info->triangles += vc->mod[vc->inst[q].model].ntri;
    info->faces = vc->faces; info->disps = vc->disps; info->rebuilds = vc->rebuilds;
    for (int l = 0; l < VOXC_LEVELS; l++) {
        info->bricks[l] = vc->lv[l].bricks; info->voxels[l] = vc->lv[l].voxels;
        info->bytes += (size_t)vc->lv[l].bricks * NV * 8;   /* the GPU form: RGBA8 colour + RGBA8 normal and coverage */
    }
}

/* one level's window, t in [t0, t1]: a DDA over its bricks, inside an occupied brick a DDA over its voxels */
static int trace_level(const Level *L, const double o[3], const double d[3], double t0, double t1, VoxHit *hit) {
    int k[3], st[3];
    double tm[3], td[3], base[3];
    for (int a = 0; a < 3; a++) {
        base[a] = L->org[a] * L->bu;
        double p = o[a] + d[a] * (t0 + 1e-4) - base[a];
        k[a] = (int)floor(p / L->bu);
        if (k[a] < 0) k[a] = 0; else if (k[a] >= VOXC_WIN) k[a] = VOXC_WIN - 1;
        if (d[a] > 1e-12) { st[a] = 1; tm[a] = t0 + ((k[a] + 1) * L->bu - p) / d[a]; td[a] = L->bu / d[a]; }
        else if (d[a] < -1e-12) { st[a] = -1; tm[a] = t0 + (k[a] * L->bu - p) / d[a]; td[a] = -L->bu / d[a]; }
        else { st[a] = 0; tm[a] = 1e30; td[a] = 1e30; }
    }
    double t = t0;
    for (;;) {
        int ax = tm[0] < tm[1] ? (tm[0] < tm[2] ? 0 : 2) : (tm[1] < tm[2] ? 1 : 2);
        double te = tm[ax] < t1 ? tm[ax] : t1;
        const CBrick *b = L->b[(k[2] * VOXC_WIN + k[1]) * VOXC_WIN + k[0]];
        if (b) {   /* the voxels of this brick between t and te */
            double bb[3] = { base[0] + k[0] * L->bu, base[1] + k[1] * L->bu, base[2] + k[2] * L->bu };
            int v[3], vs[3];
            double vm[3], vd[3];
            for (int a = 0; a < 3; a++) {
                double p = o[a] + d[a] * (t + 1e-4) - bb[a];
                v[a] = (int)floor(p / L->vs);
                if (v[a] < 0) v[a] = 0; else if (v[a] >= VOX_BRICK) v[a] = VOX_BRICK - 1;
                if (d[a] > 1e-12) { vs[a] = 1; vm[a] = t + ((v[a] + 1) * L->vs - p) / d[a]; vd[a] = L->vs / d[a]; }
                else if (d[a] < -1e-12) { vs[a] = -1; vm[a] = t + (v[a] * L->vs - p) / d[a]; vd[a] = -L->vs / d[a]; }
                else { vs[a] = 0; vm[a] = 1e30; vd[a] = 1e30; }
            }
            double tv = t;
            for (int it = 0; it < 3 * VOX_BRICK + 3; it++) {
                int i = (v[2] * VOX_BRICK + v[1]) * VOX_BRICK + v[0];
                if (b->cnt[i]) {
                    hit->t = (float)tv;
                    for (int a = 0; a < 3; a++) { hit->p[a] = (float)(o[a] + d[a] * tv); hit->n[a] = b->n[i][a]; hit->albedo[a] = b->alb[i][a]; }
                    return 1;
                }
                int a = vm[0] < vm[1] ? (vm[0] < vm[2] ? 0 : 2) : (vm[1] < vm[2] ? 1 : 2);
                tv = vm[a];
                if (tv > te) break;
                v[a] += vs[a];
                if (v[a] < 0 || v[a] >= VOX_BRICK) break;
                vm[a] += vd[a];
            }
        }
        t = tm[ax];
        if (t > t1) return 0;
        k[ax] += st[ax];
        if (k[ax] < 0 || k[ax] >= VOXC_WIN) return 0;
        tm[ax] += td[ax];
    }
}

int vox_cascades_trace(const VoxCascades *vc, const double o[3], const double d[3], double tmin, double tmax, VoxHit *hit) {
    double t = tmin;
    for (int l = 0; l < VOXC_LEVELS && t < tmax; l++) {
        const Level *L = &vc->lv[l];
        if (!L->built || !L->bricks) continue;
        double t0 = t, t1 = tmax;
        for (int a = 0; a < 3; a++) {   /* the window's box */
            double lo = L->org[a] * L->bu, hi = lo + VOXC_WIN * L->bu;
            if (fabs(d[a]) < 1e-12) { if (o[a] < lo || o[a] > hi) { t0 = 1; t1 = 0; } continue; }
            double u = (lo - o[a]) / d[a], v = (hi - o[a]) / d[a];
            if (u > v) { double x = u; u = v; v = x; }
            if (u > t0) t0 = u;
            if (v < t1) t1 = v;
        }
        if (t0 > t1) continue;
        if (trace_level(L, o, d, t0, t1, hit)) return 1;
        if (t1 > t) t = t1;   /* on from where this level's window ends */
    }
    return 0;
}

/* the GPU form (shaders/vk/ptrt_trace.comp): words 0..19 level origins (x, y, z, 0 per level), 20..31 the three light vectors (each x y z 0:
 * the direction toward the sun, the sun's colour, the sky's colour), 32 the brick count; the page table from word 64 (VOXC_LEVELS x
 * VOXC_WIN^3: brick index + 1, 0 empty); the bricks after it, 512 voxels x 2 words: albedo rgb8 + coverage8 (0 empty), normal snorm8 x 3.
 * Then the lamps (word 33 count, 34 offset). Returns the words used, 0 if cap is too small. */
size_t vox_cascades_pack(const VoxCascades *vc, uint32_t *out, size_t cap, const float sun[3], const float sunc[3], const float sky[3],
                         const float *lights, int nlights) {
    size_t pages = 64, bricks = pages + (size_t)VOXC_LEVELS * NB, at = bricks;
    if (cap < bricks) return 0;
    memset(out, 0, bricks * 4);
    const float *vec[3] = { sun, sunc, sky };
    for (int l = 0; l < VOXC_LEVELS; l++) for (int a = 0; a < 3; a++) out[l * 4 + a] = (uint32_t)vc->lv[l].org[a];
    for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) memcpy(&out[20 + k * 4 + a], &vec[k][a], 4);
    uint32_t n = 0;
    for (int l = 0; l < VOXC_LEVELS; l++) {
        const Level *L = &vc->lv[l];
        for (int bi = 0; bi < NB; bi++) {
            const CBrick *b = L->b[bi];
            if (!b) continue;
            if (at + NV * 2 > cap) return 0;
            out[pages + (size_t)l * NB + bi] = ++n;
            for (int i = 0; i < NV; i++) {
                uint32_t c = 0, nn = 0;
                if (b->cnt[i]) {
                    for (int a = 0; a < 3; a++) {
                        float v = b->alb[i][a] < 0 ? 0 : b->alb[i][a] > 1 ? 1 : b->alb[i][a];
                        c |= (uint32_t)(v * 255 + 0.5f) << (8 * a);
                        int s = (int)lrintf(b->n[i][a] * 127);
                        nn |= (uint32_t)(s & 255) << (8 * a);
                    }
                    c |= (uint32_t)(b->cnt[i] > 255 ? 255 : b->cnt[i]) << 24;
                }
                out[at + 2 * (size_t)i] = c; out[at + 2 * (size_t)i + 1] = nn;
            }
            at += NV * 2;
        }
    }
    out[32] = n;
    /* the lamps after the bricks: word 33 their count, 34 their first word; VOX_LIGHT_WORDS floats each (see vox_world.h) */
    if (at + (size_t)nlights * VOX_LIGHT_WORDS > cap) nlights = 0;
    out[33] = (uint32_t)nlights; out[34] = (uint32_t)at;
    if (nlights) memcpy(out + at, lights, (size_t)nlights * VOX_LIGHT_WORDS * 4);
    return at + (size_t)nlights * VOX_LIGHT_WORDS;
}
