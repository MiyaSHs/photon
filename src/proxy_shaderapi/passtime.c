/* OPT-VK: per-pass timing of Photon's own frame work (PHOTON_PASSTIME=1). CPU: QueryPerformanceCounter around each segment on the
 * render thread. GPU: a pair of D3DQUERYTYPE_TIMESTAMP queries per segment and frame, with a TIMESTAMPFREQ query per frame, in a ring of
 * NF frames read NF-1 frames later without flushing (a result not ready is skipped, never waited for). A segment run several times in a
 * frame keeps its first GPU pair and sums its CPU time. Averages per frame the segment ran in, logged every 600 frames. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "passtime.h"
#include "d3dconst.h"
#include "frames.h"
#include "log.h"

enum { NF = 4 };
static const char *const k_name[PT_N] = { "l3", "l1", "cast", "near", "screen", "outline" };
static int g_on = -1;
static IDirect3DQuery9 *g_q[NF][PT_N][2], *g_freq[NF];
static int g_issued[NF][PT_N], g_freq_issued[NF];
static unsigned g_f;
static LARGE_INTEGER g_t0[PT_N], g_qpf;
static double g_cpu[PT_N], g_gpu[PT_N];
static long long g_cpu_n[PT_N], g_gpu_n[PT_N];
static int g_ran[PT_N];   /* this frame */

static int on(void) {
    if (g_on < 0) {
        char e[4];
        g_on = GetEnvironmentVariableA("PHOTON_PASSTIME", e, sizeof e) > 0 && e[0] == '1';
        QueryPerformanceFrequency(&g_qpf);
        if (g_on) photon_log("passtime: on (CPU and GPU time of Photon's passes)");
    }
    return g_on;
}
static IDirect3DQuery9 *query(IDirect3DQuery9 **slot, D3DQUERYTYPE t) {
    if (!*slot) {
        IDirect3DDevice9 *d = photon_d3d_device();
        if (!d || FAILED(IDirect3DDevice9_CreateQuery(d, t, slot))) *slot = NULL;
    }
    return *slot;
}
void photon_passtime_begin(int seg) {
    if (!on() || seg < 0 || seg >= PT_N) return;
    QueryPerformanceCounter(&g_t0[seg]);
    unsigned s = g_f % NF;
    if (!g_issued[s][seg]) {
        if (!g_freq_issued[s] && query(&g_freq[s], D3DQUERYTYPE_TIMESTAMPFREQ)) { IDirect3DQuery9_Issue(g_freq[s], D3DISSUE_END); g_freq_issued[s] = 1; }
        IDirect3DQuery9 *q = query(&g_q[s][seg][0], D3DQUERYTYPE_TIMESTAMP);
        if (q) IDirect3DQuery9_Issue(q, D3DISSUE_END);
    }
}
void photon_passtime_end(int seg) {
    if (!on() || seg < 0 || seg >= PT_N) return;
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_cpu[seg] += (double)(t1.QuadPart - g_t0[seg].QuadPart) * 1e3 / (double)g_qpf.QuadPart;
    if (!g_ran[seg]) { g_ran[seg] = 1; g_cpu_n[seg]++; }
    unsigned s = g_f % NF;
    if (!g_issued[s][seg] && g_q[s][seg][0]) {
        IDirect3DQuery9 *q = query(&g_q[s][seg][1], D3DQUERYTYPE_TIMESTAMP);
        if (q) { IDirect3DQuery9_Issue(q, D3DISSUE_END); g_issued[s][seg] = 1; }
    }
}
void photon_passtime_frame(void) {
    if (!on()) return;
    memset(g_ran, 0, sizeof g_ran);
    g_f++;
    unsigned s = g_f % NF;   /* the oldest slot, issued NF-1 frames ago: read it before it is reused */
    UINT64 freq = 0;
    if (g_freq_issued[s] && IDirect3DQuery9_GetData(g_freq[s], &freq, sizeof freq, 0) == S_OK && freq)
        for (int k = 0; k < PT_N; k++) {
            if (!g_issued[s][k]) continue;
            UINT64 a = 0, b = 0;
            if (IDirect3DQuery9_GetData(g_q[s][k][0], &a, sizeof a, 0) == S_OK && IDirect3DQuery9_GetData(g_q[s][k][1], &b, sizeof b, 0) == S_OK && b >= a) {
                g_gpu[k] += (double)(b - a) * 1e3 / (double)freq; g_gpu_n[k]++;
            }
        }
    memset(g_issued[s], 0, sizeof g_issued[s]);
    g_freq_issued[s] = 0;
    if (photon_frame_count() % 600 == 0) {
        char line[512]; int n = 0;
        for (int k = 0; k < PT_N; k++) {
            if (!g_cpu_n[k]) continue;
            n += snprintf(line + n, sizeof line - (size_t)n, " %s cpu %.3f gpu %.3f ms (%lld frames);", k_name[k], g_cpu[k] / (double)g_cpu_n[k],
                          g_gpu_n[k] ? g_gpu[k] / (double)g_gpu_n[k] : 0.0, g_cpu_n[k]);
        }
        photon_log("passtime: per frame the pass ran:%s", n ? line : " none ran");
    }
}
