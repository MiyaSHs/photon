/* lua_diff.exe <lua dll> <test.lua>: run a Lua file through a given Lua DLL (GMod's lua_shared.dll or our LuaJIT build) and print
 * what it returns (one string), then "OK" or the error. The new engine's differential gate (analysis/15): both DLLs must print the same. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
typedef void *(*newstate_t)(void);
typedef void (*openlibs_t)(void *);
typedef int (*loadbufferx_t)(void *, const char *, size_t, const char *, const char *);
typedef int (*pcall_t)(void *, int, int, int);
typedef const char *(*tolstring_t)(void *, int, size_t *);
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: lua_diff <dll> <file.lua>\n"); return 2; }
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("LOADFAIL %lu\n", GetLastError()); return 1; }
    newstate_t newstate = (newstate_t)(void *)GetProcAddress(m, "luaL_newstate");
    openlibs_t openlibs = (openlibs_t)(void *)GetProcAddress(m, "luaL_openlibs");
    loadbufferx_t load = (loadbufferx_t)(void *)GetProcAddress(m, "luaL_loadbufferx");
    pcall_t pcall = (pcall_t)(void *)GetProcAddress(m, "lua_pcall");
    tolstring_t tolstring = (tolstring_t)(void *)GetProcAddress(m, "lua_tolstring");
    if (!newstate || !openlibs || !load || !pcall || !tolstring) { printf("NOSYM\n"); return 1; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { printf("NOFILE\n"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1); fread(buf, 1, (size_t)n, f); fclose(f);
    void *L = newstate();
    openlibs(L);
    int r = load(L, buf, (size_t)n, "@test.lua", "t");
    if (r == 0) r = pcall(L, 0, 1, 0);   /* the test returns its output as one string (GMod's print needs the game) */
    if (r) printf("ERROR %d %s\n", r, tolstring(L, -1, NULL));
    else { const char *o = tolstring(L, -1, NULL); printf("%s\nOK\n", o ? o : "(no output)"); }
    fflush(stdout);
    return 0;
}
