/* P5 (analysis/18_p5_lights.md): the engine's own light conversions, so a grid light's shader record is what the engine would send for the
 * same light (acceptance e). Pure functions. Engine facts [confirmed decomp, engine.dll of build 26229091]:
 *   lg_desc_from_worldlight = FUN_180107ea0 (dworldlight_t -> LightDesc_t, used by R_SetNonAmbientLightingState FUN_180103b30, which then
 *     scales the colour by d_lightstylevalue[style] / 264 for style < 0x80);
 *   lg_worldlight_from_dlight = FUN_1801dcdd0 (dlight_t / elight -> dworldlight_t, ComputeDynamicLighting FUN_1801d8550);
 *   lg_record = shaderapi's SetLight cone clamps (FUN_18001e910) and the c39.. packing of vlg.c / photon_lightblock.h, with the grid's
 *     range window. */
#ifndef PHOTON_LG_LIGHT_H
#define PHOTON_LG_LIGHT_H
#include <stdint.h>
#include "lg_map.h"

/* LightDesc_t, public mathlib/lightdesc.h, 88 bytes (the same layout as vlg.c's LightDesc) */
typedef struct LgDesc {
    int type;                       /* 0 disable, 1 point, 2 directional, 3 spot */
    float color[3], pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi, thetadot, phidot;
    unsigned flags;
    float oo_dot, range_sq;
} LgDesc;
_Static_assert(sizeof(LgDesc) == 88, "LightDesc_t is 88 bytes");

/* dlight_t as the engine stores it (refs/sourcesdk-minimal public/dlight.h with double die), 0x48 bytes */
typedef struct LgDlight {
    int flags;
    float origin[3], radius;
    uint8_t r, g, b;
    int8_t exponent;
    double die;
    float decay, minlight;
    int key, style;
    float direction[3], inner, outer;
    int pad;
} LgDlight;
_Static_assert(sizeof(LgDlight) == 0x48, "dlight_t is 0x48 bytes");
enum { LG_DLIGHT_FLAGS_REJECT = 0xe };   /* NO_MODEL_ILLUMINATION and the displacement masks: the engine skips these for models */

/* 1 and *d for a light the engine turns into a LightDesc_t; 0 for quake and sky-ambient lights (and unknown types) */
int lg_desc_from_worldlight(const LgWorldLight *w, LgDesc *d);
/* exptab: the engine's TexLightToLinear table, 256 floats, entry e + 128 for exponent e (2^e / 255); minlight_floor: [0x1804f13e0] (1.0 in
 * the file). The cluster is left -1 (the caller's point lookup). */
void lg_worldlight_from_dlight(const LgDlight *dl, const float exptab[256], float minlight_floor, LgWorldLight *w);
/* exptab as the engine's file holds it (2^e / 255 in float) */
void lg_exptab_default(float exptab[256]);

/* The style scale of R_SetNonAmbientLightingState: value / 264 for style < 128, else 1 (values: d_lightstylevalue, ints) */
float lg_style_scale(const int *values, int style);
/* The shader record (photon_lightblock.h layout): colour = d->color * scale (the engine's order), the stock cone clamp, window radius r
 * (0 = none). Returns 0 for a type it cannot pack (0, unknown). */
int lg_record(const LgDesc *d, float scale, float r, float rec[4][4]);
/* The grid's window radius of a light: the radius when the worldlight has one (dlights always do), else where intensity / att falls to
 * cutoff (intensity = the colour vector's length, as the engine's own range), capped at LG_MAX_RADIUS. 0 for directional lights. */
enum { LG_MAX_RADIUS = 8192 };
double lg_window_radius(const LgWorldLight *w, double cutoff);
/* Importance of a light with this desc (colour at style scale 1) at distance dist: luminance * 1 / att * window, the ranking of a cell's
 * candidates. */
double lg_importance(const LgDesc *d, double r, double dist);
#endif
