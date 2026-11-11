/* The mock host's view of tests/proxyhost/fake_engine.c */
#ifndef PHOTON_FAKE_ENGINE_H
#define PHOTON_FAKE_ENGINE_H
#include <windows.h>
#include <stdint.h>
typedef struct FakeEngineCtl {
    void *fs;                  /* what VFileSystem022 must return */
    void (*add_file)(const char *name, const uint8_t *data, size_t size, int gma);
    /* A map inside a GMA: a real .gma in the temp directory holding a padding file, other (under maps/other_<n>.bsp) and data (under name), opened as
     * filesystem_stdio's FS_fopen does, with its objects laid out as in the shipped DLL. mode 0: as the mount leaves it; 1: the engine's offset for name is
     * other's; 2: unmounted (the shared handle's pointer NULL, as 0x18003ee60 leaves it). 0 on success. */
    int (*add_gma)(const char *name, const uint8_t *data, size_t size, const uint8_t *other, size_t other_size, int mode);
    void (*set_map)(int signon, const char *level);
    void (*set_origin)(float x, float y, float z);
    volatile LONG opens, closes;
    volatile LONG pack_used;   /* calls through a GMA file's shared handle: reads, seeks or sizes */
    volatile int largest_read;
    /* the engine's light averaging inputs (ambvol.c reads them, never writes): client time in seconds, r_lightinterp, r_lightaverage */
    void (*set_time)(double seconds);
    void (*set_lightavg)(float rate, int average);
} FakeEngineCtl;
typedef FakeEngineCtl *(*FakeEngineControlFn)(int break_what);
#endif
