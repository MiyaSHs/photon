/* TEXSTREAM (TASKS): texture streaming, first version (PHOTON_TEXSTREAM=1, off otherwise). GMod keeps every mip of every texture in VRAM;
 * D3D9 managed textures can hold their top levels back with IDirect3DBaseTexture9::SetLOD. Large managed textures (512 px and up, with
 * mips) are tracked from the device's CreateTexture; one not bound for EVICT_MS gets SetLOD(DROP) (its top DROP levels leave VRAM, the
 * managed copy in system memory stays), and binding it again (dmirror's SetTexture hook) sets LOD 0 first, so the draw samples the full
 * texture (the driver uploads the levels at that point). A texture leaving the table on its last Release: the class's Release slot is
 * wrapped once (shared by every texture of that class). The sweep and every SetLOD run on the device thread (dmirror's EndScene and
 * SetTexture hooks), so it needs the mirror bound (it is by default). Summary in the log every 30 s. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <string.h>
#include "texstream.h"
#include "frames.h"
#include "log.h"

enum { NT = 16384, EVICT_MS = 10000, DROP = 2, MIN_SIZE = 512 };
typedef struct { IDirect3DTexture9 *t; DWORD last; uint32_t bytes; uint8_t lod, levels; } Ent;
static Ent g_e[NT];
static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_on = -1;
static void **g_tex_vt;
typedef ULONG (WINAPI *ReleaseFn)(IUnknown *);
static ReleaseFn g_release;
static volatile LONG64 g_tracked, g_evicted, g_restored, g_freed, g_bytes_out;

int photon_texstream_on(void) {
    if (g_on < 0) { char e[4]; g_on = GetEnvironmentVariableA("PHOTON_TEXSTREAM", e, sizeof e) > 0 && e[0] == '1'; if (g_on) photon_log("texstream: on (evict after %d ms unused, %d levels)", EVICT_MS, DROP); }
    return g_on;
}
static size_t slot_of(const void *t) { return (size_t)((((uintptr_t)t) >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (NT - 1); }
#define DEAD ((IDirect3DTexture9 *)(uintptr_t)1)   /* a freed entry: keeps the probe chain, reused by the next add */
static Ent *find(const void *t, int add) {
    size_t h = slot_of(t);
    Ent *dead = NULL;
    for (int k = 0; k < 64; k++, h = (h + 1) & (NT - 1)) {
        if (g_e[h].t == t) return &g_e[h];
        if (g_e[h].t == DEAD && !dead) dead = &g_e[h];
        if (!g_e[h].t) return add ? (dead ? dead : &g_e[h]) : NULL;
    }
    return add ? dead : NULL;
}

static ULONG WINAPI h_release(IUnknown *u) {   /* under the lock, so a texture created at the same address cannot slip in between */
    AcquireSRWLockExclusive(&g_lock);
    ULONG r = g_release(u);
    if (r == 0) {
        Ent *e = find(u, 0);
        if (e) {
            if (e->lod) g_bytes_out -= e->bytes - e->bytes / (1u << (2 * DROP));
            e->t = DEAD; g_freed++;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

static uint32_t bytes_of(UINT w, UINT h, UINT levels, D3DFORMAT f) {
    uint32_t bpp = f == D3DFMT_DXT1 ? 4 : (f == D3DFMT_DXT3 || f == D3DFMT_DXT5) ? 8 : 32;   /* bits per pixel */
    uint64_t s = 0;
    for (UINT l = 0; l < levels; l++) s += (uint64_t)(w >> l ? w >> l : 1) * (h >> l ? h >> l : 1) * bpp / 8;
    return s > 0xffffffffu ? 0xffffffffu : (uint32_t)s;
}

static volatile LONG64 g_seen_pool[4], g_seen_big[4];   /* textures created per pool (DEFAULT, MANAGED, SYSTEMMEM, SCRATCH); big: >= MIN_SIZE with mips */
void photon_texstream_created(void *tex, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool) {
    if ((unsigned)pool < 4) { g_seen_pool[pool]++; if (levels > DROP && (w >= MIN_SIZE || h >= MIN_SIZE) && !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) g_seen_big[pool]++; }
    if (!tex || pool != D3DPOOL_MANAGED || levels <= DROP || (w < MIN_SIZE && h < MIN_SIZE) || (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return;
    void **vt = *(void ***)tex;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_tex_vt) {   /* wrap the class's Release once */
        DWORD old;
        if (VirtualProtect(&vt[2], sizeof(void *), PAGE_READWRITE, &old)) {
            g_release = (ReleaseFn)vt[2];
            vt[2] = (void *)h_release;
            VirtualProtect(&vt[2], sizeof(void *), old, &old);
            g_tex_vt = vt;
        }
    }
    if (vt == g_tex_vt) {
        Ent *e = find(tex, 1);
        if (e) { e->t = tex; e->last = GetTickCount(); e->lod = 0; e->levels = (uint8_t)levels; e->bytes = bytes_of(w, h, levels, fmt); g_tracked++; }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void photon_texstream_bound(void *tex) {
    if ((uintptr_t)tex <= 1) return;
    AcquireSRWLockShared(&g_lock);
    Ent *e = find(tex, 0);
    int back = 0;
    if (e) { e->last = GetTickCount(); back = e->lod != 0; }
    ReleaseSRWLockShared(&g_lock);
    if (back) {
        AcquireSRWLockExclusive(&g_lock);
        e = find(tex, 0);
        if (e && e->lod) { IDirect3DTexture9_SetLOD(e->t, 0); e->lod = 0; g_restored++; InterlockedAdd64(&g_bytes_out, -(LONG64)(e->bytes - e->bytes / (1u << (2 * DROP)))); }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void photon_texstream_tick(void) {
    static DWORD s_sweep, s_log;
    DWORD now = GetTickCount();
    if (now - s_sweep >= 500) {   /* the sweep: unused textures lose their top levels */
        s_sweep = now;
        AcquireSRWLockExclusive(&g_lock);
        for (size_t i = 0; i < NT; i++) {
            Ent *e = &g_e[i];
            if ((uintptr_t)e->t <= 1 || e->lod || now - e->last < EVICT_MS) continue;
            IDirect3DTexture9_SetLOD(e->t, DROP);
            e->lod = DROP;
            g_evicted++;
            InterlockedAdd64(&g_bytes_out, (LONG64)(e->bytes - e->bytes / (1u << (2 * DROP))));
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (now - s_log >= 30000) {
        s_log = now;
        photon_log("texstream: frame %llu: tracked %lld, evictions %lld, restores %lld, freed %lld; now held back %.1f MB of VRAM", (unsigned long long)photon_frame_count(),
                   (long long)g_tracked, (long long)g_evicted, (long long)g_restored, (long long)g_freed, (double)g_bytes_out / 1048576.0);
        photon_log("texstream: textures created per pool (default / managed / sysmem / scratch): %lld / %lld / %lld / %lld; large with mips %lld / %lld / %lld / %lld",
                   (long long)g_seen_pool[0], (long long)g_seen_pool[1], (long long)g_seen_pool[2], (long long)g_seen_pool[3],
                   (long long)g_seen_big[0], (long long)g_seen_big[1], (long long)g_seen_big[2], (long long)g_seen_big[3]);
    }
}
