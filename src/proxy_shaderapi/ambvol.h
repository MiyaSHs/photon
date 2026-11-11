/* P3d, P3g, P3h: the ambient volume's device object and its life (analysis/15d sections 2 and 8). A raw D3D9 volume on sampler s15,
 * D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC (the game's D3D9Ex device rejects MANAGED, and plain DEFAULT cannot be locked), written with LockBox
 * flags 0 from the thread that issues stock's draws.
 *
 * Why s15, left bound: materialsystem's TakeSnapshot enables s15 in every snapshot for the linear-to-gamma table, so stock's
 * "enable flipped off: SetTexture(NULL)" inside RenderPass never happens on it, as it would on any other sampler. And nothing
 * samples that table by default: only CONVERT_TO_SRGB combos declare it, selected when HW slot 62 is true, which needs
 * r_shader_srgb 1 (default 0; the volume stays off then). LightmappedGeneric's flashlight noise on s15 is X360 only.
 *
 * Stock believes s15 holds its identity table and only binds when that belief changes: inside the first RenderPass after any
 * reset (device or ClearSnapshots), after our hook. So a draw that wants the volume checks stock's cache first, and if stock
 * is about to bind, that draw returns 0 and uses the engine cube instead of going black.
 *
 * Two modes, both off by default: no hooks, threads or device calls otherwise.
 *   photon_ambient 1 (photon.cfg or PHOTON_AMBIENT): the real volume. engine.dll is checked once against the validated code (byte patterns:
 *     the T1.5 guard does not cover it), and any mismatch leaves it off with one log line. From Present: is a map loaded, which one, HDR or LDR, where is the main view
 *     (ambwork.h does the rest on workers). At the first draw of a frame, on the draw thread: at most one job part goes up. A draw's weight and residual: vlg.c.
 *   PHOTON_TEST_VOL=1: the probe. It fills the atlas with a known pattern, then rewrites it slab by slab with a second one, so a pixel read through Photon_DebugVol shows that the
 *     texture, the sampler states, the slab uploads and the rebinding all reached the GPU. It wins if both are set. */
#ifndef PHOTON_AMBVOL_H
#define PHOTON_AMBVOL_H
#include <windows.h>
#include <stdio.h>
#include "wrap.h"

extern int photon_vol_on;                     /* PHOTON_TEST_VOL=1 */
extern int photon_amb_on;                     /* photon_ambient 1 at load, and nothing has switched it off since */
enum { PHOTON_AMB_OFF_CONFIG, PHOTON_AMB_OFF_ROUTE, PHOTON_AMB_OFF_GATE, PHOTON_AMB_OFF_RANGE, PHOTON_AMB_OFF_REST };
void photon_vol_setup(void);                  /* from load_stock(), after photon_vlg_setup() */
void photon_vol_install(PhotonWrapClass *cls);/* counters on ShaderShadow slot 31, ShaderApi slots 148 and 155 */
void photon_vol_note_iface(const char *name, void *real);   /* every wrapped interface: keeps the hardware config's stock object */
/* From the RenderPass hook, on the draw thread. Returns 1 if a draw that wants the volume will see it. */
int photon_vol_predraw(const void *api_real, int wants);
void photon_vol_postdraw(int wants);          /* after stock's RenderPass: was s15 replaced inside that draw? */
void photon_vol_frame(void);                  /* from the Present hook */
void photon_vol_release(void);                /* IShaderUtil::ReleaseShaderObjects: before the device Reset */
void photon_vol_restore(void);                /* IShaderUtil::RestoreShaderObjects */
void photon_vol_dump(const char *why);

/* photon_ambient 1 only. All on the draw thread, except shutdown. */
void photon_amb_first_draw(void);             /* from every RenderPass hook; acts on the first of a frame: re-creation, one upload */
/* The weight of a lit draw on the engine route, 0 if gated (*why = PHOTON_AMB_OFF_*). Above 0 the texture is bound and usable for this draw. */
float photon_amb_weight(const void *api_real, const float origin[3], int *why);
int photon_amb_covers(const float origin[3]);   /* P6: weight 1 at that point */
void photon_amb_constants(float c[4][4]);     /* c59..c62 */
void photon_amb_fold(const float origin[3], float f, float cube[6][4]);   /* c33..c38 for weight f */
void photon_amb_shutdown(const char *why);    /* IShaderDeviceMgr::Shutdown and Disconnect, before the meter thread stops; never DllMain */
void photon_amb_csv(FILE *f, int header);     /* photon_frames.csv columns */
/* PHOTON_TEST_AMBTRACE=1 only: photon_ambtrace.csv, from vol_apply */
int photon_amb_trace_on(void);
void photon_amb_trace(const float origin[3], float f, int why, const float before[6][4], const float after[6][4]);

void *photon_ms_factory_query(const char *name);   /* proxy.c: an interface from the factory materialsystem connected us with */
/* P4a (cubemap.c): from the Present hook. 1 and the facts (in game, the level's path, the main view, the map reader) once engine.dll passed the same
 * checks the volume needs; 0 not yet (try next frame); -1 for good, the build differs. Works with photon_ambient 0. */
struct AmbWorkFiles;
int photon_eng_facts(int *in_game, char *level, size_t n, double cam[3], const struct AmbWorkFiles **files);
int photon_eng_hdr(void);                     /* P5: HW slot 54 (HDR) now, 0 before the engine checks passed */
#endif
