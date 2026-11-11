/* F3: lod_model_build on one model from files: lodmodeltest base outdir (base = path without extension; reads base.mdl / .vvd / .dx90.vtx,
 * writes outdir/<name>.mdl / .vvd / .dx90.vtx with levels at 0.5 / 0.25 / 0.1). Prints the result code and triangles; the byte-for-byte
 * check against tools/f3_vtx_lod.py is tests/lod/lodmodel_cmp.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/lod/lod_model.h"

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)(l > 0 ? l : 1));
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}
static int spit(const char *dir, const char *name, const char *ext, const unsigned char *b, size_t n) {
    char p[4096];
    snprintf(p, sizeof p, "%s/%s%s", dir, name, ext);
    FILE *f = fopen(p, "wb");
    if (!f) return 1;
    int ok = fwrite(b, 1, n, f) == n;
    fclose(f);
    return !ok;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    char p[4096];
    size_t nm, nv, nx;
    snprintf(p, sizeof p, "%s.mdl", argv[1]); unsigned char *m = slurp(p, &nm);
    snprintf(p, sizeof p, "%s.vvd", argv[1]); unsigned char *v = slurp(p, &nv);
    snprintf(p, sizeof p, "%s.dx90.vtx", argv[1]); unsigned char *x = slurp(p, &nx);
    if (!m || !v || !x) { printf("LODMODEL read failed\n"); return 1; }
    double ratios[3] = { 0.5, 0.25, 0.1 };
    LodModelOut o;
    int rc = lod_model_build(m, nm, v, nv, x, nx, ratios, 3, &o);
    printf("LODMODEL rc %d tris %u %u %u %u switch %.1f %.1f %.1f\n", rc, o.tris[0], o.tris[1], o.tris[2], o.tris[3], o.switch_point[1], o.switch_point[2],
           o.switch_point[3]);
    if (rc) return 3;
    const char *name = strrchr(argv[1], '/');
    name = name ? name + 1 : argv[1];
    int bad = spit(argv[2], name, ".mdl", o.mdl, o.mdl_n) | spit(argv[2], name, ".vvd", o.vvd, o.vvd_n) | spit(argv[2], name, ".dx90.vtx", o.vtx, o.vtx_n);
    lod_model_free(&o);
    return bad;
}
