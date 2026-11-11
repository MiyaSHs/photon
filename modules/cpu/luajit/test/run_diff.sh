#!/usr/bin/env bash
# run_diff.sh [case.lua ...]: each case through GMod's lua_shared.dll (the VM's game copy) and ours, under Wine in a private prefix;
# prints SAME or the diff per case. Ours: the replacement lua_shared.dll (build/lua_shared, GMod's openlibs) if built, else plain
# LuaJIT (build/luajit/lua51.dll, upstream's library set); NEW=lua51 forces the latter.
cd "$(dirname "$0")/../../../.."
export WINEPREFIX=${WINEPREFIX:-$HOME/.cache/photon_cpu_luadiff_wine} WINEDEBUG=-all
G=$(pwd)/game/GarrysMod/bin/win64 B=$(pwd)/build/luajit O=$(pwd)/build/lua_shared
if [ "${NEW:-}" != lua51 ] && [ -f "$O/lua_shared.dll" ]; then ND=$O NF=./lua_shared.dll; else ND=$B NF=lua51.dll; fi
cases=("$@"); [ ${#cases[@]} -gt 0 ] || cases=(modules/cpu/luajit/test/cases/*.lua)
for c in "${cases[@]}"; do
  c=$(realpath "$c")
  a=$(cd "$G" && timeout 120 wine "$B/lua_diff.exe" lua_shared.dll "$c" 2>/dev/null | tr -d '\r')
  b=$(cd "$ND" && timeout 120 wine "$B/lua_diff.exe" "$NF" "$c" 2>/dev/null | tr -d '\r')
  if [ -z "$a" ] || [ -z "$b" ] || ! grep -q "^OK$" <<<"$a$b"; then echo "FAIL  $(basename "$c"): GMod: ${a:0:120} / new: ${b:0:120}"
  elif [ "$a" = "$b" ]; then echo "SAME  $(basename "$c") ($(echo "$a" | wc -l) lines)"
  else echo "DIFF  $(basename "$c")"; diff <(echo "$a") <(echo "$b") | head -20; fi
done
