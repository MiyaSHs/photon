/* P5: see lg_light.h. Float arithmetic is written in the engine's own order (SSE scalar, no FMA: -std=c11 / -ffp-contract=off). */
#include <math.h>
#include <string.h>
#include "lg_light.h"
#include "../proxy_shaderapi/photon_lightblock.h"

#ifndef LG_LIGHT_MUTANT
#define LG_LIGHT_MUTANT 0   /* test only: 1 no att0 = 1 default, 2 the cone angles not doubled, 3 the minlight floor ignored */
#endif

static const float k_one = 1.0f, k_range_k = 33.333335876464844f, k_four = 4.0f, k_half = 0.5f;
static const unsigned k_range_none_bits = 0x5f7fffff;   /* the engine's "no range" (1.8446743e19) */

int lg_desc_from_worldlight(const LgWorldLight *w, LgDesc *d) {
    memset(d, 0, sizeof *d);
    float att0 = 0.0f, att1 = 0.0f, att2 = 0.0f;
    switch (w->type) {
    case LG_EMIT_SURFACE:
        d->type = 3;
        d->att2 = 1.0f;
        d->theta = 3.14159274f;
        d->phi = 3.14159274f;
        d->falloff = 1.0f;
        att2 = k_one;
        break;
    case LG_EMIT_POINT:
    case LG_EMIT_SPOT:
        d->type = w->type == LG_EMIT_POINT ? 1 : 3;
        att0 = d->att0 = w->constant_attn;
        att1 = d->att1 = w->linear_attn;
        att2 = d->att2 = w->quadratic_attn;
        if (w->type == LG_EMIT_SPOT) {
            double t = acos((double)w->stopdot), p = acos((double)w->stopdot2);
            d->theta = LG_LIGHT_MUTANT == 2 ? (float)t : (float)(t + t);
            d->phi = LG_LIGHT_MUTANT == 2 ? (float)p : (float)(p + p);
            d->thetadot = w->stopdot;
            d->phidot = w->stopdot2;
            d->falloff = w->exponent == 0.0f ? k_one : w->exponent;
        }
        break;
    case LG_EMIT_SKYLIGHT:
        d->type = 2;
        break;
    default:            /* quake (4), sky ambient (5): the engine returns false; anything else: never seen, refused here */
        return 0;
    }
    if (att0 == 0.0f && att1 == 0.0f && att2 == 0.0f && LG_LIGHT_MUTANT != 1) { d->att0 = 1.0f; att0 = k_one; }
    else if (att0 == 0.0f && att1 == 0.0f && att2 == 0.0f) att0 = 0.0f;
    for (int k = 0; k < 3; k++) { d->pos[k] = w->origin[k]; d->dir[k] = w->normal[k]; d->color[k] = w->intensity[k]; }
    float i0 = w->intensity[0], i1 = w->intensity[1], i2 = w->intensity[2];
    float mag = sqrtf(i1 * i1 + i0 * i0 + i2 * i2);
    if (w->radius == 0.0f) {
        if (att2 == 0.0f) {
            if (att1 == 0.0f) memcpy(&d->range, &k_range_none_bits, 4);
            else d->range = (mag * k_range_k - att0) / att1;
        } else {
            float disc = att1 * att1 - (att0 - mag * k_range_k) * att2 * k_four;
            if (disc >= 0.0f) {
                float r = (sqrtf(disc) - att1) * (k_half / att2);
                d->range = r < 0.0f ? 0.0f : r;
            } else memcpy(&d->range, &k_range_none_bits, 4);
        }
    } else d->range = w->radius;
    unsigned fl = 8;
    if (att0 != 0.0f) fl = 9;
    if (att1 != 0.0f) fl |= 2;
    if (att2 != 0.0f) fl |= 4;
    d->flags = fl;
    return 1;
}

void lg_exptab_default(float exptab[256]) {
    for (int e = -128; e < 128; e++) exptab[e + 128] = (float)(ldexp(1.0, e) / 255.0);
}

void lg_worldlight_from_dlight(const LgDlight *dl, const float exptab[256], float minlight_floor, LgWorldLight *w) {
    memset(w, 0, sizeof *w);
    for (int k = 0; k < 3; k++) w->origin[k] = dl->origin[k];
    w->type = LG_EMIT_POINT;
    float s = exptab[dl->exponent + 128];
    w->intensity[0] = (float)dl->r * s;
    w->intensity[1] = (float)dl->g * s;
    w->intensity[2] = (float)dl->b * s;
    w->style = dl->style;
    w->cluster = -1;
    double ml = (double)dl->minlight, fl = (double)minlight_floor;
    float minlight = LG_LIGHT_MUTANT == 3 ? dl->minlight : (float)(ml > fl ? ml : fl);   /* maxsd */
    float r = dl->radius > 0.1f ? dl->radius : 0.100000001f;                               /* maxss with [0x1803958ac] */
    w->radius = r;
    w->constant_attn = w->linear_attn = 0.0f;
    w->quadratic_attn = k_one / (minlight * r * r);
    if (dl->outer > 0.0f) {
        w->type = LG_EMIT_SPOT;
        for (int k = 0; k < 3; k++) w->normal[k] = dl->direction[k];
        w->stopdot = (float)cos((double)dl->inner * 0.017453292519943295);
        w->stopdot2 = (float)cos((double)dl->outer * 0.017453292519943295);
    }
}

float lg_style_scale(const int *values, int style) {
    if ((unsigned)style < 0x80u) return (float)values[style] * 0.0037878789f;
    return k_one;
}

/* shaderapi SetLight (FUN_18001e910 +0x183..0x1c5): phi <= pi, theta <= phi - 0.001; as vlg.c's stock_cone_clamp */
static void stock_cone_clamp(float theta, float phi, float *t, float *p) {
    if ((double)phi > 3.141592653589793) phi = 3.14159274f;
    float d = theta - phi;
    if ((double)d > -0.001) theta = (float)((double)phi - 0.001);
    *t = theta;
    *p = phi;
}

int lg_record(const LgDesc *d, float scale, float r, float rec[4][4]) {
    float type, ci = 0.0f, co = 0.0f;
    switch (d->type) {
    case 1: type = PHOTON_LB_TYPE_POINT; break;
    case 2: type = PHOTON_LB_TYPE_DIRECTIONAL; break;
    case 3: {
        type = PHOTON_LB_TYPE_SPOT;
        float t, p;
        stock_cone_clamp(d->theta, d->phi, &t, &p);
        ci = cosf(t * 0.5f);
        co = cosf(p * 0.5f);
        break;
    }
    default: memset(rec, 0, 16 * sizeof(float)); return 0;
    }
    float c[3] = { scale * d->color[0], scale * d->color[1], scale * d->color[2] };
    photon_lb_pack_light(rec, type, c, d->pos, d->dir, d->att0, d->att1, d->att2, r, d->falloff, ci, co);
    return 1;
}

double lg_window_radius(const LgWorldLight *w, double cutoff) {
    if (w->type != LG_EMIT_SURFACE && w->type != LG_EMIT_POINT && w->type != LG_EMIT_SPOT) return 0.0;
    double r;
    if (w->radius > 0.0f) r = w->radius;
    else {
        double c = w->constant_attn, l = w->linear_attn, q = w->quadratic_attn;
        if (w->type == LG_EMIT_SURFACE) { c = 0; l = 0; q = 1; }
        else if (c == 0 && l == 0 && q == 0) c = 1;
        double i = sqrt((double)w->intensity[0] * w->intensity[0] + (double)w->intensity[1] * w->intensity[1] + (double)w->intensity[2] * w->intensity[2]);
        double t = cutoff > 0 ? i / cutoff : 0;
        if (q > 0) {
            double disc = l * l - 4 * q * (c - t);
            r = disc >= 0 ? (sqrt(disc) - l) / (2 * q) : 0;
        } else if (l > 0) r = (t - c) / l;
        else r = t >= c ? LG_MAX_RADIUS : 0;
    }
    if (!(r > 0)) return 0.0;
    return r < LG_MAX_RADIUS ? r : LG_MAX_RADIUS;
}

double lg_importance(const LgDesc *d, double r, double dist) {
    if (d->type == 2 || !(r > 0) || dist >= r) return 0.0;
    double lum = 0.3 * d->color[0] + 0.59 * d->color[1] + 0.11 * d->color[2];
    double att = d->att0 + d->att1 * dist + d->att2 * dist * dist;
    if (!(att > 1e-6)) att = 1e-6;
    double x = dist * dist / (r * r), wnd = 1.0 - x * x;
    return lum / att * wnd * wnd;
}
