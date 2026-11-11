/* Photon CPU C4c (analysis/14): the water reflection and refraction views culled to the water's screen area. Their projection is
 * unchanged, so every pixel the water shader samples is rendered exactly as before; only what can never be sampled (outside the water's
 * screen rectangle, widened by the distortion margin) is culled.
 *   PHOTON_CPU_WATERCLIP=1  narrow the views' side planes (client frustum copy + the engine's, through OverrideViewFrustum)
 *   PHOTON_CPU_WATERCLIP=2  probe: compute everything, self-check the planes, log; change nothing
 * water.c wraps IVRenderView slot 38 (Push3DView) and calls waterclip_push after the original for each view pushed inside the main
 * screen view.
 * - A water view is told by its render target's name (_rt_WaterReflection / _rt_WaterRefraction). The 3D skybox's reflection view
 *   draws into the same target, so it is covered the same way.
 * - The rectangle: every water leaf's surface quad (leaf box at the water height) in front of the main camera, projected and united.
 *   A quad that reaches behind the camera's near plane makes the rect the whole screen. Occlusion is ignored, so the rect is
 *   conservative. It is widened by the margin, then united with its vertical mirror, since the reflected camera's image is mirrored.
 * - Self-check: the four full-screen side planes rebuilt from the pushed view's own CViewSetup must match the engine's (normals within
 *   1e-3, distances within 0.5 units), each matched to its index; otherwise nothing is narrowed for that view. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

typedef struct { float n[3]; float d; } Plane;
typedef struct { float mn[3], mx[3]; int vol; } Box;

static Box *g_boxes;
static int g_nboxes;
static const void *g_world_leafs;
static int g_world_nleafs;
static LONG64 st_views, st_narrowed, st_full, st_selfcheck_fail, st_noworld, st_kept_other, st_norect;
static double st_area;
static float g_margin = 0.5f;
/* VisOverrideData_t as R_SetupAreaBits (engine FUN_18012c150) reads it */
typedef struct {
    float vis_origin[3];       /* +0x00 */
    float tolerance;           /* +0x0c distance-to-area-portal tolerance */
    float corners[4][3];       /* +0x10 */
    uint8_t trim;              /* +0x40 trim the frustum to the corners */
    uint8_t pad[3];
    float portal_origin[3];    /* +0x44 */
    float portal_forward[3];   /* +0x50 */
    float radius;              /* +0x5c */
    uint8_t tail[32];
} VisOverride;
static VisOverride g_vis;
static int g_vis_pending;
static LONG64 st_trimmed;   /* NDC units each side (a quarter of the screen); [assumed] above any $reflectamount/$refractamount */

static void world_boxes(uintptr_t engine) {
    const char *wb = *(const char *const *)(engine + 0x8afd18);
    if (!wb) { g_nboxes = 0; return; }
    const char *leafs = *(const char *const *)(wb + 0x18);
    int nleafs = *(const int *)(wb + 0x10), nwater = *(const int *)(wb + 0x20);
    const char *wdata = *(const char *const *)(wb + 0x28);
    if (leafs == g_world_leafs && nleafs == g_world_nleafs) return;
    g_world_leafs = leafs;
    g_world_nleafs = nleafs;
    g_nboxes = 0;
    if (!leafs || !wdata || nleafs <= 0 || nleafs > 1 << 20 || nwater <= 0 || nwater > 4096) return;
    int n = 0;
    for (int i = 0; i < nleafs; i++) if (*(const short *)(leafs + (size_t)i * 0x50 + 0x42) >= 0) n++;
    free(g_boxes);
    g_boxes = n ? malloc(sizeof(Box) * (size_t)n) : NULL;
    if (!g_boxes) return;
    for (int i = 0; i < nleafs; i++) {
        const char *l = leafs + (size_t)i * 0x50;
        short id = *(const short *)(l + 0x42);
        if (id < 0 || id >= nwater) continue;
        const float *c = (const float *)(l + 0x20), *hd = (const float *)(l + 0x30);
        float surf = *(const float *)(wdata + (size_t)id * 12);
        Box *b = &g_boxes[g_nboxes++];
        for (int k = 0; k < 2; k++) { b->mn[k] = c[k] - hd[k] - 8; b->mx[k] = c[k] + hd[k] + 8; }
        b->mn[2] = surf - 8;
        b->mx[2] = surf + 8;
        b->vol = id;
    }
    cpu_log("photon_cpu waterclip: %d water-leaf surface boxes", g_nboxes);
    for (int v = 0; v < nwater && v < 16; v++) {   /* each volume's extent, to place test cameras */
        float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
        int c = 0;
        for (int i = 0; i < g_nboxes; i++) {
            if (g_boxes[i].vol != v) continue;
            c++;
            for (int k = 0; k < 3; k++) { mn[k] = fminf(mn[k], g_boxes[i].mn[k]); mx[k] = fmaxf(mx[k], g_boxes[i].mx[k]); }
        }
        if (c) cpu_log("photon_cpu waterclip:   volume %d: %d boxes, x %.0f..%.0f y %.0f..%.0f surface z %.0f", v, c, mn[0], mx[0], mn[1], mx[1],
                       0.5f * (mn[2] + mx[2]));
    }
}

/* Source's AngleVectors */
static void basis(const float ang[3], float f[3], float r[3], float u[3]) {
    const float d2r = 3.14159265358979f / 180.0f;
    float sp = sinf(ang[0] * d2r), cp = cosf(ang[0] * d2r), sy = sinf(ang[1] * d2r), cy = cosf(ang[1] * d2r);
    float sr = sinf(ang[2] * d2r), cr = cosf(ang[2] * d2r);
    f[0] = cp * cy; f[1] = cp * sy; f[2] = -sp;
    r[0] = -sr * sp * cy + cr * sy; r[1] = -sr * sp * sy - cr * cy; r[2] = -sr * cp;
    u[0] = cr * sp * cy + sr * sy; u[1] = cr * sp * sy - sr * cy; u[2] = cr * cp;
}

typedef struct { float o[3], f[3], r[3], u[3], xs, ys, znear; } Cam;

static int cam_of(const char *v, Cam *c) {
    const float *o = (const float *)(v + 0x40), *a = (const float *)(v + 0x4c);
    float fov = *(const float *)(v + 0x38), aspect = *(const float *)(v + 0x6c);
    int w = *(const int *)(v + 0x10), h = *(const int *)(v + 0x18);
    if (!(fov > 1 && fov < 179) || w <= 0 || h <= 0) return 0;
    if (!(aspect > 0.1f && aspect < 10)) aspect = (float)w / (float)h;
    memcpy(c->o, o, 12);
    basis(a, c->f, c->r, c->u);
    c->xs = tanf(fov * 0.5f * 3.14159265358979f / 180.0f);
    c->ys = c->xs / aspect;
    c->znear = *(const float *)(v + 0x58);
    return 1;
}

static void plane_from(Plane *p, float a, const float f[3], float b, const float v[3], const float o[3]) {
    float n[3] = { a * f[0] + b * v[0], a * f[1] + b * v[1], a * f[2] + b * v[2] };
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l <= 0) l = 1;
    for (int k = 0; k < 3; k++) p->n[k] = n[k] / l;
    p->d = p->n[0] * o[0] + p->n[1] * o[1] + p->n[2] * o[2];
}

/* the four side planes for the NDC rect [x0,x1] x [y0,y1] (x right, y up): right, left, top, bottom */
static void side_planes(const Cam *c, float x0, float x1, float y0, float y1, Plane out[4]) {
    plane_from(&out[0], x1 * c->xs, c->f, -1, c->r, c->o);
    plane_from(&out[1], -x0 * c->xs, c->f, 1, c->r, c->o);
    plane_from(&out[2], y1 * c->ys, c->f, -1, c->u, c->o);
    plane_from(&out[3], -y0 * c->ys, c->f, 1, c->u, c->o);
}

/* the screen rect of water volume vol's surface in the main view, NDC; 0 if none of its boxes is on screen. Each box's surface quad is
 * clipped against the camera's near plane (Sutherland-Hodgman), so a quad that reaches behind the camera contributes only its visible
 * part; points just in front of the near plane project far off screen and are clamped by the caller. */
static int water_rect(const Cam *m, int vol, float *x0, float *x1, float *y0, float *y1) {
    float ax0 = 2, ax1 = -2, ay0 = 2, ay1 = -2;
    const float zmin = m->znear > 1 ? m->znear : 1;
    int any = 0;
    for (int i = 0; i < g_nboxes; i++) {
        const Box *b = &g_boxes[i];
        if (b->vol != vol) continue;
        float z = 0.5f * (b->mn[2] + b->mx[2]);
        float wx[4] = { b->mn[0], b->mx[0], b->mx[0], b->mn[0] }, wy[4] = { b->mn[1], b->mn[1], b->mx[1], b->mx[1] };
        float cam[4][3], out[8][3];
        for (int k = 0; k < 4; k++) {
            float d[3] = { wx[k] - m->o[0], wy[k] - m->o[1], z - m->o[2] };
            cam[k][0] = d[0] * m->r[0] + d[1] * m->r[1] + d[2] * m->r[2];
            cam[k][1] = d[0] * m->u[0] + d[1] * m->u[1] + d[2] * m->u[2];
            cam[k][2] = d[0] * m->f[0] + d[1] * m->f[1] + d[2] * m->f[2];
        }
        int n = 0;
        for (int k = 0; k < 4; k++) {   /* clip the polygon to camera z >= zmin */
            const float *p = cam[k], *q = cam[(k + 1) & 3];
            int pin = p[2] >= zmin, qin = q[2] >= zmin;
            if (pin) { memcpy(out[n++], p, 12); }
            if (pin != qin) {
                float t = (zmin - p[2]) / (q[2] - p[2]);
                for (int c = 0; c < 3; c++) out[n][c] = p[c] + t * (q[c] - p[c]);
                out[n++][2] = zmin;
            }
        }
        if (n < 3) continue;   /* entirely behind the camera */
        float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
        for (int k = 0; k < n; k++) {
            float px = out[k][0] / (out[k][2] * m->xs), py = out[k][1] / (out[k][2] * m->ys);
            bx0 = fminf(bx0, px); bx1 = fmaxf(bx1, px);
            by0 = fminf(by0, py); by1 = fmaxf(by1, py);
        }
        if (bx1 < -1 || bx0 > 1 || by1 < -1 || by0 > 1) continue;   /* off screen */
        ax0 = fminf(ax0, bx0); ax1 = fmaxf(ax1, bx1);
        ay0 = fminf(ay0, by0); ay1 = fmaxf(ay1, by1);
        any = 1;
    }
    if (!any) return 0;
    *x0 = ax0; *x1 = ax1; *y0 = ay0; *y1 = ay1;
    return 1;
}

static int is_water_rt(void *rt) {
    if (!rt) return 0;
    typedef const char *(*GetName)(void *);
    const char *n = (*(GetName **)rt)[0](rt);   /* ITexture::GetName, slot 0 */
    return n && (!_stricmp(n, "_rt_WaterReflection") || !_stricmp(n, "_rt_WaterRefraction"));
}

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* after Push3DView (main thread, inside the main screen view): narrow the water view's side planes */
void waterclip_push(int mode, uintptr_t engine, void *rv, void **rv_vt, const void *main_view_setup, int vol, int eye_in_water,
                    const void *view, void *rt, void *frustum) {
    if (!is_water_rt(rt) || !frustum || !main_view_setup) return;
    st_views++;
    if (vol < 0 || eye_in_water) { st_kept_other++; return; }   /* unknown volume or underwater: as the engine does */
    world_boxes(engine);
    if (!g_nboxes) { st_noworld++; return; }
    Cam mc, vc;
    if (!cam_of(main_view_setup, &mc) || !cam_of(view, &vc)) return;
    /* self-check: our full-screen planes against the engine's, matched by normal */
    Plane full[4], *eng = (Plane *)frustum;
    side_planes(&vc, -1, 1, -1, 1, full);
    int map[4];
    for (int i = 0; i < 4; i++) {
        int best = -1;
        float bestdot = 0;
        for (int j = 0; j < 6; j++) {
            float dot = full[i].n[0] * eng[j].n[0] + full[i].n[1] * eng[j].n[1] + full[i].n[2] * eng[j].n[2];
            if (dot > bestdot) { bestdot = dot; best = j; }
        }
        if (best < 0 || bestdot < 0.999f || fabsf(full[i].d - eng[best].d) > 0.5f) {
            st_selfcheck_fail++;
            if (st_selfcheck_fail <= 5)
                cpu_log("photon_cpu waterclip: self-check failed on plane %d (best %d, dot %.5f, d ours %.2f engine %.2f)", i, best, bestdot,
                        full[i].d, best >= 0 ? eng[best].d : 0.0f);
            return;
        }
        map[i] = best;
    }
    float x0, x1, y0, y1;
    /* none of the volume's world boxes on screen: the water drawn may be a brush entity's, so keep the full frustum */
    if (!water_rect(&mc, vol, &x0, &x1, &y0, &y1)) { st_norect++; return; }
    x0 = clampf(x0 - g_margin, -1, 1); x1 = clampf(x1 + g_margin, -1, 1);
    y0 = clampf(y0 - g_margin, -1, 1); y1 = clampf(y1 + g_margin, -1, 1);
    float my0 = y0 < -y1 ? y0 : -y1, my1 = y1 > -y0 ? y1 : -y0;   /* united with its vertical mirror */
    y0 = my0; y1 = my1;
    {   /* debug: PHOTON_CPU_WATERCLIP_FORCE="x0,x1,y0,y1" replaces the rect (does culling follow the planes at all?) */
        static int have = -1;
        static float fr[4];
        if (have < 0) {
            char e[64];
            have = GetEnvironmentVariableA("PHOTON_CPU_WATERCLIP_FORCE", e, sizeof e) > 0 &&
                   sscanf(e, "%f,%f,%f,%f", &fr[0], &fr[1], &fr[2], &fr[3]) == 4;
            if (have) cpu_log("photon_cpu waterclip: DEBUG forced rect %.2f..%.2f x %.2f..%.2f", fr[0], fr[1], fr[2], fr[3]);
        }
        if (have) { x0 = fr[0]; x1 = fr[1]; y0 = fr[2]; y1 = fr[3]; }
    }
    float area = (x1 - x0) * (y1 - y0) / 4;
    static int logged;
    static float last[4];
    int changed = fabsf(last[0] - x0) + fabsf(last[1] - x1) + fabsf(last[2] - y0) + fabsf(last[3] - y1) > 0.01f;
    last[0] = x0; last[1] = x1; last[2] = y0; last[3] = y1;
    if (logged < 200 && changed) {
        logged++;
        cpu_log("photon_cpu waterclip: rect x %.2f..%.2f y %.2f..%.2f (with margin %.2f and mirror), %.1f%% of the screen", x0, x1, y0, y1,
                g_margin, 100.0f * area);
    }
    st_area += area;
    if (area > 0.95f) { st_full++; return; }
    if (mode != 1) return;
    /* the engine's own frustum trim (CS:GO-branch VisOverrideData_t, used by R_SetupAreaBits when r_ClipAreaFrustums is on): four
     * corners whose projection in this view is the rect start the area flood, so the world list, its leaves and the renderables
     * built from them are culled to the rect. waterclip_visdata hands it to this view's BuildWorldLists (slot 12). */
    memset(&g_vis, 0, sizeof g_vis);
    float ox = 0, oy = 0, oz = 0;
    {
        typedef const float *(*OriginFn)(void *self);
        void *irender = *(void **)(engine + 0x4d3e28);
        const float *vo = irender ? ((OriginFn)(*(void ***)irender)[9])(irender) : NULL;   /* IRender slot 9: the current view origin */
        if (vo) { ox = vo[0]; oy = vo[1]; oz = vo[2]; }
        else { ox = vc.o[0]; oy = vc.o[1]; oz = vc.o[2]; }
    }
    g_vis.vis_origin[0] = ox; g_vis.vis_origin[1] = oy; g_vis.vis_origin[2] = oz;   /* what the engine uses without override data */
    g_vis.tolerance = 0.1f;                                                         /* the engine's default (DAT_1803958ac) */
    const float cx[4] = { x0, x1, x1, x0 }, cy[4] = { y0, y0, y1, y1 };
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 3; c++)
            g_vis.corners[k][c] = vc.o[c] + 100.0f * (vc.f[c] + cx[k] * vc.xs * vc.r[c] + cy[k] * vc.ys * vc.u[c]);
    g_vis.trim = 1;
    for (int c = 0; c < 3; c++) { g_vis.portal_origin[c] = vc.o[c] - 1000.0f * vc.f[c]; g_vis.portal_forward[c] = vc.f[c]; }
    g_vis.radius = 0;
    g_vis_pending = 1;
    Plane np[4];
    side_planes(&vc, x0, x1, y0, y1, np);
    for (int i = 0; i < 4; i++) eng[map[i]] = np[i];
    typedef void (*OverrideFn)(void *self, void *frustum);
    ((OverrideFn)rv_vt[43])(rv, frustum);   /* GMod IVRenderView slot 43: OverrideViewFrustum (the engine's culling frustum) */
    st_narrowed++;
}

void waterclip_report(void) {
    if (!st_views) return;
    LONG64 rects = st_views - st_kept_other - st_noworld - st_selfcheck_fail - st_norect;
    cpu_log("photon_cpu waterclip: %lld world lists trimmed", (long long)st_trimmed);
    st_trimmed = 0;
    cpu_log("photon_cpu waterclip: %lld water views: %lld narrowed, %lld kept full (rect >95%%), %lld self-check failures, %lld without "
            "world water, %lld with no box on screen, %lld eye in water or no volume; mean rect %.1f%% of the screen",
            (long long)st_views, (long long)st_narrowed, (long long)st_full, (long long)st_selfcheck_fail, (long long)st_noworld,
            (long long)st_norect, (long long)st_kept_other, rects > 0 ? 100.0 * st_area / (double)rects : 0.0);
    st_views = st_narrowed = st_full = st_selfcheck_fail = st_noworld = st_kept_other = st_norect = 0;
    st_area = 0;
}

/* called by water.c's BuildWorldLists wrapper: the override data for the water view just pushed, once */
const void *waterclip_visdata(void) {
    if (!g_vis_pending) return NULL;
    g_vis_pending = 0;
    st_trimmed++;
    return &g_vis;
}
void waterclip_cancel(void) { g_vis_pending = 0; }
