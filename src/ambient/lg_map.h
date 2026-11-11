/* P5 (analysis/18_p5_lights.md): what the light grid needs from a map, read once on a worker. Pure library, no globals; an LgMap is
 * immutable after the load, so any number of threads may read it.
 *
 * Read (through the same AmbReader as amb_map_load_reader, so maps inside a GMA work the same way): the header, planes (lump 1), nodes (5),
 * leaves (10, version 0 or 1: contents and cluster), visibility (4: every cluster's PVS row decompressed into a bit matrix), model 0
 * (14: bounds and head node) and the worldlights, lump 54 if prefer_hdr and it is non-empty, else lump 15 (the engine's own choice,
 * FUN_180114320, 12a section (a)), version 0 (88 B records, as in memory) or 1 (100 B, shadow_cast_offset dropped, as the engine converts).
 * Limits as amb_map_load_reader's: every range checked against the file, sizes against fixed caps before anything is read.
 * Missing PVS (no lump 4, or more than LG_MAX_CLUSTERS clusters): every cluster sees every other (info.pvs = 0).
 * An LZMA lump: planes/nodes/leaves -> AMB_ERR_LZMA; visibility -> no PVS; worldlights -> no lights (info flags it). */
#ifndef PHOTON_LG_MAP_H
#define PHOTON_LG_MAP_H
#include <stddef.h>
#include <stdint.h>
#include "amb_bake.h"

enum { LG_MAX_CLUSTERS = 16384, LG_MAX_WORLDLIGHTS = 8192 };

/* dworldlight_t version 0 (public SDK bspfile.h), 88 bytes, the engine's in-memory layout */
typedef struct LgWorldLight {
    float origin[3], intensity[3], normal[3];
    int cluster, type, style;
    float stopdot, stopdot2, exponent, radius, constant_attn, linear_attn, quadratic_attn;
    int flags, texinfo, owner;
} LgWorldLight;
_Static_assert(sizeof(LgWorldLight) == 88, "dworldlight_t v0 is 88 bytes");
enum { LG_EMIT_SURFACE = 0, LG_EMIT_POINT = 1, LG_EMIT_SPOT = 2, LG_EMIT_SKYLIGHT = 3, LG_EMIT_QUAKE = 4, LG_EMIT_SKYAMBIENT = 5 };

typedef struct LgMap LgMap;
typedef struct LgMapInfo {
    int bsp_version, leaf_version, nplanes, nnodes, nleaves, nclusters, nlights;
    int light_lump;           /* 54 or 15, 0 none */
    int light_lump_version;
    int lights_lzma;          /* the light lump was LZMA: no lights */
    int pvs;                  /* 1: a PVS matrix is loaded */
    float world_min[3], world_max[3];
} LgMapInfo;

int lg_map_load_reader(const AmbReader *r, int prefer_hdr, LgMap **out, AmbReadStats *stats);
int lg_map_load(const void *bsp, size_t len, int prefer_hdr, LgMap **out);
void lg_map_free(LgMap *m);
void lg_map_info(const LgMap *m, LgMapInfo *info);
const LgWorldLight *lg_map_lights(const LgMap *m, int *n);
/* the leaf of p (model 0's tree; d >= 0 goes to the front child, as amb_point_leaf) and its cluster, -1 for none (solid, outside) */
int lg_point_leaf(const LgMap *m, const double p[3]);
int lg_point_cluster(const LgMap *m, const double p[3]);
/* 1 if cluster b is in a's PVS (a, b >= 0); 1 for any pair without PVS data; 0 if either is negative */
int lg_cluster_sees(const LgMap *m, int a, int b);
#endif
