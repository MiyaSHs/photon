# install_photon.ps1: installs or updates Photon in a Garry's Mod install. install.bat runs it from an unzipped Photon release; update.bat
# runs it with -Latest, which downloads the newest release from GitHub first. Rerunning it is always safe: it updates in place.
#
# - The first time, every game file Photon replaces is copied to <GarrysMod>\photon_backup (the true originals); updates never overwrite a
#   backup, so uninstall.bat always restores the game as Steam installed it. The list of Photon's files is photon_backup\installed.txt.
# - A file the new version no longer has is removed again (its original comes back from the backup).
# - Network: only GitHub (the latest release with -Latest, and the optional VPhysics-Jolt / GModPatchTool downloads you say yes to).
#   No telemetry, no accounts, nothing sent anywhere.
# Photon replaces an engine DLL (shaderapidx9.dll): play singleplayer or on your own / friends' servers started with -insecure, never on
# public VAC-secured servers.
param([string]$GameDir = '', [switch]$Latest, [string]$Repo = 'MiyaSHs/photon', [switch]$Yes)
$ErrorActionPreference = 'Stop'
$Pkg = $PSScriptRoot
function Ask($q) { if ($Yes) { return $false }; (Read-Host "$q [y/N]") -match '^[yY]' }

# --- the package: this folder, or the newest GitHub release ------------------------------------------------------------------------------
if ($Latest) {
  "Looking for the newest Photon release on github.com/$Repo ..."
  $rel = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest" -Headers @{ 'User-Agent' = 'photon-installer' }
  $a = $rel.assets | Where-Object { $_.name -like 'photon-*.zip' } | Select-Object -First 1
  if (-not $a) { throw "The latest release ($($rel.tag_name)) has no photon-*.zip." }
  $z = Join-Path $env:TEMP $a.name; Invoke-WebRequest $a.browser_download_url -OutFile $z -Headers @{ 'User-Agent' = 'photon-installer' }
  $x = Join-Path $env:TEMP 'photon_release'; Remove-Item -Recurse -Force -ea 0 $x; Expand-Archive $z $x
  $Pkg = (Get-ChildItem $x -Directory | Select-Object -First 1).FullName
  "Downloaded $($rel.tag_name)"
}
if (-not (Test-Path "$Pkg\game")) { throw "No game\ folder next to this script: run it from an unzipped Photon release, or use update.bat." }
$newVer = (Get-Content -ea 0 "$Pkg\game\photon_version.txt" | Select-Object -First 1)

# --- the game: the Steam library holding Garry's Mod -------------------------------------------------------------------------------------
if (-not $GameDir) {
  $steam = (Get-ItemProperty -ea 0 'HKCU:\Software\Valve\Steam').SteamPath
  if (-not $steam) { throw 'Steam not found. Run: install.bat "D:\path\to\GarrysMod"' }
  $libs = @($steam -replace '/', '\')
  $vdf = Join-Path $libs[0] 'steamapps\libraryfolders.vdf'
  if (Test-Path $vdf) { $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }) }
  foreach ($l in $libs | Select-Object -Unique) { $c = Join-Path $l 'steamapps\common\GarrysMod'; if (Test-Path "$c\garrysmod") { $GameDir = $c; break } }
  if (-not $GameDir) { throw "Garry's Mod not found in the Steam libraries. Run: install.bat ""D:\path\to\GarrysMod""" }
}
"Garry's Mod: $GameDir"
$B = Join-Path $GameDir 'bin\win64'
if (-not (Test-Path "$B\gmod.exe")) {
  throw "Photon needs the 64-bit Garry's Mod: in Steam, Garry's Mod > Properties > Betas > 'x86-64 - Chromium + 64-bit binaries', let it update, then run this again."
}
if (Get-Process -ea 0 gmod | Where-Object { $_.Path -like "$GameDir\*" }) { throw "Close Garry's Mod first." }

# --- install or update -------------------------------------------------------------------------------------------------------------------
$Bk = Join-Path $GameDir 'photon_backup'
$listFile = Join-Path $Bk 'installed.txt'
$update = Test-Path $listFile
$old = @(); if ($update) { $old = @(Get-Content $listFile | Where-Object { $_ }) }
$oldVer = (Get-Content -ea 0 (Join-Path $GameDir 'photon_version.txt') | Select-Object -First 1)
if ($update) { "Updating Photon $oldVer -> $newVer" } else { "Installing Photon $newVer" }
New-Item -ItemType Directory -Force $Bk | Out-Null
$files = @(Get-ChildItem -Recurse -File "$Pkg\game" | ForEach-Object { $_.FullName.Substring("$Pkg\game\".Length) })
foreach ($rel in $files) {   # an original the game has, backed up once (never a file Photon put there)
  $dst = Join-Path $GameDir $rel; $bd = Join-Path $Bk $rel
  if ((Test-Path $dst) -and -not (Test-Path $bd) -and -not ($old -contains $rel)) {
    New-Item -ItemType Directory -Force (Split-Path $bd) | Out-Null; Copy-Item -Force $dst $bd
  }
}
# the proxy loads the original as shaderapidx9_stock.dll: made once, from the game's own DLL (on an update shaderapidx9.dll is Photon's)
$stock = "$B\shaderapidx9_stock.dll"
if (-not (Test-Path $stock)) {
  if (Test-Path "$Bk\bin\win64\shaderapidx9.dll") { Copy-Item -Force "$Bk\bin\win64\shaderapidx9.dll" $stock }
  else { Copy-Item -Force "$B\shaderapidx9.dll" $stock }
}
# files of the old version that the new one no longer has: removed, their original back
foreach ($rel in $old) {
  if ($files -contains $rel -or $rel -eq 'bin\win64\shaderapidx9_stock.dll' -or $rel -eq 'bin\win64\vphysics.dll') { continue }
  $dst = Join-Path $GameDir $rel; $bd = Join-Path $Bk $rel
  Remove-Item -Force -ea 0 $dst
  if (Test-Path $bd) { Copy-Item -Force $bd $dst; Remove-Item -Force $bd }
}
Copy-Item -Recurse -Force "$Pkg\game\*" $GameDir
$installed = @($files) + 'bin\win64\shaderapidx9_stock.dll'
if ($old -contains 'bin\win64\vphysics.dll') { $installed += 'bin\win64\vphysics.dll' }   # Jolt, chosen at an earlier install
"$($files.Count) Photon files in place"

# --- optional: VPhysics-Jolt (asked on the first install only) ---------------------------------------------------------------------------
# not the default; it depends on the scene (modules/cpu/analysis/16_ai.md): Jolt wins with heavy contact (400 props 11.2 vs 14.7 ms; stock
# freezes up to 9.4 s at 800), stock wins in battles with Photon's non-touching corpses (+37%)
if (-not $update -and (Ask 'Install VPhysics-Jolt? Much better with big prop piles and colliding debris (no long freezes); slower than the game''s own physics in battles with Photon''s corpse settings. Pick it if you build or spawn a lot of props. (Replaces vphysics.dll, backed up)')) {
  $rel = Invoke-RestMethod 'https://api.github.com/repos/misyltoad/VPhysics-Jolt/releases/latest' -Headers @{ 'User-Agent' = 'photon-installer' }
  $a = $rel.assets | Where-Object { $_.name -like '*gmod_win64.zip' } | Select-Object -First 1
  if ($a) {
    $z = Join-Path $env:TEMP $a.name; Invoke-WebRequest $a.browser_download_url -OutFile $z
    $x = Join-Path $env:TEMP 'photon_jolt'; Remove-Item -Recurse -Force -ea 0 $x; Expand-Archive $z $x
    $dll = Get-ChildItem -Recurse $x -Filter vphysics.dll | Select-Object -First 1
    if ($dll) {
      $rel2 = 'bin\win64\vphysics.dll'
      if ((Test-Path "$GameDir\$rel2") -and -not (Test-Path "$Bk\$rel2")) { New-Item -ItemType Directory -Force "$Bk\bin\win64" | Out-Null; Copy-Item -Force "$GameDir\$rel2" "$Bk\$rel2" }
      Copy-Item -Force $dll.FullName "$GameDir\$rel2"; $installed += $rel2; "VPhysics-Jolt $($rel.tag_name) installed"
    } else { 'VPhysics-Jolt: vphysics.dll not found in the download, skipped' }
  } else { 'VPhysics-Jolt: no win64 download found, skipped' }
}
$installed | Select-Object -Unique | Set-Content -Encoding UTF8 $listFile

# --- optional: GModPatchTool (formerly GModCEFCodecFix; asked on the first install only) -------------------------------------------------
if (-not $update -and (Ask 'Run GModPatchTool (formerly GModCEFCodecFix: fixes video playback and adds codecs; downloaded from GitHub, GPL-3)?')) {
  $rel = Invoke-RestMethod 'https://api.github.com/repos/solsticegamestudios/GModCEFCodecFix/releases/latest' -Headers @{ 'User-Agent' = 'photon-installer' }
  $a = $rel.assets | Where-Object { $_.name -eq 'GModPatchTool-Windows.zip' } | Select-Object -First 1
  if ($a) {
    $z = Join-Path $env:TEMP $a.name; Invoke-WebRequest $a.browser_download_url -OutFile $z
    $x = Join-Path $env:TEMP 'photon_gmodpatchtool'; Remove-Item -Recurse -Force -ea 0 $x; Expand-Archive $z $x
    $exe = Get-ChildItem -Recurse $x -Filter *.exe | Select-Object -First 1
    if ($exe) { & $exe.FullName } else { 'GModPatchTool: no program in the download, skipped' }
  }
}

@"

Photon $newVer is $(if ($update) { 'updated' } else { 'installed' }).
 - Steam launch options for Garry's Mod: -insecure
   Play singleplayer, or host / join friends' servers that also run -insecure. Do NOT join public VAC-secured servers with Photon.
 - Settings: in game, hold C (context menu) > Photon.
 - Update later: update.bat (downloads the newest release). Uninstall: uninstall.bat (restores every original file).
"@
