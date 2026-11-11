/* d3dcompile: compile HLSL to D3D9 bytecode with a native d3dcompiler_4x.dll under Wine.
 * usage: d3dcompile.exe <d3dcompiler.dll> <in.hlsl> <entry> <profile> <out.bin> [out.asm]
 *        d3dcompile.exe <d3dcompiler.dll> -d <in.bin> <out.asm>   (disassemble existing bytecode)
 *        d3dcompile.exe <d3dcompiler.dll> -b <jobs.txt>            (batch, one Wine start for many jobs)
 * A batch line is tab-separated: "c <in.hlsl> <entry> <profile> <out.bin> <out.asm>" compiles,
 * "m <in.hlsl> <entry> <profile> <out.bin> <out.asm|-> <NAME=V;NAME=V;...>" compiles with those macros (passed as
 * D3D_SHADER_MACRO, equivalent to #define lines in front of the source; "-" skips the listing; tools/t34_build_vlg20.py
 * uses it so thousands of combos share one flattened source), "d <in.bin> <out.asm>" disassembles; '#' lines are
 * skipped. Prints "JOB <n> OK|FAIL <file>" per job,
 * then "BATCH <ok>/<total>"; exit 0 only if every job succeeded (tools/t34_build_vlg.py).  * Loads the DLL by full path (run with WINEDLLOVERRIDES=d3dcompiler_47=n,d3dcompiler_43=n so
 * Wine does not substitute its builtin), calls D3DCompile and D3DDisassemble through
 * GetProcAddress, and writes the raw bytecode (D3D9 d3dbc for vs_/ps_2_x/3_0 profiles). */
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *PFN_Disasm)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob **);

static char *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    b[*n] = 0; fclose(f); return b;
}

static int dump(const char *p, const void *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fwrite(d, 1, n, f); fclose(f); return 1;
}

static PFN_Compile g_comp;
static PFN_Disasm g_dis;

static int disasm_file(const char *in, const char *out)
{
    long bn; char *bc = slurp(in, &bn); ID3DBlob *t = NULL;
    if (!g_dis || !bc) { free(bc); return 3; }
    HRESULT dhr = g_dis(bc, bn, 0, NULL, &t);
    free(bc);
    if (FAILED(dhr) || !t) { fprintf(stderr, "D3DDisassemble failed 0x%08lx\n", dhr); return 5; }
    int ok = dump(out, t->lpVtbl->GetBufferPointer(t), t->lpVtbl->GetBufferSize(t) - 1);
    t->lpVtbl->Release(t);
    return ok ? 0 : 6;
}

#define MAX_MACROS 64
/* "A=1;B=0" -> D3D_SHADER_MACRO[] (NULL-terminated), edited in place; returns 0 on a malformed list */
static int parse_macros(char *s, D3D_SHADER_MACRO *m)
{
    int n = 0;
    for (char *t = strtok(s, ";"); t; t = strtok(NULL, ";")) {
        char *eq = strchr(t, '=');
        if (!eq || eq == t || n >= MAX_MACROS - 1) return 0;
        *eq = 0; m[n].Name = t; m[n].Definition = eq + 1; n++;
    }
    m[n].Name = NULL; m[n].Definition = NULL;
    return 1;
}

/* D3DCOMPILE_FLAGS=<hex|dec> overrides the default D3DCOMPILE_OPTIMIZATION_LEVEL3 (the skin probe,
 * tools/p0_skin_probe.py, uses it to match the flags of GMod's shipped blobs with d3dcompiler_43). */
static UINT compile_flags(void)
{
    const char *e = getenv("D3DCOMPILE_FLAGS");
    return e && *e ? (UINT)strtoul(e, NULL, 0) : D3DCOMPILE_OPTIMIZATION_LEVEL3;
}

static const char *g_src_path; /* the last source read: kept in memory for "m" jobs on the same file */
static char *g_src; static long g_src_n;

static int compile_file_m(const char *in, const char *entry, const char *profile, const char *out, const char *asmout,
                          const D3D_SHADER_MACRO *macros)
{
    long n; char *src;
    if (macros && g_src_path && !strcmp(g_src_path, in)) { src = g_src; n = g_src_n; }
    else {
        src = slurp(in, &n);
        if (!src) { fprintf(stderr, "cannot read %s\n", in); return 4; }
        if (macros) { free(g_src); free((void *)g_src_path); g_src = src; g_src_n = n; g_src_path = _strdup(in); }
    }
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_comp(src, n, in, macros, NULL, entry, profile, compile_flags(), 0, &code, &err);
    if (src != g_src) free(src);
    if (err) { fprintf(stderr, "%.*s", (int)err->lpVtbl->GetBufferSize(err), (char *)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }
    if (FAILED(hr) || !code) { fprintf(stderr, "D3DCompile failed 0x%08lx\n", hr); return 5; }
    const void *p = code->lpVtbl->GetBufferPointer(code); size_t sz = code->lpVtbl->GetBufferSize(code);
    if (!dump(out, p, sz)) { code->lpVtbl->Release(code); return 6; }
    if (!macros) fprintf(stderr, "wrote %s (%zu bytes, token 0x%08lx)\n", out, sz, sz >= 4 ? *(const unsigned long *)p : 0ul);
    if (asmout && g_dis) {
        ID3DBlob *txt = NULL;
        if (SUCCEEDED(g_dis(p, sz, 0, NULL, &txt)) && txt) {
            dump(asmout, txt->lpVtbl->GetBufferPointer(txt), txt->lpVtbl->GetBufferSize(txt) - 1);
            txt->lpVtbl->Release(txt);
        } else fprintf(stderr, "D3DDisassemble failed\n");
    }
    code->lpVtbl->Release(code);
    return 0;
}

static int compile_file(const char *in, const char *entry, const char *profile, const char *out, const char *asmout)
{
    return compile_file_m(in, entry, profile, out, asmout, NULL);
}

static int batch(const char *list)
{
    FILE *f = fopen(list, "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", list); return 4; }
    char line[4 * MAX_PATH + 4096];
    int total = 0, ok = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *fld[7] = {0}; int nf = 0;
        for (char *t = strtok(line, "\t"); t && nf < 7; t = strtok(NULL, "\t")) fld[nf++] = t;
        int rc = 2;
        if (nf == 6 && !strcmp(fld[0], "c")) rc = compile_file(fld[1], fld[2], fld[3], fld[4], fld[5]);
        else if (nf == 7 && !strcmp(fld[0], "m")) {
            D3D_SHADER_MACRO mac[MAX_MACROS];
            if (parse_macros(fld[6], mac))
                rc = compile_file_m(fld[1], fld[2], fld[3], fld[4], strcmp(fld[5], "-") ? fld[5] : NULL, mac);
            else fprintf(stderr, "bad macro list on job line %d\n", total + 1);
        }
        else if (nf == 3 && !strcmp(fld[0], "d")) rc = disasm_file(fld[1], fld[2]);
        else fprintf(stderr, "bad job line %d\n", total + 1);
        total++;
        if (!rc) ok++;
        printf("JOB %d %s %s\n", total, rc ? "FAIL" : "OK", nf > 1 ? fld[1] : "?");
        fflush(stdout);
    }
    fclose(f);
    printf("BATCH %d/%d\n", ok, total);
    return ok == total ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s dll in.hlsl entry profile out.bin [out.asm] | dll -d in.bin out.asm | dll -b jobs.txt\n", argv[0]); return 2; }
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { fprintf(stderr, "LoadLibrary(%s) failed %lu\n", argv[1], GetLastError()); return 3; }
    g_comp = (PFN_Compile)(void (*)(void))GetProcAddress(m, "D3DCompile");
    g_dis = (PFN_Disasm)(void (*)(void))GetProcAddress(m, "D3DDisassemble");
    if (argc == 5 && !strcmp(argv[2], "-d")) return disasm_file(argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[2], "-b")) { if (!g_comp) return 3; return batch(argv[3]); }
    if (argc < 6) { fprintf(stderr, "usage: %s dll in.hlsl entry profile out.bin [out.asm]\n", argv[0]); return 2; }
    char mod[MAX_PATH]; GetModuleFileNameA(m, mod, sizeof mod);
    fprintf(stderr, "loaded %s\n", mod);
    if (!g_comp) { fprintf(stderr, "no D3DCompile\n"); return 3; }
    return compile_file(argv[2], argv[3], argv[4], argv[5], argc > 6 ? argv[6] : NULL);
}
