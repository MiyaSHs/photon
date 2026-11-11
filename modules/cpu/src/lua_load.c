/* Photon CPU, phase 2 F (compiled-Lua cache), F1: how long the map load's Lua parsing takes. PHOTON_CPU_PROBE_LUALOAD=1 inline-hooks
 * lua_shared's luaL_loadbufferx: GMod's script runner (CLuaInterface, inside lua_shared) calls it for every file and string it runs, in
 * text mode ("t"), after refusing chunks that start with ESC (bytecode). Its prologue (mov r11,rsp; push rbx; push rdi;
 * sub rsp,0xf8: 12 position-independent bytes) is hash-checked against the validated build, moved into a trampoline (the 12 bytes, then
 * jmp [rip+0] back), and replaced by mov rax,imm64; jmp rax (12 bytes; rax is free at a call).
 * Measurement only: every call goes to the original unchanged. Each load burst (loads, then 120 presented frames without one) is
 * reported: chunks, source KB and parse ms, per Lua state and per addon (chunk names "@addons/<folder>/..." or "@lua/<dir>/...").
 * It also wraps CLuaInterface::RunStringEx (vtable slot 111, FUN_180010540: every file and string GMod runs goes through it, from its
 * vtable) and times each run exclusive of nested runs and of its own parse, credited to the chunk it parsed: the files' own code.
 * F4: filesystem_stdio's imported Win32 file calls (IAT) are counted and timed on the main thread, inside a Lua run or outside, with the
 * failed lookups (no such file) apart: the search-path scans behind file.Exists, file.Find, include and Material. */
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

#define LOADX_RVA 0x1a8c0
#define LOADX_HASH 0x37255277u   /* FNV-1a of the 64 bytes at LOADX_RVA, lua_shared.dll timestamp 0x6adfccd3 */
static const uint8_t k_prologue[12] = { 0x4c, 0x8b, 0xdc, 0x53, 0x57, 0x48, 0x81, 0xec, 0xf8, 0x00, 0x00, 0x00 };

typedef int (*LoadBufferX)(void *L, const char *buf, size_t size, const char *name, const char *mode);
static LoadBufferX g_orig;
static int g_on;
static double g_mhz;

typedef struct { char name[64]; uint64_t n, bytes, ticks, run; } Group;
#define NGROUP 256
#define NSTATE 8
static Group g_groups[NGROUP];
static int g_ngroups;
static struct { void *L; uint64_t n, bytes, ticks; } g_states[NSTATE];
static uint64_t g_n, g_bytes, g_ticks, g_maxticks;
static char g_maxname[96];
static volatile LONG64 g_last_frame;   /* frame of the last load; 0 = nothing to report */
static uint64_t g_frame;

static void group_of(const char *name, char *out, size_t n) {
    const char *s = name ? name : "?";
    if (*s == '@' || *s == '=') s++;
    int slashes = (!strncmp(s, "addons/", 7) || !strncmp(s, "lua/", 4) || !strncmp(s, "gamemodes/", 10)) ? 2 : 1;
    size_t i = 0;
    for (; s[i] && i + 1 < n; i++) {
        if (s[i] == '/' && --slashes == 0) break;
        out[i] = s[i];
    }
    out[i] = 0;
}

static int group_index(const char *name) {
    char g[64];
    group_of(name, g, sizeof g);
    int i;
    for (i = 0; i < g_ngroups && strcmp(g_groups[i].name, g); i++) {}
    if (i == g_ngroups && g_ngroups < NGROUP) { strcpy(g_groups[i].name, g); g_ngroups++; }
    return i < NGROUP ? i : -1;
}

/* the RunStringEx frames open on the main thread: start, time of nested runs and parses, the group of the chunk it parsed */
typedef struct { uint64_t t0, inner, fs; int group; } Frame;
static Frame g_stack[64];
static int g_depth;
static uint64_t g_run, g_runs;

static void account(void *L, const char *name, size_t size, uint64_t t) {
    int i = group_index(name);
    if (i >= 0) { g_groups[i].n++; g_groups[i].bytes += size; g_groups[i].ticks += t; }
    if (g_depth > 0 && g_depth <= 64) { g_stack[g_depth - 1].inner += t; if (g_stack[g_depth - 1].group < 0) g_stack[g_depth - 1].group = i; }
    for (i = 0; i < NSTATE && g_states[i].L && g_states[i].L != L; i++) {}
    if (i < NSTATE) { g_states[i].L = L; g_states[i].n++; g_states[i].bytes += size; g_states[i].ticks += t; }
    g_n++; g_bytes += size; g_ticks += t;
    if (t > g_maxticks) { g_maxticks = t; snprintf(g_maxname, sizeof g_maxname, "%s", name ? name : "?"); }
    g_last_frame = (LONG64)(g_frame ? g_frame : 1);
}

static int hook_loadx(void *L, const char *buf, size_t size, const char *name, const char *mode) {
    if (GetCurrentThreadId() == cpu_host()->main_thread) jitprof_register(L);   /* PHOTON_CPU_PROBE_JIT (jitprof.c) */
    uint64_t t0 = __rdtsc();
    int r = g_orig(L, buf, size, name, mode);
    uint64_t t = __rdtsc() - t0;
    if (GetCurrentThreadId() == cpu_host()->main_thread) account(L, name, size, t);
    return r;
}

typedef uint64_t (*RunStringEx)(void *self, const char *file, const char *path, const char *code, uint64_t run, uint64_t print_errors,
                                uint64_t dont_push_errors, uint64_t no_returns);   /* the four flags are bytes, carried as raw stack slots */
static RunStringEx g_run_orig;

static uint64_t hook_run(void *self, const char *file, const char *path, const char *code, uint64_t run, uint64_t pe, uint64_t dp,
                         uint64_t nr) {
    if (GetCurrentThreadId() != cpu_host()->main_thread) return g_run_orig(self, file, path, code, run, pe, dp, nr);
    int d = g_depth++;
    if (d < 64) { g_stack[d].t0 = __rdtsc(); g_stack[d].inner = 0; g_stack[d].fs = 0; g_stack[d].group = -1; }
    uint64_t r = g_run_orig(self, file, path, code, run, pe, dp, nr);
    g_depth--;
    if (d < 64) {
        uint64_t incl = __rdtsc() - g_stack[d].t0, sub = g_stack[d].inner + g_stack[d].fs, own = incl > sub ? incl - sub : 0;
        if (g_stack[d].group >= 0) g_groups[g_stack[d].group].run += own;
        g_run += own; g_runs++;
        if (d > 0) g_stack[d - 1].inner += incl;
        g_last_frame = (LONG64)(g_frame ? g_frame : 1);
    }
    return r;
}

/* ---- F4: the file system's Win32 calls ---- */
enum { FS_CREATEA, FS_CREATEW, FS_FINDFIRSTA, FS_FINDFIRSTEXW, FS_FINDNEXTA, FS_FINDNEXTW, FS_FINDCLOSE, FS_ATTRA, FS_ATTRW, FS_READ,
       FS_CLOSE, FS_SIZEEX, FS_SIZE, FS_FINALPATH, FS_LONGPATH, FS_FULLPATHW, FS_TYPE, FS_INFO, FS_N };
static const char *const k_fs_name[FS_N] = { "CreateFileA", "CreateFileW", "FindFirstFileA", "FindFirstFileExW", "FindNextFileA",
    "FindNextFileW", "FindClose", "GetFileAttributesExA", "GetFileAttributesExW", "ReadFile", "CloseHandle", "GetFileSizeEx",
    "GetFileSize", "GetFinalPathNameByHandleA", "GetLongPathNameA", "GetFullPathNameW", "GetFileType", "GetFileInformationByHandle" };
static struct { uint64_t n, ticks, fail, lua_n, lua_ticks; } g_fs[FS_N];
static void *g_fs_orig[FS_N];

/* failed lookups by parent directory (lowercased, '\\' and '/' alike), to see whether they fall in missing directories */
typedef struct { char dir[200]; uint64_t n; } FailDir;
#define NFAIL 8192
static FailDir g_fail[NFAIL];
static int g_nfail, g_fail_overflow;
static char g_fail_sample[4][260];

static void fail_note(const char *path) {
    if (!path || GetCurrentThreadId() != cpu_host()->main_thread) return;
    char d[200];
    size_t n = 0, cut = 0;
    for (; path[n] && n + 1 < sizeof d; n++) {
        char c = path[n] == '/' ? '\\' : path[n];
        d[n] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        if (c == '\\') cut = n;
    }
    d[cut] = 0;
    uint32_t h = cpu_fnv(d, cut) & (NFAIL - 1);
    for (int probe = 0; probe < NFAIL; probe++, h = (h + 1) & (NFAIL - 1)) {
        if (!g_fail[h].n) {
            if (g_nfail >= NFAIL / 2) { g_fail_overflow++; return; }
            memcpy(g_fail[h].dir, d, cut + 1); g_fail[h].n = 1; g_nfail++;
            if (g_nfail <= 4) snprintf(g_fail_sample[g_nfail - 1], 260, "%s", path);
            return;
        }
        if (!strcmp(g_fail[h].dir, d)) { g_fail[h].n++; return; }
    }
}
static void fail_noteW(const wchar_t *w) {
    char a[260];
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof a, NULL, NULL)) return;
    fail_note(a);
}

static void fs_account(int k, uint64_t t0, int failed) {
    if (GetCurrentThreadId() != cpu_host()->main_thread) return;
    uint64_t t = __rdtsc() - t0;
    g_fs[k].n++; g_fs[k].ticks += t; g_fs[k].fail += failed;
    if (g_depth > 0) {
        g_fs[k].lua_n++; g_fs[k].lua_ticks += t;
        if (g_depth <= 64) g_stack[g_depth - 1].fs += t;   /* not the file's own code */
    }
}
#define FS_ORIG(k, T) ((T)g_fs_orig[k])
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef HANDLE (WINAPI *FindFirstFileExW_t)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
typedef BOOL (WINAPI *FindNextFileA_t)(HANDLE, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI *FindNextFileW_t)(HANDLE, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *FindClose_t)(HANDLE);
typedef BOOL (WINAPI *GetFileAttributesExA_t)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef BOOL (WINAPI *GetFileAttributesExW_t)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef BOOL (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandle_t)(HANDLE);
typedef BOOL (WINAPI *GetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
typedef DWORD (WINAPI *GetFileSize_t)(HANDLE, LPDWORD);
typedef DWORD (WINAPI *GetFinalPathNameByHandleA_t)(HANDLE, LPSTR, DWORD, DWORD);
typedef DWORD (WINAPI *GetLongPathNameA_t)(LPCSTR, LPSTR, DWORD);
typedef DWORD (WINAPI *GetFullPathNameW_t)(LPCWSTR, DWORD, LPWSTR, LPWSTR *);
typedef DWORD (WINAPI *GetFileType_t)(HANDLE);
typedef BOOL (WINAPI *GetFileInformationByHandle_t)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);

static HANDLE WINAPI w_CreateFileA(LPCSTR a, DWORD b, DWORD c, LPSECURITY_ATTRIBUTES d, DWORD e, DWORD f, HANDLE g) {
    uint64_t t = __rdtsc(); HANDLE r = FS_ORIG(FS_CREATEA, CreateFileA_t)(a, b, c, d, e, f, g);
    DWORD err = GetLastError(); fs_account(FS_CREATEA, t, r == INVALID_HANDLE_VALUE); if (r == INVALID_HANDLE_VALUE) fail_note(a); SetLastError(err); return r; }
static HANDLE WINAPI w_CreateFileW(LPCWSTR a, DWORD b, DWORD c, LPSECURITY_ATTRIBUTES d, DWORD e, DWORD f, HANDLE g) {
    uint64_t t = __rdtsc(); HANDLE r = FS_ORIG(FS_CREATEW, CreateFileW_t)(a, b, c, d, e, f, g);
    DWORD err = GetLastError(); fs_account(FS_CREATEW, t, r == INVALID_HANDLE_VALUE); if (r == INVALID_HANDLE_VALUE) fail_noteW(a); SetLastError(err); return r; }
static HANDLE WINAPI w_FindFirstFileA(LPCSTR a, LPWIN32_FIND_DATAA b) {
    uint64_t t = __rdtsc(); HANDLE r = FS_ORIG(FS_FINDFIRSTA, FindFirstFileA_t)(a, b);
    DWORD err = GetLastError(); fs_account(FS_FINDFIRSTA, t, r == INVALID_HANDLE_VALUE); if (r == INVALID_HANDLE_VALUE) fail_note(a); SetLastError(err); return r; }
static HANDLE WINAPI w_FindFirstFileExW(LPCWSTR a, FINDEX_INFO_LEVELS b, LPVOID c, FINDEX_SEARCH_OPS d, LPVOID e, DWORD f) {
    uint64_t t = __rdtsc(); HANDLE r = FS_ORIG(FS_FINDFIRSTEXW, FindFirstFileExW_t)(a, b, c, d, e, f);
    DWORD err = GetLastError(); fs_account(FS_FINDFIRSTEXW, t, r == INVALID_HANDLE_VALUE); SetLastError(err); return r; }
static BOOL WINAPI w_FindNextFileA(HANDLE a, LPWIN32_FIND_DATAA b) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_FINDNEXTA, FindNextFileA_t)(a, b);
    DWORD err = GetLastError(); fs_account(FS_FINDNEXTA, t, !r); SetLastError(err); return r; }
static BOOL WINAPI w_FindNextFileW(HANDLE a, LPWIN32_FIND_DATAW b) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_FINDNEXTW, FindNextFileW_t)(a, b);
    DWORD err = GetLastError(); fs_account(FS_FINDNEXTW, t, !r); SetLastError(err); return r; }
static BOOL WINAPI w_FindClose(HANDLE a) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_FINDCLOSE, FindClose_t)(a);
    DWORD err = GetLastError(); fs_account(FS_FINDCLOSE, t, !r); SetLastError(err); return r; }
static BOOL WINAPI w_GetFileAttributesExA(LPCSTR a, GET_FILEEX_INFO_LEVELS b, LPVOID c) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_ATTRA, GetFileAttributesExA_t)(a, b, c);
    DWORD err = GetLastError(); fs_account(FS_ATTRA, t, !r); if (!r) fail_note(a); SetLastError(err); return r; }
static BOOL WINAPI w_GetFileAttributesExW(LPCWSTR a, GET_FILEEX_INFO_LEVELS b, LPVOID c) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_ATTRW, GetFileAttributesExW_t)(a, b, c);
    DWORD err = GetLastError(); fs_account(FS_ATTRW, t, !r); SetLastError(err); return r; }
static BOOL WINAPI w_ReadFile(HANDLE a, LPVOID b, DWORD c, LPDWORD d, LPOVERLAPPED e) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_READ, ReadFile_t)(a, b, c, d, e);
    DWORD err = GetLastError(); fs_account(FS_READ, t, !r); SetLastError(err); return r; }
static BOOL WINAPI w_CloseHandle(HANDLE a) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_CLOSE, CloseHandle_t)(a);
    DWORD err = GetLastError(); fs_account(FS_CLOSE, t, !r); SetLastError(err); return r; }
static BOOL WINAPI w_GetFileSizeEx(HANDLE a, PLARGE_INTEGER b) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_SIZEEX, GetFileSizeEx_t)(a, b);
    DWORD err = GetLastError(); fs_account(FS_SIZEEX, t, !r); SetLastError(err); return r; }
static DWORD WINAPI w_GetFileSize(HANDLE a, LPDWORD b) {
    uint64_t t = __rdtsc(); DWORD r = FS_ORIG(FS_SIZE, GetFileSize_t)(a, b);
    DWORD err = GetLastError(); fs_account(FS_SIZE, t, 0); SetLastError(err); return r; }
static DWORD WINAPI w_GetFinalPathNameByHandleA(HANDLE a, LPSTR b, DWORD c, DWORD d) {
    uint64_t t = __rdtsc(); DWORD r = FS_ORIG(FS_FINALPATH, GetFinalPathNameByHandleA_t)(a, b, c, d);
    DWORD err = GetLastError(); fs_account(FS_FINALPATH, t, r == 0); SetLastError(err); return r; }
static DWORD WINAPI w_GetLongPathNameA(LPCSTR a, LPSTR b, DWORD c) {
    uint64_t t = __rdtsc(); DWORD r = FS_ORIG(FS_LONGPATH, GetLongPathNameA_t)(a, b, c);
    DWORD err = GetLastError(); fs_account(FS_LONGPATH, t, r == 0); SetLastError(err); return r; }
static DWORD WINAPI w_GetFullPathNameW(LPCWSTR a, DWORD b, LPWSTR c, LPWSTR *d) {
    uint64_t t = __rdtsc(); DWORD r = FS_ORIG(FS_FULLPATHW, GetFullPathNameW_t)(a, b, c, d);
    DWORD err = GetLastError(); fs_account(FS_FULLPATHW, t, r == 0); SetLastError(err); return r; }
static DWORD WINAPI w_GetFileType(HANDLE a) {
    uint64_t t = __rdtsc(); DWORD r = FS_ORIG(FS_TYPE, GetFileType_t)(a);
    DWORD err = GetLastError(); fs_account(FS_TYPE, t, 0); SetLastError(err); return r; }
static BOOL WINAPI w_GetFileInformationByHandle(HANDLE a, LPBY_HANDLE_FILE_INFORMATION b) {
    uint64_t t = __rdtsc(); BOOL r = FS_ORIG(FS_INFO, GetFileInformationByHandle_t)(a, b);
    DWORD err = GetLastError(); fs_account(FS_INFO, t, !r); SetLastError(err); return r; }
static void *const k_fs_wrap[FS_N] = { (void *)w_CreateFileA, (void *)w_CreateFileW, (void *)w_FindFirstFileA,
    (void *)w_FindFirstFileExW, (void *)w_FindNextFileA, (void *)w_FindNextFileW, (void *)w_FindClose, (void *)w_GetFileAttributesExA,
    (void *)w_GetFileAttributesExW, (void *)w_ReadFile, (void *)w_CloseHandle, (void *)w_GetFileSizeEx, (void *)w_GetFileSize,
    (void *)w_GetFinalPathNameByHandleA, (void *)w_GetLongPathNameA, (void *)w_GetFullPathNameW, (void *)w_GetFileType,
    (void *)w_GetFileInformationByHandle };

static void install_fs(void) {
    uint8_t *b = (uint8_t *)GetModuleHandleA("filesystem_stdio.dll");
    if (!b) { cpu_log("lua_load: filesystem_stdio.dll not loaded, file timing off"); return; }
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int hooked = 0;
    for (const IMAGE_IMPORT_DESCRIPTOR *d = (const void *)(b + dir->VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)b + d->Name, "KERNEL32.dll")) continue;
        const IMAGE_THUNK_DATA64 *names = (const void *)(b + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (void *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char *fn = (const char *)((const IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name;
            for (int k = 0; k < FS_N; k++) {
                if (strcmp(fn, k_fs_name[k])) continue;
                DWORD old;
                if (!VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &old)) break;
                g_fs_orig[k] = (void *)iat->u1.Function;
                InterlockedExchangePointer((void **)&iat->u1.Function, k_fs_wrap[k]);
                VirtualProtect(&iat->u1.Function, 8, old, &old);
                hooked++;
                break;
            }
        }
    }
    cpu_log("lua_load: filesystem_stdio: %d of %d file calls wrapped (file timing)", hooked, FS_N);
}

static void report_fs(double ms) {
    uint64_t n = 0, t = 0, ln = 0, lt = 0;
    for (int k = 0; k < FS_N; k++) { n += g_fs[k].n; t += g_fs[k].ticks; ln += g_fs[k].lua_n; lt += g_fs[k].lua_ticks; }
    cpu_log("lua_load: file calls (main thread): %llu, %.1f ms; inside Lua runs %llu, %.1f ms", (unsigned long long)n, t * ms,
            (unsigned long long)ln, lt * ms);
    for (int k = 0; k < FS_N; k++)
        if (g_fs[k].n)
            cpu_log("lua_load:   %-28s %8llu calls %6llu failed %8.1f ms; in Lua %8llu calls %8.1f ms", k_fs_name[k],
                    (unsigned long long)g_fs[k].n, (unsigned long long)g_fs[k].fail, g_fs[k].ticks * ms,
                    (unsigned long long)g_fs[k].lua_n, g_fs[k].lua_ticks * ms);
    memset(g_fs, 0, sizeof g_fs);
    /* failed lookups by directory: does the directory itself exist? (checked now, through the original call) */
    uint64_t in_missing = 0, in_existing = 0, dirs_missing = 0;
    for (int i = 0; i < NFAIL; i++) {
        if (!g_fail[i].n) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        int exists = ((GetFileAttributesExA_t)g_fs_orig[FS_ATTRA])(g_fail[i].dir, GetFileExInfoStandard, &fa) &&
                     (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (exists) in_existing += g_fail[i].n; else { in_missing += g_fail[i].n; dirs_missing++; }
        g_fail[i].dir[sizeof g_fail[i].dir - 1] = exists ? 'E' : 'M';
    }
    cpu_log("lua_load: failed lookups by directory: %d directories (%llu missing), %llu failures in missing directories, %llu in existing"
            " ones (%d directories not tallied)", g_nfail, (unsigned long long)dirs_missing, (unsigned long long)in_missing,
            (unsigned long long)in_existing, g_fail_overflow);
    for (int i = 0; i < 4 && g_fail_sample[i][0]; i++) cpu_log("lua_load:   sample failed path: %s", g_fail_sample[i]);
    for (int top = 0; top < 25; top++) {
        int best = -1;
        for (int i = 0; i < NFAIL; i++) if (g_fail[i].n && (best < 0 || g_fail[i].n > g_fail[best].n)) best = i;
        if (best < 0) break;
        cpu_log("lua_load:   %8llu failures in %s %s", (unsigned long long)g_fail[best].n,
                g_fail[best].dir[sizeof g_fail[best].dir - 1] == 'E' ? "existing" : "missing ", g_fail[best].dir);
        g_fail[best].n = 0;
    }
    memset(g_fail, 0, sizeof g_fail); g_nfail = g_fail_overflow = 0; memset(g_fail_sample, 0, sizeof g_fail_sample);
}

static int cmp_group(const void *a, const void *b) {
    uint64_t x = ((const Group *)a)->ticks + ((const Group *)a)->run, y = ((const Group *)b)->ticks + ((const Group *)b)->run;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(void) {
    double ms = 1.0 / (g_mhz * 1000.0);
    cpu_log("lua_load: burst: %llu chunks, %.0f KB source, parse %.1f ms (slowest %.1f ms: %s)", (unsigned long long)g_n,
            g_bytes / 1024.0, g_ticks * ms, g_maxticks * ms, g_maxname);
    for (int i = 0; i < NSTATE && g_states[i].L; i++)
        cpu_log("lua_load:   state %d (%p): %llu chunks, %.0f KB, %.1f ms", i, g_states[i].L, (unsigned long long)g_states[i].n,
                g_states[i].bytes / 1024.0, g_states[i].ticks * ms);
    qsort(g_groups, (size_t)g_ngroups, sizeof g_groups[0], cmp_group);
    cpu_log("lua_load: runs: %llu, own code %.1f ms (exclusive of nested runs, parsing and file calls)", (unsigned long long)g_runs,
            g_run * ms);
    report_fs(ms);
    for (int i = 0; i < g_ngroups && i < 25; i++)
        cpu_log("lua_load:   %-40s %5llu chunks %7.0f KB parse %7.1f ms run %8.1f ms", g_groups[i].name,
                (unsigned long long)g_groups[i].n, g_groups[i].bytes / 1024.0, g_groups[i].ticks * ms, g_groups[i].run * ms);
    g_ngroups = 0; g_n = g_bytes = g_ticks = g_maxticks = g_run = g_runs = 0; g_maxname[0] = 0;
    memset(g_states, 0, sizeof g_states);
}

void lua_load_present(uint64_t frame) {
    g_frame = frame;
    if (g_on && g_last_frame && frame >= (uint64_t)g_last_frame + 120) { g_last_frame = 0; report(); }
}

#define RUN_RVA 0x10540
#define RUN_HASH 0xf7a5601au   /* FNV-1a of the 64 bytes at RUN_RVA, same build */
static void install_run(uint8_t *base) {
    if (cpu_fnv(base + RUN_RVA, 64) != RUN_HASH) { cpu_log("lua_load: RunStringEx bytes differ, run timing off"); return; }
    void *fn = base + RUN_RVA;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    int slots = 0;
    g_run_orig = (RunStringEx)fn;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sh[i].Name, ".rdata", 6) && memcmp(sh[i].Name, ".data", 5)) continue;
        void **p = (void **)(base + sh[i].VirtualAddress), **e = p + sh[i].Misc.VirtualSize / sizeof(void *);
        for (; p < e; p++) {
            if (*p != fn) continue;
            DWORD old;
            if (!VirtualProtect(p, sizeof *p, PAGE_READWRITE, &old)) continue;
            InterlockedCompareExchangePointer(p, (void *)hook_run, fn);
            VirtualProtect(p, sizeof *p, old, &old);
            slots++;
        }
    }
    cpu_log("lua_load: RunStringEx wrapped in %d vtable slots (run timing)", slots);
}

/* Photon's own lua_shared (modules/cpu/lua_shared, L3) runs the profiler registration from its RunStringEx through an exported hook,
 * instead of the inline patch on GMod's build (which the hash check rightly refuses there). Parse, run and file timing stay GMod-only. */
static void photon_run_hook(void *L) {
    if (GetCurrentThreadId() == cpu_host()->main_thread) jitprof_register(L);
}

static void install(uint8_t *f) {
    void (*set_run_hook)(void (*)(void *)) = (void (*)(void (*)(void *)))(void *)GetProcAddress((HMODULE)f, "photon_lua_set_run_hook");
    if (set_run_hook) {
        set_run_hook(photon_run_hook);
        g_on = 1;
        cpu_log("lua_load: Photon's lua_shared: run hook set (jitprof only)");
        return;
    }
    f += LOADX_RVA;
    uint32_t h = cpu_fnv(f, 64);
    if (h != LOADX_HASH || memcmp(f, k_prologue, sizeof k_prologue)) {
        cpu_log("lua_load: luaL_loadbufferx bytes differ (hash %08x), off", h);
        return;
    }
    uint8_t *tr = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return;
    memcpy(tr, f, 12);
    tr[12] = 0xff; tr[13] = 0x25; memset(tr + 14, 0, 4);           /* jmp [rip+0] */
    uint64_t back = (uint64_t)(f + 12);
    memcpy(tr + 18, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tr, 26);
    g_orig = (LoadBufferX)(void *)tr;
    uint8_t patch[12] = { 0x48, 0xb8 };                              /* mov rax, imm64; jmp rax */
    uint64_t to = (uint64_t)(void *)hook_loadx;
    memcpy(patch + 2, &to, 8);
    patch[10] = 0xff; patch[11] = 0xe0;
    DWORD old;
    if (!VirtualProtect(f, 12, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(f, patch, 12);
    VirtualProtect(f, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 12);
    g_on = 1;
    cpu_log("lua_load: luaL_loadbufferx hooked (parse timing)");
    install_run(f - LOADX_RVA);
    install_fs();
}

/* lua_shared.dll loads after PhotonCpuInit. The loader's notification runs on the loading thread once the image is mapped and before
 * anything can call into it, so the patch never races a running luaL_loadbufferx. */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UStr;
typedef struct { ULONG Flags; const UStr *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } DllLoaded;
typedef VOID (CALLBACK *DllNotify)(ULONG reason, const DllLoaded *data, PVOID ctx);
typedef LONG (NTAPI *LdrRegister)(ULONG flags, DllNotify fn, PVOID ctx, PVOID *cookie);
static PVOID g_cookie;

static VOID CALLBACK on_dll(ULONG reason, const DllLoaded *d, PVOID ctx) {
    (void)ctx;
    if (reason != 1 || g_on || !d->BaseDllName || !d->BaseDllName->Buffer) return;   /* 1 = loaded */
    if (_wcsnicmp(d->BaseDllName->Buffer, L"lua_shared.dll", d->BaseDllName->Length / 2) || d->BaseDllName->Length / 2 != 14) return;
    install((uint8_t *)d->DllBase);
}

void lua_load_init(void) {
    const char *e = getenv("PHOTON_CPU_PROBE_LUALOAD"), *j = getenv("PHOTON_CPU_PROBE_JIT");
    if ((!e || *e != '1') && (!j || *j != '1')) return;   /* the JIT probe registers its function from this hook too */
    g_mhz = cpu_tsc_mhz();
    uint8_t *f = (uint8_t *)GetModuleHandleA("lua_shared.dll");
    if (f) { install(f); return; }
    LdrRegister reg = (LdrRegister)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
    if (!reg || reg(0, on_dll, NULL, &g_cookie) != 0) { cpu_log("lua_load: no DLL notification, off"); return; }
    cpu_log("lua_load: waiting for lua_shared.dll");
}
