/* Photon CPU L3: CLuaInterface slots 90-132, the error record (CLuaError) the game's ILuaGameCallback::LuaError reads,
 * AdvancedLuaErrorReporter and GMOD_LoadBinaryModule, each from the decompile of GMod's x86-64 lua_shared. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "iface.h"
#include "msstr.h"

#define L_ (self->L)

extern void tier0_Error(const char *fmt, ...);
/* from the CLuaShared wrapper (shared.c) */
void *photon_orig_shared(void);    /* the original DLL's CLuaShared */
void *photon_orig_convars(void);   /* the original DLL's CLuaConVars (a static object) */
void **photon_filesystem(void);    /* IFileSystem*, as the original's g_pFullFileSystem holds it */

int li_Top(LuaIface *self);
void li_Pop(LuaIface *self, int n);
int li_PCall(LuaIface *self, int nargs, int nres, int errfunc);
int li_GetType(LuaIface *self, int i);
const char *li_GetString(LuaIface *self, int i, unsigned int *outlen);
void li_PushString(LuaIface *self, const char *s, unsigned int len);
void li_SetTable(LuaIface *self, int i);
void li_GetTable(LuaIface *self, int i);
void li_TypeError(LuaIface *self, const char *name, int i);
void *li_CreateObject(LuaIface *self);
void li_DestroyObject(LuaIface *self, void *obj);
void *li_Global(LuaIface *self);
int li_IsServer(LuaIface *self);
int li_IsClient(LuaIface *self);
int li_IsMenu(LuaIface *self);

/* ---- the error record ---- */
static MsLuaError g_err;
void *photon_lua_error_record = &g_err;
static char g_fmtbuf[0x1000];

static void err_init(void) {
    static int done;
    if (done) return;
    done = 1;
    msstr_init(&g_err.message);
    msstr_init(&g_err.side);
}
static void err_message(const char *s) { err_init(); msstr_assign(&g_err.message, s, strlen(s)); }
static void err_side(LuaIface *self) {
    const char *s = li_IsServer(self) ? "server" : li_IsMenu(self) ? "menu" : "client";
    err_init();
    msstr_assign(&g_err.side, s, strlen(s));
}
/* ReadStackIntoError (FUN_18000f450): the record's stack becomes Lua levels 1..16 (source, function name, current line) */
static void read_stack_into_error(lua_State *L) {
    err_init();
    for (MsStackEntry *e = g_err.first; e && e != g_err.last; e++) { msstr_free(&e->source); msstr_free(&e->function); }
    g_err.last = g_err.first;
    lua_Debug ar;
    for (int level = 1; level <= 16 && lua_getstack(L, level, &ar); level++) {
        lua_getinfo(L, "Slnu", &ar);
        if (g_err.last == g_err.end) {
            /* grown as MSVC's std::vector does, with its allocator: the game's LuaError handler owns this vector type and may grow,
             * shrink or free it, so it must look like one of its own */
            size_t n = (size_t)(g_err.last - g_err.first), oldcap = (size_t)(g_err.end - g_err.first), cap = oldcap + oldcap / 2;
            if (cap < n + 1) cap = n + 1;
            MsStackEntry *v = (MsStackEntry *)ms_allocate(cap * sizeof *v);
            memset(v, 0, cap * sizeof *v);
            if (n) memcpy(v, g_err.first, n * sizeof *v);
            if (g_err.first) ms_deallocate((char *)g_err.first, oldcap * sizeof *v);
            g_err.first = v; g_err.last = v + n; g_err.end = v + cap;
        }
        MsStackEntry *e = g_err.last++;
        msstr_init(&e->source); msstr_init(&e->function);
        msstr_assign(&e->source, ar.short_src, strlen(ar.short_src));
        const char *fn = ar.name ? ar.name : "";
        msstr_assign(&e->function, fn, strlen(fn));
        e->line = ar.currentline;
        e->pad = 0;
    }
}
static void report(LuaIface *self) {
    MsStackEntry *f = g_err.first, *l = g_err.last, *e = g_err.end;
    VCALL(self->callback, LCB_LUAERROR, void, &g_err);
    if (l3_tracing && (f != g_err.first || l != g_err.last || e != g_err.end))
        l3_trace("LuaError changed the record's stack: %p %p %p -> %p %p %p", (void *)f, (void *)l, (void *)e,
                 (void *)g_err.first, (void *)g_err.last, (void *)g_err.end);
}

/* the pcall error handler every protected call installs (exported, ?AdvancedLuaErrorReporter@@YAHPEAUlua_State@@@Z) */
int photon_AdvancedLuaErrorReporter(lua_State *L) {
    if (lua_isstring(L, 1)) {
        err_message(lua_tolstring(L, 1, NULL));
        read_stack_into_error(L);
        lua_pushstring(L, "");
        return 1;
    }
    read_stack_into_error(L);
    int t = lua_type(L, 1);
    if (t == LUA_TNIL) err_message("Nil was passed as the error message!");
    else {
        char b[64];
        snprintf(b, sizeof b, "Unknown type %d was given as Lua error!", t);
        err_message(b);
    }
    return 0;
}

/* GMOD_LoadBinaryModule (exported): LuaJIT's ll_loadfunc for "gmod13_open" (the handle kept in the registry under "LOADLIB: <path>",
 * closed by package's _LOADLIB __gc), then calls it with the path. -1: the library did not load; -2: no gmod13_open; 1: done.
 * As the original, the handle's userdata and any error message stay on the stack for the caller. */
static void push_win_error(lua_State *L) {
    DWORD e = GetLastError();
    char b[128];
    if (FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM, NULL, e, 0, b, sizeof b, NULL)) lua_pushstring(L, b);
    else lua_pushfstring(L, "system error %d\n", (int)e);
}
int GMOD_LoadBinaryModule(lua_State *L, const char *path) {
    lua_pushfstring(L, "LOADLIB: %s", path);
    lua_gettable(L, LUA_REGISTRYINDEX);
    void **h;
    if (lua_type(L, -1) == LUA_TNIL) {
        lua_settop(L, -2);
        h = (void **)lua_newuserdata(L, sizeof(void *));
        *h = NULL;
        luaL_getmetatable(L, "_LOADLIB");
        lua_setmetatable(L, -2);
        lua_pushfstring(L, "LOADLIB: %s", path);
        lua_pushvalue(L, -2);
        lua_settable(L, LUA_REGISTRYINDEX);
    } else h = (void **)lua_touserdata(L, -1);
    if (!*h) {
        HMODULE m = LoadLibraryExA(path, NULL, 0);
        if (!m) { push_win_error(L); return -1; }
        *h = m;
    }
    lua_CFunction f = (lua_CFunction)(void *)GetProcAddress((HMODULE)*h, "gmod13_open");
    if (!f) { push_win_error(L); return -2; }
    lua_pushcclosure(L, f, 0);
    lua_pushstring(L, path);
    lua_call(L, 1, 0);
    return 1;
}

/* ---- slots ---- */
int li_FindObjectOnTable(LuaIface *self, int table, int key) {                                   /* 90 (bool) */
    lua_pushvalue(L_, table);
    lua_pushvalue(L_, key);
    li_GetTable(self, -2);
    return lua_type(L_, -1) != LUA_TNIL;   /* leaves the table and the value, as the original */
}
void li_SetMemberFast(LuaIface *self, void *table, int key, int value) {                         /* 91 */
    lobj_push(table);
    lua_pushvalue(L_, key);
    lua_pushvalue(L_, value);
    li_SetTable(self, -3);
    li_Pop(self, 1);
}
int li_RunStringEx(LuaIface *self, const char *filename, const char *path, const char *code, int run, int print_errors,
                   int dont_push_errors, int no_returns);
int li_RunString(LuaIface *self, const char *filename, const char *path, const char *code, int run, int show_errors) {   /* 92 */
    return li_RunStringEx(self, filename, path, code, (uint8_t)run, (uint8_t)show_errors, 1, 1);
}
int li_IsEqual(LuaIface *self, void *a, void *b) {                                               /* 93 (bool) */
    if (!a || !b) return 0;
    if (VCALL(a, 0x18, int) != VCALL(b, 0x18, int)) return 0;   /* ILuaObject::GetType */
    lobj_push(a);
    lobj_push(b);
    int r = lua_equal(L_, -1, -2);
    li_Pop(self, 2);
    return r != 0;
}
void li_Error(LuaIface *self, const char *err) { luaL_error(L_, "%s", err ? err : ""); }        /* 94 */
const char *li_GetStringOrError(LuaIface *self, int i) {                                         /* 95 */
    const char *s = lua_tolstring(L_, i, NULL);
    if (s) return s;
    li_TypeError(self, "string", i);
    return "";
}
int li_FindAndRunScript(LuaIface *self, const char *filename, int run, int show_errors, const char *source, int no_returns);
int li_RunLuaModule(LuaIface *self, const char *name) {                                          /* 96 (bool) */
    char b[0x104];
    snprintf(b, sizeof b, "includes/modules/%s.lua", name);
    return li_FindAndRunScript(self, b, 1, 1, "!MODULE", 1);
}
void li_ErrorFromLua(LuaIface *self, const char *fmt, ...);
const char *li_GetCurrentLocation(LuaIface *self);
const char *li_GetPathID(LuaIface *self);
const char *li_GetPath(LuaIface *self);
void li_PushPath(LuaIface *self, const char *path);
void li_PopPath(LuaIface *self);
static void fix_slashes(char *s, char sep) { for (; *s; s++) if (*s == '/' || *s == '\\') *s = sep; }
/* CLuaShared::LoadFile(const std::string &path, const std::string &pathId, bool fromDatatable, bool fromFile) -> LuaFile* (the cache) */
static uint8_t *load_file(LuaIface *self, const char *path, const char *path_id) {
    MsStr p, id;
    msstr_init(&p); msstr_init(&id);
    msstr_assign(&p, path, strlen(path));
    msstr_assign(&id, path_id, strlen(path_id));
    void *sh = photon_orig_shared();
    uint8_t *f = ((uint8_t * (*)(void *, MsStr *, MsStr *, int, int))(*(void ***)sh)[0x38 / 8])(sh, &p, &id, li_IsClient(self) & 0xff, 1);
    msstr_free(&p); msstr_free(&id);
    return f;
}
/* LuaFile (garrysmod_common): +0x08 name, +0x28 source, +0x48 contents, +0x90 / +0x94 server / client run counts */
static int run_file(LuaIface *self, uint8_t *f, int run, int show_errors, int no_returns) {      /* FUN_180009c70 */
    if (li_IsClient(self)) (*(int *)(f + 0x94))++;
    if (li_IsServer(self)) (*(int *)(f + 0x90))++;
    const char *name = msstr_c((MsStr *)(f + 0x08));
    if (*name == '!') name++;
    return li_RunStringEx(self, name, "", msstr_c((MsStr *)(f + 0x48)), run, show_errors, 1, no_returns);
}
int li_FindAndRunScript(LuaIface *self, const char *filename, int run, int show_errors, const char *source, int no_returns) {   /* 97 (bool) */
    run = (uint8_t)run; show_errors = (uint8_t)show_errors; no_returns = (uint8_t)no_returns;
    char file[0x104], full[0x20c], *used;
    snprintf(file, sizeof file, "%s", filename);
    fix_slashes(file, '\\');
    size_t n = strlen(file);
    if (n < 4 || memcmp(file + n - 4, ".lua", 4)) {
        if (show_errors) li_ErrorFromLua(self, "Couldn't include file '%s' - Not a .lua file! (%s)\n", file, li_GetCurrentLocation(self));
        return 0;
    }
    uint8_t *f = NULL;
    if (li_GetPath(self)) {
        snprintf(full, sizeof full, "%s/%s", li_GetPath(self), file);
        fix_slashes(full, '\\');
        f = load_file(self, full, li_GetPathID(self));
        used = full;
    }
    if (!f) {
        f = load_file(self, file, li_GetPathID(self));
        used = file;
        if (!f) {
            if (show_errors) li_ErrorFromLua(self, "Couldn't include file '%s' - File not found or is empty (%s)\n", file, li_GetCurrentLocation(self));
            return 0;
        }
    }
    msstr_assign((MsStr *)(f + 0x28), source, strlen(source));
    li_PushPath(self, used);
    int r = run_file(self, f, run, show_errors, no_returns);
    li_PopPath(self);
    return r;
}
void li_SetPathID(LuaIface *self, const char *id) {                                              /* 98: V_strncpy into 32 bytes */
    strncpy(self->path_id, id, 31);
    self->path_id[31] = 0;
}
const char *li_GetPathID(LuaIface *self) { return self->path_id; }                               /* 99 */
void li_ErrorNoHalt(LuaIface *self, const char *fmt, ...) {                                      /* 100 */
    va_list ap; va_start(ap, fmt); vsnprintf(g_fmtbuf, sizeof g_fmtbuf, fmt, ap); va_end(ap);
    VCALL(self->callback, 0x10, void, g_fmtbuf, 1);   /* ILuaGameCallback::ErrorPrint(str, true) */
}
void li_Msg(LuaIface *self, const char *fmt, ...) {                                              /* 101 */
    va_list ap; va_start(ap, fmt); vsnprintf(g_fmtbuf, sizeof g_fmtbuf, fmt, ap); va_end(ap);
    VCALL(self->callback, 0x18, void, g_fmtbuf, 1);   /* ILuaGameCallback::Msg(str, true) */
}
void li_PushPath(LuaIface *self, const char *path) {                                             /* 102 */
    if (self->npaths >= self->cappaths) {
        int cap = self->cappaths ? self->cappaths * 2 : 8;
        self->paths = (char (*)[0x104])realloc(self->paths, (size_t)cap * 0x104);
        self->cappaths = cap;
    }
    char *d = self->paths[self->npaths++];
    void **fs = photon_filesystem();
    ((void (*)(void *, const char *, char *, int))(*(void ***)fs)[0x128 / 8])(fs, path, d, 0x104);   /* IFileSystem slot 37, as the original */
    fix_slashes(d, '\\');
    /* strip the file name, then a trailing separator */
    int i = (int)strlen(d) - 1;
    if (i > 0) {
        while (i > 0 && d[i] != '\\' && d[i] != '/') i--;
        d[i] = 0;
    }
    size_t n = strlen(d);
    if (n && (d[n - 1] == '\\' || d[n - 1] == '/')) d[n - 1] = 0;
}
void li_PopPath(LuaIface *self) {                                                                /* 103 */
    if (!self->npaths) tier0_Error("CLuaInterface::PopPath - Overpopped!\n");
    self->npaths--;
}
const char *li_GetPath(LuaIface *self) {                                                         /* 104 */
    if (!self->npaths) return NULL;
    const char *p = self->paths[self->npaths - 1];
    if (!*p) return NULL;
    if ((*p == '/' || *p == '\\') && !p[1]) return NULL;
    return p;
}
static uint8_t color_member(lua_State *L, int t, const char *k) {   /* ILuaObject::GetMemberInt(k, 255) */
    lua_getfield(L, t, k);
    int v = lua_type(L, -1) == LUA_TNUMBER ? (int)lua_tonumber(L, -1) : 255;
    lua_settop(L, -2);
    return (uint8_t)v;
}
uint8_t *li_GetColor(LuaIface *self, uint8_t *out, int i) {                                      /* 105: Color returned through a hidden pointer */
    if (lua_type(L_, i) == LUA_TTABLE) {
        int t = i < 0 ? lua_gettop(L_) + i + 1 : i;
        out[3] = color_member(L_, t, "a");
        out[2] = color_member(L_, t, "b");
        out[1] = color_member(L_, t, "g");
        out[0] = color_member(L_, t, "r");
        return out;
    }
    memset(out, 0xff, 4);
    return out;
}
void li_PushColor(LuaIface *self, uint32_t c) {                                                  /* 106: Color by value (4 bytes) */
    lua_createtable(L_, 0, 0);
    lua_pushnumber(L_, (double)(c & 0xff)); lua_setfield(L_, -2, "r");
    lua_pushnumber(L_, (double)((c >> 8) & 0xff)); lua_setfield(L_, -2, "g");
    lua_pushnumber(L_, (double)((c >> 16) & 0xff)); lua_setfield(L_, -2, "b");
    lua_pushnumber(L_, (double)(c >> 24)); lua_setfield(L_, -2, "a");
    lua_getfield(L_, LUA_REGISTRYINDEX, "Color");
    if (lua_type(L_, -1) == LUA_TTABLE) lua_setmetatable(L_, -2); else lua_settop(L_, -2);
}
int li_GetStack(LuaIface *self, int level, lua_Debug *ar) { return lua_getstack(L_, level, ar); }   /* 107 */
int li_GetInfo(LuaIface *self, const char *what, lua_Debug *ar) { return lua_getinfo(L_, what, ar); }   /* 108 */
const char *li_GetLocal(LuaIface *self, lua_Debug *ar, int n) { return lua_getlocal(L_, ar, n); }       /* 109 */
const char *li_GetUpvalue(LuaIface *self, int f, int n) { return lua_getupvalue(L_, f, n); }            /* 110 */
/* Bootil::String::Util::FindAndReplace over the whole script */
static char *replace_all(const char *s, const char *from, const char *to) {
    size_t nf = strlen(from), nt = strlen(to), cnt = 0;
    for (const char *p = s; (p = strstr(p, from)); p += nf) cnt++;
    char *r = (char *)malloc(strlen(s) + cnt * (nt > nf ? nt - nf : 0) + 1), *d = r;
    for (const char *p = s, *q; ; p = q + nf) {
        q = strstr(p, from);
        if (!q) { strcpy(d, p); break; }
        memcpy(d, p, (size_t)(q - p)); d += q - p;
        memcpy(d, to, nt); d += nt;
    }
    return r;
}
int li_CallFunctionProtected(LuaIface *self, int nargs, int nres, int show_error);
/* exported photon_lua_set_run_hook: Photon's CPU module (lua_load.c) gets each state that runs a script here, for its profiler */
static void (*g_run_hook)(lua_State *L);
void photon_lua_set_run_hook(void (*f)(lua_State *L)) { g_run_hook = f; }
int li_RunStringEx(LuaIface *self, const char *filename, const char *path, const char *code, int run, int print_errors,   /* 111 (bool) */
                   int dont_push_errors, int no_returns) {
    l3_check_entry(self, self->L, self->mainL, "RunStringEx");
    run = (uint8_t)run; print_errors = (uint8_t)print_errors; dont_push_errors = (uint8_t)dont_push_errors; no_returns = (uint8_t)no_returns;
    const char *refuse;
    if (!code || !*code) {
        li_Msg(self, "Not running script %s - it's too short.\n", filename);
        refuse = "Invalid script - or too short.";
    } else if (*code == 0x1b) {
        li_Msg(self, "Cannot run byte code! %x\n", 0x1b);
        refuse = "Cannot run byte code!";
    } else {
        if (g_run_hook) g_run_hook(L_);
        char *src = replace_all(code, "DEFINE_BASECLASS", "local BaseClass = baseclass.Get");
        char chunk[0x104];
        snprintf(chunk, sizeof chunk, "@%s%s", *path == '@' ? path + 1 : path, filename);
        if (l3_tracing) {
            char head[81];
            size_t k = 0;
            for (const char *c = src; *c && k < 80; c++) head[k++] = (*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c;
            head[k] = 0;
            l3_trace("run %s (%u bytes, top %d): %s", chunk, (unsigned)strlen(src), lua_gettop(L_), head);
        }
        read_stack_into_error(L_);
        int top = li_Top(self), ok;
        if (luaL_loadbufferx(L_, src, strlen(src), chunk, "t") == 0) {
            ok = 1;
            if (run) {
                if (!li_CallFunctionProtected(self, 0, LUA_MULTRET, 0)) {
                    if (print_errors) report(self);
                    if (!dont_push_errors) li_PushString(self, msstr_c(&g_err.message), 0);
                    ok = 0;
                }
                if (no_returns) li_Pop(self, li_Top(self) - top);
            }
        } else {
            if (print_errors) {
                err_message(li_GetString(self, -1, NULL));
                err_side(self);
                report(self);
            }
            if (dont_push_errors) li_Pop(self, 1);
            ok = 0;
        }
        free(src);
        if (l3_tracing) l3_trace("ran %s -> %d", chunk, ok);
        return ok;
    }
    if (!dont_push_errors) li_PushString(self, refuse, 0);
    return 0;
}
unsigned int li_GetDataString(LuaIface *self, int i, const char **out) {                         /* 112 */
    size_t n = 0;
    *out = NULL;
    const char *s = lua_tolstring(L_, i, &n);
    if (!s) return 0;
    *out = s;
    return (unsigned int)n;
}
void li_ErrorFromLua(LuaIface *self, const char *fmt, ...) {                                     /* 113 */
    read_stack_into_error(L_);
    va_list ap; va_start(ap, fmt); vsnprintf(g_fmtbuf, sizeof g_fmtbuf, fmt, ap); va_end(ap);
    size_t n = strlen(g_fmtbuf);
    if (n && g_fmtbuf[n - 1] == '\n') g_fmtbuf[n - 1] = 0;
    err_message(g_fmtbuf);
    err_side(self);
    report(self);
}
const char *li_GetCurrentLocation(LuaIface *self) {                                              /* 114 */
    static char loc[0x200];
    lua_Debug ar;
    snprintf(loc, sizeof loc, "<nowhere>");
    for (int level = 0; level <= 1; level++)
        if (lua_getstack(L_, level, &ar)) {
            lua_getinfo(L_, "Sl", &ar);
            if (ar.currentline > 0) snprintf(loc, sizeof loc, "%s (line %i)", ar.source, ar.currentline);
        }
    return loc;
}
void li_MsgColour(LuaIface *self, const void *col, const char *fmt, ...) {                       /* 115 */
    va_list ap; va_start(ap, fmt); vsnprintf(g_fmtbuf, sizeof g_fmtbuf, fmt, ap); va_end(ap);
    VCALL(self->callback, 0x20, void, g_fmtbuf, col);   /* ILuaGameCallback::MsgColour(str, col) */
}
void li_GetCurrentFile(LuaIface *self, MsStr *out) {                                             /* 116: std::string &out */
    msstr_assign(out, "", 0);
    lua_Debug ar;
    for (int level = 1; level <= 9; level++)
        if (lua_getstack(L_, level, &ar)) {
            lua_getinfo(L_, "Sl", &ar);
            if (ar.currentline > 0 && *ar.source == '@') { msstr_assign(out, ar.source + 1, strlen(ar.source + 1)); return; }
        }
}
/* Bootil::Buffer::Write (FUN_180015530): {vt, data +8, capacity +0x10, pos +0x18, written +0x20}; EnsureCapacity is the buffer's own
 * virtual (slot 2), so its memory stays the game's. The lua_dump writer FUN_180011130 always reports success. */
static int dump_writer(lua_State *L, const void *p, size_t n, void *buf) {
    (void)L;
    uint64_t *b = (uint64_t *)buf;
    if (n && (uint8_t)VCALL(buf, 0x10, uint64_t, b[3] + n)) {
        memcpy((uint8_t *)b[1] + b[3], p, n);
        b[3] += n;
        if (b[4] < b[3]) b[4] = b[3];
    }
    return 0;
}
int li_CompileString(LuaIface *self, void *dumper, const MsStr *code) {                          /* 117 (bool) */
    if (luaL_loadbufferx(L_, msstr_c(code), code->size, "", "t")) { li_Pop(self, 1); return 0; }
    if (lua_dump(L_, dump_writer, dumper)) { li_Pop(self, 1); return 0; }
    li_Pop(self, 1);
    return 1;
}
int li_CallFunctionProtected(LuaIface *self, int nargs, int nres, int show_error) {              /* 118 (bool) */
    int base = lua_gettop(L_) - nargs;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, self->errref);
    lua_insert(L_, base);
    int r = li_PCall(self, nargs, nres, base);
    lua_remove(L_, base);
    if (!r) return 1;
    int t = li_GetType(self, -1);
    if (t == LUA_TNIL) err_message("CallFunctionProtected was passed a nil as the error message!");
    else if (t == LUA_TSTRING) {
        const char *s = li_GetString(self, -1, NULL);
        if (*s) { err_message(s); read_stack_into_error(L_); }   /* "" is AdvancedLuaErrorReporter's: it already filled the record */
    } else {
        char b[80];
        snprintf(b, sizeof b, "Unhandled error type %d in CallFunctionProtected!", t);
        err_message(b);
    }
    li_Pop(self, 1);
    err_side(self);
    if ((uint8_t)show_error) report(self);
    return 0;
}
int li_Require(LuaIface *self, const char *name_in) {                                            /* 119 (bool) */
    char name[0x80];
    strncpy(name, name_in, 0x7f);
    name[0x7f] = 0;
    /* _MODULES[name] (the original goes through ILuaObjects; the table operations are the same) */
    lua_getfield(L_, LUA_GLOBALSINDEX, "_MODULES");
    if (lua_type(L_, -1) != LUA_TTABLE) {
        lua_settop(L_, -2);
        lua_createtable(L_, 0, 0);
        lua_pushvalue(L_, -1);
        lua_setfield(L_, LUA_GLOBALSINDEX, "_MODULES");
    }
    int mods = lua_gettop(L_);
    lua_getfield(L_, mods, name);
    int loaded = lua_type(L_, -1) != LUA_TNIL;
    lua_settop(L_, -2);
    if (loaded) { lua_remove(L_, mods); return 1; }
    void **fs = photon_filesystem();
    void *bfs = fs + 1;   /* IBaseFileSystem sub-object at +8; FileExists is its slot 10 */
#define FILE_EXISTS(p) ((uint8_t)((uint64_t (*)(void *, const char *, const char *))(*(void ***)bfs)[0x50 / 8])(bfs, p, "MOD"))
    char path[0x200];
    snprintf(path, sizeof path, "lua/includes/modules/%s.lua", name);
    if (!FILE_EXISTS(path)) {
        snprintf(path, sizeof path, "lua/bin/%s%s_win64.dll", li_IsClient(self) ? "gmcl_" : "gmsv_", name);
        if (FILE_EXISTS(path)) {
            if (strstr(path, "..") || strstr(path, ":") || strstr(path, "//") || strstr(path, "\\\\")) { lua_remove(L_, mods); return 0; }
            char full[0x200];
            /* IFileSystem::RelativePathToFullPath(path, "MOD", out, 0x200, FILTER_NONE, NULL) */
            const char *ok = ((const char *(*)(void *, const char *, const char *, char *, int, int, void *))(*(void ***)fs)[0x80 / 8])(
                fs, path, "MOD", full, 0x200, 0, NULL);
            if (ok) {
                lua_remove(L_, mods);   /* the module table lives in the ILuaObject in the original, not on the stack */
                int r = GMOD_LoadBinaryModule(L_, full);
                if (r == -1 || r == -2) {
                    const char *e = lua_tolstring(L_, 3, NULL);
                    char msg[512];
                    snprintf(msg, sizeof msg, "%s", e ? e : "");
                    size_t a = strspn(msg, " \n\t\r"), b = strlen(msg);
                    while (b > a && strchr(" \n\t\r", msg[b - 1])) b--;
                    msg[b] = 0;
                    luaL_error(L_, r == -1 ? "Couldn't load module library! (%s)" : "Couldn't find function in library! (%s)", msg + a, full);
                }
                if (r == 1) {
                    lua_getfield(L_, LUA_GLOBALSINDEX, "_MODULES");
                    lua_pushboolean(L_, 1);
                    lua_setfield(L_, -2, name);
                    lua_settop(L_, -2);
                    return 1;
                }
                li_Error(self, "Couldn't load module!");
                return 0;
            }
        }
    }
    lua_remove(L_, mods);
    if (!li_RunLuaModule(self, name)) { li_Error(self, "Module not found!"); return 0; }
    lua_getfield(L_, LUA_GLOBALSINDEX, "_MODULES");
    lua_pushboolean(L_, 1);
    lua_setfield(L_, -2, name);
    lua_settop(L_, -2);
    return 1;
#undef FILE_EXISTS
}
const char *li_GetActualTypeName(LuaIface *self, int i) { return lua_typename(L_, lua_type(L_, i)); }   /* 120 */
void li_PreCreateTable(LuaIface *self, int narr, int nrec) { lua_createtable(L_, narr, nrec); }        /* 121 */
void li_PushPooledString(LuaIface *self, int i) {                                                /* 122 */
    lobj_push(self->hooknames_obj);
    lua_rawgeti(L_, -1, i + 1);
    lua_remove(L_, -2);
}
static const char *const k_pooled[] = {
#include "hooknames.inc"
};
const char *li_GetPooledString(LuaIface *self, int i) { (void)self; return k_pooled[i]; }        /* 123 */
int li_AddThreadedCall(LuaIface *self, void *call) {                                             /* 124 */
    if (self->nthreaded == self->capthreaded) {
        self->capthreaded = self->capthreaded ? self->capthreaded * 2 : 8;
        self->threaded = (void **)realloc(self->threaded, self->capthreaded * sizeof(void *));
    }
    self->threaded[self->nthreaded++] = call;
    return 0;
}
/* Cycle's FUN_1800109d0: every call whose IsDone is true gets Done(this) and leaves the list */
void li_RunThreadedCalls(LuaIface *self) {
    if (!self->nthreaded) return;
    size_t n = 0;
    void **done = (void **)malloc(self->nthreaded * sizeof(void *));
    for (size_t i = 0; i < self->nthreaded; i++)
        if ((uint8_t)VCALL(self->threaded[i], LTC_ISDONE, uint64_t)) done[n++] = self->threaded[i];
    for (size_t k = 0; k < n; k++) {
        VCALL(done[k], LTC_DONE, void, self);
        size_t w = 0;
        for (size_t i = 0; i < self->nthreaded; i++) if (self->threaded[i] != done[k]) self->threaded[w++] = self->threaded[i];
        self->nthreaded = w;
    }
    free(done);
}
static void strncat_v(char *d, const char *s, size_t cap) {   /* V_strncat(d, s, cap, COPY_ALL_CHARACTERS) */
    size_t n = strlen(d);
    if (n + 1 >= cap) return;
    snprintf(d + n, cap - n, "%s", s);
}
void li_AppendStackTrace(LuaIface *self, char *out, unsigned int cap) {                          /* 125 */
    if (!self->L) { strncat_v(out, "   Lua State = NULL\n\n", cap); return; }
    lua_Debug ar;
    int level = 0;
    while (level < 17 && lua_getstack(L_, level, &ar)) {
        char line[0x100];
        lua_getinfo(L_, "Slnu", &ar);
        snprintf(line, sizeof line, "%d. %s - %s:%d\n", level, ar.name ? ar.name : "(null)", ar.short_src, ar.currentline);
        for (int i = 0; i < level + 2; i++) strncat_v(out, "  ", cap);
        strncat_v(out, line, cap);
        level++;
    }
    if (!level) strncat_v(out, "\t*Not in Lua call OR Lua has panicked*\n", cap);
    strncat_v(out, "\n", cap);
}
static int realm_flags(LuaIface *self, int flags) {
    flags &= 0xf3fffff3;
    if (li_IsServer(self)) flags |= 0x8000004;    /* FCVAR_GAMEDLL | FCVAR_LUA_SERVER */
    if (li_IsClient(self)) flags |= 0x14000008;   /* FCVAR_CLIENTDLL | FCVAR_LUA_CLIENT | ... */
    if (li_IsMenu(self)) flags &= ~0x80;
    return flags;
}
void *li_CreateConVar(LuaIface *self, const char *name, const char *def, const char *help, int flags) {   /* 126 */
    void *cv = photon_orig_convars();
    return VCALL(cv, 0x10, void *, name, def, help, realm_flags(self, flags));
}
void *li_CreateConCommand(LuaIface *self, const char *name, const char *help, int flags, void *cb, void *complete) {   /* 127 */
    int f = realm_flags(self, flags);
    if (li_IsServer(self)) f |= 0x40000000;
    void *cv = photon_orig_convars();
    return VCALL(cv, 0x18, void *, name, help, f, cb, complete);
}
const char *li_CheckStringOpt(LuaIface *self, int i, const char *def) { return luaL_optlstring(L_, i, def, NULL); }   /* 128 */
double li_CheckNumberOpt(LuaIface *self, int i, double def) { return luaL_optnumber(L_, i, def); }                 /* 129 */
int li_RegisterMetaTable(LuaIface *self, const char *name, void *tbl) {                          /* 130 */
    lua_getfield(L_, LUA_REGISTRYINDEX, name);
    if (li_GetType(self, -1) == LUA_TNIL) {
        li_Pop(self, 1);
        int id = self->next_type++;
        ((void (*)(void *, const char *, double))(*(void ***)tbl)[0x1a8 / 8])(tbl, "MetaID", (double)id);   /* ILuaObject::SetMember(k, double) */
        ((void (*)(void *, const char *, const char *))(*(void ***)tbl)[0x80 / 8])(tbl, "MetaName", name);  /* ILuaObject::SetMember(k, str) */
        lobj_push(tbl);
        lua_setfield(L_, LUA_REGISTRYINDEX, name);
        lua_getfield(L_, LUA_REGISTRYINDEX, name);
    } else if (li_GetType(self, -1) != LUA_TTABLE) {
        li_Pop(self, 1);
        return -1;
    }
    lua_getfield(L_, -1, "MetaID");
    double id = lua_tonumber(L_, -1);
    lua_settop(L_, -2);
    return (int)id;   /* as the original: the metatable stays on the stack */
}
void li_FreeInternals(LuaIface *self) {                                                          /* 131 (GMod addition) */
    free(self->threaded); self->threaded = NULL; self->nthreaded = self->capthreaded = 0;
    free(self->paths); self->paths = NULL; self->npaths = self->cappaths = 0;
}
void li_ErrorF(LuaIface *self, const char *fmt, ...) {                                           /* 132 (GMod addition) */
    if (!fmt) tier0_Error("Error called with nullptr message!");
    va_list ap; va_start(ap, fmt); vsnprintf(g_fmtbuf, sizeof g_fmtbuf, fmt, ap); va_end(ap);
    size_t n = strlen(g_fmtbuf);
    if (n && g_fmtbuf[n - 1] == '\n') g_fmtbuf[n - 1] = 0;
    luaL_error(L_, "%s", g_fmtbuf);
}
