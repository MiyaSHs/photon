/* NANITE / PT-PROPS: a Source model's LODs as triangles. See mdl_mesh.h. Layouts as lod_model.c reads them:
 *   .vvd (vertexFileHeader_t): numLODVertexes[0] +16, numFixups +48, fixupTableStart +52, vertexDataStart +56; vertices of 48 bytes
 *     (bone weights 16, position +16, normal +28, uv +40); fixups (lod, sourceVertexID, numVertexes: 12 bytes) remap LOD 0's vertices.
 *   .mdl (studiohdr_t v48): textures +204/+208 (mstudiotexture_t 64 bytes, name offset +0 relative to it), cd paths +212/+216 (int
 *     offsets), skin refs +220, families +224, skin table +228 (int16); body parts +232/+236; mstudiomodel_t 148 bytes (meshes +72/+76,
 *     vertexindex +84); mstudiomesh_t 116 bytes (material +0, vertexoffset +12).
 *   .vtx v7: body parts +28/+32 (8 bytes), models (8: LODs +0/+4), LODs (12: meshes +0/+4, switch point +8), meshes (9), strip groups (25: verts +0/+4, indices +8/+12,
 *     strips +16/+20), vertices of 9 bytes (origMeshVertID +4), strips (27: indices +0/+4, flags +18: 1 list, 2 strip). */
#include "mdl_mesh.h"
#include <stdlib.h>
#include <string.h>

static int32_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static int16_t rd16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
#define NEED(buf_n, off, n) do { if ((off) < 0 || (size_t)(off) + (size_t)(n) > (buf_n)) goto bad; } while (0)

void mdl_mesh_free(MdlMesh *m) {
    if (!m) return;
    free(m->pos); free(m->nrm); free(m->uv); free(m->tri); free(m->mat); free(m->names); free(m->group_first); free(m->group_nv); free(m->group_lod); free(m->group_map);
    memset(m, 0, sizeof *m);
}

static int add_tri(MdlMesh *o, int *cap, int32_t a, int32_t b, int32_t c, uint16_t mat) {
    if (a == b || b == c || a == c) return 1;
    if (o->nt >= *cap) {
        int nc = *cap ? *cap * 2 : 1024;
        int32_t (*t)[3] = realloc(o->tri, sizeof *t * (size_t)nc);
        uint16_t *mm = realloc(o->mat, sizeof *mm * (size_t)nc);
        if (t) o->tri = t;
        if (mm) o->mat = mm;
        if (!t || !mm) return 0;
        *cap = nc;
    }
    o->tri[o->nt][0] = a; o->tri[o->nt][1] = b; o->tri[o->nt][2] = c; o->mat[o->nt] = mat;
    o->nt++;
    return 1;
}

int mdl_mesh_load(const uint8_t *mdl, size_t mdl_n, const uint8_t *vvd, size_t vvd_n, const uint8_t *vtx, size_t vtx_n, int skin, MdlMesh *out) {
    memset(out, 0, sizeof *out);
    if (mdl_n < 240 || vvd_n < 64 || memcmp(vvd, "IDSV", 4) != 0 || vtx_n < 36 || rd32(vtx) != 7) return 1;
    /* LOD 0's vertices, through the fixups if any */
    int32_t nv0 = rd32(vvd + 16), nfix = rd32(vvd + 48), fixo = rd32(vvd + 52), vdo = rd32(vvd + 56);
    if (nv0 <= 0 || vdo < 0) return 1;
    int32_t *map = malloc(sizeof *map * (size_t)nv0);
    if (!map) return 2;
    if (nfix > 0) {
        int32_t at = 0;
        for (int32_t f = 0; f < nfix; f++) {
            NEED(vvd_n, fixo + 12 * f, 12);
            int32_t lod = rd32(vvd + fixo + 12 * f), src = rd32(vvd + fixo + 12 * f + 4), cnt = rd32(vvd + fixo + 12 * f + 8);
            if (lod < 0) continue;   /* lod >= 0 belongs to LOD 0 */
            for (int32_t k = 0; k < cnt && at < nv0; k++) map[at++] = src + k;
        }
        if (at != nv0) goto bad;
    } else for (int32_t i = 0; i < nv0; i++) map[i] = i;
    out->nv = nv0;
    out->pos = malloc(sizeof *out->pos * (size_t)nv0); out->nrm = malloc(sizeof *out->nrm * (size_t)nv0); out->uv = malloc(sizeof *out->uv * (size_t)nv0);
    if (!out->pos || !out->nrm || !out->uv) goto mem;
    for (int32_t i = 0; i < nv0; i++) {
        int32_t o = vdo + 48 * map[i];
        NEED(vvd_n, o, 48);
        memcpy(out->pos[i], vvd + o + 16, 12); memcpy(out->nrm[i], vvd + o + 28, 12); memcpy(out->uv[i], vvd + o + 40, 8);
        for (int a = 0; a < 3; a++) {
            if (i == 0 || out->pos[i][a] < out->mins[a]) out->mins[a] = out->pos[i][a];
            if (i == 0 || out->pos[i][a] > out->maxs[a]) out->maxs[a] = out->pos[i][a];
        }
    }
    /* material names: texture name (mstudiotexture_t) with the first cd path, through the skin family */
    int32_t ntex = rd32(mdl + 204), texo = rd32(mdl + 208), ncd = rd32(mdl + 212), cdo = rd32(mdl + 216);
    int32_t nref = rd32(mdl + 220), nfam = rd32(mdl + 224), sko = rd32(mdl + 228);
    if (ntex < 0 || ntex > 1024) goto bad;
    out->nnames = ntex;
    out->names = calloc((size_t)(ntex ? ntex : 1), sizeof *out->names);
    if (!out->names) goto mem;
    const char *cd = "";
    if (ncd > 0) { NEED(mdl_n, cdo, 4); int32_t p = rd32(mdl + cdo); NEED(mdl_n, p, 1); cd = (const char *)mdl + p; }
    for (int t = 0; t < ntex; t++) {
        int32_t to = texo + 64 * t;
        NEED(mdl_n, to, 64);
        int32_t no = to + rd32(mdl + to);
        NEED(mdl_n, no, 1);
        char buf[128];
        size_t k = 0;
        for (const char *c = cd; *c && k < sizeof buf - 1; c++) buf[k++] = *c == '\\' ? '/' : (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        for (const char *c = (const char *)mdl + no; *c && k < sizeof buf - 1 && (size_t)(c - (const char *)mdl) < mdl_n; c++)
            buf[k++] = *c == '\\' ? '/' : (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        buf[k] = 0;
        memcpy(out->names[t], buf, k + 1);
    }
    /* the .vtx's material replacements per LOD (header +24: per LOD 8 bytes: count, offset; entries of 6 bytes: texture index (int16),
     * name offset relative to the entry): an extra name each, looked up through the first cd path as the textures are */
    int16_t rep_mid[64]; uint8_t rep_lod[64]; int nrep = 0;
    {
        int32_t nl = rd32(vtx + 20), mro = rd32(vtx + 24);
        for (int l = 1; l < nl && l < 8 && mro > 0; l++) {
            NEED(vtx_n, mro + 8 * l, 8);
            int32_t n = rd32(vtx + mro + 8 * l), base = mro + 8 * l + rd32(vtx + mro + 8 * l + 4);
            for (int r = 0; r < n && nrep < 64; r++) {
                NEED(vtx_n, base + 6 * r, 6);
                int16_t mid = rd16(vtx + base + 6 * r);
                int32_t no = base + 6 * r + rd32(vtx + base + 6 * r + 2);
                NEED(vtx_n, no, 1);
                char (*nn)[128] = realloc(out->names, sizeof *nn * (size_t)(out->nnames + 1));
                if (!nn) goto mem;
                out->names = nn;
                size_t k = 0;
                char *buf = out->names[out->nnames];
                for (const char *c = cd; *c && k < 127; c++) buf[k++] = *c == '\\' ? '/' : (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
                for (const char *c = (const char *)vtx + no; *c && k < 127 && (size_t)(c - (const char *)vtx) < vtx_n; c++)
                    buf[k++] = *c == '\\' ? '/' : (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
                buf[k] = 0;
                rep_mid[nrep] = mid; rep_lod[nrep] = (uint8_t)l; nrep++;
                out->nnames++;
            }
        }
    }
    /* the triangles from the .vtx, LOD by LOD (every LOD indexes the root LOD's vertices through origMeshVertID) */
    int cap = 0, gcap = 0, mcap = 0, nmap = 0;
    int32_t nbp = rd32(vtx + 28), bpo = rd32(vtx + 32), mdl_nbp = rd32(mdl + 232), mdl_bpi = rd32(mdl + 236);
    if (nbp != mdl_nbp || nbp < 1) goto bad;
    NEED(vtx_n, bpo, 8);
    {   /* the LOD count and switch points of body part 0's first model */
        int32_t mh0 = bpo + rd32(vtx + bpo + 4);
        NEED(vtx_n, mh0, 8);
        int32_t nl = rd32(vtx + mh0), lo = mh0 + rd32(vtx + mh0 + 4);
        out->nlods = 1;
        for (int l = 1; l < nl && l < 8; l++) {
            NEED(vtx_n, lo + 12 * l, 12);
            float sw; memcpy(&sw, vtx + lo + 12 * l + 8, 4);
            if (!(sw > 0)) break;
            out->switch_pt[l] = sw; out->nlods = l + 1;
        }
    }
    for (int lod = 0; lod < out->nlods; lod++) {
        out->lod_first[lod] = out->nt;
        for (int b = 0; b < nbp; b++) {
            int32_t bo = bpo + 8 * b;
            NEED(vtx_n, bo, 8);
            int32_t nm = rd32(vtx + bo), mo = bo + rd32(vtx + bo + 4);
            int32_t mbo = mdl_bpi + 16 * b;
            NEED(mdl_n, mbo, 16);
            if (rd32(mdl + mbo + 4) != nm) goto bad;
            for (int k = 0; k < nm; k++) {   /* every model for the .vhv's strip group list; the triangles of the first (the default body) */
                int tris = k == 0;
                int32_t mh = mo + 8 * k;
                NEED(vtx_n, mh, 8);
                if (lod >= rd32(vtx + mh)) continue;
                int32_t lh = mh + rd32(vtx + mh + 4) + 12 * lod;
                NEED(vtx_n, lh, 12);
                int32_t nmesh = rd32(vtx + lh), meo = lh + rd32(vtx + lh + 4);
                int32_t mmo = mbo + rd32(mdl + mbo + 12) + 148 * k;
                NEED(mdl_n, mmo, 148);
                int32_t mdl_nmesh = rd32(mdl + mmo + 72), mdl_meshi = rd32(mdl + mmo + 76), vbase_model = rd32(mdl + mmo + 84) / 48;
                if (nmesh != mdl_nmesh) goto bad;
                for (int j = 0; j < nmesh; j++) {
                    int32_t me = meo + 9 * j, mme = mmo + mdl_meshi + 116 * j;
                    NEED(vtx_n, me, 9); NEED(mdl_n, mme, 116);
                    int32_t mat = rd32(mdl + mme), vbase = vbase_model + rd32(mdl + mme + 12);
                    if (nfam > 0 && skin > 0 && skin < nfam && mat >= 0 && mat < nref) { NEED(mdl_n, sko + 2 * (skin * nref + mat), 2); mat = rd16(mdl + sko + 2 * (skin * nref + mat)); }
                    else if (nref > 0 && mat >= 0 && mat < nref) { NEED(mdl_n, sko + 2 * mat, 2); mat = rd16(mdl + sko + 2 * mat); }
                    if (mat < 0 || mat >= ntex) mat = 0;
                    for (int r = 0; r < nrep; r++) if (rep_lod[r] == lod && rep_mid[r] == mat) { mat = ntex + r; break; }
                    int32_t ng = rd32(vtx + me), sgo = me + rd32(vtx + me + 4);
                    for (int g = 0; g < ng; g++) {
                        int32_t so = sgo + 25 * g;
                        NEED(vtx_n, so, 25);
                        int32_t gnv = rd32(vtx + so), gvo = so + rd32(vtx + so + 4), gni = rd32(vtx + so + 8), gio = so + rd32(vtx + so + 12);
                        int32_t gns = rd32(vtx + so + 16), gsto = so + rd32(vtx + so + 20);
                        NEED(vtx_n, gvo, 9 * (size_t)gnv); NEED(vtx_n, gio, 2 * (size_t)gni); NEED(vtx_n, gsto, 27 * (size_t)gns);
                        if (out->ngroups >= gcap) {   /* the group's vertices -> model vertices (vbase + origMeshVertID) */
                            int nc = gcap ? gcap * 2 : 64;
                            int32_t *a = realloc(out->group_first, sizeof *a * (size_t)nc), *c2 = realloc(out->group_nv, sizeof *c2 * (size_t)nc);
                            if (a) out->group_first = a;
                            if (c2) out->group_nv = c2;
                            int32_t *c3 = a && c2 ? realloc(out->group_lod, sizeof *c3 * (size_t)nc) : NULL;
                            if (c3) out->group_lod = c3;
                            if (!a || !c2 || !c3) goto mem;
                            gcap = nc;
                        }
                        if (nmap + gnv > mcap) {
                            int nc = mcap ? mcap : 1024;
                            while (nc < nmap + gnv) nc *= 2;
                            int32_t *a = realloc(out->group_map, sizeof *a * (size_t)nc);
                            if (!a) goto mem;
                            out->group_map = a; mcap = nc;
                        }
                        out->group_first[out->ngroups] = nmap; out->group_nv[out->ngroups] = gnv; out->group_lod[out->ngroups] = lod; out->ngroups++;
                        for (int q = 0; q < gnv; q++) { uint16_t orig; memcpy(&orig, vtx + gvo + 9 * q + 4, 2); out->group_map[nmap++] = vbase + orig; }
                        if (!tris) continue;
                        for (int s = 0; s < gns; s++) {
                            const uint8_t *S = vtx + gsto + 27 * s;
                            int32_t sni = rd32(S), sio = rd32(S + 4);
                            uint8_t fl = S[18];
                            if (sni < 0 || sio < 0 || sio + sni > gni) goto bad;
                            int32_t vid[3];
                            for (int32_t q = 0; q + 2 < sni; q += (fl & 2) ? 1 : 3) {
                                for (int c = 0; c < 3; c++) {
                                    uint16_t ix; memcpy(&ix, vtx + gio + 2 * (sio + q + c), 2);
                                    int32_t vi = ix;   /* into the strip group's vertices */
                                    if (vi < 0 || vi >= gnv) goto bad;
                                    uint16_t orig; memcpy(&orig, vtx + gvo + 9 * vi + 4, 2);
                                    vid[c] = vbase + orig;
                                    if (vid[c] < 0 || vid[c] >= nv0) goto bad;
                                }
                                int odd = (fl & 2) && (q & 1);
                                if (!add_tri(out, &cap, vid[0], odd ? vid[2] : vid[1], odd ? vid[1] : vid[2], (uint16_t)mat)) goto mem;
                            }
                        }
                    }
                }
            }
        }
    }
    out->lod_first[out->nlods] = out->nt;
    while (out->nlods > 1 && out->lod_first[out->nlods - 1] == out->nt) out->nlods--;   /* a LOD without triangles: never drawn */
    free(map);
    return out->lod_first[1] > 0 ? 0 : 1;
bad:
    free(map); mdl_mesh_free(out);
    return 1;
mem:
    free(map); mdl_mesh_free(out);
    return 2;
}
