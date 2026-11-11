/* T4.2 Photon sun shadows (analysis/34). Stage 0: capability and depth-surface probe, logged once in game. Stage 1a (PHOTON_SHADOWS=1):
 * the game's automatic depth-stencil is replaced by an INTZ depth texture of the same size, so a later pass can read scene depth. */
#ifndef PHOTON_SHADOWS_H
#define PHOTON_SHADOWS_H
void photon_shadows_frame(void);       /* once a frame (Present thread): the stage-0 probe on the first frame with a device */
int photon_shadows_on(void);
int photon_shadows_drawing(void);   /* sun shadows drawn now (in-game setting or PHOTON_SHADOWS_DEBUG=3) */
void photon_shadows_bind(void *dev);   /* dmirror bound the device: note the automatic depth-stencil, create the INTZ stand-in */
void *photon_shadows_ds(void *sfc);    /* SetDepthStencilSurface: the surface to bind instead of sfc (the INTZ one for the automatic) */
void photon_shadows_pre_reset(void);   /* device Reset: release our default-pool objects first */
void photon_shadows_post_reset(void *dev, int ok);
void photon_shadows_dump(const char *why);
void photon_shadows_zclear(void *ds, void *rt0);   /* dmirror, before a depth clear on the device thread */
void photon_shadows_ds_switch(void *old_ds, void *new_ds, void *rt0);
void photon_shadows_present(void);
void photon_shadows_main_view_end(void);
void photon_shadows_model_draw(void);   /* vlg.c, draw thread, before a draw of Photon's model pairs: L3's world-only depth copy */
void photon_shadows_dme(const unsigned char *state, const unsigned char *info);
int photon_shadows_skinned(void);
int photon_shadows_skinned_lit(const float p[3]);   /* stage 2a for skinned casters: 0 = only shaded entities near p */   /* T4.2: skinned draws cast Photon shadows (PHOTON_SHADOWS_SKINNED, photon_shadow_characters) */   /* p6.c DrawModelExecute hook, main thread */
void photon_shadows_frame_3d_done(void);
int photon_shadows_caster_lit(const float m34[3][4]);   /* device thread: 0 when the world shades the caster from the sun */              /* p6.c, main thread, after the frame's 3D views */   /* p6.c, main thread: the main 3D view is about to pop */
void photon_shadows_draw(void *ds);   /* dmirror, before each DrawIndexedPrimitive with a depth surface bound */   /* the Present hook, before the real Present */
void photon_shadows_trace(const char *what, int idx, void *sfc, unsigned long flags);   /* dmirror, before SetDepthStencilSurface changes the binding */
int photon_shadows_scene_target(void);   /* the current targets are the main scene's (back buffer + scene depth) */
void *photon_shadows_motion_tex(void);   /* the motion-vector texture (IDirect3DTexture9, A16B16G16R16F, scene size; NULL: none yet) */
void *photon_shadows_model_nrm_tex(void);   /* PT-RT models: their world normals (A16B16G16R16F; NULL: none yet) */
int photon_shadows_sun_state(float rows[12][4], void **map);   /* fog: the sun map's constants this frame (0: none) */
void *photon_shadows_world_nrm_tex(void);   /* SP: the world's smooth normals (rgb * 0.5 + 0.5, a 1 where drawn) this frame, or NULL */
void *photon_shadows_world_pbr_tex(void);   /* PBR: the world's roughness / metalness target (A8R8G8B8; NULL: none yet) */
int photon_shadows_main_done(void);   /* the main view's end pass ran this frame (render thread): later draws are the view model / HUD */
#endif
