# Photon CPU (MODULES.md, START.md): photon_cpu.dll, loaded by the proxy shaderapidx9.dll (src/common/photon_cpu_host.h).
add_library(photon_cpu SHARED src/photon_cpu.c src/probe_bones.c src/probe_views.c src/water.c src/lua_load.c src/fs_cache.c src/physvalid.c src/jitprof.c src/waterclip.c src/tickcap.c src/dyn.c src/physgov.c src/sampler.c)
set_target_properties(photon_cpu PROPERTIES PREFIX "" OUTPUT_NAME "photon_cpu")
target_include_directories(photon_cpu PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
target_link_libraries(photon_cpu PRIVATE winmm)   # the sampler's 1 ms timer (timeBeginPeriod)

# DYN: the binary Lua module (lua_bin.c), the same file under the two names GMod looks for (garrysmod/lua/bin)
foreach(realm gmsv gmcl)
  add_library(${realm}_photon_cpu SHARED src/lua_bin.c)
  set_target_properties(${realm}_photon_cpu PROPERTIES PREFIX "" OUTPUT_NAME "${realm}_photon_cpu_win64")
endforeach()
