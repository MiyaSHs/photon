#!/usr/bin/env bash
# build_lua_shared.sh: phase 2 L3. The replacement lua_shared.dll: our LuaJIT (build/luajit/libluajit.a, from build_luajit.sh) plus
# GMod's CLuaShared/CLuaInterface layer (modules/cpu/lua_shared/src). Output: build/lua_shared/lua_shared.dll.
# Install (test copies only): rename the game's bin/win64/lua_shared.dll to lua_shared_gmod.dll, put this one in its place.
set -euo pipefail
cd "$(dirname "$0")/../../.."
LJ=build/luajit OUT=build/lua_shared
[ "${CHECK:-0}" = 1 ] && { LJ=build/luajit-check OUT=build/lua_shared-check; }   # CHECK=1: against the API-checked LuaJIT
[ -f $LJ/libluajit.a ] || bash modules/cpu/scripts/build_luajit.sh
mkdir -p "$OUT"
S=modules/cpu/lua_shared/src
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wno-unused-function -shared -o "$OUT/lua_shared.dll" -I$LJ/include \
  $S/iface_base.c $S/iface_iface.c $S/iface_more.c $S/vtable.c $S/msstr.c $S/tier0.c $S/openlibs.c $S/shared.c $S/trace_vt.c $S/cprof.c \
  modules/cpu/lua_shared/lua_shared.def $LJ/libluajit.a \
  -static-libgcc -Wl,--enable-stdcall-fixup -Wl,--disable-auto-import
echo "built $OUT/lua_shared.dll ($(x86_64-w64-mingw32-objdump -p "$OUT/lua_shared.dll" | grep -c '^\s*\[ *[0-9]*\]') exports)"
