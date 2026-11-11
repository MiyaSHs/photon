/* PHOTON_DIAG=1 diagnostics for the T3.6 runtime prerequisites (analysis/13_v1_prereqs.md). Off by default: with
 * PHOTON_DIAG unset (or not "1") no hook below is installed and the proxy behaves exactly as before.
 *   R15: logs IMaterialSystemHardwareConfig NumVertexShaderConstants/NumPixelShaderConstants (HWConfig012 slots
 *        22/23, called through our wrapper) and the raw CHardwareConfig fields the shaderapi constant shadow arrays
 *        are sized by ([stock+0x9b008]+0x530 etc.), at the first frame and whenever they change.
 *   R1:  typed hooks on ShaderApi030 slot 62 SetIntRenderingParameter (stock 0x18001e760) and slot 65
 *        GetIntRenderingParameter (0x180017ce0): a histogram per index (values, callers, threads), immediate log
 *        lines for any use of indices 12..19, and a per-frame audit of the raw array (+0x3930) that catches writers
 *        which bypass our wrapper.
 *   Order: a trace window of SetIntRenderingParameter(0, v) tokens and SetLightingOrigin (slot 128) draws, opened by
 *        a non-zero parm-0 token and closed by parm 0 = 0 (the harness "order=1" Lua test, render.SetLightingMode
 *        around DrawModel). Lines are "ORDER ..." in photon_proxy.log. */
#ifndef PHOTON_DIAG_H
#define PHOTON_DIAG_H
#include "wrap.h"
int photon_diag_enabled(void);
void photon_diag_install(PhotonWrapClass *cls);           /* from install_hooks (class creation) */
void photon_diag_note(const char *name, void *wrapper, void *real);   /* every wrapper handed out */
void photon_diag_frame(void);                             /* from the Present hook (after frame accounting) */
void photon_diag_dump(const char *why);                   /* histogram to the log (also every 120 frames, exit) */
#endif
