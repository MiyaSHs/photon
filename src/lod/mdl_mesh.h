/* NANITE / PT-PROPS (analysis/44): a Source model's LODs as plain triangles, from its .mdl / .vvd / .dx90.vtx in memory: model-space
 * positions, normals and texture coordinates, and per triangle its material (an index into names[], resolved through the skin table and
 * the model's material search paths). For rigid models (static props); bone weights are ignored. Pure C; free with mdl_mesh_free. */
#ifndef PHOTON_MDL_MESH_H
#define PHOTON_MDL_MESH_H
#include <stddef.h>
#include <stdint.h>

typedef struct MdlMesh {
    int nv, nt;
    float (*pos)[3], (*nrm)[3], (*uv)[2];   /* the root LOD's vertex stream: every LOD's triangles index it */
    int32_t (*tri)[3];       /* LOD by LOD: LOD l is tri[lod_first[l] .. lod_first[l + 1]) */
    int nlods;               /* drawable LODs (a shadow-only LOD, switch point <= 0 past LOD 0, is left out) */
    int lod_first[9];
    float switch_pt[8];      /* the .vtx's switch points of the first model: LOD l is drawn while the engine's metric >= switch_pt[l] */
    uint16_t *mat;           /* per triangle: index into names */
    int nnames;
    char (*names)[128];      /* "<first cd path><texture name>", lower case, e.g. "models/props_foliage/tree_deciduous_01a" (no
                              * "materials/", no ".vmt") */
    float mins[3], maxs[3];
    /* every strip group of every body part, model and LOD, LOD by LOD (within a LOD in file order: VRAD writes a static prop's .vhv per
     * strip group: body part, model, LOD, mesh, strip group, so a LOD's entries come in this order), each as the list of model vertices
     * its vertices are: group g (of LOD group_lod[g]) has vertex j = vertex group_map[group_first[g] + j] */
    int ngroups;
    int32_t *group_first, *group_nv, *group_map, *group_lod;
} MdlMesh;

/* skin: the skin family (0 = default). 0 on success. */
int mdl_mesh_load(const uint8_t *mdl, size_t mdl_n, const uint8_t *vvd, size_t vvd_n, const uint8_t *vtx, size_t vtx_n, int skin, MdlMesh *out);
void mdl_mesh_free(MdlMesh *m);
#endif
