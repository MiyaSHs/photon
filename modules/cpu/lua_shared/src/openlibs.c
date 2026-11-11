/* Photon CPU L3: GMod's luaL_openlibs (exported under that name, so libluajit's lib_init.o is not linked). The original opens
 * base, package, table, os, string, math, debug, bit, jit in that order (its table at 0x180098760), then makes _PRELOAD; no io and no
 * ffi. Its libraries are trimmed (the differential harness, analysis/15):
 *   base: no dofile, load, loadfile, loadstring, require; string.gfind and math.mod exist
 *   jit.version "LuaJIT 2.1.0-beta3", jit.version_num 20100
 *   os: clock, date, difftime, time only; package: config, loaded, loaders, preload, seeall; jit: no jit.security
 * The library definitions come from LuaJIT patches 0005 (jit.util) and 0006 (base, jit, package); os and package are rebuilt here at
 * GMod's table sizes. Checked with modules/cpu/luajit/test/cases/libtables.lua: identical pairs() order to GMod's DLL. */
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static const struct { const char *name; lua_CFunction open; } k_libs[] = {
    { "", luaopen_base }, { LUA_LOADLIBNAME, luaopen_package }, { LUA_TABLIBNAME, luaopen_table }, { LUA_OSLIBNAME, luaopen_os },
    { LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math }, { LUA_DBLIBNAME, luaopen_debug },
    { LUA_BITLIBNAME, luaopen_bit }, { LUA_JITLIBNAME, luaopen_jit },
};

/* replace library `lib` (in _G and package.loaded) by a new table of hash size `nhash` holding `fields` of the old one, in order */
static void rebuild(lua_State *L, const char *lib, int nhash, const char *const *fields, int n) {
    lua_getfield(L, LUA_GLOBALSINDEX, lib);
    int old = lua_gettop(L);
    lua_createtable(L, 0, nhash);
    for (int i = 0; i < n; i++) { lua_getfield(L, old, fields[i]); lua_setfield(L, -2, fields[i]); }
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_GLOBALSINDEX, lib);
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, lib);
    lua_settop(L, old - 1);
}

void luaL_openlibs(lua_State *L) {
    for (unsigned i = 0; i < sizeof k_libs / sizeof k_libs[0]; i++) {
        lua_pushcclosure(L, k_libs[i].open, 0);
        lua_pushstring(L, k_libs[i].name);
        lua_call(L, 1, 0);
    }
    luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOAD", 0);
    lua_settop(L, -2);
    /* os and package rebuilt as GMod builds them (its pairs() order comes from the table size and insertion order): os from a
     * 4-entry library in lib_os.c's order; package from luaL_register of seeall alone, then the fields in luaopen_package's order */
    static const char *const os_fields[] = { "clock", "date", "time", "difftime" };
    rebuild(L, LUA_OSLIBNAME, 4, os_fields, 4);
    static const char *const pkg_fields[] = { "seeall", "loaders", "config", "loaded", "preload" };
    rebuild(L, LUA_LOADLIBNAME, 1, pkg_fields, 5);
    /* GMod's _PRELOAD is empty (no table.new, table.clear, string.buffer, jit.profile) */
    lua_getfield(L, LUA_REGISTRYINDEX, "_PRELOAD");
    for (;;) {
        lua_pushnil(L);
        if (!lua_next(L, -2)) break;
        lua_settop(L, -2);
        lua_pushnil(L);
        lua_rawset(L, -3);
    }
    lua_settop(L, -2);
    lua_getfield(L, LUA_GLOBALSINDEX, LUA_STRLIBNAME);
    lua_getfield(L, -1, "gmatch");
    lua_setfield(L, -2, "gfind");
    lua_settop(L, -2);
    lua_getfield(L, LUA_GLOBALSINDEX, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "fmod");
    lua_setfield(L, -2, "mod");
    lua_settop(L, -2);
    lua_getfield(L, LUA_GLOBALSINDEX, LUA_JITLIBNAME);   /* (no jit.security: patch 0006) */
    lua_pushstring(L, "LuaJIT 2.1.0-beta3");   /* GMod's version string, which addons check */
    lua_setfield(L, -2, "version");
    /* and its version number: PAC3's luadata picks its bytecode-name table by it. Beta3's opcode numbers hold up to JMP; ours inserts
     * the bit operators before the function headers (FUNCF..), which instruction 1 onwards never contains in GMod-syntax code. */
    lua_pushnumber(L, 20100);
    lua_setfield(L, -2, "version_num");
    lua_settop(L, -2);
}
void photon_gmod_openlibs(lua_State *L) { luaL_openlibs(L); }
