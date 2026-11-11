#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "log.h"

static FILE *g_log;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK g_lock = SRWLOCK_INIT;

static BOOL CALLBACK open_log(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    char dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetEnvironmentVariableA("PHOTON_LOG_DIR", dir, sizeof dir);
    if (n == 0 || n >= sizeof dir) return TRUE;
    snprintf(path, sizeof path, "%s\\photon_proxy.log", dir);
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
