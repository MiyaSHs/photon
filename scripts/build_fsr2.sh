#!/usr/bin/env bash
# build_fsr2.sh: AMD FidelityFX FSR 2.2.1 (MIT, third_party/fsr2: git clone --depth 1 --branch v2.2.1
# https://github.com/GPUOpen-Effects/FidelityFX-FSR2.git) for Photon's Vulkan bridge (analysis/45 U4): the Vulkan shader permutations
# through AMD's FidelityFX_SC.exe under Wine (its glslang), then the API and its Vulkan backend as a static library with MinGW.
# Output: third_party/fsr2_build/{shaders/vk/*.h, libffx_fsr2_vk.a}.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/env.sh" >/dev/null 2>&1 || true
F=$ROOT/third_party/fsr2/src/ffx-fsr2-api
O=$ROOT/third_party/fsr2_build
[ -d "$F" ] || { echo "third_party/fsr2 missing (see the header)"; exit 1; }
mkdir -p "$O/shaders/vk" "$O/obj"
BASE=(-reflection -deps=gcc -DFFX_GPU=1 -DFFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
      -DFFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
      -DFFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -compiler=glslang -e main --target-env vulkan1.1 -S comp -Os -DFFX_GLSL=1
      "-DFFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}" "-DFFX_FSR2_OPTION_HDR_COLOR_INPUT={0,1}" "-DFFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}"
      "-DFFX_FSR2_OPTION_JITTERED_MOTION_VECTORS={0,1}" "-DFFX_FSR2_OPTION_INVERTED_DEPTH={0,1}" "-DFFX_FSR2_OPTION_APPLY_SHARPENING={0,1}")
cd "$F"
# the dev box on WSL (2026-11-11): no Wine; AMD's compiler runs natively on Windows (paths through wslpath)
if command -v wine >/dev/null; then SC=(wine ../../tools/sc/FidelityFX_SC.exe); wp() { winepath -w "$1"; }
else SC=(../../tools/sc/FidelityFX_SC.exe); wp() { wslpath -w "$1"; }; fi
for p in tcr_autogen autogen_reactive accumulate compute_luminance_pyramid depth_clip lock reconstruct_previous_depth rcas; do
  n=ffx_fsr2_${p}_pass
  [ -f "$O/shaders/vk/${n}_permutations.h" ] && [ "$O/shaders/vk/${n}_permutations.h" -nt "shaders/$n.glsl" ] && continue
  half="-DFFX_HALF={0,1}"; [ $p = compute_luminance_pyramid ] && half=-DFFX_HALF=0
  "${SC[@]}" "${BASE[@]}" "$half" -name=$n "-I$(wp "$F/shaders")" "-output=$(wp "$O/shaders/vk")" "$(wp "$F/shaders/$n.glsl")" </dev/null 2>&1 | grep -v "^$" | tail -1
done
CXX=x86_64-w64-mingw32-g++
VKH=$ROOT/src/external/vulkan-headers/include
# the Vulkan backend with each "backend API error" return reporting its source line to Photon's log (photon_fsr2_fail, fsr2_glue.cpp)
mkdir -p "$O/patched"
sed -e 's/return FFX_ERROR_BACKEND_API_ERROR;/{ photon_fsr2_fail(__LINE__); return FFX_ERROR_BACKEND_API_ERROR; }/' \
    -e '0,/^#include/s//extern "C" void photon_fsr2_fail(int line);\n#include/' vk/ffx_fsr2_vk.cpp > "$O/patched/ffx_fsr2_vk.cpp"
# and findMemoryTypeIndex taking a host-visible type when a device-local-only request finds none (CPU drivers such as lavapipe, whose
# memory is all host-visible; GPUs keep AMD's choice)
python3 -I - "$O/patched/ffx_fsr2_vk.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            bestCandidate = i;\n            outProperties = memProperties.memoryTypes[i].propertyFlags;\n"
assert s.count("    return bestCandidate;\n}") >= 1
s = s.replace("    return bestCandidate;\n}", "    if (bestCandidate == UINT32_MAX && requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)\n"
              "        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)\n"
              "            if ((memRequirements.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & requestedProperties)) {\n"
              "                outProperties = memProperties.memoryTypes[i].propertyFlags; return i; }\n"
              "    return bestCandidate;\n}", 1)
open(p, 'w').write(s)
PY
for src in ffx_fsr2.cpp ffx_assert.cpp "$O/patched/ffx_fsr2_vk.cpp" vk/shaders/ffx_fsr2_shaders_vk.cpp; do
  [ -f "$src" ] || continue
  obj=$O/obj/$(basename "${src%.cpp}").o
  [ -f "$obj" ] && [ "$obj" -nt "$src" ] && continue
  $CXX -std=c++17 -O2 -include locale -include codecvt -DFFX_GCC -D_WIN32 -I. -Ivk -Ivk/shaders -I"$O/shaders/vk" -I"$VKH" -fpermissive -w -c "$src" -o "$obj"
done
# Photon's C API over it (src/proxy_shaderapi/fsr2/fsr2_glue.cpp)
G=$ROOT/src/proxy_shaderapi/fsr2
$CXX -std=c++17 -O2 -DFFX_GCC -I. -Ivk -I"$G" -I"$VKH" -w -c "$G/fsr2_glue.cpp" -o "$O/obj/fsr2_glue.o"
rm -f "$O/libffx_fsr2_vk.a"; x86_64-w64-mingw32-ar rcs "$O/libffx_fsr2_vk.a" "$O"/obj/*.o
ls -la "$O/libffx_fsr2_vk.a"
