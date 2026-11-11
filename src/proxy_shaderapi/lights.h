/* P5 (analysis/18_p5_lights.md): the light grid's engine reads, texture and per-draw weight. photon_lights (photon.cfg or PHOTON_LIGHTS), default 0:
 * with 0 at load nothing here runs (no hooks, no thread, no device call); live 0 later turns the grid weight of every draw to 0 (today's path).
 *
 * ENGINE READS, read only, each checked once against the validated build (byte patterns and FNV hashes of the code they depend on; the T1.5 guard does
 * not cover engine.dll), and any mismatch leaves the grid off with one log line:
 *   VEngineEffects001 slot 6 (CL_GetActiveDLights, FUN_180138820): the dlight in-use flag, active mask and array (32 x 0x48);
 *   slot 8 (GetElightByKey, FUN_180138be0): the elight in-use flag and array (64 x 0x48);
 *   R_SetNonAmbientLightingState (FUN_180103b30): d_lightstylevalue, its 1/264 scale, and that it converts through FUN_180107ea0, whose bytes are
 *     the ones lg_desc_from_worldlight follows (hash);
 *   FUN_1801dcdd0 (dlight -> worldlight, hash): its exponent table and minlight floor, read once.
 * Copied at Present on the draw thread (the engine mutates them on main: at worst a light is one frame off), with ambvol.c's engine facts (in game,
 * the level, the main view, HDR), and handed to lgwork.c.
 *
 * THE TEXTURE: raw D3D9, 128 x 528 A32B32G32R32F, D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC, LockRect flags 0 on the dirty rows at the first draw of a frame;
 * bound on s9 and s10 with point sampling. No stock snapshot of the scenes we measured enables s9 or s10, and stock's commit (FUN_180010eb0) touches
 * a sampler only when its snapshot's enable bit differs from the cached one: so the binding stays, and a stock draw that enables the sampler binds its
 * own texture with force. A draw of ours whose predecessor left stock's enable byte at 1 would lose the texture inside RenderPass: it gets weight 0
 * (counted as a flip). Released before a device Reset (IShaderUtil slot 15), recreated and fully re-uploaded at the next draw. */
#ifndef PHOTON_LIGHTS_H
#define PHOTON_LIGHTS_H
#include <stdio.h>

extern int photon_lg_on;                       /* photon_lights at load, and nothing has switched it off since */
void photon_lg_setup(void);                    /* from load_stock(), after photon_vol_setup() */
void photon_lg_frame(void);                    /* from the Present hook */
void photon_lg_first_draw(void);               /* from every RenderPass hook; acts on the first of a frame: texture, upload, binding */
/* The grid weight of a lit draw of ours on the engine route with volume weight f: f if the grid is uploaded and bound for this pair's sampler and
 * stock will not unbind it inside this draw, else 0. bump: the pair samples s10, else s9. */
float photon_lg_weight(const void *api_real, int bump, float f);
void photon_lg_constants(float c63[4]);        /* the uploaded grid's window and cell size */
/* P5b (analysis/18): the folded part of the grid's lights at a draw's lighting origin, 6 faces rgb (+X -X +Y -Y +Z -Z); zero if there is no grid */
void photon_lg_fold(const float origin[3], float out[6][3]);
int photon_lg_k(void);                         /* photon_cluster_k now (vlg.c's config) */
void photon_lg_release(void);                  /* IShaderUtil::ReleaseShaderObjects */
void photon_lg_restore(void);                  /* IShaderUtil::RestoreShaderObjects */
void photon_lg_shutdown(const char *why);      /* IShaderDeviceMgr::Shutdown and Disconnect */
void photon_lg_csv(FILE *f, int header);
void photon_lg_cal(const void *desc);          /* PHOTON_TEST_LIGHTCAL=1: an engine LightDesc_t (vlg.c's SetLight hook, draw thread) against the grid's records */
void photon_lg_dump(const char *why);
#endif
