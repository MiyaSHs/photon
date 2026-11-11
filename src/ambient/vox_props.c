/* PT-PROPS / NANITE (analysis/44): a map's static props into the voxel cascades. Each distinct model is read once through the caller's
 * file reader (the engine's filesystem in game, plain files on the host), turned into triangles (src/lod/mdl_mesh.c), and every placement
 * transformed into the world (bsp_prop_matrix); props inside the 3D skybox (its miniatures) are left out. Colours v0: by material name
 * (leaves green, bark brown, the rest grey); the textures' own colours and alpha (leaf cards) come later. */
#include "vox_world.h"
#include "bsp_props.h"
#include "../lod/mdl_mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void colour_of(const char *mn, float col[3]) {
    col[0] = 0.35f; col[1] = 0.35f; col[2] = 0.33f;
    if (strstr(mn, "leaf") || strstr(mn, "leaves") || strstr(mn, "foliage") || strstr(mn, "needle") || strstr(mn, "pine") || strstr(mn, "bush"))
        { col[0] = 0.10f; col[1] = 0.17f; col[2] = 0.05f; }
    else if (strstr(mn, "bark") || strstr(mn, "trunk") || strstr(mn, "branch") || strstr(mn, "tree"))
        { col[0] = 0.18f; col[1] = 0.13f; col[2] = 0.09f; }
}

int vox_cascades_add_props(VoxCascades *vc, const uint8_t *bsp, size_t size, const SunMap *sm, VoxReadFn read, void *ctx, VoxPropsInfo *info) {
    memset(info, 0, sizeof *info);
    BspProp *pr; int np, ver;
    if (bsp_props_read(bsp, size, &pr, &np, &ver)) return 1;
    if (!np) return 0;
    float sb[6];
    int have_sky = vox_skybox_bounds(bsp, size, sm, sb);
    enum { NM = 2048 };
    char (*names)[128] = calloc(NM, sizeof *names);
    MdlMesh *meshes = calloc(NM, sizeof *meshes);
    uint8_t *loaded = calloc(NM, 1);
    int *id = calloc(NM, sizeof *id);
    if (!names || !meshes || !loaded || !id) { free(names); free(meshes); free(loaded); free(id); free(pr); return 2; }
    int nm = 0;
    for (int i = 0; i < np; i++) {
        const BspProp *P = &pr[i];
        if (have_sky && P->origin[0] >= sb[0] && P->origin[0] <= sb[3] && P->origin[1] >= sb[1] && P->origin[1] <= sb[4] &&
            P->origin[2] >= sb[2] && P->origin[2] <= sb[5]) { info->skybox++; continue; }
        int k;
        for (k = 0; k < nm && strcmp(names[k], P->model); k++) {}
        if (k == nm && nm < NM) {
            snprintf(names[nm], sizeof names[nm], "%s", P->model);
            char base[128], p[160];
            size_t L = strlen(P->model);
            snprintf(base, sizeof base, "%.*s", (int)(L > 4 ? L - 4 : L), P->model);   /* without ".mdl" */
            size_t a = 0, b = 0, c = 0;
            snprintf(p, sizeof p, "%s.mdl", base); uint8_t *m = read(ctx, p, &a);
            snprintf(p, sizeof p, "%s.vvd", base); uint8_t *v = m ? read(ctx, p, &b) : NULL;
            snprintf(p, sizeof p, "%s.dx90.vtx", base); uint8_t *x = v ? read(ctx, p, &c) : NULL;
            loaded[nm] = m && v && x && mdl_mesh_load(m, a, v, b, x, c, 0, &meshes[nm]) == 0;
            free(m); free(v); free(x);
            if (loaded[nm]) {   /* the model's triangles once, in its own space */
                const MdlMesh *M = &meshes[nm];
                float (*tv)[3][3] = malloc(sizeof *tv * (size_t)M->lod_first[1]), (*tc)[3] = malloc(sizeof *tc * (size_t)M->lod_first[1]), (*tr)[3] = calloc((size_t)M->lod_first[1], sizeof *tr);
                if (tv && tc && tr) {
                    for (int t = 0; t < M->lod_first[1]; t++) {
                        for (int c = 0; c < 3; c++) {
                            memcpy(tv[t][c], M->pos[M->tri[t][c]], 12);
                            for (int a = 0; a < 3; a++) tr[t][a] += M->nrm[M->tri[t][c]][a];
                        }
                        colour_of(M->names[M->mat[t]], tc[t]);
                    }
                    id[nm] = vox_cascades_add_model(vc, (const float (*)[3][3])tv, (const float (*)[3])tc, (const float (*)[3])tr, M->lod_first[1]);
                } else id[nm] = -1;
                free(tv); free(tc); free(tr);
                if (id[nm] < 0) loaded[nm] = 0;
            }
            info->models++; info->models_loaded += loaded[nm];
            nm++;
        }
        if (k >= nm || !loaded[k]) { info->missing++; continue; }
        double X[3][4];
        bsp_prop_matrix(P, X);
        if (!vox_cascades_add_instance(vc, id[k], X)) { info->missing++; continue; }
        info->triangles += meshes[k].nt;
        info->placed++;
    }
    for (int k = 0; k < nm; k++) if (loaded[k]) mdl_mesh_free(&meshes[k]);
    free(names); free(meshes); free(loaded); free(id); free(pr);
    info->props = np;
    return 0;
}
