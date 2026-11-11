#!/usr/bin/env bash
# make_package.sh [DXVK_D3D9]: the friend install package, dist/photon-<rev>.zip: game\ (the files laptop.sh overlay ships, from build/,
# plus DXVK's x64 d3d9.dll, Photon's default backend), install.bat / uninstall.bat (scripts/win/install_photon.ps1, uninstall_photon.ps1),
# README.txt and licenses\. The installer asks for VPhysics-Jolt and GModPatchTool (downloaded, never bundled) and needs the x64 beta.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
DXVK=${1:-/mnt/c/photon-run/dxvk/d3d9.dll}
REV=$(git rev-parse --short HEAD)
P=$(mktemp -d)/photon-$REV; trap 'rm -rf "$(dirname "$P")"' EXIT
G="$P/game"
mkdir -p "$G/bin/win64" "$G/garrysmod/shaders/fxc" "$G/garrysmod/lua/autorun/client" "$G/garrysmod/lua/bin" "$P/licenses"
cp src/lua/autorun/client/photon_menu.lua src/lua/autorun/client/photon_packs.lua modules/cpu/lua/photon_cpu_menu.lua "$G/garrysmod/lua/autorun/client/"
cp modules/cpu/lua/photon_cpu_corpses.lua modules/cpu/lua/photon_cpu_mplog.lua "$G/garrysmod/lua/autorun/"
for m in gmsv_photon_cpu_win64.dll gmcl_photon_cpu_win64.dll; do [ ! -f build/modules/cpu/$m ] || cp build/modules/cpu/$m "$G/garrysmod/lua/bin/"; done
cp build/src/proxy_shaderapi/shaderapidx9.dll build/src/shaderdll/photon_shaders.dll "$G/bin/win64/"
[ ! -f build/modules/cpu/photon_cpu.dll ] || cp build/modules/cpu/photon_cpu.dll "$G/bin/win64/"
cp build/shaders/fxc/photon_*.vcs build/shaders/fxc/photon_*.ids "$G/garrysmod/shaders/fxc/"
cp "$DXVK" "$G/bin/win64/d3d9.dll"
echo "$REV" > "$G/photon_version.txt"
cp licenses/* "$P/licenses/"
cat > "$P/licenses/DXVK.txt" <<'L'
bin\win64\d3d9.dll is DXVK (https://github.com/doitsujin/dxvk), zlib/libpng licence, Copyright (c) 2017 Philip Rebohle,
Copyright (c) 2019 Joshua Ashton. Unmodified release build.
L
cp scripts/win/install_photon.ps1 scripts/win/uninstall_photon.ps1 "$P/"
printf '@echo off\r\npowershell -NoProfile -ExecutionPolicy Bypass -File "%%~dp0install_photon.ps1" %%*\r\npause\r\n' > "$P/install.bat"
printf '@echo off\r\npowershell -NoProfile -ExecutionPolicy Bypass -File "%%~dp0uninstall_photon.ps1" %%*\r\npause\r\n' > "$P/uninstall.bat"
printf '@echo off\r\npowershell -NoProfile -ExecutionPolicy Bypass -File "%%~dp0install_photon.ps1" -Latest %%*\r\npause\r\n' > "$P/update.bat"
sed 's/$/\r/' > "$P/README.txt" <<R
Photon $REV (Garry's Mod renderer and performance mod), test build for friends.

Needs: Garry's Mod on the x64 beta (Steam > Garry's Mod > Properties > Betas > "x86-64 - Chromium + 64-bit binaries").
Install: close the game, run install.bat. It finds the game, backs up every file it replaces into GarrysMod\photon_backup,
  and asks about two optional extras (downloaded from their GitHub releases, both default no): VPhysics-Jolt (much better with
  big prop piles and colliding debris, no long freezes; slower than the game's own physics in battles with Photon's corpse settings:
  pick it if you build or spawn a lot of props) and GModPatchTool (video codecs).
Play: add -insecure to the game's Steam launch options. Singleplayer, or servers of friends who also run Photon with -insecure.
  Never join public VAC-secured servers with Photon installed (it replaces an engine DLL).
Settings: in game, hold C > Photon.
Update: update.bat (downloads the newest release from GitHub and applies it; your settings and the original backups stay).
Uninstall: uninstall.bat (puts every original file back), or Steam > Properties > Installed Files > Verify integrity.
Privacy: the installer only talks to GitHub (releases, and the optional downloads you say yes to). Photon sends nothing anywhere.
Multiplayer log (for bug reports): Photon menu > Multiplayer log > On, then send the files in garrysmod\data\photon\mplog.
R
mkdir -p dist; rm -f "dist/photon-$REV.zip"
(cd "$(dirname "$P")" && zip -qr "$ROOT/dist/photon-$REV.zip" "photon-$REV")
ls -l "dist/photon-$REV.zip"; unzip -l "dist/photon-$REV.zip" | tail -1
