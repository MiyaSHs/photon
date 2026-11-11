/* T4.5 stage 2a: D3D9 device state mirror (dmirror.c), PHOTON_BATCH=mirror (test only) */
#ifndef PHOTON_DMIRROR_H
#define PHOTON_DMIRROR_H
#include <stddef.h>
void photon_dmirror_setup(void);
void photon_dmirror_frame(void);     /* from Present: binds the device once */
void photon_dmirror_restore(void);   /* DLL unload */
void photon_dmirror_dump(const char *why);
/* stage 2b, PHOTON_BATCH=replay (p6.c): groups recorded at their first copy, later copies replayed in exact order */
int photon_dmirror_replay_on(void);
int photon_dmirror_replay_on_wanted(void);   /* PHOTON_BATCH=replay, bound or not */
size_t photon_dmirror_rec_size(void);
void photon_dmirror_record_begin(void *rec);
void photon_dmirror_record_end(void);
int photon_dmirror_rec_ok(const void *rec);
void photon_dmirror_rec_drop(void *rec);   /* p6.c: the recorded call turned out not replayable (a skinned VS combo) */
unsigned photon_dmirror_view_gen(void);
const float (*photon_dmirror_rec_psc(const void *rec))[4];   /* its recorded PS constants c0.. (c0..c64 are replayed) */
void *const *photon_dmirror_rec_tex(const void *rec);
void *const *photon_dmirror_cur_tex(void);   /* the mirror's textures now (cubemap.c learns from the last draw) */         /* its recorded device textures (20 entries: PS samplers 0..15, then vertex) */
/* psc: c0..c64 to use in place of the recorded ones (the copy relit, vlg.c), or NULL; ntex textures ttex on PS samplers tslot in place of the
 * recorded ones (the copy's cubemap pick, cubemap.c) */
int photon_dmirror_replay(const void *rec, const float m34[3][4], const float (*psc)[4], int ntex, const int *tslot, void *const *ttex);
/* T4.7a (cull.c): an occlusion query around the model's bounding box (see dmirror.c); 0 when it could not run */
struct IDirect3DQuery9;
int photon_dmirror_box_query(struct IDirect3DQuery9 *q, const float m34[3][4], const float mins[3], const float maxs[3], float grow, int visible,
                             const float (*vp)[4]);   /* vp: c8..c11 to use, NULL = the current ones */
void photon_dmirror_on_zclear(void (*cb)(void));   /* T4.7a: cb runs before every depth clear the engine issues */
void *photon_dmirror_ds(void);                       /* the depth-stencil surface set last */
const float (*photon_dmirror_viewproj(void))[4];     /* VS c8..c11 now (Source's cViewProj) */
int photon_dmirror_bound(void);
void photon_dmirror_flush(void);   /* the engine's view of the device back now (replay's lazy restore; Present calls it) */
void *photon_dmirror_rt0(void);
int photon_dmirror_viewport(unsigned v[4]);   /* RSCALE: the engine's viewport (x, y, width, height) now; 0 if not known */
int photon_dmirror_fullscreen(const DWORD *vs_code, const DWORD *ps_code, void **vs_cache, void **ps_cache, void *tex, const float (*psc)[4], int npsc,
                              int blend, int width, int height, void *target);   /* target: the colour surface to draw into (NULL: the bound one) */
long long photon_dmirror_fs_draws(void);
long long photon_dmirror_fs_unknown(void);
long long photon_dmirror_draw_count(void);
void photon_dmirror_fullscreen_tex2(void *tex);   /* the next photon_dmirror_fullscreen also binds tex on s1 (point, clamp) */
void photon_dmirror_fullscreen_depth(void *ds);   /* NANITE: the next full-screen pass depth-tests against and writes ds */
int photon_dmirror_cast_all(void *ds, void *rt, int size, const float (*light)[4], const DWORD *ps20, const DWORD *ps30, void **ps20_cache, void **ps30_cache);
typedef struct PhotonCastOpts { int vx, vy, size; const unsigned char *sel; int sun_gate, slot; } PhotonCastOpts;
int photon_dmirror_cast_opts(void *ds, void *rt, const PhotonCastOpts *o, const float (*light)[4], const DWORD *ps20, const DWORD *ps30, void **ps20_cache, void **ps30_cache);
long long photon_dmirror_cast_draws(void);
long long photon_dmirror_cast_cached(void);
void photon_dmirror_cast_invalidate(void);   /* the next photon_dmirror_cast_all draws even if nothing moved */   /* frames whose shadow map was kept (nothing moved) */
long long photon_dmirror_cast_skipped(void);
int photon_dmirror_ncast(void);
void photon_dmirror_cast_skin_stats(long long out[5]);
int photon_dmirror_caster_pos(int i, float p[3]);   /* -1: not a caster (kept for the motion vectors only) */
void photon_dmirror_fullscreen_texn(int slot, void *tex, int linear);   /* L3: a texture on s1..s6 of the next full-screen pass (blend 2: dst x 2 src) */   /* L1, device thread, i < photon_dmirror_ncast(): position; returns 1 if skinned */   /* skinned draws seen, left out (off), dynamic stream 0, flex streams zeroed, palette pool full */
/* AA: the main view's moving models into rt (A16B16G16R16F, w x h) depth-tested against ds: rg the offset to last frame's uv, b 1, a the
 * distance from last frame's eye. main_vp: the main view's c8..c11 (picks its draws); prev_rows: last frame's unjittered rows in the
 * target's uv space. Returns the models drawn, -1 if the pass could not run (rt not cleared) */
int photon_dmirror_velocity(void *rt, void *nrm, int all, void *ds, int w, int h, const float (*main_vp)[4], const float (*prev_rows)[4], const float prev_eye[3], int have_prev);
/* nrm (optional, A16B16G16R16F, MRT 1): the models' world normals (rgb * 0.5 + 0.5, a 1); all: every main-view model, not only those that moved */
void photon_dmirror_velocity_release(void);
/* UPSCALE: cb replaces the engine's upscale draw (the first full-size viewport on rt after this call, its next draw); NULL disarms */
void photon_dmirror_upscale_arm(int (*cb)(void), void *rt, unsigned w, unsigned h);
long long photon_dmirror_upscale_count(void);
#include <stdint.h>
void photon_dmirror_set_pbr_atlas(void *ctx, const uint8_t *rmh, const uint8_t *nrm);   /* PBR packs: the map's atlases (NULL: none) */
void photon_dmirror_set_world(const float *tri, const float *pbr, const float *ex, unsigned ntri);   /* pbr: per triangle roughness, metalness (NULL: none) */   /* PT-RT: the world's triangles (18 floats each) in the sun maps (NULL: none) */
/* PT-RT: Nanite's props in the sun maps too: 15 floats a triangle (position, uv per corner) in ranges; a range with data is an
 * alpha-tested material (its base texture: format 1..3 BC1/2/3, 4 RGBA8; mips from w x h; key: the same texture across scenes) */
typedef struct { unsigned first, count; const unsigned char *data; int format, w, h, mips; const size_t *mip_ofs; int key; } PhotonCastRange;
void photon_dmirror_cast_cut_leaves(int on);   /* PT-RT: alpha-tested casters cut by their base texture's alpha */
void photon_dmirror_set_props(const float *tri, unsigned ntri, const PhotonCastRange *r, int nr);
int photon_dmirror_world_normals(void *rt, void *rt_pbr, void *depth_tex, const float (*vp)[4], const float (*iv)[4], const float eye[3], int w, int h);   /* PT-RT: their smooth normals from the main view */
#endif
