/* PBR step 2 (analysis/41): per-material roughness and metalness. See mtl_pbr.h. */
#include "mtl_pbr.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* roughness priors calibrated on ambientCG CC0 materials (tools/pbr_score.py, analysis/41) */
static const struct { const char *name; const char *kw[20]; float rough; int metal; } k_types[] = {
    { "metal", { "metal", "steel", "chrome", "iron", "alum", "copper", "brass", "gold", "silver", "chainlink", "grate", "pipe", "vent", "canister", "combine", "solidmetal", "rail", "tinplate", NULL }, 0.35f, 1 },
    { "glass", { "glass", "window", "mirror", NULL }, 0.05f, 0 },
    { "polished", { "marble", "onyx", "terrazzo", "granite", "polished", "travertine", NULL }, 0.15f, 0 },
    { "water", { "water", "slime", "liquid", NULL }, 0.05f, 0 },
    { "plastic", { "plastic", "rubber", "tire", "computer", "monitor", NULL }, 0.30f, 0 },
    { "leather", { "leather", NULL }, 0.40f, 0 },
    { "woodfloor", { "woodfloor", "parquet", "hardwood", NULL }, 0.40f, 0 },
    { "wood", { "wood", "plank", "crate", "door", "furniture", "bark", NULL }, 0.60f, 0 },
    { "asphalt", { "asphalt", "road", "tarmac", NULL }, 0.65f, 0 },
    { "rock", { "rock", "cliff", "boulder", "gravel", NULL }, 0.55f, 0 },
    { "stone", { "concrete", "brick", "stone", "cement", "plaster", "tile", "wall", "floor", "cobble", "paving", "clay", NULL }, 0.60f, 0 },
    { "ground", { "dirt", "sand", "mud", "ground", "soil", "nature", NULL }, 0.65f, 0 },
    { "grass", { "grass", "moss", "leaf", "leaves", "foliage", NULL }, 0.50f, 0 },
    { "snow", { "snow", "ice", NULL }, 0.50f, 0 },
    { "cloth", { "cloth", "fabric", "carpet", "paper", "cardboard", "rug", NULL }, 0.75f, 0 },
    { "flesh", { "flesh", "skin", "body", "face", "alienflesh", "bloodyflesh", NULL }, 0.50f, 0 },
};
enum { NTYPES = sizeof k_types / sizeof k_types[0] };
const char *mtl_pbr_type_name(int t) { return t >= 0 && t < NTYPES ? k_types[t].name : "other"; }

/* the value after `key` in a lower-cased VMT (quoted or bare); 1 if found */
static int value(const char *txt, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = txt; (p = strstr(p, key)) != NULL; p += kl) {
        if (p > txt && !(p[-1] == '"' || isspace((unsigned char)p[-1]))) continue;
        const char *q = p + kl;
        if (*q == '"') q++;
        if (!(*q == ' ' || *q == '\t')) continue;
        while (*q == ' ' || *q == '\t') q++;
        size_t n = 0;
        if (*q == '"') { q++; while (q[n] && q[n] != '"' && q[n] != '\n' && n + 1 < cap) { out[n] = q[n]; n++; } }
        else if (*q == '[' || *q == '{') { while (q[n] && q[n] != ']' && q[n] != '}' && q[n] != '\n' && n + 1 < cap) { out[n] = q[n]; n++; } }
        else while (q[n] && !isspace((unsigned char)q[n]) && n + 1 < cap) { out[n] = q[n]; n++; }
        out[n] = 0;
        return 1;
    }
    return 0;
}
static int truthy(const char *txt, const char *key) { char v[16]; return value(txt, key, v, sizeof v) && (v[0] == '1' || !strcmp(v, "true")); }
static int classify(const char *text) {   /* a keyword that starts one of the text's tokens (split at / _ - . digits and spaces) */
    for (int t = 0; t < NTYPES; t++)
        for (int k = 0; k < 20 && k_types[t].kw[k]; k++) {
            size_t kl = strlen(k_types[t].kw[k]);
            for (const char *p = text; *p; p++) {
                int start = p == text || strchr("/_-. \t0123456789", p[-1]) != NULL;
                if (start && !strncmp(p, k_types[t].kw[k], kl)) return t;
            }
        }
    return -1;
}
/* the material's text, lower-cased, with a patch's include followed once (its own keys then win: they come first) */
static char *load_vmt(const char *material, MtlReadFn read, void *ctx, int depth) {
    char path[300];
    snprintf(path, sizeof path, "materials/%s.vmt", material);
    for (char *c = path; *c; c++) { *c = (char)tolower((unsigned char)*c); if (*c == '\\') *c = '/'; }
    size_t n = 0;
    uint8_t *b = read(ctx, path, &n);
    if (!b) return NULL;
    char *t = malloc(n + 1);
    if (!t) { free(b); return NULL; }
    for (size_t i = 0; i < n; i++) t[i] = (char)tolower(b[i]);
    t[n] = 0;
    free(b);
    char inc[256];
    if (depth == 0 && strstr(t, "patch") && value(t, "include", inc, sizeof inc)) {   /* "materials/x/y.vmt" */
        char *s = inc;
        if (!strncmp(s, "materials/", 10)) s += 10;
        char *dot = strstr(s, ".vmt"); if (dot) *dot = 0;
        char *base = load_vmt(s, read, ctx, 1);
        if (!base) { free(t); return NULL; }   /* a patch without its material: its keys (a cubemap's $envmap) say nothing alone */
        if (base) {
            size_t a = strlen(t), c = strlen(base);
            char *m = malloc(a + c + 2);
            if (m) { memcpy(m, t, a); m[a] = '\n'; memcpy(m + a + 1, base, c + 1); free(t); t = m; }
            free(base);
        }
    }
    return t;
}
void mtl_pbr_get(const char *material, MtlReadFn read, void *ctx, MtlPbr *o) {
    o->roughness = 0.5f; o->metalness = 0; o->type = -1; o->skip = 0;
    char name[256];
    snprintf(name, sizeof name, "%s", material);
    for (char *c = name; *c; c++) { *c = (char)tolower((unsigned char)*c); if (*c == '\\') *c = '/'; }
    static const char *k_skip_dir[] = { "decals/", "skybox/", "tools/", "dev/", "debug/", "effects/", "sprites/", "particle/", "vgui/", "hud/", "console/" };
    for (size_t i = 0; i < sizeof k_skip_dir / sizeof k_skip_dir[0]; i++) if (!strncmp(name, k_skip_dir[i], strlen(k_skip_dir[i]))) { o->skip = 1; return; }
    char *t = read ? load_vmt(name, read, ctx, 0) : NULL;
    char sp[64] = "", v[64];
    if (t) value(t, "$surfaceprop", sp, sizeof sp);
    int type = sp[0] ? classify(sp) : -1;
    if (type < 0) type = classify(name);
    else {   /* a generic stone surfaceprop ("concrete": Source has no marble one) yields to a more specific name: marble, steel, glass ...
              * (the PBR test map's marble floor lost its shine to "concrete" after the concrete fix) */
        int tn = classify(name);
        const char *gen = k_types[type].name, *spec = tn >= 0 ? k_types[tn].name : "";
        if (tn >= 0 && tn != type && !strcmp(gen, "stone") &&
            (!strcmp(spec, "polished") || !strcmp(spec, "metal") || !strcmp(spec, "glass") || !strcmp(spec, "woodfloor"))) type = tn;
    }
    o->type = type;
    float rough = type >= 0 ? k_types[type].rough : 0.5f;
    int metal_t = type >= 0 && k_types[type].metal;
    int envmap = 0;
    if (t) {
        if (value(t, "$phongexponent", v, sizeof v)) { float e = (float)atof(v); if (e <= 0) e = 20; rough = sqrtf(sqrtf(2.0f / (e + 2.0f))); }
        envmap = value(t, "$envmap", v, sizeof v) && strcmp(v, "0") != 0;
        if (envmap) {   /* reflective toward glossy: by the tint, and half of it where a mask says only parts reflect */
            float shine = 1;
            if (value(t, "$envmaptint", v, sizeof v)) {
                float a = 1, b = 1, c = 1;
                const char *p = v; while (*p == '[' || *p == '{' || *p == ' ') p++;
                if (sscanf(p, "%f %f %f", &a, &b, &c) >= 1) shine = (a + b + c) / 3.0f;
            }
            shine = shine < 0 ? 0 : shine > 1 ? 1 : shine;
            float m = value(t, "$envmapmask", v, sizeof v) || truthy(t, "$normalmapalphaenvmapmask") || truthy(t, "$basealphaenvmapmask") ? 0.5f : 1.0f;
            /* only materials that can be glossy go glossy: on concrete, stone, ground or cloth Source's cubemap is a faint sheen (and a
             * map's cubemap patch adds one to many floors), so those keep ~90% of their roughness (user, 2026-11-10: concrete "should
             * have near zero shine") */
            const char *tn = type >= 0 ? k_types[type].name : "other";
            int shiny = !strcmp(tn, "metal") || !strcmp(tn, "polished") || !strcmp(tn, "glass") || !strcmp(tn, "water") ||
                        !strcmp(tn, "plastic") || !strcmp(tn, "leather") || !strcmp(tn, "woodfloor");
            if (shiny) rough = rough * (1 - m * shine) + 0.15f * m * shine;
            else rough = rough * (1 - 0.1f * m * shine);
        }
        char sh[64] = "";
        sscanf(t, " \"%63[^\"]\"", sh);
        if (!sh[0]) sscanf(t, " %63s", sh);
        if (!strcmp(sh, "unlitgeneric") || !strcmp(sh, "sprite") || !strcmp(sh, "spritecard") || !strcmp(sh, "sky") || !strcmp(sh, "eyes") ||
            !strcmp(sh, "eyerefract") || !strcmp(sh, "teeth") || !strcmp(sh, "refract")) o->skip = 1;
        free(t);
    }
    o->metalness = metal_t ? (envmap ? 1.0f : 0.25f) : 0.0f;
    o->roughness = rough < 0.02f ? 0.02f : rough > 1 ? 1 : rough;
}
