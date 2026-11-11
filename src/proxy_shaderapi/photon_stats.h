/* Private stats interface of the Photon proxy, returned by CreateInterface("PhotonProxyStats001").
 * Used by tests and (later) by the metrics logger. Not a game interface. */
#ifndef PHOTON_STATS_H
#define PHOTON_STATS_H
#include "wrap.h"
#include "vlg.h"
#define PHOTON_STATS_VERSION "PhotonProxyStats001"
enum { PHOTON_MAX_CLASSES = 16 };
typedef struct PhotonStats {
    int version;                                  /* 3 (2 added the d3d_* members, 3 the vlg_* members) */
    int passthrough;                              /* 1 if wrapping is off (PHOTON_PASSTHROUGH=1 or build guard) */
    int build_ok;                                 /* 1 if the stock DLLs match the validated build (T1.5) */
    size_t nclasses;
    PhotonWrapClass *classes[PHOTON_MAX_CLASSES];  /* the wrapped interface classes */
    void *(*unwrap)(void *);
    void (*frame_end)(void);          /* the same accounting the Present hook runs (tests, external drivers) */
    void (*flush)(void);              /* write photon_slots.csv now */
    unsigned long long (*frames)(void);
    void (*diag_frame)(void);         /* PHOTON_DIAG=1: the per-frame diagnostics the Present hook runs (diag.h) */
    void (*diag_dump)(const char *);  /* PHOTON_DIAG=1: write the R1 histogram to the log now */
    /* version 2 (T3.4a2, d3dconst.h): stock's validated IDirect3DDevice9* (NULL if none), and
     * SetPixelShaderConstantF for c32..c223 on it (HRESULT; render thread only) */
    void *(*d3d_device)(void);
    long (*d3d_set_ps_const)(int start, const float *v, int count);
    /* version 3 (T3.4c, vlg.h): lighting v0 state and counters, snapshot marks, config re-read */
    const PhotonVlgStats *vlg;
    int (*vlg_snapshot_flags)(int id);
    int (*vlg_poll_config)(int force);
    void (*vlg_device_state_lost)(void);   /* S3: as IShaderUtil::RestoreShaderObjects does */
} PhotonStats;
#endif
