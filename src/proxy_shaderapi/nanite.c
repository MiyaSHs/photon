/* NANITE N2a (TASKS, analysis/44): static props drawn by Photon on the Vulkan bridge. PHOTON_NANITE=1 (off otherwise). This step draws the
 * map's static props into Photon's own images (colour, the D3D depth as R32F, and a Vulkan depth buffer) from the main view's camera, one
 * instanced draw per model; PHOTON_NANITE_DUMP=N saves draw N's colour as nanite.ppm in PHOTON_LOG_DIR, to check the geometry against the
 * game's frame. Not merged into the frame yet, and the engine still draws its own static props.
 * Worker thread: the props (src/ambient/bsp_props.c) and their models (src/lod/mdl_mesh.c, read through the engine's filesystem), into
 * one scene: vertices of 12 floats (position, normal, colour, 0 x 3), indices per model, placements' rows grouped by model (3D skybox
 * miniatures left out). The device thread uploads it once into host-visible storage buffers and records 1 draw per model each frame. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vlg.h"
#include "nanite.h"
#include "vkbridge.h"
#include "d3dconst.h"
#include "log.h"
#include "lodwork.h"
#include "cvar.h"
#include "dmirror.h"
#include "dynq.h"
#include "../ambient/bsp_props.h"
#include "../ambient/bsp_pak.h"
#include "../ambient/vox_world.h"
#include "../ambient/lg_map.h"
#include "../ambient/sun_vis.h"
#include "../ambient/amb_bake.h"
#include "../lod/mdl_mesh.h"
#include "../lod/mtl_tex.h"

static const uint32_t k_vs[] =
#include "shaders/vk/nanite_props.vert.spv.inc"
;
static const uint32_t k_fs[] =
#include "shaders/vk/nanite_props.frag.spv.inc"
;
long photon_shadows_view(float iv[4][4], float vp[4][4], float cam[3], void **depth, unsigned *w, unsigned *h);   /* shadows.c */

static int g_on = -1;
int photon_nanite_on(void) {
    if (g_on < 0) g_on = photon_vlg_nanite();   /* photon_nanite (vlg.c: env PHOTON_NANITE, settings.txt, photon.cfg), read once at the start */
    return g_on;
}

/* --- the scene ------------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t first_index, index_count; int32_t vertex_offset; uint32_t first_inst, inst_count; int tex, model, lod; } Draw;   /* one model x LOD x material */
enum { NLOD = 8 };
typedef struct {
    float *verts; size_t nverts;          /* 12 floats each: position, normal, colour, uv, 0 */
    uint32_t *idx; size_t nidx;
    float *rows; size_t ninst;            /* 16 floats per placement: 3 matrix rows, then x = its baked colours' base (int bits, minus the
                                           * model's vertex offset, so colour = cols[base + gl_VertexIndex]) */
    float *sphere;                        /* per placement: world centre xyz, radius, fade max (0: none) */
    int nmodels;                          /* draws of model m are consecutive; model m's placements are [first_inst, +inst_count) */
    float (*msw)[NLOD]; uint8_t *mnlod;   /* per model: its LODs' switch points (the engine's metric), how many LODs */
    uint32_t *cols; size_t ncols;         /* the placements' baked vertex light (VRAD's .vhv: B G R A bytes), one run per placement */
    int vhv_found, vhv_missing, dyn_lit, dyn_sunlit;
    Draw *draws; int ndraws;
    MtlTex *tex; int ntex;                /* the materials' base textures (tex[0]: white, for a material without one) */
    float sun[3];
    float fog[4], fogcol[3];              /* the map's env_fog_controller: start, end, max density, on; colour (0..1, gamma) */
    long tris_drawn;                      /* every placement's LOD 0 triangles */
    int tex_missing, alpha_tested;
    float *cast; size_t ncast;            /* PT-RT's sun map: every placement's lowest LOD in world space (15 floats a triangle: position, uv per corner) */
    PhotonCastRange *cranges; int ncranges; /* its ranges: the opaque ones first (no texture), then one per alpha-tested material */
} Scene;
static void scene_free(Scene *s) {
    if (!s) return;
    for (int i = 0; i < s->ntex; i++) mtl_tex_free(&s->tex[i]);
    free(s->cast); free(s->cranges); free(s->tex); free(s->msw); free(s->mnlod); free(s->verts); free(s->idx); free(s->rows); free(s->sphere); free(s->cols); free(s->draws); free(s);
}
/* the map's fog (the first env_fog_controller with fogenable 1 in the entity lump; GMod's Lua can change it at run time, not seen here) */
static void map_fog(const uint8_t *bsp, size_t size, Scene *s) {
    int32_t ofs, len; memcpy(&ofs, bsp + 8, 4); memcpy(&len, bsp + 12, 4);
    if (ofs < 0 || len <= 0 || (size_t)ofs + (size_t)len > size) return;
    const char *e = (const char *)bsp + ofs, *end = e + len;
    while (e < end) {
        const char *o = memchr(e, '{', (size_t)(end - e)), *c = o ? memchr(o, '}', (size_t)(end - o)) : NULL;
        if (!o || !c) break;
        size_t n = (size_t)(c - o);
        char blk[4096];
        e = c + 1;
        if (n >= sizeof blk) continue;
        memcpy(blk, o, n); blk[n] = 0;
        if (!strstr(blk, "\"env_fog_controller\"")) continue;
        const char *k;
        int on = (k = strstr(blk, "\"fogenable\"")) && atoi(k + 12 + strspn(k + 12, " \"")) != 0;
        if (!on) continue;
        float st = 0, en = 0, md = 1; int r = 255, g = 255, b = 255;
        if ((k = strstr(blk, "\"fogstart\""))) sscanf(k + 10, " \"%f", &st);
        if ((k = strstr(blk, "\"fogend\""))) sscanf(k + 8, " \"%f", &en);
        if ((k = strstr(blk, "\"fogmaxdensity\""))) sscanf(k + 15, " \"%f", &md);
        if ((k = strstr(blk, "\"fogcolor\""))) sscanf(k + 10, " \"%d %d %d", &r, &g, &b);
        if (en <= st) continue;
        s->fog[0] = st; s->fog[1] = en; s->fog[2] = md; s->fog[3] = 1;
        s->fogcol[0] = r / 255.0f; s->fogcol[1] = g / 255.0f; s->fogcol[2] = b / 255.0f;
        return;
    }
}

static uint8_t *fs_read(void *ctx, const char *path, size_t *n) { (void)ctx; unsigned u = 0; uint8_t *b = photon_fs_slurp(path, &u); *n = u; return b; }

static Scene *build_scene(const uint8_t *bsp, size_t size, const SunMap *sm) {
    BspProp *pr; int np, ver;
    if (bsp_props_read(bsp, size, &pr, &np, &ver) || !np) return NULL;
    float sb[6];
    int have_sky = vox_skybox_bounds(bsp, size, sm, sb);
    enum { NM = 2048 };
    char (*names)[128] = calloc(NM, sizeof *names);
    int *model = malloc(sizeof *model * (size_t)np), nm = 0, nskinned = 0;
    int *skins = calloc(NM, sizeof *skins);
    Scene *s = calloc(1, sizeof *s);
    MdlMesh *mesh = calloc(NM, sizeof *mesh);
    uint8_t *ok = calloc(NM, 1);
    if (!names || !model || !s || !mesh || !ok || !skins) goto fail;
    for (int i = 0; i < np; i++) {   /* each placement's model, loaded once per skin it is placed with (the skin picks the materials) */
        const BspProp *P = &pr[i];
        model[i] = -1;
        if (have_sky && P->origin[0] >= sb[0] && P->origin[0] <= sb[3] && P->origin[1] >= sb[1] && P->origin[1] <= sb[4] &&
            P->origin[2] >= sb[2] && P->origin[2] <= sb[5]) continue;
        int k;
        for (k = 0; k < nm && (skins[k] != P->skin || strcmp(names[k], P->model)); k++) {}
        if (k == nm) {
            if (nm >= NM) continue;
            snprintf(names[nm], sizeof names[nm], "%s", P->model); skins[nm] = P->skin; nskinned += P->skin != 0;
            char base[128], p[160];
            size_t L = strlen(P->model);
            snprintf(base, sizeof base, "%.*s", (int)(L > 4 ? L - 4 : L), P->model);
            unsigned a = 0, b = 0, c = 0;
            snprintf(p, sizeof p, "%s.mdl", base); uint8_t *m = photon_fs_slurp(p, &a);
            snprintf(p, sizeof p, "%s.vvd", base); uint8_t *v = m ? photon_fs_slurp(p, &b) : NULL;
            snprintf(p, sizeof p, "%s.dx90.vtx", base); uint8_t *x = v ? photon_fs_slurp(p, &c) : NULL;
            ok[nm] = m && v && x && mdl_mesh_load(m, a, v, b, x, c, P->skin, &mesh[nm]) == 0;
            free(m); free(v); free(x);
            nm++;
        }
        if (ok[k]) model[i] = k;
    }
    /* materials: each distinct name once (tex[0] white for a material without a base texture) */
    enum { NT = 1024 };
    char (*tname)[128] = calloc(NT, sizeof *tname);
    s->tex = calloc(NT, sizeof *s->tex);
    if (!tname || !s->tex) { free(tname); goto fail; }
    s->ntex = 1;
    s->tex[0].format = MTL_RGBA8; s->tex[0].w = s->tex[0].h = 4; s->tex[0].mips = 1; s->tex[0].size = 64; s->tex[0].data = malloc(64);
    if (s->tex[0].data) memset(s->tex[0].data, 200, 64);
    /* sizes, then the arrays: models in order, each model's triangles grouped by material, placements grouped by model */
    size_t nv = 0, ni = 0, nin = 0;
    int nd = 0;
    for (int k = 0; k < nm; k++) if (ok[k]) { nv += (size_t)mesh[k].nv; ni += 3 * (size_t)mesh[k].nt; nd += mesh[k].nnames * mesh[k].nlods; }
    for (int i = 0; i < np; i++) if (model[i] >= 0) nin++;
    s->verts = malloc(sizeof(float) * 12 * (nv ? nv : 1)); s->idx = malloc(sizeof(uint32_t) * (ni ? ni : 1));
    s->rows = malloc(sizeof(float) * 16 * (nin ? nin : 1)); s->draws = calloc((size_t)(nd ? nd : 1), sizeof *s->draws);
    s->sphere = malloc(sizeof(float) * 5 * (nin ? nin : 1));
    s->msw = calloc((size_t)(nm ? nm : 1), sizeof *s->msw); s->mnlod = calloc((size_t)(nm ? nm : 1), 1);
    size_t ncol = 1;   /* entry 0: the light for a placement without baked colours */
    for (int i = 0; i < np; i++) if (model[i] >= 0) ncol += (size_t)mesh[model[i]].nv + 21;
    s->cols = malloc(sizeof(uint32_t) * ncol);
    if (!s->verts || !s->idx || !s->rows || !s->draws || !s->cols || !s->sphere || !s->msw || !s->mnlod) { free(tname); goto fail; }
    s->cols[0] = 0xff606060u; s->ncols = 1;
    /* a placement without baked light (a model not compiled as a static prop: VRAD gives it no .vhv) is lit as the engine lights it, like
     * a dynamic model: the leaf ambient cube at its lighting origin plus the sun when the engine's model test sees it (the strongest
     * light outdoors; local lights are not added yet). The sun from the map's light_environment (dworldlight emit_skylight). */
    AmbMap *am = NULL;
    if (amb_map_load(bsp, size, 1, &am)) am = NULL;
    float sun_col[3] = { 0, 0, 0 };
    s->sun[2] = 1;
    LgMap *lm = NULL;
    if (!lg_map_load(bsp, size, 1, &lm)) {
        int nl = 0; const LgWorldLight *wl = lg_map_lights(lm, &nl);
        for (int i = 0; i < nl; i++) if (wl[i].type == LG_EMIT_SKYLIGHT) for (int k = 0; k < 3; k++) { s->sun[k] = -wl[i].normal[k]; sun_col[k] = wl[i].intensity[k]; }
        lg_map_free(lm);
    }
    SunScratch *ss = sun_scratch_new(sm);
    for (int k = 0; k < nm; k++) {
        if (!ok[k]) continue;
        const MdlMesh *M = &mesh[k];
        uint32_t first_inst = (uint32_t)s->ninst, inst_count = 0;
        for (int i = 0; i < np; i++) {   /* the placements first: the model's draws share them */
            if (model[i] != k) continue;
            double X[3][4];
            bsp_prop_matrix(&pr[i], X);
            float *sp = s->sphere + 5 * s->ninst;   /* the model's box centre in the world, the box's half diagonal, the fade end */
            float lc[3], rad = 0;
            for (int a = 0; a < 3; a++) { lc[a] = 0.5f * (M->mins[a] + M->maxs[a]); rad += 0.25f * (M->maxs[a] - M->mins[a]) * (M->maxs[a] - M->mins[a]); }
            for (int a = 0; a < 3; a++) sp[a] = (float)(X[a][0] * lc[0] + X[a][1] * lc[1] + X[a][2] * lc[2] + X[a][3]);
            sp[3] = sqrtf(rad); sp[4] = pr[i].fade_max > 0 ? pr[i].fade_max : 0;
            float *r = s->rows + 16 * s->ninst++;
            for (int a = 0; a < 3; a++) for (int b = 0; b < 4; b++) r[4 * a + b] = (float)X[a][b];
            /* its baked light: VRAD's sp_hdr_<i>.vhv (else sp_<i>.vhv) in the map's pakfile, LOD 0's meshes in vertex order */
            int32_t base = -(int32_t)s->nverts;   /* colour index = base + gl_VertexIndex -> entry 0 if none (set below) */
            char vn[48]; size_t vl = 0;
            snprintf(vn, sizeof vn, "sp_hdr_%d.vhv", i);
            const uint8_t *vh = bsp_pak_find(bsp, size, vn, &vl);
            if (!vh) { snprintf(vn, sizeof vn, "sp_%d.vhv", i); vh = bsp_pak_find(bsp, size, vn, &vl); }
            int got = 0;
            if (vh && vl >= 40) {   /* FileHeader_t is 40 bytes (6 ints + 4 unused), MeshHeader_t 28 */
                int32_t ver, vsize, nmesh; memcpy(&ver, vh, 4); memcpy(&vsize, vh + 12, 4); memcpy(&nmesh, vh + 20, 4);
                if (ver == 2 && vsize == 4 && nmesh > 0 && 40 + (size_t)nmesh * 28 <= vl) {
                    /* a LOD's entries in file order are that LOD's strip groups (mdl_mesh.c group_*, LOD by LOD): entry k of LOD L's colour
                     * j belongs to model vertex group_map[group_first[g] + j], g the k-th group of LOD L; LOD 0 must match whole, a lower
                     * LOD that does not match keeps the fallback for the vertices only it uses */
                    size_t at = s->ncols;
                    for (int v = 0; v < M->nv; v++) s->cols[at + v] = s->cols[0];
                    int next[NLOD + 1], okl[NLOD];   /* per LOD: its next group, whether it still matches */
                    for (int L = 0; L < NLOD; L++) { okl[L] = 1; next[L] = -1; }
                    for (int g = M->ngroups - 1; g >= 0; g--) if (M->group_lod[g] < NLOD) next[M->group_lod[g]] = g;
                    for (int m = 0; m < nmesh && okl[0]; m++) {
                        uint32_t lod, nvx, off;
                        memcpy(&lod, vh + 40 + 28 * m, 4); memcpy(&nvx, vh + 40 + 28 * m + 4, 4); memcpy(&off, vh + 40 + 28 * m + 8, 4);
                        if (lod >= NLOD || !okl[lod]) continue;
                        int g = next[lod];
                        if (g < 0 || g >= M->ngroups || M->group_lod[g] != (int32_t)lod || (int32_t)nvx != M->group_nv[g] || off + 4ull * nvx > vl) {
                            okl[lod] = 0; continue;
                        }
                        for (uint32_t j = 0; j < nvx; j++) {
                            int32_t v = M->group_map[M->group_first[g] + (int32_t)j];
                            if (v >= 0 && v < M->nv) memcpy(&s->cols[at + v], vh + off + 4 * j, 4);
                        }
                        next[lod] = g + 1;
                    }
                    int lod0_done = okl[0] && (next[0] == M->ngroups || (next[0] >= 0 && M->group_lod[next[0]] != 0));
                    if (lod0_done) { got = 1; base = (int32_t)at - (int32_t)s->nverts; s->ncols += (size_t)M->nv; }
                }
            }
            if (got) s->vhv_found++; else s->vhv_missing++;
            int32_t enc = got ? base : INT32_MIN;   /* INT32_MIN: no baked light (the shader takes cols[0]) */
            memcpy(&r[12], &enc, 4); r[15] = 0;
            if (!got && am) {   /* row 3 w = 1 + where its 21 floats start in cols: the cube's 6 faces (+x -x +y -y +z -z) rgb, the sun's rgb */
                float cube[AMB_VOXEL_FLOATS] = { 0 };
                double c[3] = { sp[0], sp[1], sp[2] };
                amb_eval_engine(am, c, 1.0, cube);
                int lit = sun_col[0] + sun_col[1] + sun_col[2] > 0 && sun_model_visible(sm, ss, c) == SUN_LIT;
                size_t at = s->ncols;
                memcpy(&s->cols[at], cube, sizeof cube);
                for (int k = 0; k < 3; k++) { float v = lit ? sun_col[k] : 0; memcpy(&s->cols[at + 18 + k], &v, 4); }
                s->ncols += 21;
                int32_t dyn = (int32_t)at + 1;
                memcpy(&r[15], &dyn, 4);
                s->dyn_lit++; s->dyn_sunlit += lit;
            }
            /* the fade range (the engine fades a static prop out between its fade min and max distance; min <= 0 or >= max: no fade start,
             * the prop drops at max) */
            r[13] = pr[i].fade_max > 0 && pr[i].fade_min > 0 && pr[i].fade_min < pr[i].fade_max ? pr[i].fade_min : pr[i].fade_max;
            r[14] = pr[i].fade_max > 0 ? pr[i].fade_max : 0;
            inst_count++;
        }
        int32_t vbase = (int32_t)s->nverts;   /* the placements above computed their colour bases against this */
        for (int v = 0; v < M->nv; v++) {
            float *o = s->verts + 12 * (s->nverts + (size_t)v);
            memcpy(o, M->pos[v], 12); memcpy(o + 3, M->nrm[v], 12);
            o[6] = o[7] = o[8] = 1.0f; memcpy(o + 9, M->uv[v], 8); o[11] = 0;
        }
        s->nverts += (size_t)M->nv;
        s->mnlod[s->nmodels] = (uint8_t)(M->nlods < NLOD ? M->nlods : NLOD);
        for (int l = 0; l < s->mnlod[s->nmodels]; l++) s->msw[s->nmodels][l] = M->switch_pt[l];
        for (int l = 0; l < s->mnlod[s->nmodels]; l++)
        for (int m = 0; m < M->nnames; m++) {   /* one draw per LOD and material the model uses */
            uint32_t first = (uint32_t)s->nidx;
            for (int t = M->lod_first[l]; t < M->lod_first[l + 1]; t++) if (M->mat[t] == m) for (int c = 0; c < 3; c++) s->idx[s->nidx++] = (uint32_t)M->tri[t][c];
            if ((uint32_t)s->nidx == first) continue;
            int ti;
            for (ti = 1; ti < s->ntex && strcmp(tname[ti], M->names[m]); ti++) {}
            if (ti == s->ntex && s->ntex < NT) {
                snprintf(tname[ti], sizeof tname[ti], "%s", M->names[m]);
                if (mtl_tex_load(M->names[m], fs_read, NULL, 512, &s->tex[ti])) {   /* none: a white 4x4 of its own (never a 0 x 0 image) */
                    s->tex_missing++;
                    MtlTex *W = &s->tex[ti];
                    memset(W, 0, sizeof *W);
                    W->format = MTL_RGBA8; W->w = W->h = 4; W->mips = 1; W->size = 64; W->data = malloc(64);
                    if (W->data) memset(W->data, 200, 64); else W->size = 0;
                }
                else s->alpha_tested += s->tex[ti].alphatest;
                s->ntex++;
            }
            Draw *D = &s->draws[s->ndraws++];
            D->vertex_offset = vbase; D->first_index = first; D->index_count = (uint32_t)s->nidx - first;
            D->first_inst = first_inst; D->inst_count = inst_count; D->model = s->nmodels; D->lod = l;
            D->tex = ti < s->ntex && s->tex[ti].format && s->tex[ti].size ? ti : 0;
        }
        s->tris_drawn += (long)M->lod_first[1] * inst_count;
        s->nmodels++;
    }
    free(tname);
    {   /* the sun map's casters (PT-RT: with Nanite on the engine draws no static prop, so none is recorded to cast): each placement's
         * lowest LOD in world space with its uv, the opaque materials first, then each alpha-tested material as its own range (the caster
         * cuts its leaves by the texture's alpha), at most 3 M triangles */
        size_t want = 0;
        for (int d = 0; d < s->ndraws; d++) { const Draw *D = &s->draws[d]; if (D->lod == s->mnlod[D->model] - 1) want += (size_t)(D->index_count / 3) * D->inst_count; }
        if (want > 3000000) want = 3000000;
        s->cast = malloc(sizeof(float) * 15 * (want ? want : 1));
        s->cranges = calloc((size_t)s->ntex + 1, sizeof *s->cranges);
        for (int pass = -1; s->cast && s->cranges && pass < s->ntex; pass++) {   /* -1: the opaque materials; t: alpha-tested material t */
            if (pass >= 0 && !s->tex[pass].alphatest) continue;
            size_t first = s->ncast;
            for (int d = 0; d < s->ndraws; d++) {
                const Draw *D = &s->draws[d];
                if (D->lod != s->mnlod[D->model] - 1) continue;
                int at = s->tex[D->tex].alphatest;
                if (pass < 0 ? at : D->tex != pass) continue;
                for (uint32_t i = D->first_inst; i < D->first_inst + D->inst_count; i++) {
                    const float *r = s->rows + 16 * i;
                    for (uint32_t t = 0; t + 2 < D->index_count && s->ncast < want; t += 3) {
                        float *o = s->cast + 15 * s->ncast++;
                        for (int c = 0; c < 3; c++) {
                            const float *v = s->verts + 12 * (size_t)(D->vertex_offset + (int32_t)s->idx[D->first_index + t + c]);
                            for (int a = 0; a < 3; a++) o[5 * c + a] = r[4 * a] * v[0] + r[4 * a + 1] * v[1] + r[4 * a + 2] * v[2] + r[4 * a + 3];
                            o[5 * c + 3] = v[9]; o[5 * c + 4] = v[10];
                        }
                    }
                }
            }
            if (s->ncast == first) continue;
            PhotonCastRange *cr = &s->cranges[s->ncranges++];
            cr->first = (unsigned)first; cr->count = (unsigned)(s->ncast - first);
            if (pass >= 0) {
                const MtlTex *T = &s->tex[pass];
                cr->data = T->data; cr->format = T->format; cr->w = T->w; cr->h = T->h; cr->mips = T->mips; cr->mip_ofs = T->mip_ofs; cr->key = pass + 1;
            }
        }
        photon_log("nanite: %zu triangles (the placements' lowest LODs) for PT-RT's sun map, %d ranges (%d alpha-tested)", s->ncast, s->ncranges,
                   s->ncranges > 0 ? s->ncranges - (s->cranges[0].data == NULL) : 0);
    }
    for (int k = 0; k < nm; k++) if (ok[k]) mdl_mesh_free(&mesh[k]);
    sun_scratch_free(ss); amb_map_free(am);
    photon_log("nanite: %d placements without baked light lit as the engine lights models (leaf ambient cube + sun: %d in the sun; sun %.3f %.3f %.3f)",
               s->dyn_lit, s->dyn_sunlit, sun_col[0], sun_col[1], sun_col[2]);
    if (nskinned) photon_log("nanite: %d of %d models placed with a non-default skin", nskinned, nm);
    free(names); free(skins); free(model); free(mesh); free(ok); free(pr);
    return s;
fail:
    if (mesh) for (int k = 0; k < nm; k++) if (ok && ok[k]) mdl_mesh_free(&mesh[k]);
    free(names); free(skins); free(model); free(mesh); free(ok); free(pr); scene_free(s);
    return NULL;
}

/* --- worker ---------------------------------------------------------------------------------------------------------------------- */
static SRWLOCK g_l = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static char g_want[260], g_key[260];
static const AmbWorkFiles *g_files;
static volatile int g_cancel;
static Scene *g_ready_scene;   /* published, under g_l */
static int g_started;

static DWORD WINAPI worker(LPVOID u) {
    (void)u;
    char level[260] = "";
    for (;;) {
        AcquireSRWLockExclusive(&g_l);
        while (!strcmp(g_want, level)) SleepConditionVariableSRW(&g_cv, &g_l, INFINITE, 0);
        char want[260]; memcpy(want, g_want, sizeof want);
        const AmbWorkFiles *files = g_files;
        ReleaseSRWLockExclusive(&g_l);
        snprintf(level, sizeof level, "%s", want);
        if (!level[0] || !files) continue;
        void *pin = files->pin ? files->pin() : NULL;
        int gma = 0;
        void *fh = files->open(level, &gma, &g_cancel);
        uint8_t *bsp = NULL; long long size = 0;
        if (fh) {
            size = files->size(fh);
            if (size > 1036 && size < (1LL << 30) && (bsp = malloc((size_t)size)) != NULL)
                for (long long o = 0; o < size;) {
                    int n = (int)(size - o > (1 << 20) ? (1 << 20) : size - o);
                    if (files->read_at(fh, o, bsp + o, n) != n) { free(bsp); bsp = NULL; break; }
                    o += n;
                }
            files->close(fh);
        }
        if (pin && files->unpin) files->unpin(pin);
        if (!bsp) { photon_log("nanite: %s: the map could not be read", level); continue; }
        SunMap *sm = NULL;
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        Scene *s = NULL;
        if (!sun_map_load(bsp, (size_t)size, 1, &sm)) s = build_scene(bsp, (size_t)size, sm);
        if (s) map_fog(bsp, (size_t)size, s);
        QueryPerformanceCounter(&t1);
        sun_map_free(sm); free(bsp);
        if (!s) { photon_log("nanite: %s: no static props", level); continue; }
        size_t tbytes = 0; for (int i = 0; i < s->ntex; i++) tbytes += s->tex[i].size;
        photon_log("nanite: %s: %zu placements, %d draws (model x material), %zu vertices, %zu indices, %ld triangles per frame drawn; %d textures "
                   "(%.1f MiB, %d missing, %d alpha-tested); built in %.0f ms", level, s->ninst, s->ndraws, s->nverts, s->nidx, s->tris_drawn, s->ntex,
                   tbytes / 1048576.0, s->tex_missing, s->alpha_tested, (double)(t1.QuadPart - t0.QuadPart) * 1e3 / (double)f.QuadPart);
        AcquireSRWLockExclusive(&g_l);
        scene_free(g_ready_scene); g_ready_scene = s;
        ReleaseSRWLockExclusive(&g_l);
    }
    return 0;
}
void photon_nanite_frame(const AmbWorkFiles *files, const char *level) {
    if (!photon_nanite_on()) return;
    if (!level) level = "";
    if (!g_started && level[0]) {
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!h) return;
        SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(h);
        g_started = 1;
    }
    if (!strcmp(level, g_key)) return;
    snprintf(g_key, sizeof g_key, "%s", level);
    AcquireSRWLockExclusive(&g_l);
    snprintf(g_want, sizeof g_want, "%s", level); g_files = files;
    WakeAllConditionVariable(&g_cv);
    ReleaseSRWLockExclusive(&g_l);
}

/* --- the bridge pass ------------------------------------------------------------------------------------------------------------- */
static Scene *g_scene;   /* the uploaded one (device thread) */
static int g_albedo;     /* PT-RT: props drawn with their surface colour alone (its composite lights them) */
void photon_nanite_set_albedo(int on) { g_albedo = on; }
void *photon_nanite_depth_tex(void);
static long g_scene_seq;
const float *photon_nanite_casters(unsigned *n, const PhotonCastRange **r, int *nr, long *seq) {
    *seq = g_scene_seq; *n = g_scene ? (unsigned)g_scene->ncast : 0; *r = g_scene ? g_scene->cranges : NULL; *nr = g_scene ? g_scene->ncranges : 0;
    return g_scene ? g_scene->cast : NULL;
}
static PhotonVkImage g_col, g_dep; static PhotonVkImage *g_imgs[2] = { &g_col, &g_dep };
static VkImage g_zimg; static VkDeviceMemory g_zmem; static VkImageView g_zview;
static VkBuffer g_vb, g_ib, g_ins, g_cols; static VkDeviceMemory g_vbm, g_ibm, g_insm, g_colsm;
enum { NVIS = 4 };   /* the visible placements' ring: one per frame, 4 > the bridge's frames in flight */
static VkBuffer g_vis[NVIS]; static VkDeviceMemory g_vism[NVIS]; static uint32_t *g_vismap[NVIS]; static VkDescriptorSet g_visset[NVIS];
static uint32_t *g_mfirst, *g_mcount;   /* indexed by model x NLOD + LOD */
static uint8_t *g_lodof;                 /* per placement, this frame: its LOD, 255 culled */
static long long g_tri_sum;   /* per model, this frame: where its visible placements start in the list, how many */
static long long g_vis_sum, g_vis_frames;
static VkRenderPass g_rp; static VkFramebuffer g_fb; static VkPipelineLayout g_pl; static VkPipeline g_pipe;
static VkDescriptorSetLayout g_dsl, g_tsl; static VkDescriptorPool g_dpool;
static VkSampler g_samp; static VkImage *g_timg; static VkDeviceMemory *g_tmem; static VkImageView *g_tview; static VkDescriptorSet *g_tset;
static VkBuffer g_stage; static VkDeviceMemory g_stagem; static int g_uploaded;
static float g_vp[4][4], g_eye[3];
static float g_c29[4], g_c30[4];   /* the frame's stock constants at the main view's end: linear fog colour, light scale (common_ps_fxc.h) */
static unsigned g_w, g_h;
static int g_ready;
static long long g_draws;
#define PF(n) static PFN_##n p##n
PF(vkCmdPipelineBarrier); PF(vkCmdCopyBufferToImage);
PF(vkCmdBeginRenderPass); PF(vkCmdEndRenderPass); PF(vkCmdBindPipeline); PF(vkCmdBindDescriptorSets); PF(vkCmdPushConstants);
PF(vkCmdBindIndexBuffer); PF(vkCmdDrawIndexed); PF(vkCmdSetViewport); PF(vkCmdSetScissor);

static int buffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, const void *data) {
    const PhotonVk *vk = photon_vk();
    PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)photon_vk_proc("vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements gr = (PFN_vkGetBufferMemoryRequirements)photon_vk_proc("vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)photon_vk_proc("vkAllocateMemory");
    PFN_vkBindBufferMemory bm = (PFN_vkBindBufferMemory)photon_vk_proc("vkBindBufferMemory");
    PFN_vkMapMemory mm = (PFN_vkMapMemory)photon_vk_proc("vkMapMemory");
    PFN_vkUnmapMemory um = (PFN_vkUnmapMemory)photon_vk_proc("vkUnmapMemory");
    if (!cb || !gr || !am || !bm || !mm || !um) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (cb(vk->dev, &bi, NULL, b)) return 0;
    VkMemoryRequirements mr; gr(vk->dev, *b, &mr);
    uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == ~0u) return 0;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    void *map;
    if (am(vk->dev, &ai, NULL, m) || bm(vk->dev, *b, *m, 0) || mm(vk->dev, *m, 0, VK_WHOLE_SIZE, 0, &map)) return 0;
    memcpy(map, data, (size_t)size);
    um(vk->dev, *m);
    return 1;
}

static int buffer_mapped(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    const PhotonVk *vk = photon_vk();
    PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)photon_vk_proc("vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements gr = (PFN_vkGetBufferMemoryRequirements)photon_vk_proc("vkGetBufferMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)photon_vk_proc("vkAllocateMemory");
    PFN_vkBindBufferMemory bm = (PFN_vkBindBufferMemory)photon_vk_proc("vkBindBufferMemory");
    PFN_vkMapMemory mm = (PFN_vkMapMemory)photon_vk_proc("vkMapMemory");
    if (!cb || !gr || !am || !bm || !mm) return 0;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (cb(vk->dev, &bi, NULL, b)) return 0;
    VkMemoryRequirements mr; gr(vk->dev, *b, &mr);
    uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    return mt != ~0u && !am(vk->dev, &ai, NULL, m) && !bm(vk->dev, *b, *m, 0) && !mm(vk->dev, *m, 0, VK_WHOLE_SIZE, 0, map);
}

/* sRGB: the stock shaders read base textures as sRGB (linear light x linear texture, then encoded for the frame) */
static VkFormat vk_fmt(int f) { return f == MTL_BC1 ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : f == MTL_BC2 ? VK_FORMAT_BC2_SRGB_BLOCK : f == MTL_BC3 ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_R8G8B8A8_SRGB; }
/* every texture as a device-local image with its mips, filled from one staging buffer in the first frame's commands (record), and its
 * descriptor set */
static int make_textures(Scene *s) {
    const PhotonVk *vk = photon_vk();
    PFN_vkCreateImage ci = (PFN_vkCreateImage)photon_vk_proc("vkCreateImage");
    PFN_vkGetImageMemoryRequirements gr = (PFN_vkGetImageMemoryRequirements)photon_vk_proc("vkGetImageMemoryRequirements");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)photon_vk_proc("vkAllocateMemory");
    PFN_vkBindImageMemory bm = (PFN_vkBindImageMemory)photon_vk_proc("vkBindImageMemory");
    PFN_vkCreateImageView cv = (PFN_vkCreateImageView)photon_vk_proc("vkCreateImageView");
    PFN_vkCreateDescriptorPool cp = (PFN_vkCreateDescriptorPool)photon_vk_proc("vkCreateDescriptorPool");
    PFN_vkAllocateDescriptorSets as = (PFN_vkAllocateDescriptorSets)photon_vk_proc("vkAllocateDescriptorSets");
    PFN_vkUpdateDescriptorSets ud = (PFN_vkUpdateDescriptorSets)photon_vk_proc("vkUpdateDescriptorSets");
    if (!ci || !gr || !am || !bm || !cv || !cp || !as || !ud) return 0;
    int n = s->ntex;
    g_timg = calloc((size_t)n, sizeof *g_timg); g_tmem = calloc((size_t)n, sizeof *g_tmem); g_tview = calloc((size_t)n, sizeof *g_tview);
    g_tset = calloc((size_t)n, sizeof *g_tset);
    if (!g_timg || !g_tmem || !g_tview || !g_tset) return 0;
    size_t total = 0;
    for (int i = 0; i < n; i++) total += s->tex[i].size;
    uint8_t *all = malloc(total ? total : 1);
    if (!all) return 0;
    size_t at = 0;
    for (int i = 0; i < n; i++) { if (s->tex[i].data) memcpy(all + at, s->tex[i].data, s->tex[i].size); at += s->tex[i].size; }
    int ok = buffer(total ? total : 1, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_stage, &g_stagem, all);
    free(all);
    if (!ok) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)n };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, (uint32_t)n, 1, &ps };
    VkDescriptorPool pool;
    if (cp(vk->dev, &dpi, NULL, &pool)) return 0;
    for (int i = 0; i < n; i++) {
        const MtlTex *T = &s->tex[i];
        if (!T->format || T->w < 1 || T->h < 1 || T->mips < 1 || !T->size) { photon_log("nanite: texture %d is empty", i); return 0; }
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, vk_fmt(T->format), { (uint32_t)T->w, (uint32_t)T->h, 1 },
                                 (uint32_t)T->mips, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
        if (ci(vk->dev, &ii, NULL, &g_timg[i])) return 0;
        VkMemoryRequirements mr; gr(vk->dev, g_timg[i], &mr);
        uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
        if (mt == ~0u || am(vk->dev, &ai, NULL, &g_tmem[i]) || bm(vk->dev, g_timg[i], g_tmem[i], 0)) return 0;
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_timg[i], VK_IMAGE_VIEW_TYPE_2D, vk_fmt(T->format), { 0 },
                                      { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)T->mips, 0, 1 } };
        if (cv(vk->dev, &vci, NULL, &g_tview[i])) return 0;
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &g_tsl };
        if (as(vk->dev, &dai, &g_tset[i])) return 0;
        VkDescriptorImageInfo dii = { g_samp, g_tview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_tset[i], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, NULL, NULL };
        ud(vk->dev, 1, &w, 0, NULL);
    }
    return 1;
}
/* the first frame: the staging buffer into every texture's mips, then each image to SHADER_READ_ONLY */
static void upload_textures(VkCommandBuffer cb) {
    size_t at = 0;
    for (int i = 0; i < g_scene->ntex; i++) {
        const MtlTex *T = &g_scene->tex[i];
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)T->mips, 0, 1 };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_timg[i], rng };
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy cp[16];
        for (int m = 0; m < T->mips && m < 16; m++) {
            uint32_t mw = (uint32_t)(T->w >> m), mh = (uint32_t)(T->h >> m);
            cp[m] = (VkBufferImageCopy){ at + T->mip_ofs[m], 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, 1 }, { 0, 0, 0 }, { mw ? mw : 1, mh ? mh : 1, 1 } };
        }
        pvkCmdCopyBufferToImage(cb, g_stage, g_timg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)(T->mips < 16 ? T->mips : 16), cp);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        at += T->size;
    }
    g_uploaded = 1;
}

static int setup(void *u) {
    (void)u;
    const PhotonVk *vk = photon_vk();
#define P(n) PFN_##n n = (PFN_##n)photon_vk_proc(#n); if (!n) return 0
    P(vkCreateDescriptorSetLayout); P(vkCreateDescriptorPool); P(vkAllocateDescriptorSets); P(vkCreatePipelineLayout); P(vkCreateShaderModule);
    P(vkCreateGraphicsPipelines); P(vkCreateRenderPass);
#define L(n) if (!(p##n = (PFN_##n)photon_vk_proc(#n))) return 0
    L(vkCmdBeginRenderPass); L(vkCmdEndRenderPass); L(vkCmdBindPipeline); L(vkCmdBindDescriptorSets); L(vkCmdPushConstants);
    L(vkCmdBindIndexBuffer); L(vkCmdDrawIndexed); L(vkCmdSetViewport); L(vkCmdSetScissor); L(vkCmdPipelineBarrier); L(vkCmdCopyBufferToImage);
    VkDescriptorSetLayoutBinding b[4] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
                                          { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL },
                                          { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b };
    if (vkCreateDescriptorSetLayout(vk->dev, &dli, NULL, &g_dsl)) return 0;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * NVIS };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, NVIS, 1, &ps };
    if (vkCreateDescriptorPool(vk->dev, &dpi, NULL, &g_dpool)) return 0;
    VkDescriptorSetLayout vl[NVIS] = { g_dsl, g_dsl, g_dsl, g_dsl };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, NVIS, vl };
    if (vkAllocateDescriptorSets(vk->dev, &dai, g_visset)) return 0;
    VkDescriptorSetLayoutBinding tb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo tli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &tb };
    if (vkCreateDescriptorSetLayout(vk->dev, &tli, NULL, &g_tsl)) return 0;
    P(vkCreateSampler);
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
                               VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, VK_FALSE, 1, VK_FALSE,
                               VK_COMPARE_OP_ALWAYS, 0, 16, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_FALSE };
    if (vkCreateSampler(vk->dev, &si, NULL, &g_samp)) return 0;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };
    VkDescriptorSetLayout sets[2] = { g_dsl, g_tsl };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, sets, 1, &pcr };
    if (vkCreatePipelineLayout(vk->dev, &pli, NULL, &g_pl)) return 0;
    /* the render pass: colour (RGBA8, the D3D9 texture), the D3D depth (R32F, the D3D9 texture), a Vulkan depth buffer */
    VkAttachmentDescription at[3] = {
        { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL },
        { 0, VK_FORMAT_R32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL },
        { 0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
    VkAttachmentReference cr[2] = { { 0, VK_IMAGE_LAYOUT_GENERAL }, { 1, VK_IMAGE_LAYOUT_GENERAL } }, dr = { 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 2, cr, NULL, &dr, 0, NULL };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 3, at, 1, &sp, 0, NULL };
    if (vkCreateRenderPass(vk->dev, &rpi, NULL, &g_rp)) return 0;
    VkShaderModule vm, fm;
    VkShaderModuleCreateInfo vmi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof k_vs, k_vs }, fmi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof k_fs, k_fs };
    if (vkCreateShaderModule(vk->dev, &vmi, NULL, &vm) || vkCreateShaderModule(vk->dev, &fmi, NULL, &fm)) return 0;
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
                                                  VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL,
                                                 VK_FALSE, VK_FALSE, { 0 }, { 0 }, 0, 1 };
    VkPipelineColorBlendAttachmentState cba[2] = { { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf }, { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf } };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 2, cba, { 0, 0, 0, 0 } };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, &ds, &cb, &dy,
                                         g_pl, g_rp, 0, VK_NULL_HANDLE, -1 };
    if (vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gpi, NULL, &g_pipe)) return 0;
    photon_log("nanite: pass ready");
    return 1;
}

/* the targets at the scene's size, the framebuffer, and the scene's buffers (once) */
static int prepare(unsigned w, unsigned h) {
    const PhotonVk *vk = photon_vk();
    if (!g_col.tex) {
        PFN_vkCreateImage ci = (PFN_vkCreateImage)photon_vk_proc("vkCreateImage");
        PFN_vkGetImageMemoryRequirements gr = (PFN_vkGetImageMemoryRequirements)photon_vk_proc("vkGetImageMemoryRequirements");
        PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)photon_vk_proc("vkAllocateMemory");
        PFN_vkBindImageMemory bm = (PFN_vkBindImageMemory)photon_vk_proc("vkBindImageMemory");
        PFN_vkCreateImageView cv = (PFN_vkCreateImageView)photon_vk_proc("vkCreateImageView");
        PFN_vkCreateFramebuffer cf = (PFN_vkCreateFramebuffer)photon_vk_proc("vkCreateFramebuffer");
        if (!ci || !gr || !am || !bm || !cv || !cf) return 0;
        if (!photon_vk_image(w, h, D3DFMT_A8B8G8R8, D3DUSAGE_RENDERTARGET, &g_col) || !photon_vk_image(w, h, D3DFMT_R32F, D3DUSAGE_RENDERTARGET, &g_dep)) return 0;
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
        if (ci(vk->dev, &ii, NULL, &g_zimg)) return 0;
        VkMemoryRequirements mr; gr(vk->dev, g_zimg, &mr);
        uint32_t mt = photon_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
        if (mt == ~0u || am(vk->dev, &ai, NULL, &g_zmem) || bm(vk->dev, g_zimg, g_zmem, 0)) return 0;
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_zimg, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D32_SFLOAT, { 0 },
                                      { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
        if (cv(vk->dev, &vci, NULL, &g_zview)) return 0;
        VkImageView views[3] = { g_col.view, g_dep.view, g_zview };
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_rp, 3, views, w, h, 1 };
        if (cf(vk->dev, &fi, NULL, &g_fb)) return 0;
        g_w = w; g_h = h;
    }
    if (w != g_w || h != g_h) return 0;   /* v0: no resize */
    if (!g_scene) {
        AcquireSRWLockExclusive(&g_l);
        Scene *s = g_ready_scene; g_ready_scene = NULL;
        ReleaseSRWLockExclusive(&g_l);
        if (!s) return 0;
        if (!buffer(sizeof(float) * 12 * s->nverts, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_vb, &g_vbm, s->verts) ||
            !buffer(sizeof(uint32_t) * s->nidx, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g_ib, &g_ibm, s->idx) ||
            !buffer(sizeof(float) * 16 * s->ninst, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_ins, &g_insm, s->rows) ||
            !buffer(sizeof(uint32_t) * s->ncols, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_cols, &g_colsm, s->cols)) { scene_free(s); return 0; }
        size_t ng = (size_t)(s->nmodels ? s->nmodels : 1) * NLOD;
        g_mfirst = calloc(ng, sizeof *g_mfirst); g_mcount = calloc(ng, sizeof *g_mcount); g_lodof = malloc(s->ninst ? s->ninst : 1);
        if (!g_mfirst || !g_mcount || !g_lodof) { scene_free(s); return 0; }
        for (int k = 0; k < NVIS; k++) {   /* each frame's visible list, and a descriptor set pointing at it */
            if (!buffer_mapped(sizeof(uint32_t) * (s->ninst ? s->ninst : 1), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g_vis[k], &g_vism[k], (void **)&g_vismap[k])) { scene_free(s); return 0; }
            VkDescriptorBufferInfo b0 = { g_vb, 0, VK_WHOLE_SIZE }, b1 = { g_ins, 0, VK_WHOLE_SIZE }, b2 = { g_cols, 0, VK_WHOLE_SIZE }, b3 = { g_vis[k], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet wr[4] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_visset[k], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &b0, NULL },
                                           { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_visset[k], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &b1, NULL },
                                           { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_visset[k], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &b2, NULL },
                                           { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_visset[k], 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &b3, NULL } };
            ((PFN_vkUpdateDescriptorSets)photon_vk_proc("vkUpdateDescriptorSets"))(vk->dev, 4, wr, 0, NULL);
        }
        if (!make_textures(s)) { photon_log("nanite: the textures could not be made"); scene_free(s); return 0; }
        g_scene = s; g_scene_seq++;
        photon_log("nanite: scene uploaded (%.1f MiB; baked prop light for %d placements, %d without)", (12.0 * 4 * (double)s->nverts + 4.0 * (double)s->nidx +
                   64.0 * (double)s->ninst + 4.0 * (double)s->ncols) / 1048576.0, s->vhv_found, s->vhv_missing);
    }
    return 1;
}

static void record(VkCommandBuffer cb, void *u) {
    (void)u;
    if (!g_uploaded) upload_textures(cb);
    VkClearValue cl[3];
    memset(cl, 0, sizeof cl);
    cl[1].color.float32[0] = 1.0f; cl[2].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_rp, g_fb, { { 0, 0 }, { g_w, g_h } }, 3, cl };
    pvkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0, 0, (float)g_w, (float)g_h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { g_w, g_h } };
    pvkCmdSetViewport(cb, 0, 1, &vp); pvkCmdSetScissor(cb, 0, 1, &sc);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
    /* culling on the CPU (a few thousand sphere tests): the frustum's planes from the view-projection rows (D3D clip: |x|, |y| <= w,
     * 0 <= z <= w) and each placement's fade end (the map's fade max, as the engine's static props stop drawing there) */
    static int cull = -1;   /* PHOTON_NANITE_CULL: 0 none, 1 frustum, 2 frustum and fade (default) */
    if (cull < 0) { char e[4]; cull = GetEnvironmentVariableA("PHOTON_NANITE_CULL", e, sizeof e) > 0 ? atoi(e) : 2; }
    uint32_t slot = (uint32_t)(g_draws % NVIS), nvis = 0;
    uint32_t *vis = g_vismap[slot];
    float pl[6][4];
    for (int a = 0; a < 4; a++) {
        pl[0][a] = g_vp[3][a] + g_vp[0][a]; pl[1][a] = g_vp[3][a] - g_vp[0][a]; pl[2][a] = g_vp[3][a] + g_vp[1][a];
        pl[3][a] = g_vp[3][a] - g_vp[1][a]; pl[4][a] = g_vp[2][a]; pl[5][a] = g_vp[3][a] - g_vp[2][a];
    }
    float pn[6];
    for (int q = 0; q < 6; q++) pn[q] = sqrtf(pl[q][0] * pl[q][0] + pl[q][1] * pl[q][1] + pl[q][2] * pl[q][2]);
    /* LOD as the engine picks it: metric = 100 / (pixels per unit at the model), pixels per unit = (width / 2) |row 0 xyz| / w; LOD l
     * while metric >= its switch point (analysis/38). PHOTON_NANITE_LOD=n draws LOD n (or the model's last) everywhere. */
    static int force = -2;
    if (force == -2) { char e[8]; force = GetEnvironmentVariableA("PHOTON_NANITE_LOD", e, sizeof e) > 0 ? atoi(e) : -1; }
    float r0 = sqrtf(g_vp[0][0] * g_vp[0][0] + g_vp[0][1] * g_vp[0][1] + g_vp[0][2] * g_vp[0][2]);
    float kmet = r0 > 0 && g_w ? 200.0f / ((float)g_w * r0) : 0;
    if (photon_dynq_nanite_lod_bias()) kmet *= 2;   /* DYN nanitelod: each LOD at half the distance */
    int last = -1;
    for (int d = 0; d < g_scene->ndraws; d++) {
        const Draw *D = &g_scene->draws[d];
        if (D->model == last) continue;
        last = D->model;
        int nl = g_scene->mnlod[D->model];
        const float *sw = g_scene->msw[D->model];
        for (uint32_t i = D->first_inst; i < D->first_inst + D->inst_count; i++) {
            const float *sp = g_scene->sphere + 5 * i;
            int in = 1;
            for (int q = 0; q < 6 && in && cull >= 1; q++) in = pl[q][0] * sp[0] + pl[q][1] * sp[1] + pl[q][2] * sp[2] + pl[q][3] >= -sp[3] * pn[q];
            if (in && sp[4] > 0 && cull >= 2) {
                float dx = sp[0] - g_eye[0], dy = sp[1] - g_eye[1], dz = sp[2] - g_eye[2];
                in = dx * dx + dy * dy + dz * dz <= (sp[4] + sp[3]) * (sp[4] + sp[3]);
            }
            int l = 255;
            if (in) {
                const float *r = g_scene->rows + 16 * i;
                float met = kmet * (g_vp[3][0] * r[3] + g_vp[3][1] * r[7] + g_vp[3][2] * r[11] + g_vp[3][3]);
                if (force >= 0) l = force < nl ? force : nl - 1;
                else for (l = 0; l + 1 < nl && met >= sw[l + 1]; l++) {}
            }
            g_lodof[i] = (uint8_t)l;
        }
        for (int l = 0; l < nl; l++) {   /* the visible list grouped by model, then LOD */
            uint32_t g = (uint32_t)D->model * NLOD + (uint32_t)l;
            g_mfirst[g] = nvis; g_mcount[g] = 0;
            for (uint32_t i = D->first_inst; i < D->first_inst + D->inst_count; i++) if (g_lodof[i] == l) { vis[nvis++] = i; g_mcount[g]++; }
        }
    }
    g_vis_sum += nvis; g_vis_frames++;
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &g_visset[slot], 0, NULL);
    float pc[32];
    memcpy(pc, g_vp, 64); pc[16] = g_scene->sun[0]; pc[17] = g_scene->sun[1]; pc[18] = g_scene->sun[2]; pc[19] = 0;
    pc[20] = g_eye[0]; pc[21] = g_eye[1]; pc[22] = g_eye[2]; pc[23] = cull >= 2;   /* the camera; w: fades on */
    /* the stock range fog (common_ps_fxc.h CalcRangeFog, BlendPixelFog): f = saturate(min(max density, clip z / range - start / range)),
     * colour = lerp(colour, linear fog colour, f^2), after the HDR light scale (FinalOutput, TONEMAP_SCALE_LINEAR): the map's fog
     * distances, stock's c29 colour (else the map's, made linear) and c30 scale as they stand at the main view's end */
    const float *F = g_scene->fog;
    float range = F[1] - F[0];
    pc[24] = F[3] > 0 ? F[0] / range : 0; pc[25] = F[3] > 0 ? 1.0f / range : 0; pc[26] = F[3] > 0 ? F[2] : 0; pc[27] = (float)g_albedo;   /* PT-RT: the surface colour alone */
    int c29ok = g_c29[0] + g_c29[1] + g_c29[2] > 0;
    for (int k = 0; k < 3; k++) pc[28 + k] = c29ok ? g_c29[k] : powf(g_scene->fogcol[k], 2.2f);
    pc[31] = g_c30[0] > 0 && g_c30[0] < 64 ? g_c30[0] : 1;
    static int logged;
    if (!logged) { logged = 1; photon_log("nanite: fog %s (start %.0f end %.0f max %.2f), fog colour %s %.3f %.3f %.3f, light scale %.3f", F[3] > 0 ? "on" : "off",
                                          F[0], F[1], F[2], c29ok ? "c29" : "map", pc[28], pc[29], pc[30], pc[31]); }
    pvkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    pvkCmdBindIndexBuffer(cb, g_ib, 0, VK_INDEX_TYPE_UINT32);
    int alpha = 0;
    for (int d = 0; d < g_scene->ndraws; d++) {
        const Draw *D = &g_scene->draws[d];
        uint32_t grp = (uint32_t)D->model * NLOD + (uint32_t)D->lod;
        if (!g_mcount[grp]) continue;
        g_tri_sum += (long long)(D->index_count / 3) * g_mcount[grp];
        int a = g_scene->tex[D->tex].alphatest;
        if (a != alpha) { alpha = a; float w = (float)a; pvkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 76, 4, &w); }
        pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 1, 1, &g_tset[D->tex], 0, NULL);
        pvkCmdDrawIndexed(cb, D->index_count, g_mcount[grp], D->first_index, D->vertex_offset, g_mfirst[grp]);
    }
    pvkCmdEndRenderPass(cb);
    g_draws++;
}

static void after(void *u) {   /* the check: the colour read back at draw PHOTON_NANITE_DUMP */
    (void)u;
    static long long want = -2;
    if (want == -2) { char e[16]; want = GetEnvironmentVariableA("PHOTON_NANITE_DUMP", e, sizeof e) > 0 ? atoll(e) : -1; }
    if (want < 0 || g_draws != want) return;
    IDirect3DDevice9 *d = photon_d3d_device();
    IDirect3DSurface9 *src = NULL, *sys = NULL;
    if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_col.tex, 0, &src))) return;
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(d, g_w, g_h, D3DFMT_A8B8G8R8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(d, src, sys);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        char dir[MAX_PATH], path[MAX_PATH + 16];
        DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
        long covered = 0;
        FILE *f = NULL;
        if (n && n < sizeof dir) { snprintf(path, sizeof path, "%s\\nanite.ppm", dir); f = fopen(path, "wb"); }
        if (f) fprintf(f, "P6\n%u %u\n255\n", g_w, g_h);
        for (unsigned y = 0; y < g_h; y++)
            for (unsigned x = 0; x < g_w; x++) {
                const uint8_t *q = (const uint8_t *)lr.pBits + y * lr.Pitch + x * 4;
                if (q[3]) covered++;
                if (f) { fputc(q[0], f); fputc(q[1], f); fputc(q[2], f); }
            }
        if (f) fclose(f);
        photon_log("nanite: dump at draw %lld: %ux%u, %.1f%% of the pixels covered by props; visible placements per frame %.0f of %zu, triangles %.0f (LOD 0 everywhere: %ld)", g_draws, g_w, g_h,
                   100.0 * covered / ((double)g_w * g_h), g_vis_frames ? (double)g_vis_sum / (double)g_vis_frames : 0.0, g_scene->ninst,
                   g_vis_frames ? (double)g_tri_sum / (double)g_vis_frames : 0.0, g_scene->tris_drawn);
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(src);
}

static PhotonVkPass g_pass;
int photon_shadows_nanite_merge(void *col, void *dep);   /* shadows.c */
static void *g_cv_props; static int g_cv_tried, g_engine_off;
static long long g_merges;
void *photon_nanite_depth_tex(void) { return g_ready ? g_dep.tex : NULL; }   /* PT-RT's composite: where Nanite drew (D3D depth, R32F) */
void photon_nanite_main_view(void) {
    if (!photon_nanite_on() || !photon_vk_available()) return;
    void *depth; unsigned w, h;
    float iv[4][4], vp[4][4], cam[3];
    if (!photon_shadows_view(iv, vp, cam, &depth, &w, &h) || !w) return;
    memcpy(g_vp, vp, sizeof vp); memcpy(g_eye, cam, sizeof cam);
    IDirect3DDevice9 *d = photon_d3d_device();
    if (!d || FAILED(d->lpVtbl->GetPixelShaderConstantF(d, 29, g_c29, 1)) || FAILED(d->lpVtbl->GetPixelShaderConstantF(d, 30, g_c30, 1))) {
        memset(g_c29, 0, sizeof g_c29); g_c30[0] = 1;
    }
    g_ready = prepare(w, h) && g_scene;
    g_pass.enabled = g_ready;
    if (!g_ready) return;
    photon_vk_run(&g_pass);
    if (photon_shadows_nanite_merge(g_col.tex, g_dep.tex)) g_merges++;
    /* the engine's own static prop drawing off while Photon draws them (r_drawstaticprops, found by its name in engine.dll's data) */
    if (!g_cv_tried) { g_cv_tried = 1; g_cv_props = photon_cvar_find("r_drawstaticprops"); }
    if (g_cv_props && !g_engine_off) { photon_cvar_set_int(g_cv_props, 0); g_engine_off = 1; photon_log("nanite: the engine's static prop drawing off (r_drawstaticprops 0)"); }
}

void photon_nanite_init(void) {
    if (!photon_nanite_on()) return;
    g_pass = (PhotonVkPass){ "nanite", setup, record, after, g_imgs, 2, NULL, 0, 1 };
    photon_vk_add_pass(&g_pass);
    photon_log("nanite: on (N2b: static props drawn by Photon and merged into the frame; the engine's own off)");
}
