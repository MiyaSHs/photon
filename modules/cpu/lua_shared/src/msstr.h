/* Photon CPU L3: MSVC std::string and std::vector as the game DLLs (MSVC, x64) lay them out, for the few CLuaInterface methods that
 * take or fill them (CLuaError, GetCurrentFile, CLuaShared::LoadFile's arguments). The game's CRT is static with Source's memoverride,
 * so every allocation it may free goes through tier0's g_pMemAlloc; these helpers allocate the same way, including MSVC's 32-byte
 * aligned blocks for 4 KB and up (the original pointer stored just before the buffer). */
#ifndef PHOTON_MSSTR_H
#define PHOTON_MSSTR_H
#include <stddef.h>

typedef struct MsStr {
    union { char buf[16]; char *ptr; } u;
    size_t size;
    size_t cap;          /* 15 while the characters are inline */
} MsStr;

/* CLuaError (garrysmod_common): message, side ("client"/"server"/"menu"), and the Lua stack at the error */
typedef struct MsStackEntry { MsStr source; MsStr function; int line; int pad; } MsStackEntry;   /* 0x48 */
typedef struct MsLuaError {
    MsStr message;       /* +0x00 */
    MsStr side;          /* +0x20 */
    MsStackEntry *first, *last, *end;   /* +0x40 std::vector<StackEntry> */
} MsLuaError;

static inline const char *msstr_c(const MsStr *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
void msstr_init(MsStr *s);
void msstr_assign(MsStr *s, const char *p, size_t n);   /* replaces the contents; the buffer comes from g_pMemAlloc */
void msstr_free(MsStr *s);
/* MSVC std::allocator: n bytes through g_pMemAlloc (32-byte aligned with the raw pointer before the block from 4 KB up); the game
 * may free or reallocate what these return (a std::vector it was handed), so vectors use them too */
char *ms_allocate(size_t n);
void ms_deallocate(char *p, size_t n);
#endif
