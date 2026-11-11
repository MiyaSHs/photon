/* NANITE / PT-PROPS (analysis/44): a map's static props (the game lump "sprp"): each prop's model name, origin, angles, skin and fade
 * distances, and its model-to-world transform (Source's AngleMatrix: pitch, yaw, roll in degrees). Pure C. */
#ifndef PHOTON_BSP_PROPS_H
#define PHOTON_BSP_PROPS_H
#include <stddef.h>
#include <stdint.h>

typedef struct BspProp {
    char model[128];
    float origin[3], angles[3];
    int skin, solid, flags;
    float fade_min, fade_max;
} BspProp;

/* all static props; *out malloc'd (free it). 0 on success, also for a map without any. */
int bsp_props_read(const uint8_t *bsp, size_t size, BspProp **out, int *n, int *version);
/* world = m[i][0..2] . local + m[i][3] */
void bsp_prop_matrix(const BspProp *p, double m[3][4]);
#endif
