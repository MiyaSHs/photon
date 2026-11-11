/* P4a: which env_cubemap the engine bound for each of our draws (risk K16), checked against the engine's rule, and the pick table built in game
 * (lighting plan v2 P4, analysis/15a, analysis/17). Test only: everything here is off unless PHOTON_TEST_CUBEPROBE=1 (read once at load), and then it
 * only observes: no device call, no ConVar, no change to any draw.
 *
 * TEXTURE HANDLES. ShaderAPI texture handles are Texture_t pointers that stock recycles: CreateTextures (ShaderApi030 slot 243, FUN_180014620) takes freed
 * records first (FUN_180014490), so after a map change a handle may name another map's cubemap or any other texture. Every creation through slots 243 and
 * 143 (CreateTexture, which calls 243 through the real vtable, not our wrapper) records the handle's name, and every DeleteTexture (144) forgets it:
 * a cubemap name maps/<map>/c<x>_<y>_<z>[.hdr] (Mod_LoadCubemapSamples, 15a section 1.1) gives (map, origin), maps/<map>/cubemapdefault and
 * engine/defaultcubemap the defaults; any other name drops the handle from the table.
 *
 * THE BIND. VertexLitGeneric and its bump variant bind the envmap on s1 through the dynamic command buffer (SDK vertexlitgeneric_dx9_helper.cpp:1226,
 * CBCMD_BIND_SHADERAPI_TEXTURE_HANDLE 0x201), which ExecuteCommandBuffer (slot 94, FUN_180015800) executes by calling BindTexture through the real
 * object, so only a parse of the buffer sees it. Skin binds s8 directly (skin_dx9_helper.cpp:640: BindTexture, slot 155, through our wrapper). Both are
 * recorded per thread since the last RenderPass; the opcodes and sizes are stock's own (FUN_180015800), and an unknown opcode stops the parse (counted).
 *
 * PER DRAW, for draws of our pairs (vlg.c's snapshot marks): the class of the handle bound on the pair's envmap sampler (a sample of the current map,
 * of another map = aliasing, a default, unknown, none bound), counted by pair and route; the sample index; the pick table's entry at the lighting origin
 * (slot 128). Unique (pair, origin, handle) rows go to photon_cubeprobe.csv in the log directory for tools/p4/probecheck.py; a map change is logged with
 * the handles each map holds. The pick table itself: cubework.h, fed from the Present hook with the engine facts ambvol.c reads. */
#ifndef PHOTON_CUBEMAP_H
#define PHOTON_CUBEMAP_H
#include "wrap.h"

extern int photon_cube_on;
void photon_cube_setup(void);                 /* from load_stock() */
void photon_cube_install(PhotonWrapClass *cls);   /* ShaderApi030 slots 94, 143, 144, 155, 243 */
/* vlg.c's RenderPass hook, before stock's RenderPass: snap = the draw's PHOTON_VLG_SNAP_* flags, route/array as vlg.c's, origin = the last slot-128 origin */
/* With photon_cubemap_pick (live value pick) the visible sample A from the table replaces the engine's cubemap on s1 (s8 skin); with
 * photon_cubemap_blend (blend) the second sample B goes on s13 (s5 skin, only when lit: c32.x on) and c57/c58 get B's weight plane (P4b,
 * analysis/17b). Both through the real BindTexture (slot 155), so stock's sampler cache stays coherent. */
void photon_cube_draw(PhotonWrap *w, int snap, int route, int array, const float origin[3], int lit, int blend, int pick);
extern int photon_cube_blend_wanted, photon_cube_pick_wanted;   /* the settings at load: the hooks, the worker, the snapshot sampler */
void photon_cube_device_lost(void);           /* vlg.c: device Reset, snapshot clear */
void photon_cube_frame(void);                 /* from the Present hook */
void photon_cube_shutdown(const char *why);   /* IShaderDeviceMgr::Shutdown and Disconnect */
void photon_cube_dump(const char *why);
/* T4.5 stage 2b (p6.c, PHOTON_BATCH=replay): a replayed copy skips RenderPass, so the cubemap pick at its own origin is redone here.
 * take copies the last suppressed draw's pick context (what cube_feature knew: pair, eligibility, settings) into ctx and clears it;
 * learn notes which D3D texture each handle that draw bound turned out to be (tex = the recorded device textures, 20 entries);
 * replay fills, for the copy at origin, the samplers to change and their textures plus c57/c58 in psc. replay returns 0 when it cannot
 * tell (a handle whose texture is not learned yet, no handle for the sample): the engine must draw that copy (and learn records it). */
typedef struct { int ok, pair, eligible, blend, pick, lit; uint64_t ha, hb; } PhotonCubeCtx;
void photon_cube_replay_take(PhotonCubeCtx *ctx);
void photon_cube_learn(const PhotonCubeCtx *ctx, void *const tex[20]);
int photon_cube_replay(const PhotonCubeCtx *ctx, const float origin[3], int *ntex, int slot[2], void *tex[2], float psc[65][4]);
#endif
