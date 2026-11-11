#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

static FILE *g_log;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK g_lock = SRWLOCK_INIT;

static BOOL CALLBACK open_log(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    char dir[MAX_PATH], path[2 * MAX_PATH + 8];
    DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) return TRUE;
    /* <module basename>.log: photon_shaders.log for the base copy, game_shader_dx9.log for a mod copy, so the two
     * copies of a proxy-plus-fallback install never share a file */
    char mod[MAX_PATH] = "photon_shaders";
    HMODULE self = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)open_log, &self)) {
        char full[MAX_PATH];
        DWORD m = GetModuleFileNameA(self, full, sizeof full);
        if (m > 0 && m < sizeof full) {
            const char *b = full;
            for (const char *q = full; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
            snprintf(mod, sizeof mod, "%s", b);
            char *dot = strrchr(mod, '.');
            if (dot && _stricmp(dot, ".dll") == 0) *dot = 0;
        }
    }
    snprintf(path, sizeof path, "%s\\%s.log", dir, mod);
    g_log = fopen(path, "a");
    return TRUE;
}

void photon_log(const char *fmt, ...) {
    InitOnceExecuteOnce(&g_once, open_log, NULL, NULL);
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    AcquireSRWLockExclusive(&g_lock);
    fprintf(g_log, "[%lu] ", GetCurrentThreadId());
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    ReleaseSRWLockExclusive(&g_lock);
    va_end(ap);
}
