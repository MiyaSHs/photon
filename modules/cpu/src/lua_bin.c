/* Photon CPU DYN: the binary Lua module (gmsv_photon_cpu_win64.dll / gmcl_photon_cpu_win64.dll, the same file under the two names GMod
 * looks for in garrysmod/lua/bin), so Lua can read what photon_cpu.dll decided: require("photon_cpu") gives a global table photon_cpu
 * with Loaded(), SetViewportScale(scale, upscale) (the gpu:renderscale steps; mat_viewport* are blocked from Lua), DynMask() (the steps the frame-rate controller has engaged, bit i = step i) and DynSteps() (their names, 1-based).
 * The Lua API comes from the loaded lua_shared.dll by name (GMod's own or Photon's), as binary modules commonly do; photon_cpu.dll is
 * found already loaded (the proxy loads it), never loaded from here. Without it, Loaded() is false and DynMask() 0. */
#include <windows.h>
#include <stddef.h>
#include <string.h>

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State *L);
#define LUA_GLOBALSINDEX (-10002)

static void (*p_createtable)(lua_State *, int, int);
static void (*p_pushcclosure)(lua_State *, lua_CFunction, int);
static void (*p_setfield)(lua_State *, int, const char *);
static void (*p_pushnumber)(lua_State *, double);
static void (*p_pushboolean)(lua_State *, int);
static void (*p_pushstring)(lua_State *, const char *);
static void (*p_rawseti)(lua_State *, int, int);

static HMODULE cpu(void) { return GetModuleHandleA("photon_cpu.dll"); }

static int l_loaded(lua_State *L) { p_pushboolean(L, cpu() != NULL); return 1; }
static int l_mask(lua_State *L) {
    HMODULE m = cpu();
    int (*f)(void) = m ? (int (*)(void))(void *)GetProcAddress(m, "photon_cpu_dyn_mask") : NULL;
    p_pushnumber(L, f ? (double)f() : 0.0);
    return 1;
}
static int l_steps(lua_State *L) {
    HMODULE m = cpu();
    int (*f)(const char *const **) = m ? (int (*)(const char *const **))(void *)GetProcAddress(m, "photon_cpu_dyn_steps") : NULL;
    p_createtable(L, 8, 0);
    if (f) {
        const char *const *names;
        int n = f(&names);
        for (int i = 0; i < n; i++) { p_pushstring(L, names[i]); p_rawseti(L, -2, i + 1); }
    }
    return 1;
}

static double (*p_tonumber)(lua_State *, int);
static int l_viewport(lua_State *L) {   /* SetViewportScale(scale, upscale) -> true when it took */
    HMODULE m = cpu();
    int (*f)(float, float) = m ? (int (*)(float, float))(void *)GetProcAddress(m, "photon_cpu_set_viewport") : NULL;
    p_pushboolean(L, f ? f((float)p_tonumber(L, 1), (float)p_tonumber(L, 2)) : 0);
    return 1;
}

static int l_counts(lua_State *L) {   /* ReportCounts(loot, debris, corpses): what the DYN steps could act on */
    HMODULE m = cpu();
    void (*f)(int, int, int) = m ? (void (*)(int, int, int))(void *)GetProcAddress(m, "photon_cpu_dyn_counts") : NULL;
    if (f) f((int)p_tonumber(L, 1), (int)p_tonumber(L, 2), (int)p_tonumber(L, 3));
    return 0;
}

static int l_awake(lua_State *L) {   /* ReportAwake(total, loot, debris, c64, c32, c16): awake physics objects, for DYN's savings */
    HMODULE m = cpu();
    void (*f)(int, int, int, int, int, int) = m ? (void (*)(int, int, int, int, int, int))(void *)GetProcAddress(m, "photon_cpu_dyn_awake") : NULL;
    if (f) f((int)p_tonumber(L, 1), (int)p_tonumber(L, 2), (int)p_tonumber(L, 3), (int)p_tonumber(L, 4), (int)p_tonumber(L, 5), (int)p_tonumber(L, 6));
    return 0;
}

static int (*p_gettop)(lua_State *);
static int (*p_type)(lua_State *, int);
static const char *(*p_tolstring)(lua_State *, int, size_t *);
static size_t (*p_objlen)(lua_State *, int);
static void (*p_rawgeti)(lua_State *, int, int);
static void (*p_settop)(lua_State *, int);
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5
/* Push(channel, {floats...}) or Push(channel, f1, f2, ...): a named float array for the proxy's shader-pack channels (host v8),
 * latest per frame wins; at most 4096 floats. Returns true when the proxy took it. */
static int l_push(lua_State *L) {
    static float buf[4096];
    HMODULE m = cpu();
    int (*f)(const char *, const float *, int) = m ? (int (*)(const char *, const float *, int))(void *)GetProcAddress(m, "photon_cpu_push") : NULL;
    if (!f || p_type(L, 1) != LUA_TSTRING) { p_pushboolean(L, 0); return 1; }
    const char *ch = p_tolstring(L, 1, NULL);
    int n = 0;
    if (p_type(L, 2) == LUA_TTABLE) {
        int len = (int)p_objlen(L, 2);
        for (int i = 1; i <= len && n < 4096; i++) {
            p_rawgeti(L, 2, i);
            buf[n++] = p_type(L, -1) == LUA_TNUMBER ? (float)p_tonumber(L, -1) : 0.0f;
            p_settop(L, -2);
        }
    } else {
        int top = p_gettop(L);
        for (int i = 2; i <= top && n < 4096; i++) buf[n++] = (float)p_tonumber(L, i);
    }
    p_pushboolean(L, f(ch, buf, n));
    return 1;
}

/* Setting(key) -> the number Photon resolves for key now (shader packs included), or nil when unknown */
static void (*p_pushnil)(lua_State *);
static int l_setting(lua_State *L) {
    HMODULE m = cpu();
    int (*f)(const char *, float *) = m ? (int (*)(const char *, float *))(void *)GetProcAddress(m, "photon_cpu_setting") : NULL;
    float v = 0;
    if (f && p_type(L, 1) == LUA_TSTRING && f(p_tolstring(L, 1, NULL), &v)) p_pushnumber(L, (double)v);
    else p_pushnil(L);
    return 1;
}

__declspec(dllexport) int gmod13_open(lua_State *L) {
    HMODULE ls = GetModuleHandleA("lua_shared.dll");
    if (!ls) return 0;
#define GET(v, n) { FARPROC fp = GetProcAddress(ls, n); if (!fp) return 0; memcpy(&v, &fp, sizeof v); }
    GET(p_createtable, "lua_createtable") GET(p_pushcclosure, "lua_pushcclosure") GET(p_setfield, "lua_setfield")
    GET(p_pushnumber, "lua_pushnumber") GET(p_pushboolean, "lua_pushboolean") GET(p_pushstring, "lua_pushstring")
    GET(p_rawseti, "lua_rawseti") GET(p_tonumber, "lua_tonumber")
    GET(p_gettop, "lua_gettop") GET(p_type, "lua_type") GET(p_tolstring, "lua_tolstring") GET(p_objlen, "lua_objlen")
    GET(p_rawgeti, "lua_rawgeti") GET(p_settop, "lua_settop") GET(p_pushnil, "lua_pushnil")
    p_createtable(L, 0, 3);
    p_pushcclosure(L, l_loaded, 0); p_setfield(L, -2, "Loaded");
    p_pushcclosure(L, l_mask, 0); p_setfield(L, -2, "DynMask");
    p_pushcclosure(L, l_steps, 0); p_setfield(L, -2, "DynSteps");
    p_pushcclosure(L, l_viewport, 0); p_setfield(L, -2, "SetViewportScale");
    p_pushcclosure(L, l_counts, 0); p_setfield(L, -2, "ReportCounts");
    p_pushcclosure(L, l_awake, 0); p_setfield(L, -2, "ReportAwake");
    p_pushcclosure(L, l_push, 0); p_setfield(L, -2, "Push");
    p_pushcclosure(L, l_setting, 0); p_setfield(L, -2, "Setting");
    p_setfield(L, LUA_GLOBALSINDEX, "photon_cpu");
    return 0;
}
__declspec(dllexport) int gmod13_close(lua_State *L) { (void)L; return 0; }
