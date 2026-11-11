# Source me (bash): shared Wine environment for Photon tests.
export PHOTON_ROOT="${PHOTON_ROOT:-$HOME/Projects/Photon}"
export WINEPREFIX="$PHOTON_ROOT/third_party/wineprefix"
export WINEARCH=win64
export WINEDEBUG="${WINEDEBUG:--all}"
# Skip Mono/Gecko install prompts (headless).
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
export GMOD_BIN="$PHOTON_ROOT/game/GarrysMod/bin/win64"
