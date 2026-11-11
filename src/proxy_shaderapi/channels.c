/* Lua -> proxy data channels (host version 8; analysis/47): shader packs push named float arrays every frame through the CPU module's Lua
 * binary module (photon_cpu.Push(name, floats) -> photon_cpu.dll -> host->push), and Photon's features read the latest values. Latest
 * value wins per channel; up to NCH channels of up to MAXV floats. Push: the main thread; read: any thread (a short lock and a copy). */
#include <windows.h>
#include <string.h>
#include "channels.h"
#include "frames.h"

enum { NCH = 16, MAXV = 4096 };
typedef struct { char name[32]; float v[MAXV]; int n; unsigned long long frame; } Ch;
static Ch g_ch[NCH]; static int g_nch; static SRWLOCK g_l = SRWLOCK_INIT;

void photon_chan_push(const char *name, const float *v, int n) {
    if (!name || !name[0] || n < 0) return;
    if (n > MAXV) n = MAXV;
    AcquireSRWLockExclusive(&g_l);
    Ch *c = NULL;
    for (int i = 0; i < g_nch; i++) if (!strncmp(g_ch[i].name, name, sizeof g_ch[i].name - 1)) { c = &g_ch[i]; break; }
    if (!c && g_nch < NCH) { c = &g_ch[g_nch++]; strncpy(c->name, name, sizeof c->name - 1); c->name[sizeof c->name - 1] = 0; }
    if (c) { if (n && v) memcpy(c->v, v, sizeof(float) * (size_t)n); c->n = n; c->frame = photon_frame_count(); }
    ReleaseSRWLockExclusive(&g_l);
}
int photon_chan_get(const char *name, float *out, int max, unsigned long long *frame) {
    int n = -1;
    AcquireSRWLockShared(&g_l);
    for (int i = 0; i < g_nch; i++)
        if (!strncmp(g_ch[i].name, name, sizeof g_ch[i].name - 1)) {
            n = g_ch[i].n < max ? g_ch[i].n : max;
            if (n > 0) memcpy(out, g_ch[i].v, sizeof(float) * (size_t)n);
            if (frame) *frame = g_ch[i].frame;
            break;
        }
    ReleaseSRWLockShared(&g_l);
    return n;
}
