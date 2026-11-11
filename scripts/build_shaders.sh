#!/usr/bin/env bash
# build_shaders.sh: HLSL -> D3D9 bytecode -> VCS v6 for Photon's own shaders (T3.3, analysis/11 §7, m3_vcs.md §3).
#   scripts/build_shaders.sh [--vpk PATH] [--sdk DIR] [--no-vlg] [--no-vlg20] [--no-bump20] [--no-skin20]
#     builds everything listed in src/shaderdll/hlsl/shaders.lst, then photon_vlg_ps30.vcs (T3.4b) with
#     tools/t34_build_vlg.py: GMod's vertexlit_and_unlit_generic_ps30 layout, the pilot statics' dynamic combo 0
#     compiled from src/shaderdll/hlsl/photon_vlg_ps2x.fxc, every other combo copied from the stock file in the VPK.
#     --vpk (or VPK=): the user's own garrysmod_dir.vpk, read only (default game/GarrysMod/garrysmod/garrysmod_dir.vpk);
#     the stock bytecode goes only to build/ (never committed). --sdk (or PHOTON_SDK_SHADERS=): SDK 2013 legacy
#     stdshaders dir with the base source and includes (default refs/sdk2013-legacy-stdshaders/sp). --no-vlg (VLG=0)
#     skips it. Its output: build/shaders/fxc/photon_vlg_ps30.{vcs,ids} (+ a copy in fxc_lzma), build/shaders/vlg/.
#     Then the Option A SM3 pair (analysis/14 Erratum 2 decision, A1) with tools/t34_build_vlg20.py:
#     photon_vlg_vs20.{vcs,ids} (vs_3_0 at GMod's vertexlit_and_unlit_generic_vs20 layout, SDK master vs20.fxc with the
#     VS_2_0 define) and photon_vlg_ps20b.{vcs,ids} (ps_3_0 at the _ps20b layout, photon_vlg_ps2x.fxc with PS_2_B and
#     the Photon block); only the pilot statics, every stored dynamic compiled, nothing copied from stock. Compile
#     cache in build/shaders/vlg20/ (VLG20_ARGS=--no-cache forces a full rebuild; VLG20_ARGS="--jobs N").
#     --no-vlg20 (VLG20=0) skips it. Test: scripts/run_vlgpixel.sh --pair.
#     Then the bump pair (lighting plan v2 P1/P2) with tools/t34_build_bump20.py: photon_bump_vs20.{vcs,ids} and
#     photon_bump_ps20b.{vcs,ids} at GMod's bump vs20/ps20b layouts (Photon block + P1 envmap occlusion); cache in
#     build/shaders/bump20/ (BUMP20_ARGS=--no-cache). --no-bump20 (BUMP20=0) skips it. Test: run_vlgpixel.sh --bump.
#     Then the skin (phong) pair (plan v2 P2) with tools/t34_build_skin20.py: photon_skin_vs20.{vcs,ids} and
#     photon_skin_ps20b.{vcs,ids} at GMod's skin_vs20/skin_ps20b layouts (skin's own light loop on the Photon block +
#     P1 envmap occlusion; needs the probe's build/shaders/skinprobe/report_sweep.json, SKIN20_ARGS="--quirks none"
#     searches every mask instead); cache in build/shaders/skin20/. --no-skin20 (SKIN20=0) skips it. Test:
#     scripts/run_skinpair.sh --pair. The proxy (vlg.c) renames the pair in game.
# Compiler: the native d3dcompiler_47.dll under Wine through build/tools/d3dcompile/d3dcompile.exe (ninja -C build
# first). D3DCOMPILER=<path> overrides the DLL (default: $GMOD_BIN/d3dcompiler_47.dll, from the GModPatchTool CEF
# stack; m3_vcs.md risk 4). WINEDLLOVERRIDES d3dcompiler_47=n keeps Wine from substituting its vkd3d builtin.
# Output (one static combo, one dynamic combo per file):
#   build/shaders/bin/<name>.{bin,asm}       bytecode and D3DDisassemble listing
#   build/shaders/fxc/<name>.vcs             uncompressed chunks (0x80000000|n), the default install set
#   build/shaders/fxc_lzma/<name>.vcs        the same bytecode in Valve LZMA chunks (install with VCSDIR=...fxc_lzma)
# Checks (any failure exits 1): version token per profile; every vs_3_0 transforms the position with dp4 o0.x..w
# against c4..c7, the same code as the stock g_bokehblur_vs30 (extracted from garrysmod_dir.vpk and disassembled
# here); vcs_write.py check (strict parse + loader emulation at alignments 1/512/4096 + task-07 readers);
# vcs_write.py rt (passthrough re-emit byte-identical, raw re-encode structure-equal); the bytecode decoded from
# each VCS by vcs_write.parse and by vcs_peek is byte-identical to the compiler output.
# Prints BUILD_SHADERS OK.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/env.sh"
export WINEDLLOVERRIDES="mscoree,mshtml=;d3dcompiler_47=n;d3dcompiler_43=n"
TOOL=$ROOT/build/tools/d3dcompile/d3dcompile.exe
DLL=${D3DCOMPILER:-$GMOD_BIN/d3dcompiler_47.dll}
SRC=$ROOT/src/shaderdll/hlsl
OUT=$ROOT/build/shaders
VPK=${VPK:-$ROOT/game/GarrysMod/garrysmod/garrysmod_dir.vpk}
SDK=${PHOTON_SDK_SHADERS:-$ROOT/refs/sdk2013-legacy-stdshaders/sp}
VLG=${VLG:-1}
VLG20=${VLG20:-1}
BUMP20=${BUMP20:-1}
SKIN20=${SKIN20:-1}
while [ $# -gt 0 ]; do
  case "$1" in
    --vpk) VPK=$2; shift 2 ;;
    --sdk) SDK=$2; shift 2 ;;
    --no-vlg) VLG=0; shift ;;
    --no-vlg20) VLG20=0; shift ;;
    --no-bump20) BUMP20=0; shift ;;
    --no-skin20) SKIN20=0; shift ;;
    *) echo "usage: $0 [--vpk garrysmod_dir.vpk] [--sdk dir] [--no-vlg] [--no-vlg20] [--no-bump20] [--no-skin20]"; exit 2 ;;
  esac
done
[ -f "$VPK" ] || { echo "no VPK $VPK (--vpk)"; exit 1; }
[ -f "$TOOL" ] || { echo "no $TOOL (ninja -C build)"; exit 1; }
[ -f "$DLL" ] || { echo "no compiler DLL $DLL (set D3DCOMPILER)"; exit 1; }
mkdir -p "$OUT/bin" "$OUT/fxc" "$OUT/fxc_lzma"
w() { winepath -w "$1" 2>/dev/null; }
D=$(w "$DLL")
py() { python3 -I -B "$@"; }

# position transform lines of a vs_3_0 listing, register names normalised (r0/v0 -> R)
xform() { grep -E '^\s*dp4 o0\.[xyzw], [rv][0-9]+, c[0-9]+' "$1" | sed -E 's/[rv][0-9]+,/R,/; s/^\s+//'; }

echo "[ref] stock g_bokehblur_vs30 (garrysmod_dir.vpk), static 0 dynamic 0"
py - "$VPK" "$OUT/bin/stock_g_bokehblur_vs30.bin" <<'PY'
import os, sys; sys.path.insert(0, os.path.join(os.environ['PHOTON_ROOT'], 'tools'))
from vpk import VPK
import vcs_write
p = vcs_write.parse(VPK(sys.argv[1]).read('shaders/fxc/g_bokehblur_vs30.vcs'))
open(sys.argv[2], 'wb').write(p['statics'][0]['combos'][0])
PY
wine "$TOOL" "$D" -d "$(w "$OUT/bin/stock_g_bokehblur_vs30.bin")" "$(w "$OUT/bin/stock_g_bokehblur_vs30.asm")"
REF=$(xform "$OUT/bin/stock_g_bokehblur_vs30.asm")
grep -E '^\s*(vs_|dcl|mad|dp4|mov)' "$OUT/bin/stock_g_bokehblur_vs30.asm" | sed 's/^/  stock: /'
[ "$(echo "$REF" | wc -l)" = 4 ] || { echo "FAIL: stock reference has no 4-dp4 transform"; exit 1; }

fail=0
while read -r name profile file entry; do
  case "$name" in ''|'#'*) continue ;; esac
  entry=${entry:-main}
  echo "[$name] $profile $file:$entry"
  bin=$OUT/bin/$name.bin; asm=$OUT/bin/$name.asm
  wine "$TOOL" "$D" "$(w "$SRC/$file")" "$entry" "$profile" "$(w "$bin")" "$(w "$asm")" 2>&1 | sed 's/^/  /'
  [ -s "$bin" ] || { echo "FAIL: no bytecode"; fail=1; continue; }
  grep -q 'Microsoft (R) HLSL Shader Compiler' "$asm" || { echo "FAIL: not compiled by the native d3dcompiler"; fail=1; }
  grep -E '^\s*(vs_|ps_|def|dcl|mad|dp4|mov)' "$asm" | sed 's/^/  ours:  /'
  tok=$(py -c "import struct,sys;print('%08x'%struct.unpack_from('<I',open(sys.argv[1],'rb').read())[0])" "$bin")
  want=$([ "${profile%%_*}" = vs ] && echo fffe || echo ffff)0300
  [ "$tok" = "$want" ] || { echo "FAIL: token $tok, expected $want for $profile"; fail=1; }
  if [ "${profile%%_*}" = vs ]; then
    if [ "$(xform "$asm")" = "$REF" ]; then echo "  transform matches stock g_bokehblur_vs30: dp4 o0.xyzw against c4..c7"
    else echo "FAIL: transform differs from stock:"; xform "$asm"; fail=1; fi
  fi
  for var in fxc fxc_lzma; do
    vcs=$OUT/$var/$name.vcs
    extra=(); [ "$var" = fxc_lzma ] && extra=(--lzma)
    py "$ROOT/tools/vcs_write.py" build -o "$vcs" --dynamic 1 --static 1 --crc-of "$SRC/$file" "${extra[@]}" "0:0=$bin" \
      | sed "s/^/  $var: /" || { echo "FAIL: vcs_write build/check ($var)"; fail=1; }
    py "$ROOT/tools/vcs_write.py" rt "$vcs" | sed "s|^.*/|  $var: rt |" || { echo "FAIL: vcs_write rt ($var)"; fail=1; }
    py - "$vcs" "$bin" <<'PY' | sed "s/^/  $var: /" || fail=1
import os, sys; sys.path.insert(0, os.path.join(os.environ['PHOTON_ROOT'], 'tools'))
import vcs_write, vcs_peek
b = open(sys.argv[1], 'rb').read(); code = open(sys.argv[2], 'rb').read()
p = vcs_write.parse(b)
ok = sorted(p['statics']) == [0] and p['statics'][0]['combos'] == {0: code} and p['dynamic'] == 1
pk = vcs_peek.combos(b, 4)
ok = ok and [(s, d, n) for s, d, n, _v, _c in pk] == [(0, 0, len(code))]
kind = {0x80000000: 'raw', 0x40000000: 'lzma'}.get(p['statics'][0]['chunks'][0][0] & 0xc0000000, '?') if p['statics'][0].get('chunks') else '?'
print('round trip %s: %d bytes, chunk %s, vcs_peek %s %s, bytecode identical' % ('OK' if ok else 'FAIL', len(b), kind, pk[0][3], pk[0][4]) if ok else 'round trip FAIL')
sys.exit(0 if ok else 1)
PY
  done
done < "$SRC/shaders.lst"
if [ "$VLG" = 1 ]; then
  echo "[photon_vlg_ps30] tools/t34_build_vlg.py --vpk $VPK --sdk $SDK"
  if py "$ROOT/tools/t34_build_vlg.py" --vpk "$VPK" --sdk "$SDK" --dll "$DLL" --tool "$TOOL" --out "$OUT" | sed 's/^/  /'; then
    cp "$OUT/fxc/photon_vlg_ps30.vcs" "$OUT/fxc/photon_vlg_ps30.ids" "$OUT/fxc_lzma/"
  else
    echo "FAIL: photon_vlg_ps30"; fail=1
    rm -f "$OUT"/fxc/photon_vlg_ps30.* "$OUT"/fxc_lzma/photon_vlg_ps30.*
  fi
fi
if [ "$VLG20" = 1 ]; then
  echo "[photon_vlg_vs20 + photon_vlg_ps20b] tools/t34_build_vlg20.py --vpk $VPK --sdk $SDK ${VLG20_ARGS:-}"
  # shellcheck disable=SC2086
  if py "$ROOT/tools/t34_build_vlg20.py" --vpk "$VPK" --sdk "$SDK" --dll "$DLL" --tool "$TOOL" --out "$OUT" ${VLG20_ARGS:-} \
       | sed 's/^/  /'; then
    cp "$OUT"/fxc/photon_vlg_vs20.{vcs,ids} "$OUT"/fxc/photon_vlg_ps20b.{vcs,ids} "$OUT/fxc_lzma/"
  else
    echo "FAIL: photon_vlg_vs20/ps20b"; fail=1
    rm -f "$OUT"/fxc/photon_vlg_vs20.* "$OUT"/fxc/photon_vlg_ps20b.* "$OUT"/fxc_lzma/photon_vlg_vs20.* "$OUT"/fxc_lzma/photon_vlg_ps20b.*
  fi
fi
if [ "$BUMP20" = 1 ]; then
  echo "[photon_bump_vs20 + photon_bump_ps20b] tools/t34_build_bump20.py --vpk $VPK --sdk $SDK ${BUMP20_ARGS:-}"
  # shellcheck disable=SC2086
  if env -u D3DCOMPILE_FLAGS python3 -I -B "$ROOT/tools/t34_build_bump20.py" --vpk "$VPK" --sdk "$SDK" --dll "$DLL" --tool "$TOOL" \
       --out "$OUT" ${BUMP20_ARGS:-} | sed 's/^/  /'; then
    cp "$OUT"/fxc/photon_bump_vs20.{vcs,ids} "$OUT"/fxc/photon_bump_ps20b.{vcs,ids} "$OUT/fxc_lzma/"
  else
    echo "FAIL: photon_bump_vs20/ps20b"; fail=1
    rm -f "$OUT"/fxc/photon_bump_vs20.* "$OUT"/fxc/photon_bump_ps20b.* "$OUT"/fxc_lzma/photon_bump_vs20.* "$OUT"/fxc_lzma/photon_bump_ps20b.*
  fi
fi
if [ "$SKIN20" = 1 ]; then
  echo "[photon_skin_vs20 + photon_skin_ps20b] tools/t34_build_skin20.py --vpk $VPK --sdk $SDK ${SKIN20_ARGS:-}"
  # shellcheck disable=SC2086
  if env -u D3DCOMPILE_FLAGS python3 -I -B "$ROOT/tools/t34_build_skin20.py" --vpk "$VPK" --sdk "$SDK" --dll "$DLL" --tool "$TOOL" \
       --out "$OUT" ${SKIN20_ARGS:-} | sed 's/^/  /'; then
    cp "$OUT"/fxc/photon_skin_vs20.{vcs,ids} "$OUT"/fxc/photon_skin_ps20b.{vcs,ids} "$OUT/fxc_lzma/"
  else
    echo "FAIL: photon_skin_vs20/ps20b"; fail=1
    rm -f "$OUT"/fxc/photon_skin_vs20.* "$OUT"/fxc/photon_skin_ps20b.* "$OUT"/fxc_lzma/photon_skin_vs20.* "$OUT"/fxc_lzma/photon_skin_ps20b.*
  fi
fi
[ "$fail" = 0 ] || { echo "BUILD_SHADERS FAIL"; exit 1; }
ls -l "$OUT"/fxc/*.vcs "$OUT"/fxc_lzma/*.vcs | awk '{print "  " $5, $NF}'
echo "BUILD_SHADERS OK"
