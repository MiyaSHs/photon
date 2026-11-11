# uninstall_photon.ps1: removes Photon from a Garry's Mod install: every file install_photon.ps1 listed in photon_backup\installed.txt is
# deleted, then the backed-up originals are put back (shaderapidx9.dll, vphysics.dll if Jolt was installed, anything else replaced).
param([string]$GameDir = '')
$ErrorActionPreference = 'Stop'
if (-not $GameDir) {
  $steam = (Get-ItemProperty -ea 0 'HKCU:\Software\Valve\Steam').SteamPath
  $libs = @(($steam -replace '/', '\'))
  $vdf = Join-Path $libs[0] 'steamapps\libraryfolders.vdf'
  if (Test-Path $vdf) { $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }) }
  foreach ($l in $libs | Select-Object -Unique) { $c = Join-Path $l 'steamapps\common\GarrysMod'; if (Test-Path "$c\photon_backup\installed.txt") { $GameDir = $c; break } }
  if (-not $GameDir) { throw "No Photon install found. Run: uninstall.bat ""D:\path\to\GarrysMod""" }
}
if (Get-Process -ea 0 gmod | Where-Object { $_.Path -like "$GameDir\*" }) { throw "Close Garry's Mod first." }
$Bk = Join-Path $GameDir 'photon_backup'
$list = Get-Content "$Bk\installed.txt"
foreach ($rel in $list) { Remove-Item -Force -ea 0 (Join-Path $GameDir $rel) }
Get-ChildItem -Recurse -File $Bk | Where-Object { $_.Name -ne 'installed.txt' } | ForEach-Object {
  $rel = $_.FullName.Substring("$Bk\".Length); $dst = Join-Path $GameDir $rel
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null; Copy-Item -Force $_.FullName $dst
}
Remove-Item -Recurse -Force $Bk
"Photon removed; the original files are back. (Steam > Garry's Mod > Properties > Installed Files > Verify also restores everything.)"
