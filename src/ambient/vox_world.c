/* PT-RT voxel world (analysis/43). See vox_world.h.
 * Build: every lit brush face (its polygon from the edges, a triangle fan) and every displacement triangle is sampled on a grid of
 * half a voxel; each sample adds its texture's colour and the surface normal to the voxel it falls in, nudged half a voxel against the
 * normal so a wall's voxels sit inside the wall's surface layer rather than straddling into the air. Faces that are not drawn (sky, nodraw,
 * hint, skip, trigger) are left out.
 * Trace: a DDA over bricks; inside an occupied brick a DDA over its voxels (Amanatides & Woo), both on the same ray parameter t. */
#include "vox_world.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { NV = VOX_BRICK * VOX_BRICK * VOX_BRICK, BRICK_UNITS = VOX_SIZE * VOX_BRICK };
enum { SURF_SKY = 0x4, SURF_TRANS = 0x10, SURF_NODRAW = 0x80, SURF_HINT = 0x100, SURF_SKIP = 0x200, SURF_TRIGGER = 0x40 };
typedef struct { int k[3]; float alb[NV][3], n[NV][3]; uint16_t cnt[NV]; } Brick;
struct VoxWorld {
    Brick **slot; size_t cap, n;   /* open addressing on the brick coordinates */
    int lo[3], hi[3];              /* the bricks' bounds, inclusive */
    long samples, faces, disps;
    int final;
};

static size_t hash3(const int k[3], size_t cap) {
    uint64_t h = (uint64_t)(uint32_t)k[0] * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)k[1] * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)(uint32_t)k[2] * 0x165667B19E3779F9ull;
    return (size_t)(h ^ (h >> 29)) & (cap - 1);
}
static Brick *find(const VoxWorld *w, const int k[3]) {
    size_t h = hash3(k, w->cap);
    for (;;) {
        Brick *b = w->slot[h];
        if (!b) return NULL;
        if (b->k[0] == k[0] && b->k[1] == k[1] && b->k[2] == k[2]) return b;
        h = (h + 1) & (w->cap - 1);
    }
}
static int grow(VoxWorld *w) {
    size_t cap = w->cap ? w->cap * 2 : 4096;
    Brick **s = calloc(cap, sizeof *s);
    if (!s) return 0;
    for (size_t i = 0; i < w->cap; i++) if (w->slot[i]) {
        size_t h = hash3(w->slot[i]->k, cap);
        while (s[h]) h = (h + 1) & (cap - 1);
        s[h] = w->slot[i];
    }
    free(w->slot); w->slot = s; w->cap = cap;
    return 1;
}
static Brick *get(VoxWorld *w, const int k[3]) {
    if (w->cap) { Brick *b = find(w, k); if (b) return b; }
    if ((w->n + 1) * 2 > w->cap && !grow(w)) return NULL;
    Brick *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    memcpy(b->k, k, sizeof b->k);
    size_t h = hash3(k, w->cap);
    while (w->slot[h]) h = (h + 1) & (w->cap - 1);
    w->slot[h] = b; w->n++;
    for (int a = 0; a < 3; a++) {
        if (w->n == 1 || k[a] < w->lo[a]) w->lo[a] = k[a];
        if (w->n == 1 || k[a] > w->hi[a]) w->hi[a] = k[a];
    }
    return b;
}
static void add(VoxWorld *w, const double p[3], const double n[3], const float c[3]) {
    int v[3], k[3];
    for (int a = 0; a < 3; a++) {
        v[a] = (int)floor((p[a] - n[a] * (VOX_SIZE * 0.5)) / VOX_SIZE);
        k[a] = (int)floor((double)v[a] / VOX_BRICK);
    }
    Brick *b = get(w, k);
    if (!b) return;
    int i = ((v[2] - k[2] * VOX_BRICK) * VOX_BRICK + (v[1] - k[1] * VOX_BRICK)) * VOX_BRICK + (v[0] - k[0] * VOX_BRICK);
    for (int a = 0; a < 3; a++) { b->alb[i][a] += c[a]; b->n[i][a] += (float)n[a]; }
    if (b->cnt[i] < 65535) b->cnt[i]++;
    w->samples++;
}
/* ref: the face's own plane normal (the side it is seen from); the triangle's normal is turned to agree with it: Source winds face
 * vertices so that their cross product points into the surface */
static void tri(VoxWorld *w, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]) {
    double e1[3], e2[3], n[3];
    for (int k = 0; k < 3; k++) { e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l < 1e-9) return;
    for (int k = 0; k < 3; k++) n[k] /= l;
    if (n[0] * ref[0] + n[1] * ref[1] + n[2] * ref[2] < 0) for (int k = 0; k < 3; k++) n[k] = -n[k];
    double l1 = sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]), l2 = sqrt(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]);
    int s1 = (int)ceil(l1 / (VOX_SIZE * 0.5)) + 1, s2 = (int)ceil(l2 / (VOX_SIZE * 0.5)) + 1;
    for (int i = 0; i < s1; i++)
        for (int j = 0; j < s2; j++) {
            double u = (i + 0.5) / s1, v = (j + 0.5) / s2;
            if (u + v > 1) continue;
            double p[3] = { a[0] + e1[0] * u + e2[0] * v, a[1] + e1[1] * u + e2[1] * v, a[2] + e1[2] * u + e2[2] * v };
            add(w, p, n, col);
        }
}

typedef struct { int ofs, len, ver; char cc[4]; } Lump;
typedef struct { float tv[2][4], lv[2][4]; int flags, texdata; } TexInfo;
typedef struct { float refl[3]; int name, w, h, vw, vh; } TexData;
#pragma pack(push, 1)
typedef struct {
    uint16_t plane; uint8_t side, on_node; int32_t first_edge; int16_t nedges, texinfo, dispinfo, fog;
    uint8_t styles[4]; int32_t lightofs; float area; int32_t lm_mins[2], lm_size[2]; int32_t orig; uint16_t nprims, first_prim; uint32_t smoothing;
} Face;
#pragma pack(pop)
static const void *lump(const uint8_t *bsp, size_t size, int i, int rec, int *n) {
    const Lump *l = (const Lump *)(bsp + 8 + 16 * i);
    *n = l->len > 0 && (size_t)l->ofs + (size_t)l->len <= size ? l->len / rec : 0;
    return *n ? bsp + l->ofs : NULL;
}
static void colour_of(const TexInfo *ti, int nti, const TexData *td, int ntd, const float (*colour)[3], int ncolour, int t, float c[3]) {
    c[0] = c[1] = c[2] = 0.3f;
    if (t < 0 || t >= nti) return;
    int d = ti[t].texdata;
    if (colour && d >= 0 && d < ncolour && (colour[d][0] + colour[d][1] + colour[d][2]) > 0) { memcpy(c, colour[d], sizeof(float) * 3); return; }
    if (d >= 0 && d < ntd) memcpy(c, td[d].refl, sizeof(float) * 3);
}

/* faces in the 3D skybox's area (the leaf of the sky_camera entity's origin): 1 per face, NULL if the map has none */
static uint8_t *sky_faces(const uint8_t *bsp, size_t size, const SunMap *sm, int nf, float box[6]) {
    int ne_, nl, nlf;
    const char *ents = lump(bsp, size, 0, 1, &ne_);
    if (!ents || !sm) return NULL;
    const char *e = ents, *end = ents + ne_;
    double org[3]; int found = 0;
    while (e < end && !found) {   /* each { ... } block: the one with "classname" "sky_camera" */
        const char *o = memchr(e, '{', (size_t)(end - e)), *c = o ? memchr(o, '}', (size_t)(end - o)) : NULL;
        if (!o || !c) break;
        size_t n = (size_t)(c - o);
        char blk[4096];
        if (n < sizeof blk) {
            memcpy(blk, o, n); blk[n] = 0;
            const char *org_s;
            if (strstr(blk, "\"sky_camera\"") && (org_s = strstr(blk, "\"origin\"")) && sscanf(org_s + 8, " \"%lf %lf %lf", &org[0], &org[1], &org[2]) == 3) found = 1;
        }
        e = c + 1;
    }
    if (!found) return NULL;
    int leaf = sun_point_leaf(sm, org);
    const uint8_t *leaves = lump(bsp, size, 10, 32, &nl);
    const uint16_t *lf = lump(bsp, size, 16, 2, &nlf);
    if (leaf < 0 || !leaves || !lf || leaf >= nl) return NULL;
    uint16_t af; memcpy(&af, leaves + 32 * (size_t)leaf + 6, 2);
    int area = af & 0x1ff;
    if (area == 0) return NULL;   /* area 0 is the void / no areas: nothing to separate */
    uint8_t *sky = calloc((size_t)nf, 1);
    if (!sky) return NULL;
    box[0] = box[1] = box[2] = 1e30f; box[3] = box[4] = box[5] = -1e30f;
    for (int i = 0; i < nl; i++) {
        const uint8_t *L = leaves + 32 * (size_t)i;
        memcpy(&af, L + 6, 2);
        if ((af & 0x1ff) != area) continue;
        int16_t mm[6]; memcpy(mm, L + 8, 12);   /* the area's bounds: displacements are not in the leaf face lists */
        for (int k = 0; k < 3; k++) { if (mm[k] < box[k]) box[k] = mm[k]; if (mm[3 + k] > box[3 + k]) box[3 + k] = mm[3 + k]; }
        uint16_t first, num; memcpy(&first, L + 20, 2); memcpy(&num, L + 22, 2);
        for (int k = 0; k < num && first + k < nlf; k++) if (lf[first + k] < nf) sky[lf[first + k]] = 1;
    }
    return sky;
}

static int g_disp_parity = 0;   /* which quads take the other diagonal: (row + column) & 1 == this; -1: none (one diagonal everywhere) */
void vox_set_disp_parity(int p) { g_disp_parity = p; }

int vox_skybox_bounds(const uint8_t *bsp, size_t size, const SunMap *sm, float box[6]) {
    int nf;
    if (!lump(bsp, size, 7, sizeof(Face), &nf)) return 0;
    uint8_t *sky = sky_faces(bsp, size, sm, nf, box);
    free(sky);
    return sky != NULL;
}

__thread int vox_tri_texinfo = -1;
int vox_triangles(const uint8_t *bsp, size_t size, const SunMap *sm, const float (*colour)[3], int ncolour, VoxTriFn fn, void *ctx,
                  long *nfaces, long *ndisps) {
    *nfaces = *ndisps = 0;
    vox_tri_texinfo = -1;
    if (size < 1036 || memcmp(bsp, "VBSP", 4) != 0) return 1;
    int nf, nv, ne, nse, nti, ntd;
    const Face *faces = lump(bsp, size, 7, sizeof(Face), &nf);
    const float (*verts)[3] = lump(bsp, size, 3, 12, &nv);
    const uint16_t (*edges)[2] = lump(bsp, size, 12, 4, &ne);
    const int32_t *se = lump(bsp, size, 13, 4, &nse);
    const TexInfo *ti = lump(bsp, size, 6, sizeof(TexInfo), &nti);
    const TexData *td = lump(bsp, size, 2, sizeof(TexData), &ntd);
    int npl;
    const float (*planes)[5] = lump(bsp, size, 1, 20, &npl);   /* n[3], d, type */
    if (!faces || !verts || !edges || !se || !ti || !planes) return 3;
    float skybox[6] = { 1, 1, 1, -1, -1, -1 };   /* empty unless the map has a 3D skybox */
    uint8_t *sky = sky_faces(bsp, size, sm, nf, skybox);   /* the 3D skybox's faces: left out (it sits inside the map's bounds, above the play area) */
    /* only the world's own faces (model 0): brush entities' faces (doors, func_brush) are stored relative to their entity's origin, and
     * read raw they land around the world origin (gm_construct: a 2480 x 1936 "color room" plane above spawn); they can also move */
    int nmod, f0 = 0, f1 = nf;
    const uint8_t *models = lump(bsp, size, 14, 48, &nmod);
    if (models && nmod > 0) {
        int32_t ff, fn; memcpy(&ff, models + 40, 4); memcpy(&fn, models + 44, 4);
        if (ff >= 0 && fn >= 0 && ff + fn <= nf) { f0 = ff; f1 = ff + fn; }
    }
    for (int f = f0; f < f1; f++) {
        const Face *fc = &faces[f];
        if (sky && sky[f]) continue;
        if (fc->dispinfo >= 0 || fc->texinfo < 0 || fc->texinfo >= nti || fc->nedges < 3 || fc->plane >= npl) continue;
        if (ti[fc->texinfo].flags & (SURF_SKY | SURF_TRANS | SURF_NODRAW | SURF_HINT | SURF_SKIP | SURF_TRIGGER)) continue;   /* translucent (glass): light passes, as the brush trace (MASK_OPAQUE) has it */
        if (fc->first_edge < 0 || fc->first_edge + fc->nedges > nse) continue;
        double p[64][3];
        int np = 0;
        for (int k = 0; k < fc->nedges && np < 64; k++) {
            int e = se[fc->first_edge + k], ei = e >= 0 ? e : -e;
            if (ei >= ne) { np = 0; break; }
            int vi = edges[ei][e >= 0 ? 0 : 1];
            if (vi >= nv) { np = 0; break; }
            for (int a = 0; a < 3; a++) p[np][a] = verts[vi][a];
            np++;
        }
        if (np < 3) continue;
        float c[3];
        colour_of(ti, nti, td, ntd, colour, ncolour, fc->texinfo, c);
        vox_tri_texinfo = fc->texinfo;
        for (int k = 1; k + 1 < np; k++) fn(ctx, p[0], p[k], p[k + 1], c, planes[fc->plane]);   /* the plane already faces the face's side */
        (*nfaces)++;
    }
    int nd = sm ? sun_map_disp_count(sm) : 0, ndi;
    const uint8_t *dinfo = lump(bsp, size, 26, 176, &ndi);
    for (int i = 0; i < nd; i++) {
        const float (*v)[3]; int gw, tix;
        if (!sun_map_disp_grid(sm, i, &v, &gw, &tix) || gw < 2) continue;
        uint16_t mf = 0xffff;
        if (dinfo && i < ndi) memcpy(&mf, dinfo + 176 * (size_t)i + 36, 2);
        if (sky && mf < nf && sky[mf]) continue;
        if (v[0][0] >= skybox[0] && v[0][0] <= skybox[3] && v[0][1] >= skybox[1] && v[0][1] <= skybox[4] && v[0][2] >= skybox[2] && v[0][2] <= skybox[5]) continue;
        float ref[3] = { 0, 0, 1 };
        if (mf < nf && faces[mf].plane < npl) memcpy(ref, planes[faces[mf].plane], sizeof ref);
        float c[3];
        colour_of(ti, nti, td, ntd, colour, ncolour, tix, c);
        vox_tri_texinfo = tix;
        for (int r = 0; r + 1 < gw; r++)
            for (int q = 0; q + 1 < gw; q++) {
                int x = r * gw + q;
                double a[3], b[3], cc[3], d[3];
                for (int k = 0; k < 3; k++) { a[k] = v[x][k]; b[k] = v[x + 1][k]; cc[k] = v[x + gw][k]; d[k] = v[x + gw + 1][k]; }
                /* the engine splits a displacement's quads along alternating diagonals (a checkerboard); a fixed split differs from
                 * its surface by a few units on curved terrain, which a sun map of these triangles shows as triangle-shaped shadows */
                int alt = g_disp_parity < 0 ? 0 : ((r + q) & 1) == g_disp_parity;
                if (alt) { fn(ctx, a, cc, d, c, ref); fn(ctx, a, d, b, c, ref); }
                else { fn(ctx, a, cc, b, c, ref); fn(ctx, b, cc, d, c, ref); }
            }
        (*ndisps)++;
    }
    free(sky);
    vox_tri_texinfo = -1;
    return 0;
}

static void tri_cb(void *ctx, const double a[3], const double b[3], const double c[3], const float col[3], const float ref[3]) { tri(ctx, a, b, c, col, ref); }

int vox_build(const uint8_t *bsp, size_t size, const SunMap *sm, const float (*colour)[3], int ncolour, VoxWorld **out) {
    *out = NULL;
    VoxWorld *w = calloc(1, sizeof *w);
    if (!w || !grow(w)) { free(w); return 2; }
    int rc = vox_triangles(bsp, size, sm, colour, ncolour, tri_cb, w, &w->faces, &w->disps);
    if (rc) { vox_free(w); return rc; }
    /* finalize: averages, unit normals */
    for (size_t s = 0; s < w->cap; s++) {
        Brick *b = w->slot[s];
        if (!b) continue;
        for (int i = 0; i < NV; i++) {
            if (!b->cnt[i]) continue;
            float inv = 1.0f / b->cnt[i], l;
            for (int a = 0; a < 3; a++) b->alb[i][a] *= inv;
            l = sqrtf(b->n[i][0] * b->n[i][0] + b->n[i][1] * b->n[i][1] + b->n[i][2] * b->n[i][2]);
            for (int a = 0; a < 3; a++) b->n[i][a] = l > 1e-6f ? b->n[i][a] / l : 0;
        }
    }
    w->final = 1;
    *out = w;
    return 0;
}

void vox_info(const VoxWorld *w, VoxInfo *info) {
    memset(info, 0, sizeof *info);
    info->bricks = (long)w->n; info->samples = w->samples; info->faces = w->faces; info->disps = w->disps;
    for (size_t s = 0; s < w->cap; s++) if (w->slot[s]) for (int i = 0; i < NV; i++) info->voxels += w->slot[s]->cnt[i] != 0;
    info->bytes = (size_t)w->n * NV * 8;   /* the GPU form: RGBA8 colour + RGBA8 normal and coverage per voxel */
}
void vox_free(VoxWorld *w) {
    if (!w) return;
    for (size_t s = 0; s < w->cap; s++) free(w->slot[s]);
    free(w->slot);
    free(w);
}

/* the voxels of one brick along the ray, t in [t0, t1] */
static int trace_brick(const Brick *b, const double o[3], const double d[3], double t0, double t1, VoxHit *hit) {
    double base[3] = { b->k[0] * (double)BRICK_UNITS, b->k[1] * (double)BRICK_UNITS, b->k[2] * (double)BRICK_UNITS };
    double p[3];
    int v[3], st[3];
    double tmax[3], tdel[3];
    for (int a = 0; a < 3; a++) {
        p[a] = o[a] + d[a] * (t0 + 1e-4) - base[a];
        v[a] = (int)floor(p[a] / VOX_SIZE);
        if (v[a] < 0) v[a] = 0; else if (v[a] >= VOX_BRICK) v[a] = VOX_BRICK - 1;
        if (d[a] > 1e-12) { st[a] = 1; tmax[a] = t0 + ((v[a] + 1) * VOX_SIZE - p[a]) / d[a]; tdel[a] = VOX_SIZE / d[a]; }
        else if (d[a] < -1e-12) { st[a] = -1; tmax[a] = t0 + (v[a] * VOX_SIZE - p[a]) / d[a]; tdel[a] = -VOX_SIZE / d[a]; }
        else { st[a] = 0; tmax[a] = 1e30; tdel[a] = 1e30; }
    }
    double t = t0;
    for (int it = 0; it < 3 * VOX_BRICK + 3; it++) {
        int i = (v[2] * VOX_BRICK + v[1]) * VOX_BRICK + v[0];
        if (b->cnt[i]) {
            hit->t = (float)t;
            for (int a = 0; a < 3; a++) { hit->p[a] = (float)(o[a] + d[a] * t); hit->n[a] = b->n[i][a]; hit->albedo[a] = b->alb[i][a]; }
            return 1;
        }
        int a = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        t = tmax[a];
        if (t > t1) return 0;
        v[a] += st[a];
        if (v[a] < 0 || v[a] >= VOX_BRICK) return 0;
        tmax[a] += tdel[a];
    }
    return 0;
}

int vox_trace(const VoxWorld *w, const double o[3], const double d[3], double tmin, double tmax, VoxHit *hit) {
    if (!w->n) return 0;
    /* clip the ray to the bricks' bounds */
    double t0 = tmin, t1 = tmax;
    for (int a = 0; a < 3; a++) {
        double lo = w->lo[a] * (double)BRICK_UNITS, hi = (w->hi[a] + 1) * (double)BRICK_UNITS;
        if (fabs(d[a]) < 1e-12) { if (o[a] < lo || o[a] > hi) return 0; continue; }
        double u = (lo - o[a]) / d[a], v = (hi - o[a]) / d[a];
        if (u > v) { double x = u; u = v; v = x; }
        if (u > t0) t0 = u;
        if (v < t1) t1 = v;
    }
    if (t0 > t1) return 0;
    int k[3], st[3];
    double tm[3], td[3];
    for (int a = 0; a < 3; a++) {
        double p = o[a] + d[a] * (t0 + 1e-4);
        k[a] = (int)floor(p / BRICK_UNITS);
        if (k[a] < w->lo[a]) k[a] = w->lo[a]; else if (k[a] > w->hi[a]) k[a] = w->hi[a];
        if (d[a] > 1e-12) { st[a] = 1; tm[a] = t0 + ((k[a] + 1) * (double)BRICK_UNITS - p) / d[a]; td[a] = BRICK_UNITS / d[a]; }
        else if (d[a] < -1e-12) { st[a] = -1; tm[a] = t0 + (k[a] * (double)BRICK_UNITS - p) / d[a]; td[a] = -BRICK_UNITS / d[a]; }
        else { st[a] = 0; tm[a] = 1e30; td[a] = 1e30; }
    }
    double t = t0;
    for (;;) {
        const Brick *b = find(w, k);
        int a = tm[0] < tm[1] ? (tm[0] < tm[2] ? 0 : 2) : (tm[1] < tm[2] ? 1 : 2);
        double te = tm[a] < t1 ? tm[a] : t1;
        if (b && trace_brick(b, o, d, t, te, hit)) return 1;
        t = tm[a];
        if (t > t1) return 0;
        k[a] += st[a];
        if (k[a] < w->lo[a] || k[a] > w->hi[a]) return 0;
        tm[a] += td[a];
    }
}
