/* Photon CPU, phase 2 F5: a cache of failed file lookups (analysis/11). The engine's file system (filesystem_stdio) looks every file up in
 * each search path in turn, and every mounted addon folder adds paths: a play-set map load makes ~640,000 Win32 lookups that find no file,
 * ~16 µs each, 10 s of main-thread time. This answers "no such file" from directory listings instead.
 *   on by default (since 2026-11-09, after the gates in analysis/11): a lookup the cache knows fails returns the same failure (error code
 *     included) without the call
 *   PHOTON_CPU_FSCACHE=0  off
 *   PHOTON_CPU_FSCACHE=2  check: compute the cache's answer, still make the call, log every difference (the exactness gate)
 * Exactness, by construction:
 *   - only failures are answered. A lookup the listings say can succeed is always made, so every success is Windows' own;
 *   - existence comes from listings made with FindFirstFileExW: "dir\name" with a cached directory whose listing lacks name fails
 *     with ERROR_FILE_NOT_FOUND, and a path through a directory missing from its parent's listing fails with ERROR_PATH_NOT_FOUND, as
 *     Windows answers both;
 *   - any call through filesystem_stdio that can create, delete or rename (CreateFile with a disposition other than OPEN_EXISTING,
 *     DeleteFile, MoveFileEx, CreateDirectory, RemoveDirectory) drops, before and after the call, its path's parent listing and
 *     everything cached at or below the path (an unusual path inside the root, or a relative one, drops the whole cache);
 *   - changes made by anything else (other modules, other processes such as Steam) are seen by a recursive directory watch on the
 *     game's root folder (ReadDirectoryChangesW, names and directories), checked before every cached answer: each changed path drops its
 *     parent's listing and everything cached at or below it; a watch overflow or an unusual name drops the whole cache;
 *   - only simple paths are answered: absolute, inside the game root, printable ASCII, no empty, "." or ".." components, no wildcard,
 *     quote, angle bracket, pipe or stream colon, no '~' (8.3 names), no trailing dot or space, no DOS device names, under 240
 *     characters; directory entries that are reparse points are not descended. Everything else passes straight through.
 * Main thread only for answers (other threads pass through); invalidation from any thread. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "photon_cpu.h"

static int g_mode;   /* 0 off, 1 serve, 2 check */
static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE g_watch = INVALID_HANDLE_VALUE;   /* the root folder, opened for ReadDirectoryChangesW */
static OVERLAPPED g_ov;
static DWORD g_wbuf[16384];                      /* 64 KB, DWORD-aligned as the call requires */
static uint64_t g_changes, g_change_overflows;
static char g_change_sample[8][200];
static int g_nsample;
static char g_root[MAX_PATH];   /* lowercased, '\\', no trailing separator: "c:\\photon-run\\garrysmod" */
static size_t g_rootlen;

/* ---- directory table: lowercased path -> state and, once listed, its sorted lowercased entry names ---- */
enum { D_UNKNOWN, D_MISSING, D_DIR, D_NOTDIR };
typedef struct { char *name; uint8_t isdir, reparse; } Ent;
typedef struct { char *path; uint32_t hash; uint8_t state, listed, list_failed; Ent *ents; int nents; } Dir;
static Dir *g_dirs;
static uint32_t g_cap, g_count;
static uint64_t g_epoch_clears;
static int g_full;
static struct { uint64_t answered, passed, unsimple, lists, mismatch, checked; } g_st;

static uint32_t hash_str(const char *s, size_t n) { return cpu_fnv(s, n) | 1u; }

static void clear_all(void) {
    for (uint32_t i = 0; i < g_cap; i++) {
        Dir *d = &g_dirs[i];
        if (!d->path) continue;
        for (int k = 0; k < d->nents; k++) free(d->ents[k].name);
        free(d->ents); free(d->path);
    }
    memset(g_dirs, 0, sizeof(Dir) * g_cap);
    g_count = 0;
    g_epoch_clears++;
}

static Dir *find_dir(const char *p, size_t n, int create) {
    uint32_t h = hash_str(p, n);
    for (uint32_t i = h & (g_cap - 1);; i = (i + 1) & (g_cap - 1)) {
        Dir *d = &g_dirs[i];
        if (!d->path) {
            if (!create) return NULL;
            if (g_count * 2 >= g_cap) { g_full = 1; return NULL; }   /* full: no caching until the next lookup starts over */
            d->path = malloc(n + 1);
            if (!d->path) return NULL;
            memcpy(d->path, p, n); d->path[n] = 0; d->hash = h; g_count++;
            return d;
        }
        if (d->hash == h && !strncmp(d->path, p, n) && !d->path[n]) return d;
    }
}

static int cmp_ent(const void *a, const void *b) { return strcmp(((const Ent *)a)->name, ((const Ent *)b)->name); }

/* list directory d once (through kernel32 directly, not through the hooked import) */
static void list_dir(Dir *d) {
    if (d->listed || d->list_failed) return;
    wchar_t w[MAX_PATH + 4];
    int n = MultiByteToWideChar(CP_ACP, 0, d->path, -1, w, MAX_PATH);
    if (n <= 0 || n > MAX_PATH - 3) { d->list_failed = 1; return; }
    wcscpy(w + n - 1, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { d->list_failed = 1; return; }
    int cap = 64, cnt = 0;
    Ent *e = malloc(sizeof(Ent) * cap);
    int ok = e != NULL;
    do {
        if (!ok) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char a[MAX_PATH * 3];
        int m = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, a, sizeof a, NULL, NULL);
        if (m <= 0) { ok = 0; break; }
        for (char *c = a; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;   /* queries are ASCII; non-ASCII names never match them */
        if (cnt == cap) { Ent *g = realloc(e, sizeof(Ent) * (cap *= 2)); if (!g) { ok = 0; break; } e = g; }
        e[cnt].name = _strdup(a);
        if (!e[cnt].name) { ok = 0; break; }
        e[cnt].isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e[cnt].reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        cnt++;
    } while (FindNextFileW(h, &fd));
    DWORD end = GetLastError();
    FindClose(h);
    if (!ok || end != ERROR_NO_MORE_FILES) {
        for (int k = 0; k < cnt; k++) free(e[k].name);
        free(e);
        d->list_failed = 1;
        return;
    }
    qsort(e, (size_t)cnt, sizeof(Ent), cmp_ent);
    d->ents = e; d->nents = cnt; d->listed = 1;
    g_st.lists++;
}

static const Ent *lookup_ent(const Dir *d, const char *name) {
    int lo = 0, hi = d->nents - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(d->ents[mid].name, name);
        if (!c) return &d->ents[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* state of the directory p[0..n) (lowercased, inside the root) */
static int dir_state(const char *p, size_t n) {
    if (n == g_rootlen) return D_DIR;
    Dir *d = find_dir(p, n, 1);
    if (!d) return D_UNKNOWN;
    if (d->state != D_UNKNOWN) return d->state;
    size_t cut = n;
    while (cut > g_rootlen && p[cut - 1] != '\\') cut--;
    if (cut <= g_rootlen) return D_UNKNOWN;
    int ps = dir_state(p, cut - 1);
    if (ps == D_MISSING || ps == D_NOTDIR) { d->state = D_MISSING; return D_MISSING; }
    if (ps != D_DIR) return D_UNKNOWN;
    Dir *parent = find_dir(p, cut - 1, 0);
    if (!parent) { if (cut - 1 != g_rootlen) return D_UNKNOWN; parent = find_dir(p, cut - 1, 1); if (!parent) return D_UNKNOWN; }
    list_dir(parent);
    if (!parent->listed) return D_UNKNOWN;
    char name[MAX_PATH];
    memcpy(name, p + cut, n - cut); name[n - cut] = 0;
    const Ent *e = lookup_ent(parent, name);
    if (!e) { d->state = D_MISSING; return D_MISSING; }
    if (e->reparse) return D_UNKNOWN;
    d->state = e->isdir ? D_DIR : D_NOTDIR;
    return d->state;
}

static int reserved_name(const char *c, size_t n) {
    size_t b = 0;
    while (b < n && c[b] != '.') b++;
    static const char *const dev[] = { "con", "prn", "aux", "nul" };
    if (b == 3) for (int i = 0; i < 4; i++) if (!strncmp(c, dev[i], 3)) return 1;
    if (b == 4 && (!strncmp(c, "com", 3) || !strncmp(c, "lpt", 3)) && c[3] >= '0' && c[3] <= '9') return 1;
    return 0;
}

/* normalize a path to out (lowercase, '\\'); 0 if it is not a simple path inside the root */
static int simple_path(const char *in, char *out, size_t *len) {
    size_t n = 0;
    for (; in[n]; n++) {
        if (n >= 240) return 0;
        unsigned char c = (unsigned char)in[n];
        if (c < 0x20 || c > 0x7e || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '~') return 0;
        if (c == ':' && n != 1) return 0;
        out[n] = c == '/' ? '\\' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[n] = 0;
    if (n <= g_rootlen + 1 || strncmp(out, g_root, g_rootlen) || out[g_rootlen] != '\\') return 0;
    for (size_t s = g_rootlen + 1; s <= n;) {   /* components after the root */
        size_t e = s;
        while (e < n && out[e] != '\\') e++;
        size_t cl = e - s;
        if (!cl) return 0;
        if ((cl == 1 && out[s] == '.') || (cl == 2 && out[s] == '.' && out[s + 1] == '.')) return 0;
        if (out[e - 1] == '.' || out[e - 1] == ' ') return 0;
        if (reserved_name(out + s, cl)) return 0;
        s = e + 1;
    }
    *len = n;
    return 1;
}

static void forget(Dir *d) {
    for (int k = 0; k < d->nents; k++) free(d->ents[k].name);
    free(d->ents);
    d->ents = NULL; d->nents = 0; d->listed = 0; d->list_failed = 0; d->state = D_UNKNOWN;
}

/* a create, delete or rename at p (lowercased, inside the root): the parent's listing and everything cached at or below p go */
static void invalidate_path(const char *p, size_t n) {
    size_t cut = n;
    while (cut > g_rootlen && p[cut - 1] != '\\') cut--;
    if (cut > g_rootlen) {
        Dir *parent = find_dir(p, cut - 1, 0);
        if (parent) { uint8_t st = parent->state; forget(parent); parent->state = st; }
    } else {
        Dir *root = find_dir(g_root, g_rootlen, 0);
        if (root) forget(root);
    }
    for (uint32_t i = 0; i < g_cap; i++) {
        Dir *d = &g_dirs[i];
        if (d->path && !strncmp(d->path, p, n) && (d->path[n] == 0 || d->path[n] == '\\')) forget(d);
    }
}

static int watch_arm(void) {
    return ReadDirectoryChangesW(g_watch, g_wbuf, sizeof g_wbuf, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                 NULL, &g_ov, NULL);
}

/* apply what the watch reported since the last look (changes after its completion are buffered by the system for the next call) */
static void watch_poll(void) {
    /* no system call while nothing changed: the kernel writes the OVERLAPPED's status when the watch completes, before its event */
    if (!HasOverlappedIoCompleted(&g_ov)) return;
    DWORD got = 0;
    int ok = GetOverlappedResult(g_watch, &g_ov, &got, FALSE);
    static DWORD copy[16384];
    if (ok && got) memcpy(copy, g_wbuf, got);
    ResetEvent(g_ov.hEvent);
    if (!watch_arm()) { cpu_log("fs_cache: directory watch failed (%lu), off", GetLastError()); g_mode = 0; clear_all(); return; }
    if (!ok || !got) { g_change_overflows++; clear_all(); return; }
    for (const uint8_t *q = (const uint8_t *)copy;;) {
        const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)q;
        char p[256];
        size_t n = g_rootlen, len = fi->FileNameLength / 2;
        int simple = len && n + 1 + len < sizeof p;
        if (simple) {
            memcpy(p, g_root, n);
            p[n++] = '\\';
            for (size_t i = 0; i < len; i++) {
                wchar_t c = fi->FileName[i];
                if (c < 0x20 || c > 0x7e) { simple = 0; break; }
                p[n++] = c == '/' ? '\\' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
            }
            p[n] = 0;
        }
        g_changes++;
        if (!simple) { clear_all(); return; }
        if (g_nsample < 8) {
            int seen = 0;
            for (int i = 0; i < g_nsample; i++) if (!strcmp(g_change_sample[i], p + g_rootlen + 1)) seen = 1;
            if (!seen) snprintf(g_change_sample[g_nsample++], sizeof g_change_sample[0], "%s", p + g_rootlen + 1);
        }
        invalidate_path(p, n);
        if (!fi->NextEntryOffset) break;
        q += fi->NextEntryOffset;
    }
}

/* 0: unknown (make the call); otherwise the Win32 error the lookup fails with */
static DWORD cached_failure(const char *path) {
    char p[256];
    size_t n;
    if (!simple_path(path, p, &n)) { g_st.unsimple++; return 0; }
    watch_poll();
    if (!g_mode) return 0;
    if (g_full) { g_full = 0; clear_all(); }
    size_t cut = n;
    while (p[cut - 1] != '\\') cut--;
    int ds = dir_state(p, cut - 1);
    if (ds == D_MISSING) return ERROR_PATH_NOT_FOUND;   /* a path through a file (D_NOTDIR) is left to Windows */
    if (ds != D_DIR) return 0;
    Dir *d = find_dir(p, cut - 1, 1);
    if (!d) return 0;
    list_dir(d);
    if (!d->listed) return 0;
    return lookup_ent(d, p + cut) ? 0 : ERROR_FILE_NOT_FOUND;
}

static DWORD cached_failureW(const wchar_t *w) {
    char a[256];
    size_t i = 0;
    for (; w[i]; i++) { if (i >= 240 || w[i] > 0x7e) { g_st.unsimple++; return 0; } a[i] = (char)w[i]; }
    a[i] = 0;
    return cached_failure(a);
}

/* ---- the wrapped imports ---- */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *GetFileAttributesExA_t)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef BOOL (WINAPI *DeleteFileA_t)(LPCSTR);
typedef BOOL (WINAPI *DeleteFileW_t)(LPCWSTR);
typedef BOOL (WINAPI *MoveFileExW_t)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI *CreateDirectoryW_t)(LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI *RemoveDirectoryW_t)(LPCWSTR);
static CreateFileA_t o_CreateFileA;
static CreateFileW_t o_CreateFileW;
static GetFileAttributesExA_t o_GetFileAttributesExA;
static DeleteFileA_t o_DeleteFileA;
static DeleteFileW_t o_DeleteFileW;
static MoveFileExW_t o_MoveFileExW;
static CreateDirectoryW_t o_CreateDirectoryW;
static RemoveDirectoryW_t o_RemoveDirectoryW;

static void drop(void) { AcquireSRWLockExclusive(&g_lock); clear_all(); ReleaseSRWLockExclusive(&g_lock); }
/* a call that may create, delete or rename path: only what it can change goes (outside the root: nothing cached; unusual: all) */
static void drop_path(const char *a) {
    char p[256];
    size_t n;
    AcquireSRWLockExclusive(&g_lock);
    if (a && simple_path(a, p, &n)) invalidate_path(p, n);
    else if (a) {
        char q[256];
        size_t i = 0;
        for (; a[i] && i + 1 < sizeof q; i++) q[i] = (a[i] >= 'A' && a[i] <= 'Z') ? (char)(a[i] + 32) : a[i] == '/' ? '\\' : a[i];
        q[i] = 0;
        if (!strncmp(q, g_root, g_rootlen) || !(q[0] && q[1] == ':')) clear_all();   /* inside the root, or relative: all */
    } else clear_all();
    ReleaseSRWLockExclusive(&g_lock);
}
static void drop_pathW(const wchar_t *w) {
    char a[256];
    size_t i = 0;
    if (!w) { drop(); return; }
    for (; w[i]; i++) { if (i >= 240 || w[i] > 0x7e) { drop(); return; } a[i] = (char)w[i]; }
    a[i] = 0;
    drop_path(a);
}

static int is_main(void) { return GetCurrentThreadId() == cpu_host()->main_thread; }

static void mismatch(const char *what, const char *path, DWORD want, BOOL ok, DWORD got) {
    g_st.mismatch++;
    if (g_st.mismatch <= 20) cpu_log("fs_cache: MISMATCH %s %s: cache said error %lu, Windows %s (error %lu)", what, path, want,
                                     ok ? "found it" : "failed", got);
}

static HANDLE WINAPI w_CreateFileA(LPCSTR a, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    if (disp != OPEN_EXISTING) { drop_path(a); HANDLE r = o_CreateFileA(a, acc, sh, sa, disp, fl, t); DWORD e = GetLastError(); drop_path(a); SetLastError(e); return r; }
    if (!a || !is_main()) return o_CreateFileA(a, acc, sh, sa, disp, fl, t);
    AcquireSRWLockExclusive(&g_lock);
    DWORD f = cached_failure(a);
    ReleaseSRWLockExclusive(&g_lock);
    if (f && g_mode == 1) { g_st.answered++; SetLastError(f); return INVALID_HANDLE_VALUE; }
    HANDLE r = o_CreateFileA(a, acc, sh, sa, disp, fl, t);
    DWORD e = GetLastError();
    if (f) { g_st.checked++; if (r != INVALID_HANDLE_VALUE || e != f) mismatch("CreateFileA", a, f, r != INVALID_HANDLE_VALUE, e); }
    else g_st.passed++;
    SetLastError(e);
    return r;
}

static HANDLE WINAPI w_CreateFileW(LPCWSTR w, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    if (disp != OPEN_EXISTING) { drop_pathW(w); HANDLE r = o_CreateFileW(w, acc, sh, sa, disp, fl, t); DWORD e = GetLastError(); drop_pathW(w); SetLastError(e); return r; }
    if (!w || !is_main()) return o_CreateFileW(w, acc, sh, sa, disp, fl, t);
    AcquireSRWLockExclusive(&g_lock);
    DWORD f = cached_failureW(w);
    ReleaseSRWLockExclusive(&g_lock);
    if (f && g_mode == 1) { g_st.answered++; SetLastError(f); return INVALID_HANDLE_VALUE; }
    HANDLE r = o_CreateFileW(w, acc, sh, sa, disp, fl, t);
    DWORD e = GetLastError();
    if (f) {
        g_st.checked++;
        if (r != INVALID_HANDLE_VALUE || e != f) { char a[260]; WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof a, NULL, NULL); mismatch("CreateFileW", a, f, r != INVALID_HANDLE_VALUE, e); }
    } else g_st.passed++;
    SetLastError(e);
    return r;
}

static BOOL WINAPI w_GetFileAttributesExA(LPCSTR a, GET_FILEEX_INFO_LEVELS lv, LPVOID out) {
    if (!a || !is_main()) return o_GetFileAttributesExA(a, lv, out);
    AcquireSRWLockExclusive(&g_lock);
    DWORD f = cached_failure(a);
    ReleaseSRWLockExclusive(&g_lock);
    if (f && g_mode == 1) { g_st.answered++; SetLastError(f); return FALSE; }
    uint8_t before[sizeof(WIN32_FILE_ATTRIBUTE_DATA)];
    int probe_buf = f && out && lv == GetFileExInfoStandard;
    if (probe_buf) memcpy(before, out, sizeof before);
    BOOL r = o_GetFileAttributesExA(a, lv, out);
    DWORD e = GetLastError();
    if (f) {
        g_st.checked++;
        if (r || e != f) mismatch("GetFileAttributesExA", a, f, r, e);
        else if (probe_buf && memcmp(before, out, sizeof before)) mismatch("GetFileAttributesExA (buffer written on failure)", a, f, r, e);
    } else g_st.passed++;
    SetLastError(e);
    return r;
}

static BOOL WINAPI w_DeleteFileA(LPCSTR a) { drop_path(a); BOOL r = o_DeleteFileA(a); DWORD e = GetLastError(); drop_path(a); SetLastError(e); return r; }
static BOOL WINAPI w_DeleteFileW(LPCWSTR a) { drop_pathW(a); BOOL r = o_DeleteFileW(a); DWORD e = GetLastError(); drop_pathW(a); SetLastError(e); return r; }
static BOOL WINAPI w_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) {
    drop_pathW(a); drop_pathW(b); BOOL r = o_MoveFileExW(a, b, f); DWORD e = GetLastError(); drop_pathW(a); drop_pathW(b); SetLastError(e); return r; }
static BOOL WINAPI w_CreateDirectoryW(LPCWSTR a, LPSECURITY_ATTRIBUTES s) { drop_pathW(a); BOOL r = o_CreateDirectoryW(a, s); DWORD e = GetLastError(); drop_pathW(a); SetLastError(e); return r; }
static BOOL WINAPI w_RemoveDirectoryW(LPCWSTR a) { drop_pathW(a); BOOL r = o_RemoveDirectoryW(a); DWORD e = GetLastError(); drop_pathW(a); SetLastError(e); return r; }

static const struct { const char *name; void *wrap; void **orig; } k_imports[] = {
    { "CreateFileA", (void *)w_CreateFileA, (void **)&o_CreateFileA },
    { "CreateFileW", (void *)w_CreateFileW, (void **)&o_CreateFileW },
    { "GetFileAttributesExA", (void *)w_GetFileAttributesExA, (void **)&o_GetFileAttributesExA },
    { "DeleteFileA", (void *)w_DeleteFileA, (void **)&o_DeleteFileA },
    { "DeleteFileW", (void *)w_DeleteFileW, (void **)&o_DeleteFileW },
    { "MoveFileExW", (void *)w_MoveFileExW, (void **)&o_MoveFileExW },
    { "CreateDirectoryW", (void *)w_CreateDirectoryW, (void **)&o_CreateDirectoryW },
    { "RemoveDirectoryW", (void *)w_RemoveDirectoryW, (void **)&o_RemoveDirectoryW },
};
#define NIMPORTS (int)(sizeof k_imports / sizeof k_imports[0])

/* the outside-writer gate (PHOTON_CPU_FSCACHE_SELFTEST=<seconds after the cache starts>, 1 = 60): then, and 10 s later, this writes and
 * deletes garrysmod\\data\\photon_fsgate_ext.txt with plain kernel32 calls (not through filesystem_stdio, so only the directory watch can
 * see them, as with another process); the Lua gate (photon_cpu_fsgate ext) logs file.Exists as it changes */
static ULONGLONG g_t0;
static void selftest(void) {
    static long long at = -1;
    static int step;
    if (at < 0) { const char *e = getenv("PHOTON_CPU_FSCACHE_SELFTEST"); at = e ? atoll(e) : 0; if (at == 1) at = 60; }
    if (at <= 0 || step >= 2) return;
    ULONGLONG t = (GetTickCount64() - g_t0) / 1000;
    if (t < (ULONGLONG)at + 10 * (ULONGLONG)step) return;
    char p[MAX_PATH + 64];
    snprintf(p, sizeof p, "%s\\garrysmod\\data\\photon_fsgate_ext.txt", g_root);
    if (step++ == 0) {
        HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, "x", 1, &w, NULL); CloseHandle(h); }
        cpu_log("fs_cache: selftest: wrote %s (%s)", p, h != INVALID_HANDLE_VALUE ? "ok" : "failed");
    } else cpu_log("fs_cache: selftest: deleted %s (%s)", p, DeleteFileA(p) ? "ok" : "failed");
}

void fs_cache_present(uint64_t frame) {   /* the stats every 15 s while they change (frame rates differ too much for a frame count) */
    (void)frame;
    if (!g_mode) return;
    selftest();
    static ULONGLONG last_t;
    ULONGLONG now = GetTickCount64();
    if (now - last_t < 15000) return;
    last_t = now;
    static uint64_t last_answered, last_checked;
    if (g_st.answered == last_answered && g_st.checked == last_checked) return;
    last_answered = g_st.answered; last_checked = g_st.checked;
    cpu_log("fs_cache: %s: %llu failures answered, %llu checked, %llu mismatches, %llu made (unknown), %llu not simple; %llu listings,"
            " %u directories, %llu clears", g_mode == 1 ? "serve" : "check", (unsigned long long)g_st.answered,
            (unsigned long long)g_st.checked, (unsigned long long)g_st.mismatch, (unsigned long long)g_st.passed,
            (unsigned long long)g_st.unsimple, (unsigned long long)g_st.lists, g_count, (unsigned long long)g_epoch_clears);
    cpu_log("fs_cache: watch: %llu changes, %llu overflows; first changed: %s %s %s %s %s %s %s %s", (unsigned long long)g_changes,
            (unsigned long long)g_change_overflows, g_change_sample[0], g_change_sample[1], g_change_sample[2], g_change_sample[3],
            g_change_sample[4], g_change_sample[5], g_change_sample[6], g_change_sample[7]);
}

void fs_cache_init(void) {
    int m = cpu_setting("photon_cpu_fscache", "PHOTON_CPU_FSCACHE", 1);
    if (m != 1 && m != 2) { cpu_log("fs_cache: off (setting %d)", m); return; }
    const char *e = m == 1 ? "1" : "2";
    uint8_t *b = (uint8_t *)GetModuleHandleA("filesystem_stdio.dll");
    if (!b) { cpu_log("fs_cache: filesystem_stdio.dll not loaded, off"); return; }
    /* the root: the folder of the game's executable (it holds garrysmod\ and bin\) */
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    while (n && exe[n - 1] != '\\' && exe[n - 1] != '/') n--;
    if (n < 4) return;
    exe[--n] = 0;
    if (n > 10 && !_stricmp(exe + n - 10, "\\bin\\win64")) exe[n -= 10] = 0;   /* the x64 build runs from bin\win64 */
    else if (n > 4 && !_stricmp(exe + n - 4, "\\bin")) exe[n -= 4] = 0;
    for (size_t i = 0; exe[i]; i++) {
        unsigned char c = (unsigned char)exe[i];
        if (c < 0x20 || c > 0x7e || c == '~') { cpu_log("fs_cache: game root is not a simple path, off"); return; }
        g_root[i] = c == '/' ? '\\' : (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    g_rootlen = strlen(g_root);
    g_watch = CreateFileA(exe, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    g_ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_watch == INVALID_HANDLE_VALUE || !g_ov.hEvent || !watch_arm()) { cpu_log("fs_cache: no directory watch on %s, off", exe); return; }
    g_cap = 1u << 17;
    g_dirs = calloc(g_cap, sizeof(Dir));
    if (!g_dirs) return;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void **slot[NIMPORTS] = { 0 };
    for (const IMAGE_IMPORT_DESCRIPTOR *d = (const void *)(b + dir->VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)b + d->Name, "KERNEL32.dll")) continue;
        const IMAGE_THUNK_DATA64 *names = (const void *)(b + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (void *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char *fn = (const char *)((const IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name;
            for (int k = 0; k < NIMPORTS; k++) if (!strcmp(fn, k_imports[k].name)) slot[k] = (void **)&iat->u1.Function;
        }
    }
    for (int k = 0; k < NIMPORTS; k++)
        if (!slot[k]) { cpu_log("fs_cache: filesystem_stdio does not import %s, off", k_imports[k].name); return; }
    for (int k = 0; k < NIMPORTS; k++) *k_imports[k].orig = *slot[k];
    for (int k = 0; k < NIMPORTS; k++) {   /* the invalidating calls first, then the answering ones */
        int j = (k + 3) % NIMPORTS;
        DWORD old;
        if (!VirtualProtect(slot[j], 8, PAGE_READWRITE, &old)) continue;
        InterlockedExchangePointer(slot[j], k_imports[j].wrap);
        VirtualProtect(slot[j], 8, old, &old);
    }
    g_mode = *e - '0';
    g_t0 = GetTickCount64();
    cpu_log("fs_cache: %s mode, root %s", g_mode == 1 ? "serve" : "check", g_root);
}
