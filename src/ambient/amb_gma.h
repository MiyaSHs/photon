/* The index of a GMA (Garry's Mod addon archive), read through an AmbReader (amb_bake.h), so the proxy can check where a file of a mounted
 * GMA lies before it reads it with a handle of its own (src/proxy_shaderapi/ambvol.c). Pure library, no globals; the file is untrusted
 * Workshop data, so every field is bounded and every read lies inside r->size.
 *
 * Format, as Facepunch's open-source gmad writes it and filesystem_stdio's mount reads it (0x18004a410: "GMAD", required content only
 * when the version is above 1, 0x18004a74a):
 *   "GMAD", u8 version, u64 steamid, u64 timestamp, [version > 1: c-strings up to an empty one], c-string name, description, author,
 *   i32 addon version, then entries { u32 number (0 ends the list), c-string path, i64 size, u32 crc }, then every entry's bytes in
 *   list order, then an optional u32 crc. An entry's offset is the end of the list plus the sizes before it, absolute in the file. */
#ifndef PHOTON_AMB_GMA_H
#define PHOTON_AMB_GMA_H
#include "amb_bake.h"

enum { AMB_GMA_ABSENT = 1 };   /* a well-formed index without that path */
/* Bounds on the index; past them the GMA is refused. gm_fork's index is 52 KB for 917 entries. */
enum { AMB_GMA_MAX_ENTRIES = 1 << 20, AMB_GMA_MAX_PATH = 1024, AMB_GMA_MAX_STRING = 1 << 20, AMB_GMA_MAX_REQUIRED = 4096, AMB_GMA_MAX_INDEX = 64 << 20 };

typedef struct AmbGmaInfo {
    int version, entries;
    long long data_start, data_end;   /* the entries' bytes: [data_start, data_end), inside the file */
    long long index_bytes_read;
} AmbGmaInfo;

/* Looks up path (compared without case, '\\' as '/') in the index. want_ofs >= 0: the entry with that path at exactly that offset (a GMA
 * may list a path twice); want_ofs < 0: the first one. AMB_OK with *ofs and *size (the entry lies inside the file), AMB_GMA_ABSENT,
 * or AMB_ERR_FORMAT, _READ, _CANCELLED, _NOMEM, _ARG. info may be NULL; on AMB_OK and AMB_GMA_ABSENT it describes the whole index. */
int amb_gma_find(const AmbReader *r, const char *path, long long want_ofs, long long *ofs, long long *size, AmbGmaInfo *info);

/* A reader of bytes [base, base + size) of another reader: the BSP inside a GMA. AMB_ERR_ARG if that range is not inside parent. */
typedef struct AmbSubReader { const AmbReader *parent; long long base; } AmbSubReader;
int amb_sub_reader(AmbReader *out, AmbSubReader *ctx, const AmbReader *parent, long long base, long long size);
#endif
