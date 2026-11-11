# d3dcompile.exe: HLSL -> D3D9 bytecode (and disassembly) with a native d3dcompiler_47.dll under Wine
# (analysis/scratch/m3_vcs.md §3). Used by scripts/build_shaders.sh; links nothing but KERNEL32 + UCRT.
add_executable(d3dcompile d3dcompile.c)
