/* T4.5 stage 2a (analysis/23 §3.3, §5): a mirror of the D3D9 device's state. Stock creates a pure device (no Get* calls), so instancing needs
 * its own record of what is bound to replay a group and to restore the device afterwards. Behind PHOTON_BATCH=mirror (test only):
 * - the device gets a private copy of its vtable (the T4.6 / P6 pattern); the state-setting slots store into the mirror and call stock;
 * - counters: device calls per frame by kind, draws inside P6-suppressed DrawModelExecute calls (one per eligible draw is the precondition
 *   for capturing it), and at each suppressed call's first draw the translation in VS c58..c60 against the draw's origin (where the per-
 *   instance model matrix will go, §3.1).
 * Nothing is replayed yet; with the switch unset nothing is installed. */
#include <windows.h>
#define COBJMACROS
#include <d3d9.h>
#include <math.h>
#include <string.h>
#include "dmirror.h"
#include "pbrpack.h"
#include "d3dconst.h"
#include "p6.h"
#include "vlg.h"
#include "log.h"
#include "wrap.h"
#include "frames.h"
#include "shadows.h"
#include "texstream.h"
#include "texfilter.h"
#include "taa.h"
static int g_ts;   /* TEXSTREAM (PHOTON_TEXSTREAM=1) */

enum { S_SETRT = 37, S_SETDS = 39, S_SETVP = 47, S_SETRS = 57, S_SETTEX = 65, S_SETSAMP = 69, S_DP = 81, S_DIP = 82, S_SETDECL = 87, S_SETFVF = 89, S_SETVS = 92, S_VSCF = 94, S_VSCI = 96,
       S_VSCB = 98, S_SETSTREAM = 100, S_SETFREQ = 102, S_SETIB = 104, S_SETPS = 107, S_PSCF = 109, S_PSCI = 111, S_PSCB = 113, NSLOTS_MAX = 192 };   /* IDirect3DDevice9Ex has 134 slots (GMod uses D3D9Ex): copy them all */

typedef struct Mirror {
    DWORD rs[256];
    DWORD samp[20][16];
    void *tex[20];
    void *vs, *ps, *decl, *ib;
    DWORD fvf;
    struct { void *vb; UINT off, stride, freq; } stream[16];
    float vsc[256][4], psc[224][4];
    int vsi[16][4], psi[16][4];
    BOOL vsb[16], psb[16];
} Mirror;
static Mirror g_m;

static int g_want, g_bound, g_failed;
static int g_replay;                 /* PHOTON_BATCH=replay: stage 2b, the mirror plus exact-order replay of recorded draws */
static unsigned g_view_gen = 1;      /* bumped by SetRenderTarget / SetDepthStencilSurface / SetViewport: groups never cross views */
static void *g_dev;
static void **g_real_vt, **g_vt;
static void *g_fn[NSLOTS_MAX];   /* stock's functions */
static volatile LONG64 g_calls[NSLOTS_MAX], g_draws, g_draws_supp, g_supp_first, g_c58_ok, g_c58_bad, g_frames;
static int t_seen_supp_draw;   /* a draw was already counted in the current suppressed call (reset by p6 per call) */

#define FN(slot, type) ((type)g_fn[slot])
static const struct Rec *g_held;   /* lazy restore (see restore_held): the replayed group whose state the device holds, NULL = the mirror's */
static void restore_held(void);
#define FLUSH() do { if (g_held) restore_held(); } while (0)
typedef HRESULT (WINAPI *RsFn)(void *, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (WINAPI *TexFn)(void *, DWORD, void *);
typedef HRESULT (WINAPI *SampFn)(void *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (WINAPI *PtrFn)(void *, void *);
typedef HRESULT (WINAPI *DwFn)(void *, DWORD);
typedef HRESULT (WINAPI *CfFn)(void *, UINT, const float *, UINT);
typedef HRESULT (WINAPI *CiFn)(void *, UINT, const int *, UINT);
typedef HRESULT (WINAPI *CbFn)(void *, UINT, const BOOL *, UINT);
typedef HRESULT (WINAPI *StreamFn)(void *, UINT, void *, UINT, UINT);
typedef HRESULT (WINAPI *FreqFn)(void *, UINT, UINT);
typedef HRESULT (WINAPI *DpFn)(void *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (WINAPI *DipFn)(void *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

typedef HRESULT (WINAPI *Rt2Fn)(void *, DWORD, void *);
static void *g_rt0;   /* render target 0 the engine set last (T4.2 stage 1a diagnostics) */
static HRESULT WINAPI h_setrt(void *d, DWORD i, void *sfc) { FLUSH(); g_view_gen++; if (i == 0) g_rt0 = sfc; photon_shadows_trace("SetRT", (int)i, sfc, 0); return FN(S_SETRT, Rt2Fn)(d, i, sfc); }
void *photon_dmirror_rt0(void) { return g_rt0; }
static void *g_ds;   /* the depth-stencil surface the engine set last (T4.7a: end-of-frame box queries test against it) */
static void (*g_zclear_cb)(void);   /* T4.7a (cull.c): called before the engine clears depth, while the frame's depth still holds the view */
void photon_dmirror_on_zclear(void (*cb)(void)) { g_zclear_cb = cb; }
enum { S_CLEAR = 43 };
typedef HRESULT (WINAPI *ClearFn)(void *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
static volatile LONG64 g_zclears;
static HRESULT WINAPI h_clear(void *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z, DWORD st) { FLUSH();
    if ((flags & D3DCLEAR_ZBUFFER) && g_zclear_cb) { g_zclears++; g_zclear_cb(); }
    if (flags & D3DCLEAR_ZBUFFER) photon_shadows_zclear(g_ds, g_rt0);   /* T4.2: the main view's depth is still there */
    photon_shadows_trace("Clear ds", 0, g_ds, flags);
    return FN(S_CLEAR, ClearFn)(d, n, r, flags, c, z, st);
}
static HRESULT WINAPI h_setds(void *d, void *sfc) {
    FLUSH(); g_view_gen++;
    photon_shadows_trace("SetDS", 0, sfc, 0);
    if (sfc != g_ds) photon_shadows_ds_switch(g_ds, sfc, g_rt0);   /* T4.2: leaving the main view's depth: it is complete and still bound */
    sfc = photon_shadows_ds(sfc); g_ds = sfc; return FN(S_SETDS, PtrFn)(d, sfc);
}
/* device Reset (slot 16): T4.2's INTZ stand-in is a default-pool object, released before and recreated after */
enum { S_RESET = 16 };
static HRESULT WINAPI h_reset(void *d, void *pp) {
    FLUSH();
    photon_shadows_pre_reset();
    HRESULT hr = FN(S_RESET, PtrFn)(d, pp);
    photon_shadows_post_reset(d, SUCCEEDED(hr));
    return hr;
}
static D3DVIEWPORT9 g_vp;   /* the viewport the engine set last (T4.2's pass restores it) */
static int g_vp_known;
int photon_dmirror_viewport(unsigned v[4]) { if (!g_vp_known) return 0; v[0] = g_vp.X; v[1] = g_vp.Y; v[2] = g_vp.Width; v[3] = g_vp.Height; return 1; }   /* the engine's viewport now */
/* UPSCALE (analysis/45): armed at the main view's end when it was scaled; the first full-size viewport on that back buffer after it is the
 * engine's dev/upscale (viewrender.cpp's mat_viewportupscale block), and its next draw is replaced by the callback (returns 1: done) */
static int g_up_arm; static int (*g_up_cb)(void); static void *g_up_rt; static DWORD g_up_w, g_up_h;
static volatile LONG64 g_up_done, g_up_missed;
void photon_dmirror_upscale_arm(int (*cb)(void), void *rt, unsigned w, unsigned h) {
    if (g_up_arm) g_up_missed++;   /* armed and never seen (no upscale draw this frame) */
    g_up_arm = cb ? 1 : 0; g_up_cb = cb; g_up_rt = rt; g_up_w = w; g_up_h = h;
}
long long photon_dmirror_upscale_count(void) { return g_up_done; }
static int upscale_now(void) {   /* the draw hooks: this draw is the engine's upscale */
    if (g_up_arm != 2) return 0;
    g_up_arm = 0;
    if (!g_up_cb || !g_up_cb()) return 0;
    g_up_done++;
    return 1;
}
static HRESULT WINAPI h_setvp(void *d, const void *vp) { FLUSH(); g_view_gen++;
    if (vp && g_up_arm) {
        const D3DVIEWPORT9 *v = vp;
        g_up_arm = g_up_arm == 1 && g_rt0 == g_up_rt && v->X == 0 && v->Y == 0 && v->Width == g_up_w && v->Height == g_up_h ? 2 : g_up_arm == 2 ? 0 : 1;
    }
    if (vp) { g_vp = *(const D3DVIEWPORT9 *)vp; g_vp_known = 1;
              photon_shadows_trace("SetVP", (int)(g_vp.X << 16 | g_vp.Y), NULL, g_vp.Width << 16 | g_vp.Height); }   /* UPSCALE probe: idx x<<16|y, flags w<<16|h */
    return FN(S_SETVP, PtrFn)(d, (void *)vp); }
static unsigned char g_rs_known[256];   /* the render state was set since the mirror bound (a pure device cannot be asked) */
static HRESULT WINAPI h_setrs(void *d, D3DRENDERSTATETYPE s, DWORD v) { FLUSH();
    g_calls[S_SETRS]++;
    if ((unsigned)s < 256) { g_m.rs[s] = v; g_rs_known[s] = 1; }
    return FN(S_SETRS, RsFn)(d, s, v);
}
static HRESULT WINAPI h_settex(void *d, DWORD st, void *t) { FLUSH();
    g_calls[S_SETTEX]++;
    DWORD i = st >= D3DVERTEXTEXTURESAMPLER0 ? 16 + (st - D3DVERTEXTEXTURESAMPLER0) : st;
    if (g_ts) photon_texstream_bound(t);   /* TEXSTREAM: an evicted texture gets its top levels back before the draw */
    t = photon_texf_bind(st, t);           /* SP 1b: a shader pack's filtered copy of a base texture (the mirror records what is bound) */
    if (i < 20) g_m.tex[i] = t;
    return FN(S_SETTEX, TexFn)(d, st, t);
}
static unsigned char g_samp_known[20][16];   /* the sampler state was set since the mirror bound */
static HRESULT WINAPI h_setsamp(void *d, DWORD st, D3DSAMPLERSTATETYPE s, DWORD v) { FLUSH();
    g_calls[S_SETSAMP]++;
    DWORD i = st >= D3DVERTEXTEXTURESAMPLER0 ? 16 + (st - D3DVERTEXTEXTURESAMPLER0) : st;
    if (i < 20 && (unsigned)s < 16) { g_m.samp[i][s] = v; g_samp_known[i][s] = 1; }
    return FN(S_SETSAMP, SampFn)(d, st, s, v);
}
static void sh_used(void *s);
static void *lazy_real(void *d, void *s);
static HRESULT WINAPI h_setvs(void *d, void *s) { FLUSH(); g_calls[S_SETVS]++; sh_used(s); s = lazy_real(d, s); g_m.vs = s; return FN(S_SETVS, PtrFn)(d, s); }
/* created shaders seen set at least once (load-time study, analysis/32): a set of the pointers CreateVertexShader/CreatePixelShader
 * returned, with a used flag and the size, filled on the creating thread and marked on the draw thread (single writer each way: the
 * flag is a byte). Stale entries after a Release are harmless for a count. */
enum { SHSET = 1 << 15 };
typedef struct { void *volatile p; volatile uint32_t bytes; volatile uint8_t kind, used; } ShEnt;
static ShEnt g_sh[SHSET];
static volatile LONG g_sh_full;
static ShEnt *sh_slot(void *p, int add) {
    uint32_t h = (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) & (SHSET - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (SHSET - 1)) {
        void *q = g_sh[h].p;
        if (q == p) return &g_sh[h];
        if (!q) {
            if (!add) return NULL;
            if (InterlockedCompareExchangePointer((PVOID volatile *)&g_sh[h].p, p, NULL) == NULL) return &g_sh[h];
            if (g_sh[h].p == p) return &g_sh[h];
        }
    }
    if (add) InterlockedIncrement(&g_sh_full);
    return NULL;
}
static void sh_used(void *s) { if (s) { ShEnt *e = sh_slot(s, 0); if (e && !e->used) e->used = 1; } }
static HRESULT WINAPI h_setps(void *d, void *s) { FLUSH(); g_calls[S_SETPS]++; sh_used(s); s = lazy_real(d, s); g_m.ps = s; return FN(S_SETPS, PtrFn)(d, s); }
static HRESULT WINAPI h_setdecl(void *d, void *s) { FLUSH(); g_calls[S_SETDECL]++; g_m.decl = s; return FN(S_SETDECL, PtrFn)(d, s); }
static HRESULT WINAPI h_setib(void *d, void *s) { FLUSH(); g_calls[S_SETIB]++; g_m.ib = s; return FN(S_SETIB, PtrFn)(d, s); }
static HRESULT WINAPI h_setfvf(void *d, DWORD f) { FLUSH(); g_calls[S_SETFVF]++; g_m.fvf = f; return FN(S_SETFVF, DwFn)(d, f); }
static int g_rec_target_any(void);
static int g_rec_vshi;   /* the recorded call wrote VS constants at c61 or above (a bone palette): not replayable */
static HRESULT WINAPI h_vscf(void *d, UINT r, const float *v, UINT n) { FLUSH();
    g_calls[S_VSCF]++;
    if (r + n > 61 && g_rec_target_any()) g_rec_vshi = 1;
    if (v && r < 256) memcpy(g_m.vsc[r], v, (size_t)(n <= 256 - r ? n : 256 - r) * 16);
    return FN(S_VSCF, CfFn)(d, r, v, n);
}
static HRESULT WINAPI h_pscf(void *d, UINT r, const float *v, UINT n) { FLUSH();
    g_calls[S_PSCF]++;
    if (v && r < 224) memcpy(g_m.psc[r], v, (size_t)(n <= 224 - r ? n : 224 - r) * 16);
    return FN(S_PSCF, CfFn)(d, r, v, n);
}
static HRESULT WINAPI h_vsci(void *d, UINT r, const int *v, UINT n) { FLUSH();
    g_calls[S_VSCI]++;
    if (v && r < 16) memcpy(g_m.vsi[r], v, (size_t)(n <= 16 - r ? n : 16 - r) * 16);
    return FN(S_VSCI, CiFn)(d, r, v, n);
}
static HRESULT WINAPI h_psci(void *d, UINT r, const int *v, UINT n) { FLUSH();
    g_calls[S_PSCI]++;
    if (v && r < 16) memcpy(g_m.psi[r], v, (size_t)(n <= 16 - r ? n : 16 - r) * 16);
    return FN(S_PSCI, CiFn)(d, r, v, n);
}
static HRESULT WINAPI h_vscb(void *d, UINT r, const BOOL *v, UINT n) { FLUSH();
    g_calls[S_VSCB]++;
    if (v && r < 16) memcpy(&g_m.vsb[r], v, (size_t)(n <= 16 - r ? n : 16 - r) * sizeof(BOOL));
    return FN(S_VSCB, CbFn)(d, r, v, n);
}
static HRESULT WINAPI h_pscb(void *d, UINT r, const BOOL *v, UINT n) { FLUSH();
    g_calls[S_PSCB]++;
    if (v && r < 16) memcpy(&g_m.psb[r], v, (size_t)(n <= 16 - r ? n : 16 - r) * sizeof(BOOL));
    return FN(S_PSCB, CbFn)(d, r, v, n);
}
static HRESULT WINAPI h_stream(void *d, UINT s, void *vb, UINT off, UINT stride) { FLUSH();
    g_calls[S_SETSTREAM]++;
    if (s < 16) { g_m.stream[s].vb = vb; g_m.stream[s].off = off; g_m.stream[s].stride = stride; }
    return FN(S_SETSTREAM, StreamFn)(d, s, vb, off, stride);
}
static HRESULT WINAPI h_freq(void *d, UINT s, UINT f) { FLUSH(); g_calls[S_SETFREQ]++; if (s < 16) g_m.stream[s].freq = f; return FN(S_SETFREQ, FreqFn)(d, s, f); }

/* --- stage 2b: record and replay (analysis/23 §1.5, §3.3) ---------------------------------------------------------------------------- */
typedef struct Rec {
    int ok;
    Mirror st;   /* the device state at the draw */
    int dip;     /* 1 DrawIndexedPrimitive, 0 DrawPrimitive */
    D3DPRIMITIVETYPE type; INT bv; UINT mi, nv, si, pc;
} Rec;
static Rec *g_rec_target;            /* the group being recorded by the current suppressed call (p6.c), NULL when not recording */
static int g_rec_target_any(void) { return g_rec_target != NULL; }
static int g_rec_draws;
static volatile LONG64 g_replays, g_replay_fail, g_rec_ok, g_rec_multi, g_rec_bones, g_rec_dropped;
/* T4.2 stage 1c: this frame's shadow casters (device thread): every recording that completed and every replayed copy, with its model
 * matrix; shadows.c draws them into the sun shadow map at the main view's end */
enum { NCAST = 8192 };
typedef struct {
    const Rec *r;          /* a replayed copy: its recording (state) and m34 below; NULL: the snapshot below */
    float m34[3][4];
    void *vs, *decl, *ib;
    struct { void *vb; UINT off, stride; } stream[4];
    float vsc[58][4];
    int dip; D3DPRIMITIVETYPE type; INT bv; UINT mi, nv, si, pc;
    int bones;             /* skinned: its bone palette c58..c216 in g_bones[bones]; -1 rigid (c58..c60 = m34) */
    unsigned zstreams;     /* streams 1..3 bound to a dynamic VB (the flex deltas): drawn from the zero VB instead */
    void *tex0; int alpha; /* the draw's stage-0 texture and its alpha test (PT-RT: leaves cut in the sun map) */
    int noshadow;          /* AA: kept for the motion vectors only (shadows off, or characters not casting): the sun maps skip it */
} Caster;
static Caster *g_cast;
static int g_ncast;
static unsigned long long g_cast_frame = ~0ull;
static volatile LONG64 g_cast_full;
enum { NBONEREG = 159, NBONES = 2048 };
static float (*g_bones)[NBONEREG][4];
static int g_nbones;
static void cast_note(const Rec *r, const float m34[3][4]) {
    if (!photon_shadows_drawing()) return;
    unsigned long long f = photon_frame_count();
    if (f != g_cast_frame) { g_cast_frame = f; g_ncast = 0; g_nbones = 0; }
    if (!g_cast && !(g_cast = HeapAlloc(GetProcessHeap(), 0, sizeof(Caster) * NCAST))) return;
    if (g_ncast >= NCAST) { g_cast_full++; return; }
    g_cast[g_ncast].r = r;
    g_cast[g_ncast].bones = -1; g_cast[g_ncast].zstreams = 0; g_cast[g_ncast].noshadow = 0;
    g_cast[g_ncast].tex0 = r->st.tex[0]; g_cast[g_ncast].alpha = r->st.rs[D3DRS_ALPHATESTENABLE] != 0;
    memcpy(g_cast[g_ncast].m34, m34, sizeof g_cast[g_ncast].m34);
    g_ncast++;
}
/* a draw of one of our model pairs: its own snapshot. Skinned draws (T4.2 skinned casters: the vlg pass's SKINNING combo, or a VS that
 * reads c[a0 + 58..60], which also catches the bump pair whose combo is not decoded; a vertex declaration with BLENDINDICES
 * does not tell: rigid physics props carry bone indices too) keep their bone palette c58..c216 (53 bones,
 * 3 rows each; the engine writes the ones the mesh uses before each draw, so the mirror holds exactly this draw's set). A dynamic VB on
 * stream 0 (CPU-skinned or CPU-flexed) is rewritten later in the frame: left out. A dynamic VB on streams 1..3 is the flex-delta stream:
 * drawn from a zero VB (the neutral face; a shadow cannot show an expression). */
static volatile LONG64 g_cast_snap_skinned, g_cast_skin_dynvb, g_cast_skin_flex, g_cast_skin_full, g_cast_skin_off;
static int vs_indexed(void *vs) {   /* the VS reads c[a0 + 58..60]: the skinned variant's bone palette lookup (other relative reads, such as
                                     * Photon's light data, do not count) */
    enum { NK = 1024 };
    static void *k[NK]; static signed char v[NK];
    if (!vs) return 0;
    size_t hs = ((uintptr_t)vs >> 4) & (NK - 1);
    for (size_t q = 0; q < 8; q++) { size_t e = (hs + q) & (NK - 1); if (k[e] == vs) return v[e]; if (!k[e]) break; }
    UINT sz = 0;
    IDirect3DVertexShader9 *sh = vs;
    int r = 0;
    if (SUCCEEDED(sh->lpVtbl->GetFunction(sh, NULL, &sz)) && sz >= 8) {
        DWORD *b = HeapAlloc(GetProcessHeap(), 0, sz);
        if (b && SUCCEEDED(sh->lpVtbl->GetFunction(sh, b, &sz))) {
            UINT n = sz / 4;
            for (UINT t = 1; t < n;) {   /* SM2/3 tokens: the instruction length is in bits 24..27 */
                DWORD tok = b[t];
                if (tok == 0x0000FFFF) break;   /* end */
                if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }   /* comment */
                UINT len = (tok >> 24) & 0xF, op = tok & 0xFFFF;
                if (op != 31 && op != 81 && op != 82 && op != 83)   /* not dcl / def / defi / defb (literal tokens) */
                    for (UINT q = 1; q <= len && t + q < n; q++) {
                        DWORD pt = b[t + q];
                        UINT type = ((pt >> 28) & 7) | ((pt >> 8) & 0x18), reg = pt & 0x7FF;
                        if ((pt & 0x80000000u) && (pt & 0x2000) && type == 2 && reg >= 58 && reg <= 60) { r = 1; break; }   /* c[a0 + 58..60] */
                    }
                if (r) break;
                t += 1 + len;
            }
        }
        if (b) HeapFree(GetProcessHeap(), 0, b);
    }
    for (size_t q = 0; q < 8; q++) { size_t e = (hs + q) & (NK - 1); if (!k[e] || q == 7) { k[e] = vs; v[e] = (signed char)r; break; } }
    return r;
}
static int vb_dynamic(void *vb) {
    static void *k[256]; static signed char v[256]; static int n;
    if (!vb) return 0;
    for (int i = 0; i < n && i < 256; i++) if (k[i] == vb) return v[i];
    D3DVERTEXBUFFER_DESC dsc;
    int r = SUCCEEDED(IDirect3DVertexBuffer9_GetDesc((IDirect3DVertexBuffer9 *)vb, &dsc)) && (dsc.Usage & D3DUSAGE_DYNAMIC);
    k[n & 255] = vb; v[n & 255] = (signed char)r; n++;
    return r;
}
static void cast_snap(int dip, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    int skinned = 0;
    int shadows = photon_shadows_drawing(), mv = photon_taa_on();   /* AA: TAA's motion vectors take the same list */
    if (!(shadows || mv) || !photon_vlg_model_pass(&skinned)) return;
    unsigned long long f = photon_frame_count();
    if (f != g_cast_frame) { g_cast_frame = f; g_ncast = 0; g_nbones = 0; }
    unsigned zs = 0;
    int noshadow = !shadows;
    if (skinned || vs_indexed(g_m.vs)) {
        g_cast_snap_skinned++;
        if (!photon_shadows_skinned()) {   /* PHOTON_SHADOWS_SKINNED=0, or photon_shadow_characters 0 */
            g_cast_skin_off++;
            if (!mv) return;
            noshadow = 1;
        }
        if (vb_dynamic(g_m.stream[0].vb)) { g_cast_skin_dynvb++; return; }
        for (int i = 1; i < 4; i++) if (g_m.stream[i].vb && vb_dynamic(g_m.stream[i].vb)) zs |= 1u << i;
        if (zs) g_cast_skin_flex++;
        if (!g_bones && !(g_bones = HeapAlloc(GetProcessHeap(), 0, sizeof(*g_bones) * NBONES))) return;
        if (g_nbones >= NBONES) { g_cast_skin_full++; return; }
        skinned = 1;
    } else if (vb_dynamic(g_m.stream[0].vb)) return;
    if (!g_cast && !(g_cast = HeapAlloc(GetProcessHeap(), 0, sizeof(Caster) * NCAST))) return;
    if (g_ncast >= NCAST) { g_cast_full++; return; }
    Caster *c = &g_cast[g_ncast++];
    c->r = NULL;
    c->zstreams = zs;
    c->bones = -1;
    c->noshadow = noshadow;
    if (skinned) { c->bones = g_nbones++; memcpy(g_bones[c->bones], g_m.vsc[58], sizeof g_bones[0]); }
    memcpy(c->m34, g_m.vsc[58], sizeof c->m34);
    c->vs = g_m.vs; c->decl = g_m.decl; c->ib = g_m.ib;
    for (int i = 0; i < 4; i++) { c->stream[i].vb = g_m.stream[i].vb; c->stream[i].off = g_m.stream[i].off; c->stream[i].stride = g_m.stream[i].stride; }
    memcpy(c->vsc, g_m.vsc, sizeof c->vsc);
    c->dip = dip; c->type = t; c->bv = bv; c->mi = mi; c->nv = nv; c->si = si; c->pc = pc;
    c->tex0 = g_m.tex[0]; c->alpha = g_m.rs[D3DRS_ALPHATESTENABLE] != 0;
}

static void record_draw(int dip, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    if (!g_rec_target) return;
    if (g_rec_draws++ == 0) {
        g_rec_target->st = g_m;
        g_rec_target->dip = dip; g_rec_target->type = t; g_rec_target->bv = bv; g_rec_target->mi = mi; g_rec_target->nv = nv;
        g_rec_target->si = si; g_rec_target->pc = pc;
    }
}

static void note_draw(void) {
    g_draws++;
    if (!photon_p6_suppressed()) return;
    g_draws_supp++;
    if (photon_p6_call_draws_inc() == 0) {   /* the first draw of this suppressed call: is c58..c60 the model matrix? */
        g_supp_first++;
        float o[3];
        photon_p6_origin(o);
        float tx = g_m.vsc[58][3], ty = g_m.vsc[59][3], tz = g_m.vsc[60][3];
        if (fabsf(tx - o[0]) < 1.0f && fabsf(ty - o[1]) < 1.0f && fabsf(tz - o[2]) < 1.0f) g_c58_ok++;
        else if (g_c58_bad++ < 6)
            photon_log("dmirror: suppressed draw: VS c58..c60 translation (%.1f %.1f %.1f) vs lighting origin (%.1f %.1f %.1f)", tx, ty, tz, o[0], o[1], o[2]);
    }
}
static HRESULT WINAPI h_dp(void *d, D3DPRIMITIVETYPE t, UINT a, UINT b) { FLUSH();
    if (g_up_arm == 2 && upscale_now()) return S_OK;
    g_calls[S_DP]++; note_draw(); record_draw(0, t, (INT)a, 0, 0, 0, b); cast_snap(0, t, (INT)a, 0, 0, 0, b); return FN(S_DP, DpFn)(d, t, a, b);
}
static HRESULT WINAPI h_dip(void *d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) { FLUSH();
    if (g_up_arm == 2 && upscale_now()) return S_OK;
    g_calls[S_DIP]++; note_draw(); record_draw(1, t, bv, mi, nv, si, pc); cast_snap(1, t, bv, mi, nv, si, pc);
    if (g_ds) photon_shadows_draw(g_ds);   /* T4.2: the segment's first draw captures the view matrices */
    return FN(S_DIP, DipFn)(d, t, bv, mi, nv, si, pc);
}

/* p6.c, around a suppressed call that records group r */
void photon_dmirror_record_begin(void *r) { g_rec_target = r; g_rec_draws = 0; g_rec_vshi = 0; if (r) ((Rec *)r)->ok = 0; }
void photon_dmirror_rec_drop(void *r) { if (r && ((Rec *)r)->ok) { ((Rec *)r)->ok = 0; g_rec_ok--; g_rec_dropped++; } }
void photon_dmirror_record_end(void) {
    Rec *r = g_rec_target;
    g_rec_target = NULL;
    if (!r) return;
    if (g_rec_vshi) g_rec_bones++;   /* replay sets c58..c60 only: a bone palette (c58..c216) or anything else above would be the device's */
    else if (g_rec_draws == 1) { r->ok = 1; g_rec_ok++; } else g_rec_multi++;   /* only single-draw models replay (stage 2b) */
}
size_t photon_dmirror_rec_size(void) { return sizeof(Rec); }
const float (*photon_dmirror_rec_psc(const void *r))[4] { return (const float (*)[4])((const Rec *)r)->st.psc; }
void *const *photon_dmirror_rec_tex(const void *r) { return ((const Rec *)r)->st.tex; }
void *const *photon_dmirror_cur_tex(void) { return g_m.tex; }
int photon_dmirror_rec_ok(const void *r) { return r && ((const Rec *)r)->ok; }
unsigned photon_dmirror_view_gen(void) { return g_view_gen; }
int photon_dmirror_replay_on(void) { return g_replay && g_bound; }
int photon_dmirror_replay_on_wanted(void) { return g_replay; }

/* Replay group r with this copy's model matrix (3 rows, c58..c60): every recorded item that differs from the mirror is set through stock's
 * own function, the draw is issued, then everything changed is put back to the mirror's (the engine's) view. 1 on success. */
#define SETIF(cond, call) do { if (cond) { if (FAILED(call)) goto fail; } } while (0)
/* PS c0..c64 runs where the copy's values differ from the device's (gaps under 3 registers merged): a relit copy differs mostly in the
 * ambient cube (c33..c38), so it sends a few registers, not 65, and restores the same. At most 8 runs, else one run over all. */
enum { NRUN = 8 };
static int psc_runs_from(int from, const float (*x)[4], const float (*y)[4], int run[NRUN][2]) {
    int n = 0;
    for (int i = from; i < 65; i++) {
        if (memcmp(x[i], y[i], 16) == 0) continue;
        if (n && i - run[n - 1][1] <= 3) { run[n - 1][1] = i + 1; continue; }
        if (n == NRUN) { run[0][0] = from; run[0][1] = 65; return 1; }
        run[n][0] = i; run[n][1] = i + 1; n++;
    }
    return n;
}
static int psc_runs(const float (*x)[4], const float (*y)[4], int run[NRUN][2]) { return psc_runs_from(0, x, y, run); }
/* Lazy restore: after a replay the device keeps the group's state (g_held: its record, with the copy's textures and PS c0..c64 in g_held_tex
 * and g_held_psc). Consecutive copies of the same group then set only their own items (c58..c60, the relit PS runs, the cubemap textures).
 * The next engine call through any hook, and Present, EndScene, the UP draws, Clear and the box query, put the engine's view back first. */
static void *g_held_tex[16];
static float g_held_psc[65][4];
static int g_held_vsc_ch;
static volatile LONG64 g_replay_fast;
static int g_nolazy;
/* What the full set of the held group changed (filled in photon_dmirror_replay), so restore_held sets back only those */
enum { CH_VS = 1, CH_PS = 2, CH_DECL = 4, CH_IB = 8, CH_VSB = 16, CH_PSB = 32, CH_VSI = 64, CH_PSI = 128 };
static unsigned g_ch_flags, g_ch_streams;
static unsigned short g_ch_rs[256], g_ch_samp[16 * 13];
static int g_ch_nrs, g_ch_nsamp;
static void restore_held(void) {
    const Rec *r = g_held;
    if (!r) return;
    g_held = NULL;
    const Mirror *b = &g_m;
    void *d = g_dev;
    if (g_ch_flags & CH_VS) FN(S_SETVS, PtrFn)(d, b->vs);
    if (g_ch_flags & CH_PS) FN(S_SETPS, PtrFn)(d, b->ps);
    if (g_ch_flags & CH_DECL) FN(S_SETDECL, PtrFn)(d, b->decl);
    if (g_ch_flags & CH_IB) FN(S_SETIB, PtrFn)(d, b->ib);
    for (UINT i = 0; i < 4; i++) if (g_ch_streams & (1u << i)) FN(S_SETSTREAM, StreamFn)(d, i, b->stream[i].vb, b->stream[i].off, b->stream[i].stride);
    for (DWORD i = 0; i < 16; i++) if (g_held_tex[i] != b->tex[i]) FN(S_SETTEX, TexFn)(d, i, b->tex[i]);   /* the copies may have changed any */
    for (int k = 0; k < g_ch_nsamp; k++) { int i = g_ch_samp[k] >> 4, t = g_ch_samp[k] & 15; FN(S_SETSAMP, SampFn)(d, (DWORD)i, (D3DSAMPLERSTATETYPE)t, b->samp[i][t]); }
    for (int k = 0; k < g_ch_nrs; k++) FN(S_SETRS, RsFn)(d, (D3DRENDERSTATETYPE)g_ch_rs[k], b->rs[g_ch_rs[k]]);
    if (g_held_vsc_ch) FN(S_VSCF, CfFn)(d, 0, b->vsc[0], 61);
    else FN(S_VSCF, CfFn)(d, 58, b->vsc[58], 3);
    int run[NRUN][2], nrun = psc_runs((const float (*)[4])g_held_psc, (const float (*)[4])b->psc, run);
    for (int i = 0; i < nrun; i++) FN(S_PSCF, CfFn)(d, (UINT)run[i][0], b->psc[run[i][0]], (UINT)(run[i][1] - run[i][0]));
    if (g_ch_flags & CH_VSB) FN(S_VSCB, CbFn)(d, 0, b->vsb, 16);
    if (g_ch_flags & CH_PSB) FN(S_PSCB, CbFn)(d, 0, b->psb, 16);
    if (g_ch_flags & CH_VSI) FN(S_VSCI, CiFn)(d, 0, b->vsi[0], 16);
    if (g_ch_flags & CH_PSI) FN(S_PSCI, CiFn)(d, 0, b->psi[0], 16);
}
void photon_dmirror_flush(void) { FLUSH(); }

int photon_dmirror_replay(const void *rp, const float m34[3][4], const float (*psc)[4], int ntex, const int *tslot, void *const *ttex) {
    const Rec *r = rp;
    const Mirror *a = &r->st, *b = &g_m;
    const float (*ap)[4] = psc ? psc : (const float (*)[4])a->psc;
    void *tx[20];
    memcpy(tx, a->tex, sizeof tx);
    for (int i = 0; i < ntex; i++) if (tslot[i] >= 0 && tslot[i] < 16) tx[tslot[i]] = ttex[i];
    void *d = g_dev;
    if (!r->ok || !d) return 0;
    if (g_held == r) {   /* the device holds this group's state: this copy's own items only */
        for (DWORD i = 0; i < 16; i++) if (tx[i] != g_held_tex[i]) { SETIF(1, FN(S_SETTEX, TexFn)(d, i, tx[i])); g_held_tex[i] = tx[i]; }
        SETIF(1, FN(S_VSCF, CfFn)(d, 58, m34[0], 3));
        /* copies of one group differ only in the relit c32..c64 (c0..c31 are the recording's in both) */
        int run[NRUN][2], nrun = psc_runs_from(32, ap, (const float (*)[4])g_held_psc, run);
        for (int i = 0; i < nrun; i++) {
            SETIF(1, FN(S_PSCF, CfFn)(d, (UINT)run[i][0], ap[run[i][0]], (UINT)(run[i][1] - run[i][0])));
            memcpy(g_held_psc[run[i][0]], ap[run[i][0]], (size_t)(run[i][1] - run[i][0]) * 16);
        }
        if (r->dip) SETIF(1, FN(S_DIP, DipFn)(d, r->type, r->bv, r->mi, r->nv, r->si, r->pc));
        else SETIF(1, FN(S_DP, DpFn)(d, r->type, (UINT)r->bv, r->pc));
        g_replays++; g_replay_fast++;
        cast_note(r, m34);
        return 1;
    }
    FLUSH();
    g_ch_flags = g_ch_streams = 0; g_ch_nrs = g_ch_nsamp = 0;
#define CH(cond, flag, call) do { if (cond) { g_ch_flags |= (flag); if (FAILED(call)) goto fail; } } while (0)
    CH(a->vs != b->vs, CH_VS, FN(S_SETVS, PtrFn)(d, a->vs));
    CH(a->ps != b->ps, CH_PS, FN(S_SETPS, PtrFn)(d, a->ps));
    CH(a->decl != b->decl, CH_DECL, FN(S_SETDECL, PtrFn)(d, a->decl));
    CH(a->ib != b->ib, CH_IB, FN(S_SETIB, PtrFn)(d, a->ib));
    for (UINT i = 0; i < 4; i++)
        if (memcmp(&a->stream[i], &b->stream[i], sizeof a->stream[i]) != 0) {
            g_ch_streams |= 1u << i;
            SETIF(1, FN(S_SETSTREAM, StreamFn)(d, i, a->stream[i].vb, a->stream[i].off, a->stream[i].stride));
        }
    for (DWORD i = 0; i < 16; i++) {
        SETIF(tx[i] != b->tex[i], FN(S_SETTEX, TexFn)(d, i, tx[i]));
        for (int k = 1; k < 14; k++)
            if (a->samp[i][k] != b->samp[i][k]) { g_ch_samp[g_ch_nsamp++] = (unsigned short)(i << 4 | k); SETIF(1, FN(S_SETSAMP, SampFn)(d, i, (D3DSAMPLERSTATETYPE)k, a->samp[i][k])); }
    }
    for (int i = 0; i < 256; i++) if (a->rs[i] != b->rs[i]) { g_ch_rs[g_ch_nrs++] = (unsigned short)i; SETIF(1, FN(S_SETRS, RsFn)(d, (D3DRENDERSTATETYPE)i, a->rs[i])); }
    int vsc_ch = memcmp(a->vsc, b->vsc, 58 * 16) != 0;
    SETIF(vsc_ch, FN(S_VSCF, CfFn)(d, 0, a->vsc[0], 58));
    SETIF(1, FN(S_VSCF, CfFn)(d, 58, m34[0], 3));
    int run[NRUN][2], nrun = psc_runs(ap, (const float (*)[4])b->psc, run);
    for (int i = 0; i < nrun; i++) SETIF(1, FN(S_PSCF, CfFn)(d, (UINT)run[i][0], ap[run[i][0]], (UINT)(run[i][1] - run[i][0])));
    CH(memcmp(a->vsb, b->vsb, sizeof a->vsb) != 0, CH_VSB, FN(S_VSCB, CbFn)(d, 0, a->vsb, 16));
    CH(memcmp(a->psb, b->psb, sizeof a->psb) != 0, CH_PSB, FN(S_PSCB, CbFn)(d, 0, a->psb, 16));
    CH(memcmp(a->vsi, b->vsi, sizeof a->vsi) != 0, CH_VSI, FN(S_VSCI, CiFn)(d, 0, a->vsi[0], 16));
    CH(memcmp(a->psi, b->psi, sizeof a->psi) != 0, CH_PSI, FN(S_PSCI, CiFn)(d, 0, a->psi[0], 16));
#undef CH
    /* held from here: restore_held puts the engine's view back at the next engine call */
    g_held = r; memcpy(g_held_tex, tx, sizeof g_held_tex); memcpy(g_held_psc, ap, sizeof g_held_psc); g_held_vsc_ch = vsc_ch;
    if (r->dip) SETIF(1, FN(S_DIP, DipFn)(d, r->type, r->bv, r->mi, r->nv, r->si, r->pc));
    else SETIF(1, FN(S_DP, DpFn)(d, r->type, (UINT)r->bv, r->pc));
    if (g_nolazy) FLUSH();   /* PHOTON_TEST_NOLAZY=1: restore at once (A/B) */
    g_replays++;
    cast_note(r, m34);
    return 1;
fail:
    FLUSH();
    if (g_replay_fail++ < 4) photon_log("dmirror: a device call failed during a replay: replay off");
    g_replay = 0;
    return 0;
}

/* --- T4.7a: a bounding-box occlusion query (cull.c) ---------------------------------------------------------------------------------------
 * The box mins..maxs (model space, grown by grow units) under the model matrix m34 and the view-projection the engine set for this view
 * (Source's cViewProj, VS c8..c11, column-major: clip_j = dot((world, 1), c[8 + j])), drawn with the fixed-function pipeline (world =
 * model x view-projection, view = projection = identity), depth test on, depth and colour writes off (colour on with visible: a debug
 * view). Everything changed is put back from the mirror through stock's functions; a render state the mirror has not seen yet makes the
 * query not run (0). 1 when the query was issued around the box. */
enum { S_SETTRANSFORM = 44, S_DPUP = 83 };
typedef HRESULT (WINAPI *XfFn)(void *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT (WINAPI *DpUpFn)(void *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT (WINAPI *DipUpFn)(void *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
static void lazy_trickle(void *d);
static int g_lazy_on;
static HRESULT WINAPI h_endscene(void *d) { FLUSH(); if (g_lazy_on) lazy_trickle(d); if (g_ts) photon_texstream_tick(); photon_texf_tick(); return FN(42, HRESULT (WINAPI *)(void *))(d); }
static HRESULT WINAPI h_dpup(void *d, D3DPRIMITIVETYPE t, UINT n, const void *v, UINT st) { FLUSH(); return FN(S_DPUP, DpUpFn)(d, t, n, v, st); }
static HRESULT WINAPI h_dipup(void *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *ix, D3DFORMAT f, const void *v, UINT st) { FLUSH(); return FN(84, DipUpFn)(d, t, mi, nv, pc, ix, f, v, st); }
static const D3DRENDERSTATETYPE k_box_rs[] = { D3DRS_COLORWRITEENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE,
                                               D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_FOGENABLE,
                                               D3DRS_LIGHTING, D3DRS_CLIPPLANEENABLE };
static volatile LONG64 g_box_unknown_rs, g_box_issued;
int photon_dmirror_box_query(IDirect3DQuery9 *q, const float m34[3][4], const float mins[3], const float maxs[3], float grow, int visible, const float (*vp)[4]) {
    void *d = g_dev;
    if (!d || !g_bound) return 0;
    FLUSH();
    for (size_t i = 0; i < sizeof k_box_rs / sizeof k_box_rs[0]; i++) if (!g_rs_known[k_box_rs[i]]) { g_box_unknown_rs++; return 0; }
    float M4[4][4] = { { m34[0][0], m34[0][1], m34[0][2], m34[0][3] }, { m34[1][0], m34[1][1], m34[1][2], m34[1][3] },
                       { m34[2][0], m34[2][1], m34[2][2], m34[2][3] }, { 0, 0, 0, 1 } };
    D3DMATRIX W, I;
    memset(&I, 0, sizeof I);
    I._11 = I._22 = I._33 = I._44 = 1.0f;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += M4[k][i] * (vp ? vp[j][k] : g_m.vsc[8 + j][k]);
            W.m[i][j] = v;
        }
    float a[3], b[3];
    for (int k = 0; k < 3; k++) { a[k] = mins[k] - grow; b[k] = maxs[k] + grow; }
    float c[8][3];
    for (int i = 0; i < 8; i++) { c[i][0] = i & 1 ? b[0] : a[0]; c[i][1] = i & 2 ? b[1] : a[1]; c[i][2] = i & 4 ? b[2] : a[2]; }
    static const unsigned char tri[36] = { 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3 };
    float v[36][3];
    for (int i = 0; i < 36; i++) memcpy(v[i], c[tri[i]], sizeof v[i]);
    RsFn rs = FN(S_SETRS, RsFn);
    rs(d, D3DRS_COLORWRITEENABLE, visible ? 0xf : 0);
    rs(d, D3DRS_ZWRITEENABLE, FALSE); rs(d, D3DRS_ZENABLE, D3DZB_TRUE); rs(d, D3DRS_ZFUNC, D3DCMP_LESSEQUAL); rs(d, D3DRS_CULLMODE, D3DCULL_NONE);
    rs(d, D3DRS_ALPHABLENDENABLE, FALSE); rs(d, D3DRS_ALPHATESTENABLE, FALSE); rs(d, D3DRS_STENCILENABLE, FALSE); rs(d, D3DRS_FOGENABLE, FALSE);
    rs(d, D3DRS_LIGHTING, FALSE); rs(d, D3DRS_CLIPPLANEENABLE, 0);
    FN(S_SETVS, PtrFn)(d, NULL); FN(S_SETPS, PtrFn)(d, NULL);
    FN(S_SETFVF, DwFn)(d, D3DFVF_XYZ);
    FN(S_SETTRANSFORM, XfFn)(d, D3DTS_WORLD, &W); FN(S_SETTRANSFORM, XfFn)(d, D3DTS_VIEW, &I); FN(S_SETTRANSFORM, XfFn)(d, D3DTS_PROJECTION, &I);
    int ok = q ? SUCCEEDED(IDirect3DQuery9_Issue(q, D3DISSUE_BEGIN)) : 1;   /* q NULL: the debug view only */
    FN(S_DPUP, DpUpFn)(d, D3DPT_TRIANGLELIST, 12, v, sizeof v[0]);
    if (ok && q) IDirect3DQuery9_Issue(q, D3DISSUE_END);
    /* back to the engine's view of the device (DrawPrimitiveUP left stream 0 unset) */
    for (size_t i = 0; i < sizeof k_box_rs / sizeof k_box_rs[0]; i++) rs(d, k_box_rs[i], g_m.rs[k_box_rs[i]]);
    FN(S_SETVS, PtrFn)(d, g_m.vs); FN(S_SETPS, PtrFn)(d, g_m.ps);
    if (g_m.decl) FN(S_SETDECL, PtrFn)(d, g_m.decl); else FN(S_SETFVF, DwFn)(d, g_m.fvf);
    FN(S_SETSTREAM, StreamFn)(d, 0, g_m.stream[0].vb, g_m.stream[0].off, g_m.stream[0].stride);
    if (ok) g_box_issued++;
    return ok;
}
/* T4.2: one full-screen quad with our own shaders (shadows.c), on the device thread, then back to the engine's state from the mirror. The
 * depth-stencil is unbound for the draw (the pass reads the main view's depth texture), restored after. Returns 0 if a state it touches
 * is unknown to the mirror (nothing drawn). */
static void *g_fs_ds;   /* photon_dmirror_fullscreen_depth: the next pass's depth surface (NULL: none) */
static const D3DRENDERSTATETYPE k_fs_rs[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                              D3DRS_CULLMODE, D3DRS_STENCILENABLE, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE,
                                              D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
static const D3DSAMPLERSTATETYPE k_fs_samp[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
static const DWORD k_fs_samp_default[] = { D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, 0 };
static volatile LONG64 g_fs_draws, g_fs_unknown;
typedef HRESULT (WINAPI *CreateShFn2)(void *, const DWORD *, void **);
typedef HRESULT (WINAPI *SampFn2)(void *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT (WINAPI *ConstFn2)(void *, UINT, const float *, UINT);
static void *g_fs_tex2;   /* photon_dmirror_fullscreen2: the second texture (s1) of the next pass */
enum { FS_NX = 13 };
static void *g_fs_tx[FS_NX];   /* L3: textures on s1..s6 of the next pass (s0 is the pass's own), with their filter */
static int g_fs_lin[FS_NX], g_fs_nx;
int photon_dmirror_fullscreen(const DWORD *vs_code, const DWORD *ps_code, void **vs_cache, void **ps_cache, void *tex, const float (*psc)[4], int npsc,
                              int blend, int width, int height, void *target) {
    void *d = g_dev;
    if (!d || !g_bound || npsc > 64 || !g_vp_known) return 0;   /* the mirror restores up to 224 */
    FLUSH();
    for (size_t i = 0; i < sizeof k_fs_rs / sizeof k_fs_rs[0]; i++) if (!g_rs_known[k_fs_rs[i]]) { g_fs_unknown++; return 0; }
    if (!*vs_cache && FAILED(((CreateShFn2)g_fn[91])(d, vs_code, vs_cache))) return 0;
    if (!*ps_cache && FAILED(((CreateShFn2)g_fn[106])(d, ps_code, ps_cache))) return 0;
    RsFn rs = FN(S_SETRS, RsFn);
    SampFn2 ss = (SampFn2)g_fn[S_SETSAMP];
    void *fs_ds = g_fs_ds; g_fs_ds = NULL;
    if (fs_ds) { rs(d, D3DRS_ZENABLE, D3DZB_TRUE); rs(d, D3DRS_ZWRITEENABLE, TRUE); rs(d, D3DRS_ZFUNC, D3DCMP_LESSEQUAL); }   /* NANITE's merge */
    else { rs(d, D3DRS_ZENABLE, D3DZB_FALSE); rs(d, D3DRS_ZWRITEENABLE, FALSE); }
    rs(d, D3DRS_ALPHABLENDENABLE, blend ? TRUE : FALSE);
    if (blend == 2) { rs(d, D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR); rs(d, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR); }   /* L3: dst x 2 src (src = ratio / 2) */
    else { rs(d, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); rs(d, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); }
    rs(d, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    rs(d, D3DRS_CULLMODE, D3DCULL_NONE); rs(d, D3DRS_STENCILENABLE, FALSE); rs(d, D3DRS_ALPHATESTENABLE, FALSE);
    rs(d, D3DRS_COLORWRITEENABLE, 7); rs(d, D3DRS_FOGENABLE, FALSE); rs(d, D3DRS_SCISSORTESTENABLE, FALSE); rs(d, D3DRS_CLIPPLANEENABLE, 0);
    rs(d, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    /* no sRGB encode on write: the passes work in the frame's own (gamma) values; the engine's dev/upscale draw, which UPSCALE replaces,
     * runs with it on (put back below when the mirror knows the engine's value, else left off as at the main view) */
    rs(d, D3DRS_SRGBWRITEENABLE, FALSE);
    FN(S_SETDS, PtrFn)(d, fs_ds);
    void *rt_was = g_rt0;
    if (target && target != rt_was) ((Rt2Fn)g_fn[S_SETRT])(d, 0, target);   /* resets the viewport to the whole target */
    D3DVIEWPORT9 full = { 0, 0, (DWORD)width, (DWORD)height, 0.0f, 1.0f };
    FN(S_SETVP, PtrFn)(d, &full);
    FN(S_SETTEX, TexFn)(d, 0, tex);
    if (g_fs_tex2) {
        FN(S_SETTEX, TexFn)(d, 1, g_fs_tex2);
        ss(d, 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(d, 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(d, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        ss(d, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(d, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE); ss(d, 1, D3DSAMP_SRGBTEXTURE, 0);
    }
    for (int i = 1; i < g_fs_nx; i++) {
        if (!g_fs_tx[i]) continue;
        DWORD fl = g_fs_lin[i] ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        FN(S_SETTEX, TexFn)(d, (DWORD)i, g_fs_tx[i]);
        ss(d, (DWORD)i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(d, (DWORD)i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(d, (DWORD)i, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
        ss(d, (DWORD)i, D3DSAMP_MAGFILTER, fl); ss(d, (DWORD)i, D3DSAMP_MINFILTER, fl); ss(d, (DWORD)i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        ss(d, (DWORD)i, D3DSAMP_SRGBTEXTURE, 0);
    }
    ss(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    ss(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE); ss(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    FN(S_SETVS, PtrFn)(d, *vs_cache); FN(S_SETPS, PtrFn)(d, *ps_cache);
    ((ConstFn2)g_fn[S_PSCF])(d, 0, &psc[0][0], (UINT)npsc);
    FN(S_SETFVF, DwFn)(d, D3DFVF_XYZW | D3DFVF_TEX1);
    float hx = 1.0f / (float)width, hy = 1.0f / (float)height;   /* D3D9's half-pixel: clip-space offset of half a pixel = 1/size */
    float v[4][6] = { { -1 - hx, 1 + hy, 0, 1, 0, 0 }, { 1 - hx, 1 + hy, 0, 1, 1, 0 }, { -1 - hx, -1 + hy, 0, 1, 0, 1 }, { 1 - hx, -1 + hy, 0, 1, 1, 1 } };
    FN(S_DPUP, DpUpFn)(d, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
    g_fs_draws++;
    /* back to the engine's view of the device */
    for (size_t i = 0; i < sizeof k_fs_rs / sizeof k_fs_rs[0]; i++) rs(d, k_fs_rs[i], g_m.rs[k_fs_rs[i]]);
    rs(d, D3DRS_SRGBWRITEENABLE, g_rs_known[D3DRS_SRGBWRITEENABLE] ? g_m.rs[D3DRS_SRGBWRITEENABLE] : FALSE);
    if (target && target != rt_was) ((Rt2Fn)g_fn[S_SETRT])(d, 0, rt_was);
    FN(S_SETDS, PtrFn)(d, g_ds);
    FN(S_SETVP, PtrFn)(d, &g_vp);
    FN(S_SETTEX, TexFn)(d, 0, g_m.tex[0]);
    for (size_t i = 0; i < sizeof k_fs_samp / sizeof k_fs_samp[0]; i++)
        ss(d, 0, k_fs_samp[i], g_samp_known[0][k_fs_samp[i]] ? g_m.samp[0][k_fs_samp[i]] : k_fs_samp_default[i]);
    if (g_fs_tex2) {
        FN(S_SETTEX, TexFn)(d, 1, g_m.tex[1]);
        for (size_t i = 0; i < sizeof k_fs_samp / sizeof k_fs_samp[0]; i++)
            ss(d, 1, k_fs_samp[i], g_samp_known[1][k_fs_samp[i]] ? g_m.samp[1][k_fs_samp[i]] : k_fs_samp_default[i]);
        g_fs_tex2 = NULL;
    }
    for (int i = 1; i < g_fs_nx; i++) {
        if (!g_fs_tx[i]) continue;
        FN(S_SETTEX, TexFn)(d, (DWORD)i, g_m.tex[i]);
        for (size_t k = 0; k < sizeof k_fs_samp / sizeof k_fs_samp[0]; k++)
            ss(d, (DWORD)i, k_fs_samp[k], g_samp_known[i][k_fs_samp[k]] ? g_m.samp[i][k_fs_samp[k]] : k_fs_samp_default[k]);
        ss(d, (DWORD)i, D3DSAMP_ADDRESSW, g_samp_known[i][D3DSAMP_ADDRESSW] ? g_m.samp[i][D3DSAMP_ADDRESSW] : D3DTADDRESS_WRAP);
        g_fs_tx[i] = NULL;
    }
    g_fs_nx = 0;
    FN(S_SETVS, PtrFn)(d, g_m.vs); FN(S_SETPS, PtrFn)(d, g_m.ps);
    ((ConstFn2)g_fn[S_PSCF])(d, 0, &g_m.psc[0][0], (UINT)npsc);
    if (g_m.decl) FN(S_SETDECL, PtrFn)(d, g_m.decl); else FN(S_SETFVF, DwFn)(d, g_m.fvf);
    FN(S_SETSTREAM, StreamFn)(d, 0, g_m.stream[0].vb, g_m.stream[0].off, g_m.stream[0].stride);
    return 1;
}
void photon_dmirror_fullscreen_tex2(void *tex) { g_fs_tex2 = tex; }
void photon_dmirror_fullscreen_depth(void *ds) { g_fs_ds = ds; }   /* NANITE: the next pass tests and writes this depth (the PS may write oDepth) */
void photon_dmirror_fullscreen_texn(int slot, void *tex, int linear) {   /* L3: s1..s6 for the next pass */
    if (slot < 1 || slot >= FS_NX) return;
    g_fs_tx[slot] = tex; g_fs_lin[slot] = linear;
    if (slot + 1 > g_fs_nx) g_fs_nx = slot + 1;
}
long long photon_dmirror_fs_draws(void) { return g_fs_draws; }

/* T4.2 stage 1c: this frame's casters into the sun shadow map. Each recording's VS, declaration, buffers and VS constants c0..c57 with
 * cViewProj (c8..c11) replaced by the light's rows and the copy's model matrix in c58..c60, a do-nothing PS of the VS's shader model,
 * colour writes off, depth into ds (rt: a NULL-format surface of the same size). The engine's state is put back from the mirror after. */
static const D3DRENDERSTATETYPE k_cast_rs[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE,
                                                D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE, D3DRS_FOGENABLE, D3DRS_SCISSORTESTENABLE,
                                                D3DRS_CLIPPLANEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS };
static volatile LONG64 g_cast_draws, g_cast_skipped, g_cast_unknown, g_cast_cached, g_cast_indoor;
static volatile LONG g_cast_invalid;   /* the shadow map was recreated: draw it again even if nothing moved */
void photon_dmirror_cast_invalidate(void) { g_cast_invalid = 1; }
static uint64_t mix64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }
static int vs_major(void *vs) {   /* the VS's shader model major version (cached for the last few) */
    static void *k[16]; static int v[16]; static int n;
    for (int i = 0; i < 16; i++) if (k[i] == vs) return v[i];
    UINT sz = 0;
    IDirect3DVertexShader9 *s = vs;
    int major = 0;
    if (s && SUCCEEDED(s->lpVtbl->GetFunction(s, NULL, &sz)) && sz >= 4) {
        DWORD *b = HeapAlloc(GetProcessHeap(), 0, sz);
        if (b && SUCCEEDED(s->lpVtbl->GetFunction(s, b, &sz))) major = (int)((b[0] >> 8) & 0xff);
        if (b) HeapFree(GetProcessHeap(), 0, b);
    }
    k[n & 15] = vs; v[n & 15] = major; n++;
    return major;
}
/* the zero VB the flex-delta streams of skinned casters read from (4 MiB: 65536 vertices of up to 64 bytes) */
enum { ZERO_VB_BYTES = 4 << 20, S_CREATEVB = 26 };
typedef HRESULT (WINAPI *CreateVbFn)(void *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
static IDirect3DVertexBuffer9 *g_zero_vb;
static int g_zero_failed;
static IDirect3DVertexBuffer9 *zero_vb(void *d) {
    if (g_zero_vb || g_zero_failed) return g_zero_vb;
    IDirect3DVertexBuffer9 *vb = NULL;
    void *p = NULL;
    if (FAILED(((CreateVbFn)g_fn[S_CREATEVB])(d, ZERO_VB_BYTES, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL)) || !vb ||
        FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0))) {
        if (vb) IDirect3DVertexBuffer9_Release(vb);
        g_zero_failed = 1;
        photon_log("dmirror: zero VB for the skinned casters' flex streams failed: those casters are left out");
        return NULL;
    }
    memset(p, 0, ZERO_VB_BYTES);
    IDirect3DVertexBuffer9_Unlock(vb);
    return g_zero_vb = vb;
}
/* PT-RT (analysis/43): the world's own triangles (model 0, from its worker) in the sun maps (slot 0 and its near cascade 17), so the
 * composite's sun has the world's shadows as sharp as the props'. Set by shadows.c on the device thread; drawn after the casters. */
#include "shaders/worldcast_vs.h"
typedef HRESULT (WINAPI *CreateDeclFn)(void *, const D3DVERTEXELEMENT9 *, void **);
static IDirect3DVertexBuffer9 *g_world_vb; static UINT g_world_ntri; static void *g_world_decl, *g_world_vs; static uint64_t g_world_gen;
static IDirect3DVertexBuffer9 *g_world_pbr_vb; static void *g_world_pbr_decl;   /* PBR: per corner (roughness, metalness) as D3DCOLOR, stream 1 */
void photon_dmirror_set_world(const float *tri, const float *pbr, const float *ex, unsigned ntri) {
    void *d = g_dev;
    if (!d) return;
    if (g_world_vb) { IDirect3DVertexBuffer9_Release(g_world_vb); g_world_vb = NULL; g_world_ntri = 0; }
    if (g_world_pbr_vb) { IDirect3DVertexBuffer9_Release(g_world_pbr_vb); g_world_pbr_vb = NULL; }
    g_world_gen++;
    if (!tri || !ntri) return;
    if (pbr) {   /* PBR step 2: the world normals pass writes them beside the normals (stream 1, only its own declaration reads it) */
        IDirect3DVertexBuffer9 *pv = NULL; void *pp = NULL;
        /* per corner 44 bytes: COLOR0 (roughness, metalness), TEXCOORD3 (u, v, atlas tile or -1, has a pack normal), TEXCOORD4 the
         * texture's u axis, TEXCOORD5 its v axis (PBR packs; tile -1 without one) */
        if (SUCCEEDED(((CreateVbFn)g_fn[S_CREATEVB])(d, ntri * 3u * 44u, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &pv, NULL)) && pv &&
            SUCCEEDED(IDirect3DVertexBuffer9_Lock(pv, 0, 0, &pp, 0))) {
            unsigned char *c = pp;
            for (unsigned t = 0; t < ntri; t++) {
                float r = pbr[2 * t], m = pbr[2 * t + 1];
                DWORD rb = (DWORD)(r < 0 ? 0 : r > 1 ? 255 : r * 255 + 0.5f), mb = (DWORD)(m < 0 ? 0 : m > 1 ? 255 : m * 255 + 0.5f);
                DWORD col = D3DCOLOR_ARGB(255, rb, mb, 0);   /* the shader sees r = roughness, g = metalness */
                const float *x = ex ? ex + 14 * (size_t)t : NULL;
                int tile = x ? (int)x[0] : -1;
                for (int k = 0; k < 3; k++) {
                    unsigned char *v = c + ((size_t)t * 3 + (size_t)k) * 44;
                    float tc[4] = { x ? x[7 + 2 * k] : 0, x ? x[8 + 2 * k] : 0, (float)(tile < 0 ? -1 : (tile & 0xffff)), tile >= 0 && (tile & 0x10000) ? 1.0f : 0.0f };
                    memcpy(v, &col, 4); memcpy(v + 4, tc, 16);
                    if (x) { memcpy(v + 20, x + 1, 12); memcpy(v + 32, x + 4, 12); } else memset(v + 20, 0, 24);
                }
            }
            IDirect3DVertexBuffer9_Unlock(pv);
            g_world_pbr_vb = pv;
        } else if (pv) IDirect3DVertexBuffer9_Release(pv);
        if (g_world_pbr_vb && !g_world_pbr_decl) {
            D3DVERTEXELEMENT9 el[7] = { { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                                        { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
                                        { 1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
                                        { 1, 4, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
                                        { 1, 20, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
                                        { 1, 32, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 }, D3DDECL_END() };
            if (FAILED(((CreateDeclFn)g_fn[86])(d, el, &g_world_pbr_decl))) g_world_pbr_decl = NULL;
        }
    }
    UINT bytes = ntri * 72u;   /* per corner: position, smooth normal */
    IDirect3DVertexBuffer9 *vb = NULL;
    void *p = NULL;
    if (FAILED(((CreateVbFn)g_fn[S_CREATEVB])(d, bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL)) || !vb || FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0))) {
        if (vb) IDirect3DVertexBuffer9_Release(vb);
        photon_log("dmirror: the world's vertex buffer for the sun map (%u triangles) failed", ntri);
        return;
    }
    memcpy(p, tri, bytes);
    IDirect3DVertexBuffer9_Unlock(vb);
    if (!g_world_decl) {
        D3DVERTEXELEMENT9 el[3] = { { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                                    { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 }, D3DDECL_END() };
        if (FAILED(((CreateDeclFn)g_fn[86])(d, el, &g_world_decl))) g_world_decl = NULL;
    }
    if (!g_world_vs && FAILED(((CreateShFn2)g_fn[91])(d, k_worldcast_vs, &g_world_vs))) g_world_vs = NULL;
    if (!g_world_decl || !g_world_vs) { IDirect3DVertexBuffer9_Release(vb); return; }
    g_world_vb = vb; g_world_ntri = ntri;
    photon_log("dmirror: the world in the sun map: %u triangles (%.1f MiB)", ntri, bytes / 1048576.0);
}
/* PT-RT: the world's smooth normals from the main view into rt (encoded, alpha 1 where written), depth-tested against the scene's depth
 * surface ds with a small pull toward the camera (the engine drew the same triangles there) */
#include "shaders/worldnrm_vs.h"
#include "shaders/worldnrm_ps.h"
static void *g_wn_vs, *g_wn_ps;
/* PT-RT: Nanite's props as further sun casters, the same way as the world: 20-byte vertices (position, uv), in ranges; an alpha-tested
 * range draws with its own base texture (a D3D9 copy of the decoded VTF, made once) and cuts its leaves by alpha */
#include "shaders/worldcastuv_vs.h"
#include "shaders/castalpha_ps.h"
typedef HRESULT (WINAPI *CastCreateTexFn)(void *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *UpdateTexFn)(void *, void *, void *);
enum { MAXPR = 256 };
static IDirect3DVertexBuffer9 *g_props_vb; static UINT g_props_ntri; static void *g_props_decl, *g_cuv_vs, *g_calpha_ps;
static struct { unsigned first, count; IDirect3DTexture9 *tex; } g_pr[MAXPR]; static int g_npr;
static IDirect3DTexture9 *cast_texture(void *d, const PhotonCastRange *r) {
    D3DFORMAT f = r->format == 1 ? D3DFMT_DXT1 : r->format == 2 ? D3DFMT_DXT3 : r->format == 3 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
    IDirect3DTexture9 *sys = NULL, *gpu = NULL;
    if (FAILED(((CastCreateTexFn)g_fn[23])(d, (UINT)r->w, (UINT)r->h, (UINT)r->mips, 0, f, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys) return NULL;
    for (int m = 0; m < r->mips; m++) {
        D3DLOCKED_RECT lr;
        if (FAILED(IDirect3DTexture9_LockRect(sys, (UINT)m, &lr, NULL, 0))) continue;
        int mw = r->w >> m, mh = r->h >> m; if (mw < 1) mw = 1; if (mh < 1) mh = 1;
        const unsigned char *src = r->data + r->mip_ofs[m];
        if (r->format >= 1 && r->format <= 3) {   /* block rows */
            int bpb = r->format == 1 ? 8 : 16, bw = (mw + 3) / 4, bh = (mh + 3) / 4;
            for (int y = 0; y < bh; y++) memcpy((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch, src + (size_t)y * (size_t)bw * (size_t)bpb, (size_t)bw * (size_t)bpb);
        } else {   /* RGBA8 -> A8R8G8B8 (B G R A in memory) */
            for (int y = 0; y < mh; y++) {
                unsigned char *o = (unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
                const unsigned char *i = src + (size_t)y * (size_t)mw * 4;
                for (int x = 0; x < mw; x++) { o[4 * x] = i[4 * x + 2]; o[4 * x + 1] = i[4 * x + 1]; o[4 * x + 2] = i[4 * x]; o[4 * x + 3] = i[4 * x + 3]; }
            }
        }
        IDirect3DTexture9_UnlockRect(sys, (UINT)m);
    }
    if (FAILED(((CastCreateTexFn)g_fn[23])(d, (UINT)r->w, (UINT)r->h, (UINT)r->mips, 0, f, D3DPOOL_DEFAULT, &gpu, NULL)) || !gpu ||
        FAILED(((UpdateTexFn)g_fn[31])(d, sys, gpu))) { if (gpu) IDirect3DTexture9_Release(gpu); gpu = NULL; }
    IDirect3DTexture9_Release(sys);
    return gpu;
}
/* PBR packs (pbrpack.c): the map's atlases as DXT1 textures, sampled by the world-normals pass (s1 roughness / metalness / height,
 * s2 normals); NULL when the map has no pack */
static IDirect3DTexture9 *g_atl_rmh, *g_atl_nrm;
static IDirect3DTexture9 *dxt1_texture(void *d, UINT side, const uint8_t *blocks) {
    IDirect3DTexture9 *sys = NULL, *gpu = NULL;
    if (FAILED(((CastCreateTexFn)g_fn[23])(d, side, side, 1, 0, D3DFMT_DXT1, D3DPOOL_SYSTEMMEM, &sys, NULL)) || !sys) return NULL;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(sys, 0, &lr, NULL, 0))) {
        UINT bw = side / 4;
        for (UINT y = 0; y < bw; y++) memcpy((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch, blocks + (size_t)y * bw * 8, (size_t)bw * 8);
        IDirect3DTexture9_UnlockRect(sys, 0);
        if (FAILED(((CastCreateTexFn)g_fn[23])(d, side, side, 1, 0, D3DFMT_DXT1, D3DPOOL_DEFAULT, &gpu, NULL)) || !gpu ||
            FAILED(((UpdateTexFn)g_fn[31])(d, sys, gpu))) { if (gpu) IDirect3DTexture9_Release(gpu); gpu = NULL; }
    }
    IDirect3DTexture9_Release(sys);
    return gpu;
}
void photon_dmirror_set_pbr_atlas(void *ctx, const uint8_t *rmh, const uint8_t *nrm) {
    (void)ctx;
    void *d = g_dev;
    if (g_atl_rmh) { IDirect3DTexture9_Release(g_atl_rmh); g_atl_rmh = NULL; }
    if (g_atl_nrm) { IDirect3DTexture9_Release(g_atl_nrm); g_atl_nrm = NULL; }
    if (!d || !rmh || !nrm) return;
    g_atl_rmh = dxt1_texture(d, PBR_RMH_SIDE, rmh); g_atl_nrm = dxt1_texture(d, PBR_NRM_SIDE, nrm);
    photon_log("dmirror: PBR pack atlases %s (%d / %d px)", g_atl_rmh && g_atl_nrm ? "made" : "FAILED", PBR_RMH_SIDE, PBR_NRM_SIDE);
}
void photon_dmirror_set_props(const float *tri, unsigned ntri, const PhotonCastRange *r, int nr) {
    void *d = g_dev;
    if (!d) return;
    if (g_props_vb) { IDirect3DVertexBuffer9_Release(g_props_vb); g_props_vb = NULL; g_props_ntri = 0; }
    for (int i = 0; i < g_npr; i++) if (g_pr[i].tex) IDirect3DTexture9_Release(g_pr[i].tex);
    g_npr = 0;
    g_world_gen++;
    if (!tri || !ntri) return;
    UINT bytes = ntri * 60u;
    IDirect3DVertexBuffer9 *vb = NULL;
    void *p = NULL;
    if (FAILED(((CreateVbFn)g_fn[S_CREATEVB])(d, bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL)) || !vb || FAILED(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0))) {
        if (vb) IDirect3DVertexBuffer9_Release(vb);
        photon_log("dmirror: the props' vertex buffer for the sun map (%u triangles) failed", ntri);
        return;
    }
    memcpy(p, tri, bytes);
    IDirect3DVertexBuffer9_Unlock(vb);
    if (!g_props_decl) {
        D3DVERTEXELEMENT9 el[3] = { { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                                    { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
        if (FAILED(((CreateDeclFn)g_fn[86])(d, el, &g_props_decl))) g_props_decl = NULL;
    }
    if (!g_world_vs && FAILED(((CreateShFn2)g_fn[91])(d, k_worldcast_vs, &g_world_vs))) g_world_vs = NULL;
    if (!g_cuv_vs && FAILED(((CreateShFn2)g_fn[91])(d, k_worldcastuv_vs, &g_cuv_vs))) g_cuv_vs = NULL;
    if (!g_calpha_ps && FAILED(((CreateShFn2)g_fn[106])(d, k_castalpha_ps, &g_calpha_ps))) g_calpha_ps = NULL;
    if (!g_props_decl || !g_world_vs) { IDirect3DVertexBuffer9_Release(vb); return; }
    int alpha = 0, failed = 0;
    for (int i = 0; i < nr && g_npr < MAXPR; i++) {
        g_pr[g_npr].first = r[i].first; g_pr[g_npr].count = r[i].count; g_pr[g_npr].tex = NULL;
        if (r[i].data && g_cuv_vs && g_calpha_ps) { g_pr[g_npr].tex = cast_texture(d, &r[i]); if (g_pr[g_npr].tex) alpha++; else failed++; }
        g_npr++;
    }
    g_props_vb = vb; g_props_ntri = ntri;
    photon_log("dmirror: Nanite's props in the sun map: %u triangles (%.1f MiB), %d ranges, %d cut by their texture's alpha (%d textures failed)", ntri,
               bytes / 1048576.0, g_npr, alpha, failed);
}
int photon_dmirror_world_normals(void *rt, void *rt_pbr, void *depth_tex, const float (*vp)[4], const float (*iv)[4], const float eye[3], int w, int h) {
    void *d = g_dev;
    static int why_logged;
    if (!d || !g_bound || !g_world_vb || !rt || !depth_tex) {
        if (!why_logged++) photon_log("dmirror: world normals: not ready (device %p, bound %d, world VB %p, rt %p, depth %p)", d, g_bound, (void *)g_world_vb, rt, depth_tex);
        return 0;
    }
    for (size_t i = 0; i < sizeof k_cast_rs / sizeof k_cast_rs[0]; i++)
        if (!g_rs_known[k_cast_rs[i]]) { if (!why_logged++) photon_log("dmirror: world normals: render state %d not known yet", (int)k_cast_rs[i]); return 0; }
    if (!g_wn_vs && FAILED(((CreateShFn2)g_fn[91])(d, k_worldnrm_vs, &g_wn_vs))) return 0;
    if (!g_wn_ps && FAILED(((CreateShFn2)g_fn[106])(d, k_worldnrm_ps, &g_wn_ps))) return 0;
    FLUSH();
    RsFn rs = FN(S_SETRS, RsFn);
    static int wdbg0 = -1;
    if (wdbg0 < 0) { char e[4]; wdbg0 = GetEnvironmentVariableA("PHOTON_PTRT_WN_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    static int wnpbr = -1;   /* PHOTON_PTRT_WN_PBR=1 with PHOTON_PTRT_WN_DEBUG=1: the normals' view with the PBR stream (and packs) on, no test triangle */
    if (wnpbr < 0) { char e[4]; wnpbr = GetEnvironmentVariableA("PHOTON_PTRT_WN_PBR", e, sizeof e) > 0 && e[0] == '1'; }
    int pbr = rt_pbr && g_world_pbr_vb && g_world_pbr_decl && (!wdbg0 || wnpbr);   /* (the debug triangle draws without stream 1) */
    ((Rt2Fn)g_fn[S_SETRT])(d, 0, rt);
    if (pbr) ((Rt2Fn)g_fn[S_SETRT])(d, 1, rt_pbr);   /* PBR: roughness / metalness as MRT 1 */
    FN(S_SETDS, PtrFn)(d, NULL);
    D3DVIEWPORT9 vpt = { 0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f };
    FN(S_SETVP, PtrFn)(d, &vpt);
    FN(S_CLEAR, ClearFn)(d, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
    float cb = -0.0004f, sb = -2.0f;   /* our displacement triangles split their quads differently from the engine's in places */ DWORD cbw, sbw; memcpy(&cbw, &cb, 4); memcpy(&sbw, &sb, 4);
    rs(d, D3DRS_ZENABLE, D3DZB_FALSE); rs(d, D3DRS_ZWRITEENABLE, FALSE); rs(d, D3DRS_ZFUNC, D3DCMP_LESSEQUAL); rs(d, D3DRS_COLORWRITEENABLE, 15);   /* the PS tests depth */
    rs(d, D3DRS_ALPHABLENDENABLE, FALSE); rs(d, D3DRS_ALPHATESTENABLE, FALSE); rs(d, D3DRS_CULLMODE, D3DCULL_NONE); rs(d, D3DRS_STENCILENABLE, FALSE);
    rs(d, D3DRS_FOGENABLE, FALSE); rs(d, D3DRS_SCISSORTESTENABLE, FALSE); rs(d, D3DRS_CLIPPLANEENABLE, 0);
    (void)cbw; (void)sbw;
    rs(d, D3DRS_DEPTHBIAS, 0); rs(d, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    SampFn2 ss = (SampFn2)g_fn[S_SETSAMP];
    FN(S_SETTEX, TexFn)(d, 0, depth_tex);
    ss(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    ss(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT); ss(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE); ss(d, 0, D3DSAMP_SRGBTEXTURE, 0);
    float pc0[6][4];
    static int wdbg = -1;
    if (wdbg < 0) { char e[4]; wdbg = GetEnvironmentVariableA("PHOTON_PTRT_WN_DEBUG", e, sizeof e) > 0 && e[0] == '1'; }
    pc0[0][0] = 1.0f / (float)w; pc0[0][1] = 1.0f / (float)h; pc0[0][2] = wnpbr ? 0.0f : (float)wdbg; pc0[0][3] = 0;
    memcpy(pc0[1], iv, sizeof(float) * 16); pc0[5][0] = eye[0]; pc0[5][1] = eye[1]; pc0[5][2] = eye[2]; pc0[5][3] = 1;
    ((ConstFn2)g_fn[S_PSCF])(d, 0, &pc0[0][0], 6);
    FN(S_SETVS, PtrFn)(d, g_wn_vs); FN(S_SETPS, PtrFn)(d, g_wn_ps);
    FN(S_SETDECL, PtrFn)(d, pbr ? g_world_pbr_decl : g_world_decl);
    FN(S_SETSTREAM, StreamFn)(d, 0, g_world_vb, 0, 24);
    for (UINT i = 1; i < 4; i++) FN(S_SETSTREAM, StreamFn)(d, i, i == 1 && pbr ? (void *)g_world_pbr_vb : NULL, 0, i == 1 && pbr ? 44 : 0);
    int atl = pbr && g_atl_rmh && g_atl_nrm;   /* PBR packs: s1 / s2 the atlases (c6.x 1: sample them) */
    for (DWORD sI = 1; sI <= 2; sI++) {
        FN(S_SETTEX, TexFn)(d, sI, atl ? (void *)(sI == 1 ? g_atl_rmh : g_atl_nrm) : NULL);
        ss(d, sI, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(d, sI, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); ss(d, sI, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        ss(d, sI, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); ss(d, sI, D3DSAMP_MIPFILTER, D3DTEXF_NONE); ss(d, sI, D3DSAMP_SRGBTEXTURE, 0);
    }
    { float c6[4] = { atl ? 1.0f : 0.0f, 0, 0, 0 }; ((ConstFn2)g_fn[S_PSCF])(d, 6, c6, 1); }
    FN(S_VSCF, CfFn)(d, 8, &vp[0][0], 4);   /* c8..c11, as the casters take their light rows */
    for (UINT first = 0; first < g_world_ntri; first += 1u << 20) {
        UINT cnt = g_world_ntri - first < (1u << 20) ? g_world_ntri - first : 1u << 20;
        FN(S_DP, DpFn)(d, D3DPT_TRIANGLELIST, first * 3, cnt);
    }
    if (wdbg && !pbr) {   /* debug: a known triangle 500 units ahead (from the view rows' axes), expected centred, about a third of the screen */
        float ax[3][3];
        for (int r = 0; r < 3; r++) { int row = r == 2 ? 3 : r; float l = sqrtf(vp[row][0] * vp[row][0] + vp[row][1] * vp[row][1] + vp[row][2] * vp[row][2]);
            for (int k = 0; k < 3; k++) ax[r][k] = l > 0 ? vp[row][k] / l : 0; }   /* right, up, forward */
        float tv[3][6];
        const float off[3][2] = { { -160, -90 }, { 160, -90 }, { 0, 110 } };
        for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) { tv[c][k] = eye[k] + ax[2][k] * 500 + ax[0][k] * off[c][0] + ax[1][k] * off[c][1]; tv[c][3 + k] = k == 2; }
        FN(S_SETSTREAM, StreamFn)(d, 0, NULL, 0, 0);
        float pd[4] = { 1.0f / (float)w, 1.0f / (float)h, 2.0f, 0 };   /* z = 2: the PS's cyan */
        ((ConstFn2)g_fn[S_PSCF])(d, 0, pd, 1);
        FN(S_DPUP, DpUpFn)(d, D3DPT_TRIANGLELIST, 1, tv, 24);
    }
    for (size_t i = 0; i < sizeof k_cast_rs / sizeof k_cast_rs[0]; i++) rs(d, k_cast_rs[i], g_m.rs[k_cast_rs[i]]);   /* back to the engine's view */
    if (pbr) ((Rt2Fn)g_fn[S_SETRT])(d, 1, NULL);
    ((Rt2Fn)g_fn[S_SETRT])(d, 0, g_rt0);
    FN(S_SETDS, PtrFn)(d, g_ds);
    FN(S_SETVP, PtrFn)(d, &g_vp);
    FN(S_SETVS, PtrFn)(d, g_m.vs); FN(S_SETPS, PtrFn)(d, g_m.ps);
    if (g_m.decl) FN(S_SETDECL, PtrFn)(d, g_m.decl); else FN(S_SETFVF, DwFn)(d, g_m.fvf);
    for (UINT i = 0; i < 4; i++) FN(S_SETSTREAM, StreamFn)(d, i, g_m.stream[i].vb, g_m.stream[i].off, g_m.stream[i].stride);
    FN(S_VSCF, CfFn)(d, 8, g_m.vsc[8], 4);
    ((ConstFn2)g_fn[S_PSCF])(d, 0, &g_m.psc[0][0], 6);
    FN(S_SETTEX, TexFn)(d, 0, g_m.tex[0]);
    for (size_t k = 0; k < sizeof k_fs_samp / sizeof k_fs_samp[0]; k++)
        ss(d, 0, k_fs_samp[k], g_samp_known[0][k_fs_samp[k]] ? g_m.samp[0][k_fs_samp[k]] : k_fs_samp_default[k]);
    for (DWORD sI = 1; sI <= 2; sI++) {   /* the atlases' samplers back to the engine's */
        FN(S_SETTEX, TexFn)(d, sI, g_m.tex[sI]);
        for (size_t k = 0; k < sizeof k_fs_samp / sizeof k_fs_samp[0]; k++)
            ss(d, sI, k_fs_samp[k], g_samp_known[sI][k_fs_samp[k]] ? g_m.samp[sI][k_fs_samp[k]] : k_fs_samp_default[k]);
    }
    ((ConstFn2)g_fn[S_PSCF])(d, 6, g_m.psc[6], 1);
    return 1;
}
/* PT-RT: alpha-tested casters cut by their base texture (castalpha_ps / castalpha20_ps); only while PT-RT casts the map's props */
#include "shaders/castalpha20_ps.h"
static void *g_ca20, *g_ca30; static int g_cut_leaves; static volatile LONG64 g_cast_cut;
void photon_dmirror_cast_cut_leaves(int on) { g_cut_leaves = on; }
static int alpha_ps(void *d, int maj) {
    if (maj == 3) { if (!g_ca30 && FAILED(((CreateShFn2)g_fn[106])(d, k_castalpha_ps, &g_ca30))) g_ca30 = NULL; return g_ca30 != NULL; }
    if (!g_ca20 && FAILED(((CreateShFn2)g_fn[106])(d, k_castalpha20_ps, &g_ca20))) g_ca20 = NULL;
    return g_ca20 != NULL;
}
int photon_dmirror_cast_all(void *ds, void *rt, int size, const float (*light)[4], const DWORD *ps20, const DWORD *ps30, void **ps20_cache, void **ps30_cache) {
    PhotonCastOpts o = { 0, 0, size, NULL, 1, 0 };
    return photon_dmirror_cast_opts(ds, rt, &o, light, ps20, ps30, ps20_cache, ps30_cache);
}
/* L1b: the same into a tile of an atlas (vx, vy, size), only the casters sel marks (NULL: all), with or without stage 2a's sun gate, and
 * its own at-rest cache slot (0 the sun, 1..16 the lamp atlas tiles) */
int photon_dmirror_cast_opts(void *ds, void *rt, const PhotonCastOpts *o, const float (*light)[4], const DWORD *ps20, const DWORD *ps30, void **ps20_cache, void **ps30_cache) {
    void *d = g_dev;
    int size = o->size;
    if (!d || !g_bound || !g_vp_known) return 0;
    int world = g_world_vb && (o->slot == 0 || o->slot == 17);   /* PT-RT: the world casts even with no recorded caster */
    if (g_cast_frame != photon_frame_count()) { if (!world) return 0; g_ncast = 0; }   /* last frame's list is stale */
    if (g_ncast == 0 && !world) return 0;
    FLUSH();
    for (size_t i = 0; i < sizeof k_cast_rs / sizeof k_cast_rs[0]; i++) if (!g_rs_known[k_cast_rs[i]]) { g_cast_unknown++; return 0; }
    if (!*ps20_cache && FAILED(((CreateShFn2)g_fn[106])(d, ps20, ps20_cache))) return 0;
    if (!*ps30_cache && FAILED(((CreateShFn2)g_fn[106])(d, ps30, ps30_cache))) return 0;
    /* at rest: the same casters at the same places under the same light box draw the same map, so last frame's map is kept
     * (PHOTON_SHADOWS_NOCACHE=1: redraw every frame, A/B) */
    {
        static int nocache = -1;
        if (nocache < 0) { char e[4]; nocache = GetEnvironmentVariableA("PHOTON_SHADOWS_NOCACHE", e, sizeof e) > 0 && e[0] == '1'; }
        /* each caster's key once per frame (word-wise mixing; its buffers, draw range, matrix and bone palette), then per map only the
         * selected casters' keys and the light rows: before, every map re-hashed the whole list byte by byte (4% of MatQueue at 100 ragdolls) */
        static uint64_t ckey[NCAST];
        static unsigned long long ckey_frame = ~0ull;
        static int ckey_n = -1;
        if (ckey_frame != g_cast_frame || ckey_n != g_ncast) {
            for (int c = 0; c < g_ncast; c++) {
                const Caster *k = &g_cast[c];
                const Rec *r = k->r;
                uint64_t v[6] = { (uint64_t)(uintptr_t)(r ? r->st.ib : k->ib), (uint64_t)(uintptr_t)(r ? r->st.stream[0].vb : k->stream[0].vb),
                                  (uint64_t)(uint32_t)(r ? r->bv : k->bv), (uint64_t)(r ? r->si : k->si) << 32 | (uint64_t)(r ? r->pc : k->pc), 0, 0 };
                uint64_t hk = 0x9e3779b97f4a7c15ull;
                for (int q = 0; q < 4; q++) hk = mix64(hk ^ v[q]);
                const uint64_t *w = (const uint64_t *)k->m34;
                for (size_t q = 0; q < sizeof k->m34 / 8; q++) hk = mix64(hk ^ w[q]);
                if (k->bones >= 0) {   /* an animating character moves its bones, not c58..c60 */
                    const uint64_t *b = (const uint64_t *)g_bones[k->bones];
                    for (size_t q = 0; q < sizeof g_bones[0] / 8; q++) hk = mix64(hk ^ b[q]);
                }
                ckey[c] = hk;
            }
            ckey_frame = g_cast_frame;
            ckey_n = g_ncast;
        }
        uint64_t h = 1469598103934665603ull;
        { const uint64_t *lw = (const uint64_t *)light; for (int q = 0; q < 8; q++) h = mix64(h ^ lw[q]); }
        int nsel = 0;
        for (int c = 0; c < g_ncast; c++) {
            if ((o->sel && !o->sel[c]) || g_cast[c].noshadow) continue;
            nsel++;
            h = mix64(h ^ ckey[c]);
        }
        enum { NSLOT = 18 };   /* 0 the sun, 1..16 the lamp atlas tiles, 17 the sun's near cascade */
        static uint64_t last[NSLOT];
        static int last_n[NSLOT], inited;
        if (!inited) { inited = 1; for (int i = 0; i < NSLOT; i++) last_n[i] = -1; }
        int sl = o->slot >= 0 && o->slot < NSLOT ? o->slot : 0;
        if ((sl == 0 || sl == 17) && g_world_vb) h = mix64(h ^ (0x9e3779b97f4a7c15ull + g_world_gen));   /* the world changed: redraw */
        if (g_cast_invalid) { g_cast_invalid = 0; for (int i = 0; i < NSLOT; i++) last_n[i] = -1; }
        if (!nocache && h == last[sl] && nsel == last_n[sl]) { g_cast_cached++; return nsel + world; }   /* the world counts as a caster (PT-RT) */
        last[sl] = h; last_n[sl] = nsel;
    }
    RsFn rs = FN(S_SETRS, RsFn);
    ((Rt2Fn)g_fn[S_SETRT])(d, 0, rt);
    FN(S_SETDS, PtrFn)(d, ds);
    D3DVIEWPORT9 vp = { (DWORD)o->vx, (DWORD)o->vy, (DWORD)size, (DWORD)size, 0.0f, 1.0f };   /* Clear below clears only this rect */
    FN(S_SETVP, PtrFn)(d, &vp);
    FN(S_CLEAR, ClearFn)(d, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    rs(d, D3DRS_ZENABLE, D3DZB_TRUE); rs(d, D3DRS_ZWRITEENABLE, TRUE); rs(d, D3DRS_ZFUNC, D3DCMP_LESSEQUAL); rs(d, D3DRS_COLORWRITEENABLE, 0);
    rs(d, D3DRS_ALPHABLENDENABLE, FALSE); rs(d, D3DRS_ALPHATESTENABLE, FALSE); rs(d, D3DRS_CULLMODE, D3DCULL_NONE); rs(d, D3DRS_STENCILENABLE, FALSE);
    rs(d, D3DRS_FOGENABLE, FALSE); rs(d, D3DRS_SCISSORTESTENABLE, FALSE); rs(d, D3DRS_CLIPPLANEENABLE, 0);
    rs(d, D3DRS_DEPTHBIAS, 0); rs(d, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    int n = 0, any_bones = 0, any_alpha = 0;
    for (int c = 0; c < g_ncast; c++) {
        const Caster *k = &g_cast[c];
        const Rec *r = k->r;
        if ((o->sel && !o->sel[c]) || k->noshadow) continue;
        if (k->zstreams) {   /* the zero VB must cover every vertex the draw reads */
            int fits = zero_vb(d) != NULL;
            for (int i = 1; i < 4 && fits; i++)
                if ((k->zstreams >> i & 1) && ((unsigned long long)(UINT)(k->bv > 0 ? k->bv : 0) + k->mi + k->nv) * k->stream[i].stride > ZERO_VB_BYTES) fits = 0;
            if (!fits) { g_cast_skipped++; continue; }
        }
        if (o->sun_gate) {   /* stage 2a: indoor casters do not cast the sun (skinned: by their bone 0's cell) */
            if (k->bones < 0 ? !photon_shadows_caster_lit((const float (*)[4])k->m34)
                             : !photon_shadows_skinned_lit((const float[3]){ g_bones[k->bones][0][3], g_bones[k->bones][1][3], g_bones[k->bones][2][3] })) { g_cast_indoor++; continue; }
        }   /* stage 2a: indoor props do not cast */
        void *vs = r ? r->st.vs : k->vs;
        int maj = vs_major(vs);
        if (!vs || (maj != 2 && maj != 3)) { g_cast_skipped++; continue; }
        FN(S_SETVS, PtrFn)(d, vs);
        if (k->alpha && k->tex0 && g_cut_leaves && alpha_ps(d, maj)) {   /* an alpha-tested material (leaves): cut by its base texture */
            FN(S_SETPS, PtrFn)(d, maj == 3 ? g_ca30 : g_ca20);
            FN(S_SETTEX, TexFn)(d, 0, k->tex0);
            if (!any_alpha) {
                any_alpha = 1;
                SampFn2 ssa = (SampFn2)g_fn[S_SETSAMP];
                ssa(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); ssa(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP); ssa(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                ssa(d, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); ssa(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); ssa(d, 0, D3DSAMP_SRGBTEXTURE, 0);
            }
            g_cast_cut++;
        } else FN(S_SETPS, PtrFn)(d, maj == 3 ? *ps30_cache : *ps20_cache);
        if (r) {
            const Mirror *a = &r->st;
            FN(S_SETDECL, PtrFn)(d, a->decl);
            FN(S_SETIB, PtrFn)(d, a->ib);
            for (UINT i = 0; i < 4; i++) FN(S_SETSTREAM, StreamFn)(d, i, a->stream[i].vb, a->stream[i].off, a->stream[i].stride);
            FN(S_VSCF, CfFn)(d, 0, a->vsc[0], 58);
        } else {
            FN(S_SETDECL, PtrFn)(d, k->decl);
            FN(S_SETIB, PtrFn)(d, k->ib);
            for (UINT i = 0; i < 4; i++)
                FN(S_SETSTREAM, StreamFn)(d, i, (k->zstreams >> i & 1) ? (void *)g_zero_vb : k->stream[i].vb, (k->zstreams >> i & 1) ? 0 : k->stream[i].off, k->stream[i].stride);
            FN(S_VSCF, CfFn)(d, 0, k->vsc[0], 58);
        }
        FN(S_VSCF, CfFn)(d, 8, &light[0][0], 4);
        if (k->bones >= 0) { FN(S_VSCF, CfFn)(d, 58, g_bones[k->bones][0], NBONEREG); any_bones = 1; }
        else FN(S_VSCF, CfFn)(d, 58, k->m34[0], 3);
        int dip = r ? r->dip : k->dip;
        if (dip) FN(S_DIP, DipFn)(d, r ? r->type : k->type, r ? r->bv : k->bv, r ? r->mi : k->mi, r ? r->nv : k->nv, r ? r->si : k->si, r ? r->pc : k->pc);
        else FN(S_DP, DpFn)(d, r ? r->type : k->type, (UINT)(r ? r->bv : k->bv), r ? r->pc : k->pc);
        n++;
    }
    if (g_world_vb && (o->slot == 0 || o->slot == 17)) {   /* the world (PT-RT), after the casters, into the same map */
        if (!*ps30_cache) ((CreateShFn2)g_fn[106])(d, ps30, ps30_cache);
        if (*ps30_cache) {
            FN(S_SETVS, PtrFn)(d, g_world_vs);
            FN(S_SETPS, PtrFn)(d, *ps30_cache);
            FN(S_SETDECL, PtrFn)(d, g_world_decl);
            FN(S_SETSTREAM, StreamFn)(d, 0, g_world_vb, 0, 24);
            for (UINT i = 1; i < 4; i++) FN(S_SETSTREAM, StreamFn)(d, i, NULL, 0, 0);
            FN(S_VSCF, CfFn)(d, 8, &light[0][0], 4);
            /* the world (displacement terrain at grazing sun) would shadow itself: a slope-scaled bias pushes its depth back along the light */
            RsFn rsb = FN(S_SETRS, RsFn);
            float sb = 2.5f, cb = 0.0004f;
            DWORD sbw, cbw; memcpy(&sbw, &sb, 4); memcpy(&cbw, &cb, 4);
            rsb(d, D3DRS_SLOPESCALEDEPTHBIAS, sbw); rsb(d, D3DRS_DEPTHBIAS, cbw);
            for (UINT first = 0; first < g_world_ntri; first += 1u << 20) {
                UINT cnt = g_world_ntri - first < (1u << 20) ? g_world_ntri - first : 1u << 20;
                FN(S_DP, DpFn)(d, D3DPT_TRIANGLELIST, first * 3, cnt);
            }
            if (g_props_vb && g_props_decl) {   /* Nanite's props after the world, same bias; alpha-tested ranges cut by their texture */
                FN(S_SETDECL, PtrFn)(d, g_props_decl);
                FN(S_SETSTREAM, StreamFn)(d, 0, g_props_vb, 0, 20);
                SampFn2 ssb = (SampFn2)g_fn[S_SETSAMP];
                int bound = 0;
                for (int i = 0; i < g_npr; i++) {
                    if (g_pr[i].tex) {
                        FN(S_SETVS, PtrFn)(d, g_cuv_vs); FN(S_SETPS, PtrFn)(d, g_calpha_ps);
                        FN(S_SETTEX, TexFn)(d, 0, g_pr[i].tex);
                        if (!bound) {
                            bound = 1;
                            ssb(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); ssb(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP); ssb(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                            ssb(d, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); ssb(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR); ssb(d, 0, D3DSAMP_SRGBTEXTURE, 0);
                        }
                    } else { FN(S_SETVS, PtrFn)(d, g_world_vs); FN(S_SETPS, PtrFn)(d, *ps30_cache); }
                    for (UINT first = g_pr[i].first; first < g_pr[i].first + g_pr[i].count; first += 1u << 20) {
                        UINT end = g_pr[i].first + g_pr[i].count, cnt = end - first < (1u << 20) ? end - first : 1u << 20;
                        FN(S_DP, DpFn)(d, D3DPT_TRIANGLELIST, first * 3, cnt);
                    }
                }
                if (bound) {   /* stage 0 back to the engine's */
                    FN(S_SETTEX, TexFn)(d, 0, g_m.tex[0]);
                    for (size_t k = 0; k < sizeof k_fs_samp / sizeof k_fs_samp[0]; k++)
                        ssb(d, 0, k_fs_samp[k], g_samp_known[0][k_fs_samp[k]] ? g_m.samp[0][k_fs_samp[k]] : k_fs_samp_default[k]);
                }
            }
            rsb(d, D3DRS_SLOPESCALEDEPTHBIAS, 0); rsb(d, D3DRS_DEPTHBIAS, 0);
            n++;
        }
    }
    g_cast_draws += n;
    if (any_alpha) {   /* stage 0 back to the engine's */
        SampFn2 ssa = (SampFn2)g_fn[S_SETSAMP];
        FN(S_SETTEX, TexFn)(d, 0, g_m.tex[0]);
        for (size_t q = 0; q < sizeof k_fs_samp / sizeof k_fs_samp[0]; q++)
            ssa(d, 0, k_fs_samp[q], g_samp_known[0][k_fs_samp[q]] ? g_m.samp[0][k_fs_samp[q]] : k_fs_samp_default[q]);
    }
    /* back to the engine's view of the device */
    for (size_t i = 0; i < sizeof k_cast_rs / sizeof k_cast_rs[0]; i++) rs(d, k_cast_rs[i], g_m.rs[k_cast_rs[i]]);
    ((Rt2Fn)g_fn[S_SETRT])(d, 0, g_rt0);
    FN(S_SETDS, PtrFn)(d, g_ds);
    FN(S_SETVP, PtrFn)(d, &g_vp);
    FN(S_SETVS, PtrFn)(d, g_m.vs); FN(S_SETPS, PtrFn)(d, g_m.ps);
    if (g_m.decl) FN(S_SETDECL, PtrFn)(d, g_m.decl); else FN(S_SETFVF, DwFn)(d, g_m.fvf);
    FN(S_SETIB, PtrFn)(d, g_m.ib);
    for (UINT i = 0; i < 4; i++) FN(S_SETSTREAM, StreamFn)(d, i, g_m.stream[i].vb, g_m.stream[i].off, g_m.stream[i].stride);
    FN(S_VSCF, CfFn)(d, 0, g_m.vsc[0], any_bones ? 58 + NBONEREG : 61);
    return n;
}
/* AA (motion vectors): the main view's moving models drawn again into a velocity target at its end (velocity_vs / velocity_ps), for the
 * TAA resolve and later the upscalers. This frame's casters (the main view's: their c8..c11 is its view-projection) are matched to last
 * frame's by their mesh (buffers and draw range) and their order among the draws of that mesh; one whose model matrix or bone palette
 * did not change is left out (the camera-only reprojection from depth is exact there), so a still scene costs no draw. A rigid draw
 * takes last frame's matrix in c61..c63; a skinned one its palette from a vertex texture row. Depth-tested (not written) against the
 * main view's depth with a small bias, so only the visible surface gets the model's motion. */
#include "shaders/velocity_vs.h"
#include "shaders/velocity_ps.h"
enum { VHASH = 16384, VROWS = 512, VTEXW = 256 };
typedef struct { uint64_t key; float m34[3][4]; int bones; } VelPrev;
static VelPrev *g_vprev; static int g_nvprev; static unsigned long long g_vprev_frame = ~0ull;
static float (*g_vbones)[NBONEREG][4]; static int g_nvbones;
static IDirect3DTexture9 *g_vtex; static void *g_vel_vs, *g_vel_ps; static int g_vel_failed;
static volatile LONG64 g_vel_frames, g_vel_draws, g_vel_still, g_vel_unmatched, g_vel_other_view, g_vel_rows_full, g_vel_skipped;
static const D3DRENDERSTATETYPE k_vel_rs[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE,
                                               D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_STENCILENABLE, D3DRS_FOGENABLE, D3DRS_SCISSORTESTENABLE,
                                               D3DRS_CLIPPLANEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_SRGBWRITEENABLE };
static uint64_t vel_key(const Caster *k) {
    uint64_t v[4] = { (uint64_t)(uintptr_t)k->ib, (uint64_t)(uintptr_t)k->stream[0].vb ^ ((uint64_t)k->stream[0].off << 40),
                      (uint64_t)(uint32_t)k->bv | (uint64_t)k->mi << 32, (uint64_t)k->si << 32 | (uint64_t)k->pc };
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (int q = 0; q < 4; q++) h = mix64(h ^ v[q]);
    return h ? h : 1;
}
static int decl_compressed(void *decl) {   /* BLENDWEIGHT as SHORT2 / SHORT4: the stock shaders' COMPRESSED_VERTS weights */
    static void *k[256]; static signed char v[256]; static int n;
    if (!decl) return 0;
    for (int i = 0; i < n && i < 256; i++) if (k[i] == decl) return v[i];
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]; UINT ne = MAXD3DDECLLENGTH + 1;
    int r = 0;
    if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)decl, el, &ne)))
        for (UINT i = 0; i < ne && el[i].Stream != 0xFF; i++)
            if (el[i].Usage == D3DDECLUSAGE_BLENDWEIGHT && (el[i].Type == D3DDECLTYPE_SHORT2 || el[i].Type == D3DDECLTYPE_SHORT4)) r = 1;
    k[n & 255] = decl; v[n & 255] = (signed char)r; n++;
    return r;
}
static int decl_normal(void *decl) {   /* the NORMAL element: 0 float, 1 UBYTE4 (compressed), -1 none / other (no normal written) */
    static void *k[256]; static signed char v[256]; static int n;
    if (!decl) return -1;
    for (int i = 0; i < n && i < 256; i++) if (k[i] == decl) return v[i];
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]; UINT ne = MAXD3DDECLLENGTH + 1;
    int r = -1;
    if (SUCCEEDED(IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)decl, el, &ne)))
        for (UINT i = 0; i < ne && el[i].Stream != 0xFF; i++)
            if (el[i].Usage == D3DDECLUSAGE_NORMAL && el[i].UsageIndex == 0)
                r = el[i].Type == D3DDECLTYPE_FLOAT3 || el[i].Type == D3DDECLTYPE_FLOAT4 ? 0 : el[i].Type == D3DDECLTYPE_UBYTE4 ? 1 : -1;
    k[n & 255] = decl; v[n & 255] = (signed char)r; n++;
    return r;
}
static int same_rows(const float (*a)[4], const float (*b)[4]) {   /* a caster's c8..c11 against the main view's (float noise only) */
    float mx = 0;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float x = fabsf(b[i][j]); if (x > mx) mx = x; }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) if (fabsf(a[i][j] - b[i][j]) > 1e-4f * (mx > 1 ? mx : 1)) return 0;
    return 1;
}
int photon_dmirror_velocity(void *rt, void *nrm, int all, void *ds, int w, int h, const float (*main_vp)[4], const float (*prev_rows)[4], const float prev_eye[3], int have_prev) {
    void *d = g_dev;
    if (!d || !g_bound || !g_vp_known || g_vel_failed || !rt || !ds) return -1;
    unsigned long long f = photon_frame_count();
    int ncast = g_cast_frame == f ? g_ncast : 0;
    if (!g_vprev && !(g_vprev = HeapAlloc(GetProcessHeap(), 0, sizeof(VelPrev) * NCAST))) return -1;
    if (!g_vbones && !(g_vbones = HeapAlloc(GetProcessHeap(), 0, sizeof(*g_vbones) * NBONES))) return -1;
    static int tail[VHASH], cur[VHASH], nxt[NCAST]; static uint64_t hkey[VHASH];
    static int sel[NCAST], row[NCAST];
    static const float (*prvm[NCAST])[4];   /* last frame's matrix (rigid, 3 rows) or palette (skinned) of each draw; its own when still / new */
    int nsel = 0, nrow = 0;
    int prev_ok = have_prev && g_vprev_frame + 1 == f;
    if (prev_ok) {
        memset(hkey, 0, sizeof hkey);
        for (int i = 0; i < g_nvprev; i++) {
            uint64_t kk = g_vprev[i].key; unsigned s = (unsigned)kk & (VHASH - 1);
            while (hkey[s] && hkey[s] != kk) s = (s + 1) & (VHASH - 1);
            nxt[i] = -1;
            if (!hkey[s]) { hkey[s] = kk; tail[s] = cur[s] = i; }
            else { nxt[tail[s]] = i; tail[s] = i; }
        }
    }
    static uint64_t ckey[NCAST]; static unsigned char cmain[NCAST];
    for (int c = 0; c < ncast; c++) {
        const Caster *k = &g_cast[c];
        cmain[c] = 0;
        if (k->r) { g_vel_skipped++; continue; }   /* a replayed copy (P6, static props): not followed */
        if (!same_rows((const float (*)[4])k->vsc[8], main_vp)) { g_vel_other_view++; continue; }
        cmain[c] = 1;
        ckey[c] = vel_key(k);
        const float (*m)[4] = k->bones >= 0 ? (const float (*)[4])g_bones[k->bones] : (const float (*)[4])k->m34;
        const float (*pm)[4] = NULL;
        if (prev_ok) {
            unsigned s = (unsigned)ckey[c] & (VHASH - 1);
            while (hkey[s] && hkey[s] != ckey[c]) s = (s + 1) & (VHASH - 1);
            int j = hkey[s] ? cur[s] : -1;
            if (j >= 0) cur[s] = nxt[j];
            if (j >= 0 && (g_vprev[j].bones >= 0) == (k->bones >= 0)) {
                pm = g_vprev[j].bones >= 0 ? (const float (*)[4])g_vbones[g_vprev[j].bones] : (const float (*)[4])g_vprev[j].m34;
                float dx = m[0][3] - pm[0][3], dy = m[1][3] - pm[1][3], dz = m[2][3] - pm[2][3];
                if (dx * dx + dy * dy + dz * dz > 512.0f * 512.0f) pm = NULL;   /* a teleport, or another instance */
            }
        }
        int still = pm && (k->bones >= 0 ? !memcmp(m, pm, sizeof g_bones[0]) : !memcmp(m, pm, sizeof k->m34));
        if (!pm) g_vel_unmatched++;
        else if (still) g_vel_still++;
        if (!all && (!pm || still)) continue;   /* motion only: a still or new model reprojects with the camera */
        if (!pm) pm = m;                        /* drawn for its normal: last frame's pose taken as this one's */
        if (k->bones >= 0) { if (nrow >= VROWS) { g_vel_rows_full++; continue; } row[nsel] = nrow++; } else row[nsel] = -1;
        sel[nsel] = c; prvm[nsel] = pm; nsel++;
    }
    FLUSH();
    int drawn = 0, ok = 1;
    for (size_t i = 0; i < sizeof k_vel_rs / sizeof k_vel_rs[0]; i++) if (!g_rs_known[k_vel_rs[i]] && k_vel_rs[i] != D3DRS_SRGBWRITEENABLE) ok = 0;
    if (ok && !g_vel_vs && FAILED(((CreateShFn2)g_fn[91])(d, k_velocity_vs, &g_vel_vs))) { g_vel_failed = 1; photon_log("dmirror: velocity VS failed"); }
    if (ok && !g_vel_ps && FAILED(((CreateShFn2)g_fn[106])(d, k_velocity_ps, &g_vel_ps))) { g_vel_failed = 1; photon_log("dmirror: velocity PS failed"); }
    if (ok && nrow && !g_vtex) {
        if (FAILED(((HRESULT (WINAPI *)(void *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *))g_fn[23])(
                d, VTEXW, VROWS, 1, D3DUSAGE_DYNAMIC, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g_vtex, NULL))) {
            g_vtex = NULL; photon_log("dmirror: velocity bone texture failed: skinned models get the camera's motion only");
            for (int i = 0; i < nsel; i++) if (row[i] >= 0) sel[i] = -1;
            nrow = 0;
        }
    }
    if (ok && nrow) {   /* last frame's palettes, one row per skinned draw */
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(g_vtex, 0, &lr, NULL, D3DLOCK_DISCARD))) {
            for (int i = 0; i < nsel; i++) if (sel[i] >= 0 && row[i] >= 0)
                memcpy((char *)lr.pBits + (size_t)row[i] * lr.Pitch, prvm[i], sizeof g_vbones[0]);
            IDirect3DTexture9_UnlockRect(g_vtex, 0);
        } else for (int i = 0; i < nsel; i++) if (row[i] >= 0) sel[i] = -1;
    }
    if (ok && !g_vel_failed) {
        RsFn rs = FN(S_SETRS, RsFn);
        SampFn2 ss = (SampFn2)g_fn[S_SETSAMP];
        ((Rt2Fn)g_fn[S_SETRT])(d, 0, rt);
        if (nrm) ((Rt2Fn)g_fn[S_SETRT])(d, 1, nrm);   /* PT-RT models: their normals beside the motion (COLOR1) */
        FN(S_SETDS, PtrFn)(d, ds);
        D3DVIEWPORT9 full = { 0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f };
        FN(S_SETVP, PtrFn)(d, &full);
        rs(d, D3DRS_SCISSORTESTENABLE, FALSE);
        FN(S_CLEAR, ClearFn)(d, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
        if (nsel) {
            FN(S_SETVP, PtrFn)(d, &g_vp);   /* the engine's main viewport (a scaled render covers part of the target) */
            rs(d, D3DRS_ZENABLE, D3DZB_TRUE); rs(d, D3DRS_ZWRITEENABLE, FALSE); rs(d, D3DRS_ZFUNC, D3DCMP_LESSEQUAL); rs(d, D3DRS_COLORWRITEENABLE, 0xF);
            rs(d, D3DRS_ALPHABLENDENABLE, FALSE); rs(d, D3DRS_ALPHATESTENABLE, FALSE); rs(d, D3DRS_CULLMODE, D3DCULL_NONE); rs(d, D3DRS_STENCILENABLE, FALSE);
            rs(d, D3DRS_FOGENABLE, FALSE); rs(d, D3DRS_CLIPPLANEENABLE, 0); rs(d, D3DRS_SRGBWRITEENABLE, FALSE);
            float db = -0.00002f; DWORD dbw; memcpy(&dbw, &db, 4);
            rs(d, D3DRS_DEPTHBIAS, dbw); rs(d, D3DRS_SLOPESCALEDEPTHBIAS, 0);
            FN(S_SETVS, PtrFn)(d, g_vel_vs); FN(S_SETPS, PtrFn)(d, g_vel_ps);
            float pc[1][4] = { { 1.0f / (float)w, 1.0f / (float)h, 0, 0 } };
            FN(S_PSCF, CfFn)(d, 0, pc[0], 1);
            float vc[13][4];
            memset(vc, 0, sizeof vc);
            memcpy(vc[4], prev_rows, sizeof(float) * 16);
            vc[12][0] = prev_eye[0]; vc[12][1] = prev_eye[1]; vc[12][2] = prev_eye[2];
            if (g_vtex) {
                FN(S_SETTEX, TexFn)(d, D3DVERTEXTEXTURESAMPLER0, g_vtex);
                ss(d, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); ss(d, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                ss(d, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                ss(d, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); ss(d, D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            }
            int any_bones = 0;
            for (int i = 0; i < nsel; i++) {
                if (sel[i] < 0) continue;
                const Caster *k = &g_cast[sel[i]];
                if (k->zstreams) {
                    int fits = zero_vb(d) != NULL;
                    for (int q = 1; q < 4 && fits; q++)
                        if ((k->zstreams >> q & 1) && ((unsigned long long)(UINT)(k->bv > 0 ? k->bv : 0) + k->mi + k->nv) * k->stream[q].stride > ZERO_VB_BYTES) fits = 0;
                    if (!fits) { g_vel_skipped++; continue; }
                }
                FN(S_SETDECL, PtrFn)(d, k->decl);
                FN(S_SETIB, PtrFn)(d, k->ib);
                for (UINT q = 0; q < 4; q++)
                    FN(S_SETSTREAM, StreamFn)(d, q, (k->zstreams >> q & 1) ? (void *)g_zero_vb : k->stream[q].vb, (k->zstreams >> q & 1) ? 0 : k->stream[q].off, k->stream[q].stride);
                vc[0][0] = k->bones >= 0 ? 1.0f : 0.0f;
                vc[0][1] = (float)decl_compressed(k->decl);
                vc[0][2] = row[i] >= 0 ? ((float)row[i] + 0.5f) / (float)VROWS : 0;
                vc[0][3] = nrm ? (float)decl_normal(k->decl) : -1.0f;
                memcpy(vc[8], k->vsc[8], sizeof(float) * 16);
                FN(S_VSCF, CfFn)(d, 0, vc[0], 13);
                if (k->bones >= 0) { FN(S_VSCF, CfFn)(d, 58, g_bones[k->bones][0], NBONEREG); any_bones = 1; }
                else {
                    float m6[6][4];
                    memcpy(m6[0], k->m34, sizeof k->m34); memcpy(m6[3], prvm[i], sizeof k->m34);
                    FN(S_VSCF, CfFn)(d, 58, m6[0], 6);
                }
                if (k->dip) FN(S_DIP, DipFn)(d, k->type, k->bv, k->mi, k->nv, k->si, k->pc);
                else FN(S_DP, DpFn)(d, k->type, (UINT)k->bv, k->pc);
                drawn++;
            }
            if (g_vtex) {   /* vertex sampler 0 back to the engine's */
                FN(S_SETTEX, TexFn)(d, D3DVERTEXTEXTURESAMPLER0, g_m.tex[16]);
                static const D3DSAMPLERSTATETYPE vs_s[] = { D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV };
                static const DWORD vs_def[] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP };
                for (size_t q = 0; q < 5; q++) ss(d, D3DVERTEXTEXTURESAMPLER0, vs_s[q], g_samp_known[16][vs_s[q]] ? g_m.samp[16][vs_s[q]] : vs_def[q]);
            }
            FN(S_PSCF, CfFn)(d, 0, g_m.psc[0], 1);
            FN(S_VSCF, CfFn)(d, 0, g_m.vsc[0], any_bones ? 58 + NBONEREG : 64);
        }
        /* back to the engine's view of the device */
        for (size_t i = 0; i < sizeof k_vel_rs / sizeof k_vel_rs[0]; i++)
            if (g_rs_known[k_vel_rs[i]]) rs(d, k_vel_rs[i], g_m.rs[k_vel_rs[i]]);
            else if (k_vel_rs[i] == D3DRS_SRGBWRITEENABLE) rs(d, D3DRS_SRGBWRITEENABLE, FALSE);
        if (nrm) ((Rt2Fn)g_fn[S_SETRT])(d, 1, NULL);
        ((Rt2Fn)g_fn[S_SETRT])(d, 0, g_rt0);
        FN(S_SETDS, PtrFn)(d, g_ds);
        FN(S_SETVP, PtrFn)(d, &g_vp);
        FN(S_SETVS, PtrFn)(d, g_m.vs); FN(S_SETPS, PtrFn)(d, g_m.ps);
        if (g_m.decl) FN(S_SETDECL, PtrFn)(d, g_m.decl); else FN(S_SETFVF, DwFn)(d, g_m.fvf);
        FN(S_SETIB, PtrFn)(d, g_m.ib);
        for (UINT q = 0; q < 4; q++) FN(S_SETSTREAM, StreamFn)(d, q, g_m.stream[q].vb, g_m.stream[q].off, g_m.stream[q].stride);
    }
    /* this frame's main-view casters: next frame's "last frame" */
    g_nvprev = 0; g_nvbones = 0;
    for (int c = 0; c < ncast; c++) {
        if (!cmain[c]) continue;
        const Caster *k = &g_cast[c];
        VelPrev *p = &g_vprev[g_nvprev];
        p->key = ckey[c];
        memcpy(p->m34, k->m34, sizeof p->m34);
        p->bones = -1;
        if (k->bones >= 0) { if (g_nvbones >= NBONES) continue; p->bones = g_nvbones++; memcpy(g_vbones[p->bones], g_bones[k->bones], sizeof g_vbones[0]); }
        g_nvprev++;
    }
    g_vprev_frame = f;
    g_vel_frames++; g_vel_draws += drawn;
    if (g_vel_frames % 100 == 50)
        photon_log("dmirror: motion vectors: frame %lld: casters %d, drawn %d (total %lld), still %lld, unmatched %lld, other views %lld, skipped %lld, rows full %lld",
                   (long long)g_vel_frames, ncast, drawn, (long long)g_vel_draws, (long long)g_vel_still, (long long)g_vel_unmatched,
                   (long long)g_vel_other_view, (long long)g_vel_skipped, (long long)g_vel_rows_full);
    return ok && !g_vel_failed ? drawn : -1;
}
void photon_dmirror_velocity_release(void) {   /* a device reset: the DEFAULT-pool bone texture goes */
    if (g_vtex) { IDirect3DTexture9_Release(g_vtex); g_vtex = NULL; }
    g_vprev_frame = ~0ull;
}
int photon_dmirror_caster_pos(int i, float p[3]) {   /* L1: this frame's caster i: its translation (rigid) or bone 0's (skinned); 1 skinned */
    const Caster *k = &g_cast[i];
    const float (*m)[4] = k->bones >= 0 ? (const float (*)[4])g_bones[k->bones] : (const float (*)[4])k->m34;
    p[0] = m[0][3]; p[1] = m[1][3]; p[2] = m[2][3];
    return k->noshadow ? -1 : k->bones >= 0;
}
void photon_dmirror_cast_skin_stats(long long out[5]) {
    out[0] = g_cast_snap_skinned; out[1] = g_cast_skin_off; out[2] = g_cast_skin_dynvb; out[3] = g_cast_skin_flex; out[4] = g_cast_skin_full;
}
long long photon_dmirror_cast_draws(void) { return g_cast_draws; }
long long photon_dmirror_cast_cached(void) { return g_cast_cached; }
long long photon_dmirror_cast_skipped(void) { return g_cast_skipped + g_cast_unknown * 1000000 + g_cast_indoor * 1000; }
int photon_dmirror_ncast(void) { return g_cast_frame == photon_frame_count() ? g_ncast : 0; }   /* this frame's casters */
long long photon_dmirror_fs_unknown(void) { return g_fs_unknown; }
long long photon_dmirror_draw_count(void) { return g_draws; }
int photon_dmirror_bound(void) { return g_bound; }
void *photon_dmirror_ds(void) { return g_ds; }
const float (*photon_dmirror_viewproj(void))[4] { return (const float (*)[4])g_m.vsc[8]; }

/* shader creation: count and time (load-time cost of Photon's extra shaders, 2026-11-07); not mirrored state */
enum { S_CREATEVS = 91, S_CREATEPS = 106 };
static volatile LONG64 g_create_n[2], g_create_ticks[2], g_create_bytes[2];
/* Lazy shader creation (default on, PHOTON_LAZY_SHADERS=0 off; analysis/32): Create*Shader of bytecode of at least g_lazy_min bytes returns a stand-in
 * that keeps a copy of the bytecode; Set*Shader creates the real shader on first use and passes it on. The shader manager creates every
 * dynamic combo of every static a loaded material names, ~96% of which are never set, and Photon's large ps_3_0 skin combos cost the
 * native driver ~2 s of compiling during a map load. The stand-in is a full IDirect3D{Pixel,Vertex}Shader9 (IUnknown, GetDevice,
 * GetFunction), so nothing else in the game can tell; Get*Shader returns the real one the device holds. */
typedef struct Lazy Lazy;
typedef struct {
    HRESULT (WINAPI *qi)(Lazy *, REFIID, void **);
    ULONG (WINAPI *addref)(Lazy *);
    ULONG (WINAPI *release)(Lazy *);
    HRESULT (WINAPI *getdevice)(Lazy *, void **);
    HRESULT (WINAPI *getfunction)(Lazy *, void *, UINT *);
} LazyVt;
struct Lazy { const LazyVt *vt; volatile LONG ref; int kind; void *dev; DWORD *code; UINT bytes; void *volatile real; };
static UINT g_lazy_min = 4096;
/* the trickle: stand-ins not yet real, in creation order, each holding one reference of ours; EndScene (the device thread) creates up
 * to g_lazy_trickle of them once per frame, so the driver compiles them in the background before most are first set, instead of
 * all inside the load (eager) or one by one on first sight (~20 ms each on the GTX 1050). 0 = no trickle. */
enum { LAZYQ = 8192 };
static Lazy *g_lazyq[LAZYQ];
static volatile LONG g_lazyq_head, g_lazyq_tail;
static int g_lazy_trickle = 2;
static volatile LONG64 g_lazy_trickled, g_lazy_trickle_ticks;
static unsigned long long g_lazy_frame = ~0ull;
static volatile LONG64 g_lazy_made, g_lazy_real, g_lazy_freed, g_lazy_real_ticks, g_lazy_fail;
static HRESULT WINAPI lz_qi(Lazy *l, REFIID riid, void **out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, l->kind ? &IID_IDirect3DPixelShader9 : &IID_IDirect3DVertexShader9)) {
        InterlockedIncrement(&l->ref); *out = l; return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI lz_addref(Lazy *l) { return (ULONG)InterlockedIncrement(&l->ref); }
static ULONG WINAPI lz_release(Lazy *l) {
    LONG r = InterlockedDecrement(&l->ref);
    if (r == 0) {
        IUnknown *u = l->real;
        if (u) u->lpVtbl->Release(u);
        ((IUnknown *)l->dev)->lpVtbl->Release((IUnknown *)l->dev);
        HeapFree(GetProcessHeap(), 0, l->code);
        HeapFree(GetProcessHeap(), 0, l);
        InterlockedIncrement64(&g_lazy_freed);
    }
    return (ULONG)(r < 0 ? 0 : r);
}
static HRESULT WINAPI lz_getdevice(Lazy *l, void **out) {
    if (!out) return D3DERR_INVALIDCALL;
    ((IUnknown *)l->dev)->lpVtbl->AddRef((IUnknown *)l->dev); *out = l->dev; return D3D_OK;
}
static HRESULT WINAPI lz_getfunction(Lazy *l, void *data, UINT *size) {
    if (!size) return D3DERR_INVALIDCALL;
    if (!data) { *size = l->bytes; return D3D_OK; }
    if (*size < l->bytes) return D3DERR_INVALIDCALL;
    memcpy(data, l->code, l->bytes); *size = l->bytes; return D3D_OK;
}
static const LazyVt k_lazy_vt = { lz_qi, lz_addref, lz_release, lz_getdevice, lz_getfunction };
typedef HRESULT (WINAPI *CreateShFn)(void *, const DWORD *, void **);
static void *lazy_real(void *d, void *s) {
    if (!s || *(const LazyVt *const *)s != &k_lazy_vt) return s;
    Lazy *l = s;
    void *r = l->real;
    if (r) return r;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = ((CreateShFn)g_fn[l->kind ? 106 : 91])(d, l->code, &r);
    QueryPerformanceCounter(&b);
    if (FAILED(hr) || !r) { InterlockedIncrement64(&g_lazy_fail); return NULL; }
    void *prev = InterlockedCompareExchangePointer((PVOID volatile *)&l->real, r, NULL);
    if (prev) { ((IUnknown *)r)->lpVtbl->Release((IUnknown *)r); return prev; }
    InterlockedIncrement64(&g_lazy_real);
    InterlockedAdd64(&g_lazy_real_ticks, b.QuadPart - a.QuadPart);
    return r;
}

static void lazy_trickle(void *d) {
    unsigned long long f = photon_frame_count();
    if (f == g_lazy_frame) return;
    g_lazy_frame = f;
    for (int n = 0; n < g_lazy_trickle && g_lazyq_head != g_lazyq_tail; ) {
        Lazy *l = g_lazyq[g_lazyq_head % LAZYQ];
        g_lazyq_head++;
        if (l->ref > 1 && !l->real) {   /* still owned by the game and not yet set */
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            lazy_real(d, l);
            QueryPerformanceCounter(&b);
            InterlockedIncrement64(&g_lazy_trickled);
            InterlockedAdd64(&g_lazy_trickle_ticks, b.QuadPart - a.QuadPart);
            n++;
        }
        lz_release(l);
    }
}
static HRESULT create_timed(int k, int slot, void *self, const DWORD *fn, void **out) {
    if (g_lazy_on && fn && out) {
        const DWORD *p = fn; while (*p != 0x0000FFFF && p - fn < 65536) p++;
        UINT bytes = (UINT)((p - fn + 1) * 4);
        if (*p == 0x0000FFFF && bytes >= g_lazy_min) {
            Lazy *l = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *l);
            DWORD *code = l ? HeapAlloc(GetProcessHeap(), 0, bytes) : NULL;
            if (l && code) {
                memcpy(code, fn, bytes);
                l->vt = &k_lazy_vt; l->ref = 1; l->kind = k; l->dev = self; l->code = code; l->bytes = bytes;
                ((IUnknown *)self)->lpVtbl->AddRef((IUnknown *)self);
                *out = l;
                InterlockedIncrement64(&g_create_n[k]);
                InterlockedAdd64(&g_create_bytes[k], (LONG64)bytes);
                InterlockedIncrement64(&g_lazy_made);
                ShEnt *e = sh_slot(l, 1); if (e) { e->bytes = bytes; e->kind = (uint8_t)k; e->used = 0; }
                if (g_lazy_trickle > 0) {
                    LONG t = g_lazyq_tail;
                    if (t - g_lazyq_head < LAZYQ) { InterlockedIncrement(&l->ref); g_lazyq[t % LAZYQ] = l; MemoryBarrier(); g_lazyq_tail = t + 1; }
                }
                return D3D_OK;
            }
            if (l) HeapFree(GetProcessHeap(), 0, l);
        }
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = ((CreateShFn)g_fn[slot])(self, fn, out);
    QueryPerformanceCounter(&b);
    InterlockedIncrement64(&g_create_n[k]);
    InterlockedAdd64(&g_create_ticks[k], b.QuadPart - a.QuadPart);
    if (fn) {
        const DWORD *p = fn; while (*p != 0x0000FFFF && p - fn < 65536) p++;
        uint32_t bytes = (uint32_t)((p - fn + 1) * 4);
        InterlockedAdd64(&g_create_bytes[k], (LONG64)bytes);
        if (SUCCEEDED(hr) && out && *out) { ShEnt *e = sh_slot(*out, 1); if (e) { e->bytes = bytes; e->kind = (uint8_t)k; e->used = 0; } }
    }
    return hr;
}
static HRESULT WINAPI h_createvs(void *self, const DWORD *fn, void **out) { return create_timed(0, S_CREATEVS, self, fn, out); }
static HRESULT WINAPI h_createps(void *self, const DWORD *fn, void **out) { return create_timed(1, S_CREATEPS, self, fn, out); }

enum { S_CREATETEX = 23 };
typedef HRESULT (WINAPI *CreateTexFn)(void *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, void **, HANDLE *);
static HRESULT WINAPI h_createtex(void *d, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT f, D3DPOOL pool, void **out, HANDLE *sh) {
    HRESULT hr = FN(S_CREATETEX, CreateTexFn)(d, w, h, lv, use, f, pool, out, sh);
    if (g_ts && SUCCEEDED(hr) && out && *out) {
        UINT n = lv ? lv : (UINT)IDirect3DTexture9_GetLevelCount((IDirect3DTexture9 *)*out);   /* 0 asks for the full chain */
        photon_texstream_created(*out, w, h, n, use, f, pool);
    }
    return hr;
}

static const struct { int slot; void *hook; } k_hooks[] = {
    { S_CREATETEX, (void *)h_createtex },
    { S_CREATEVS, (void *)h_createvs }, { S_CREATEPS, (void *)h_createps },
    { S_CLEAR, (void *)h_clear }, { 42, (void *)h_endscene }, { S_DPUP, (void *)h_dpup }, { 84, (void *)h_dipup },
    { S_RESET, (void *)h_reset }, { S_SETRT, (void *)h_setrt }, { S_SETDS, (void *)h_setds }, { S_SETVP, (void *)h_setvp },
    { S_SETRS, (void *)h_setrs }, { S_SETTEX, (void *)h_settex }, { S_SETSAMP, (void *)h_setsamp }, { S_DP, (void *)h_dp }, { S_DIP, (void *)h_dip },
    { S_SETDECL, (void *)h_setdecl }, { S_SETFVF, (void *)h_setfvf }, { S_SETVS, (void *)h_setvs }, { S_VSCF, (void *)h_vscf },
    { S_VSCI, (void *)h_vsci }, { S_VSCB, (void *)h_vscb }, { S_SETSTREAM, (void *)h_stream }, { S_SETFREQ, (void *)h_freq },
    { S_SETIB, (void *)h_setib }, { S_SETPS, (void *)h_setps }, { S_PSCF, (void *)h_pscf }, { S_PSCI, (void *)h_psci }, { S_PSCB, (void *)h_pscb },
};

static int bind_device(void) {
    void *dev = photon_d3d_device();
    if (!dev) return 0;
    void **vt = *(void ***)dev;
    uintptr_t lo, hi;
    if (!photon_module_text(vt[0], &lo, &hi)) { photon_log("dmirror: device vtable slot 0 not in a module: off"); return -1; }
    size_t n = 0;
    while (n < NSLOTS_MAX && (uintptr_t)vt[n] >= lo && (uintptr_t)vt[n] < hi) n++;
    if (n <= S_PSCB) { photon_log("dmirror: device vtable has %zu slots: off", n); return -1; }
    void **copy = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(void *));
    if (!copy) return -1;
    copy[0] = vt[-1];
    memcpy(copy + 1, vt, n * sizeof(void *));
    memcpy(g_fn, vt, n * sizeof(void *));
    for (size_t i = 0; i < sizeof k_hooks / sizeof k_hooks[0]; i++) copy[1 + k_hooks[i].slot] = k_hooks[i].hook;
    g_real_vt = vt; g_vt = copy + 1; g_dev = dev;
    InterlockedExchangePointer((PVOID volatile *)dev, g_vt);
    photon_shadows_bind(dev);   /* T4.2 stage 1a (PHOTON_SHADOWS=1): the INTZ stand-in for the automatic depth-stencil */
    photon_log("dmirror: TEST PHOTON_BATCH=mirror: device %p, vtable copied (%zu slots), %zu state slots mirrored", dev, n,
               sizeof k_hooks / sizeof k_hooks[0]);
    return 1;
}

void photon_dmirror_setup(void) {
    char e[16];
    int set = GetEnvironmentVariableA("PHOTON_BATCH", e, sizeof e) > 0;
    if (!set) {   /* replay by default (2026-11-07) unless P6 is off (replay only serves P6's suppressed draws); PHOTON_BATCH=off (or 0) turns it off */
        char z[4];
        if (!(GetEnvironmentVariableA("PHOTON_P6", z, sizeof z) > 0 && z[0] == '0')) { strcpy(e, "replay"); set = 1; }
    }
    g_replay = set && strcmp(e, "replay") == 0;
    { char z[4]; g_nolazy = GetEnvironmentVariableA("PHOTON_TEST_NOLAZY", z, sizeof z) > 0 && z[0] == '1'; }
    { char z[16];
      /* on by default (2026-11-07: native load -1.4..-1.9 s on the GTX 1050, DXVK slightly faster, image and frame times unchanged);
       * PHOTON_LAZY_SHADERS=0 creates every shader at once as stock does */
      g_lazy_on = !(GetEnvironmentVariableA("PHOTON_LAZY_SHADERS", z, sizeof z) > 0 && z[0] == '0');
      if (GetEnvironmentVariableA("PHOTON_LAZY_MIN", z, sizeof z) > 0) g_lazy_min = (UINT)atoi(z);
      if (GetEnvironmentVariableA("PHOTON_LAZY_TRICKLE", z, sizeof z) > 0) g_lazy_trickle = atoi(z); }
    g_want = g_replay || (set && strcmp(e, "mirror") == 0);
    g_ts = g_want && photon_texstream_on();
}
void photon_dmirror_frame(void) {
    if (!g_want) return;
    g_frames++;
    if (g_bound && g_dev && *(void ***)g_dev != g_vt) { photon_log("dmirror: the device's vtable changed (reset or a new device): mirror off"); g_failed = 1; g_bound = 0; }
    if (g_bound || g_failed) return;
    int r = bind_device();
    if (r > 0) g_bound = 1; else if (r < 0) g_failed = 1;
}
void photon_dmirror_restore(void) {
    if (g_dev && *(void ***)g_dev == g_vt) InterlockedExchangePointer((PVOID volatile *)g_dev, g_real_vt);
}
void photon_dmirror_dump(const char *why) {
    if (!g_want) return;
    {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        photon_log("dmirror shader creates (%s): VS %lld in %.1f ms (%.1f KiB), PS %lld in %.1f ms (%.1f KiB)", why,
                   (long long)g_create_n[0], g_create_ticks[0] * 1e3 / (double)f.QuadPart, g_create_bytes[0] / 1024.0,
                   (long long)g_create_n[1], g_create_ticks[1] * 1e3 / (double)f.QuadPart, g_create_bytes[1] / 1024.0);
        long long n[2] = {0, 0}, u[2] = {0, 0}, nb[2] = {0, 0}, ub[2] = {0, 0}, big[2] = {0, 0}, bigu[2] = {0, 0};
        for (int i = 0; i < SHSET; i++) if (g_sh[i].p) {
            int k = g_sh[i].kind & 1;
            n[k]++; nb[k] += g_sh[i].bytes;
            if (g_sh[i].used) { u[k]++; ub[k] += g_sh[i].bytes; }
            if (g_sh[i].bytes > 4096) { big[k]++; if (g_sh[i].used) bigu[k]++; }
        }
        photon_log("dmirror shaders used (%s): VS %lld of %lld created (%.0f of %.0f KiB), PS %lld of %lld (%.0f of %.0f KiB); over 4 KiB: VS %lld of %lld, PS %lld of %lld used; set full %ld",
                   why, u[0], n[0], ub[0] / 1024.0, nb[0] / 1024.0, u[1], n[1], ub[1] / 1024.0, nb[1] / 1024.0, bigu[0], big[0], bigu[1], big[1], (long)g_sh_full);
        photon_log("dmirror lazy shaders (%s): %s, min %u bytes; stand-ins %lld, created on first use %lld in %.1f ms, failed %lld, freed %lld", why,
                   g_lazy_on ? "on" : "off", g_lazy_min, (long long)g_lazy_made, (long long)g_lazy_real, g_lazy_real_ticks * 1e3 / (double)f.QuadPart,
                   (long long)g_lazy_fail, (long long)g_lazy_freed);
        photon_log("dmirror lazy trickle (%s): %d per frame; created ahead of use %lld in %.1f ms; queue left %ld", why, g_lazy_trickle,
                   (long long)g_lazy_trickled, g_lazy_trickle_ticks * 1e3 / (double)f.QuadPart, (long)(g_lazyq_tail - g_lazyq_head));
        IO_COUNTERS io;
        if (GetProcessIoCounters(GetCurrentProcess(), &io))
            photon_log("dmirror process I/O (%s): reads %llu (%.1f MiB), writes %llu, other %llu", why, (unsigned long long)io.ReadOperationCount,
                       io.ReadTransferCount / 1048576.0, (unsigned long long)io.WriteOperationCount, (unsigned long long)io.OtherOperationCount);
    }
    long long f = g_frames ? g_frames : 1;
    photon_log("dmirror summary (%s): frames %lld; per frame: draws %.0f (in suppressed calls %.0f), set RS %.0f tex %.0f samp %.0f VS %.0f PS %.0f decl %.0f"
               " stream %.0f IB %.0f VSc %.0f PSc %.0f; suppressed calls with a draw %lld: c58..c60 = origin %lld, differs %lld; replay %d: groups"
               " recorded %lld (multi-draw, not replayed %lld; VS constants above c60 written, not replayed %lld; skinned VS combo, dropped %lld), replays %lld (same group as the last, state kept %lld), failed %lld", why, f,
               (double)g_draws / f, (double)g_draws_supp / f, (double)g_calls[S_SETRS] / f, (double)g_calls[S_SETTEX] / f, (double)g_calls[S_SETSAMP] / f,
               (double)g_calls[S_SETVS] / f, (double)g_calls[S_SETPS] / f, (double)g_calls[S_SETDECL] / f, (double)g_calls[S_SETSTREAM] / f,
               (double)g_calls[S_SETIB] / f, (double)g_calls[S_VSCF] / f, (double)g_calls[S_PSCF] / f, (long long)g_supp_first, (long long)g_c58_ok,
               (long long)g_c58_bad, g_replay, (long long)g_rec_ok, (long long)g_rec_multi, (long long)g_rec_bones, (long long)g_rec_dropped, (long long)g_replays, (long long)g_replay_fast, (long long)g_replay_fail);
    (void)t_seen_supp_draw;
}
