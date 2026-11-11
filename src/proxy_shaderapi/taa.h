/* AA (TASKS): temporal anti-aliasing for the main 3D view: photon_taa (menu, live; PHOTON_TAA=1), off by default. The engine's perspective projections
 * get a sub-pixel jitter (Halton 2,3) from Present to the main view's end (the 3D sky and the main view: not the view model or the HUD,
 * drawn after), through ShaderApi030's MatrixMode / LoadMatrix (vlg.c hooks them); at the main view's end shadows.c resolves the frame
 * against its history (reprojected with the camera's motion: depth and last frame's unjittered matrix; neighbourhood-clamped). The same
 * jitter and motion are what an upscaler (FSR / DLSS / XeSS) takes later. */
#ifndef PHOTON_TAA_H
#define PHOTON_TAA_H
int photon_taa_on(void);
void photon_taa_matrix_mode(int mode);              /* vlg.c: the MatrixMode hook */
void photon_taa_load_matrix(float m[16]);           /* vlg.c: the LoadMatrix hook (edits a copy of the matrix in place) */
void photon_taa_present(void);                      /* proxy.c after Present: the next frame's jitter, on */
void photon_taa_main_view_end(void);                /* shadows.c: the jitter off (the main view is done) */
void photon_taa_jitter_ndc(float j[2]);
void photon_taa_view_size(unsigned w, unsigned h, unsigned tw, unsigned th);   /* shadows.c at the main view's end: its viewport and the target */             /* this frame's jitter in NDC units (0 while off) */
#endif
