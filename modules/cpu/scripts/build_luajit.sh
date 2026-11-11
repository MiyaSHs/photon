#!/usr/bin/env bash
# build_luajit.sh: phase 2 L2. Current LuaJIT 2.1 (refs/LuaJIT, pinned commit below) with GMod's syntax (modules/cpu/luajit/*.patch,
# compiled with LUAJIT_GMOD_SYNTAX=1), cross-built for Windows x64 with mingw. Output: build/luajit/lua51.dll, luajit.exe.
#   GMod's LuaJIT 2.1.0-beta3 adds to Lua 5.1: `!` `!=` `&&` `||` (upstream has them since its v3.0 syntax backport), `continue`
#   (upstream too), and C-style comments `//` and `/* */` (patch 0001); pairs() order as beta3 (patch 0002 + SECURITY_STRHASH=0); L->luabase at +0x78 as GMod (patch 0003); MetaName/MetaID in luaL_newmetatable and luaL_newmetatable_type (patch 0004); jit.util as beta3 registers it (0005); the sandbox library definition (0006); luabase on the VM thread that runs __gc (0007); table.insert returns the index (0008); a print output hook (0009).
#   Also builds libluajit.a + headers for the replacement lua_shared.dll. analysis/15.
set -euo pipefail
cd "$(dirname "$0")/../../.."
PIN=c4b1e2f7
OUT=build/luajit; XF=""
# CHECK=1: LuaJIT's API checks on (bad indices etc. abort with "LuaJIT ASSERT" on stderr), into build/luajit-check
[ "${CHECK:-0}" = 1 ] && { OUT=build/luajit-check; XF="-DLUA_USE_APICHECK"; }
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
git -C refs/LuaJIT archive "$PIN" | tar -x -C "$W"
for p in modules/cpu/luajit/*.patch; do patch -s -p1 -d "$W" < "$p"; done
make -s -C "$W" -j4 HOST_CC=gcc CROSS=x86_64-w64-mingw32- TARGET_SYS=Windows BUILDMODE=dynamic \
  XCFLAGS="-DLUAJIT_GMOD_SYNTAX=1 -DLUAJIT_SECURITY_STRHASH=0 $XF" >/dev/null
mkdir -p "$OUT"
cp "$W"/src/lua51.dll "$W"/src/luajit.exe "$OUT"/
# the static library for the replacement lua_shared.dll (L3), and the headers it compiles against
make -s -C "$W" clean >/dev/null
make -s -C "$W" -j4 HOST_CC=gcc CROSS=x86_64-w64-mingw32- TARGET_SYS=Windows BUILDMODE=static \
  XCFLAGS="-DLUAJIT_GMOD_SYNTAX=1 -DLUAJIT_SECURITY_STRHASH=0 $XF" >/dev/null
cp "$W"/src/libluajit.a "$OUT"/
mkdir -p "$OUT/include"; cp "$W"/src/lua.h "$W"/src/lualib.h "$W"/src/lauxlib.h "$W"/src/luaconf.h "$W"/src/luajit.h "$W"/src/lua.hpp "$OUT/include/"
mkdir -p "$OUT/jit"; cp "$W"/src/jit/*.lua "$OUT/jit/"
echo "built LuaJIT $PIN + $(ls modules/cpu/luajit/*.patch | wc -l) patch(es) -> $OUT"
