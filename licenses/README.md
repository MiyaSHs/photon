# Third-party code shipped inside Photon's binaries

- `FidelityFX-FSR2.txt`: AMD FidelityFX Super Resolution 2.2.1 (MIT), statically linked into shaderapidx9.dll when built with
  scripts/build_fsr2.sh (the upscaler behind photon_upscale 3; source: https://github.com/GPUOpen-Effects/FidelityFX-FSR2, tag v2.2.1).
- Khronos Vulkan-Headers (Apache 2.0, headers only, src/external/vulkan-headers): see its own LICENSE there.

# Third-party code in this repository but not in the release package

- `LuaJIT.txt`: LuaJIT (MIT). `modules/cpu/luajit/*.patch` are patches to it, and `modules/cpu/lua_shared/` builds an optional
  replacement `lua_shared.dll` from it (a test build; not part of the release package yet). LuaJIT's source is fetched at build time.
- VPhysics-Jolt / Jolt Physics (MIT): `modules/cpu/patches/vphysics_jolt_pair_prefilter.patch` is a patch to
  https://github.com/schweiziske/GMod-Jolt-Physics and its Jolt submodule. The installer can download the official VPhysics-Jolt
  release (optional).
- GModPatchTool (GPL-3): downloaded by the installer from its own release if the user chooses; never bundled.

Everything else in this repository is Photon's own code, under the MIT No Attribution license in `LICENSE`.
