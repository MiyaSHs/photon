/* P6: skip the engine's per-model light setup for draws Photon lights itself (p6.c). */
#ifndef PHOTON_P6_H
#define PHOTON_P6_H
void photon_p6_setup(void);            /* PHOTON_P6=1 (test, stage 1) */
int photon_p6_wanted(void);            /* P6 on at load: the cubemap pick follows (the engine's BindLocalCubemap is skipped) */
void photon_p6_frame(void);            /* from Present: binds the engine's CModelRender once in game */
void photon_p6_restore(void);          /* DLL unload: stock vtable back */
void photon_p6_dump(const char *why);
enum { P6_PASS_LIT = 0, P6_PASS_STOCK = 1, P6_PASS_UNLIT = 2, P6_PASS_ROUTE = 3, P6_PASS_OFF = 4 };
void photon_p6_pass(int code, int vsidx);   /* vlg.c, per RenderPass on the draw thread: P6_PASS_LIT, or why not; the VS dynamic index */
int photon_p6_suppressed(void);        /* vlg.c: this RenderPass is inside a suppressed DrawModelExecute */
void photon_p6_origin(float o[3]);     /* its lighting origin (the engine set none) */
int photon_p6_is_skinned(void);       /* T4.2: the current suppressed call has had a skinned pass */
void photon_p6_skinned(void);          /* vlg.c: this suppressed pass's VS combo has SKINNING (more than one bone): its group is not replayed */
int photon_p6_call_draws_inc(void);    /* dmirror.c: the device draws in this suppressed call so far (then +1) */
int photon_p6_sun(void *desc);         /* vlg.c: 1 and the 88-byte directional LightDesc_t if this suppressed draw is sunlit (p6sun.c) */
#endif
