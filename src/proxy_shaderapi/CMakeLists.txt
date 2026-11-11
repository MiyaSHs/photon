# Photon logging/forwarding proxy for shaderapidx9.dll (FEASIBILITY.md section 8).
add_library(proxy_shaderapi SHARED proxy.c log.c wrap.c frames.c slot_names.c diag.c d3dconst.c vlg.c ambvol.c ambwork.c cubemap.c cubework.c p6.c p6sun.c dmirror.c texstream.c vkbridge.c vktest.c passtime.c dynq.c taa.c fsr2.c ptrt.c pbrpack.c hiz.c packs.c texfilter.c channels.c fog.c nanite.c cvar.c ../lod/mtl_tex.c ../lod/mtl_pbr.c ../ambient/bsp_pak.c ../ambient/vox_world.c ../ambient/vox_cascade.c ../ambient/vox_props.c ../ambient/bsp_props.c ../lod/mdl_mesh.c cull.c waterq.c lodwork.c l4work.c ../ambient/l4_bake.c ../lod/lod_model.c ../lod/lod_simplify.c clprobe.c shadows.c mqueue.c lights.c lgwork.c l3work.c ../ambient/l3_bake.c ../ambient/amb_bake.c ../ambient/amb_clip.c ../ambient/amb_gma.c ../ambient/amb_track.c ../ambient/amb_pick.c ../ambient/lg_map.c ../ambient/lg_light.c ../ambient/lg_grid.c ../ambient/sun_vis.c proxy_shaderapi.def)
set_target_properties(proxy_shaderapi PROPERTIES PREFIX "" OUTPUT_NAME "shaderapidx9")
target_link_libraries(proxy_shaderapi PRIVATE bcrypt psapi)
# Test-only mutant (tests/proxyhost, scripts/run_proxyhost.sh): vlg.c with PHOTON_VLG_MUTANT=1 skips the pair reverts
# and the TakeSnapshot pairing check, so the mock host's pairing invariant must catch it. Built into mutant/ and never
# installed (scripts/install_proxy.sh takes build/src/proxy_shaderapi/shaderapidx9.dll); it lights nothing outside the
# mock host (PHOTON_TEST_VLG unset).
add_library(proxy_shaderapi_mutant SHARED proxy.c log.c wrap.c frames.c slot_names.c diag.c d3dconst.c vlg.c ambvol.c ambwork.c cubemap.c cubework.c p6.c p6sun.c dmirror.c texstream.c vkbridge.c vktest.c passtime.c dynq.c taa.c fsr2.c ptrt.c pbrpack.c hiz.c packs.c texfilter.c channels.c fog.c nanite.c cvar.c ../lod/mtl_tex.c ../lod/mtl_pbr.c ../ambient/bsp_pak.c ../ambient/vox_world.c ../ambient/vox_cascade.c ../ambient/vox_props.c ../ambient/bsp_props.c ../lod/mdl_mesh.c cull.c waterq.c lodwork.c l4work.c ../ambient/l4_bake.c ../lod/lod_model.c ../lod/lod_simplify.c clprobe.c shadows.c mqueue.c lights.c lgwork.c l3work.c ../ambient/l3_bake.c ../ambient/amb_bake.c ../ambient/amb_clip.c ../ambient/amb_gma.c ../ambient/amb_track.c ../ambient/amb_pick.c ../ambient/lg_map.c ../ambient/lg_light.c ../ambient/lg_grid.c ../ambient/sun_vis.c proxy_shaderapi.def)
target_compile_definitions(proxy_shaderapi_mutant PRIVATE PHOTON_VLG_MUTANT=1)
set_target_properties(proxy_shaderapi_mutant PROPERTIES PREFIX "" OUTPUT_NAME "shaderapidx9"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mutant"
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mutant"
                      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mutant")
target_link_libraries(proxy_shaderapi_mutant PRIVATE bcrypt psapi)
# VK0: Vulkan headers for the DXVK interop bridge (vkbridge.c; Khronos Vulkan-Headers, vendored)
target_include_directories(proxy_shaderapi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/vulkan-headers/include)
target_include_directories(proxy_shaderapi_mutant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/vulkan-headers/include)
# UPSCALE U4: AMD FSR 2.2 (scripts/build_fsr2.sh builds third_party/fsr2_build/libffx_fsr2_vk.a; without it photon_upscale 3 falls back to TAAU)
set(PHOTON_FSR2_LIB ${CMAKE_SOURCE_DIR}/third_party/fsr2_build/libffx_fsr2_vk.a)
if(EXISTS ${PHOTON_FSR2_LIB})
  target_compile_definitions(proxy_shaderapi PRIVATE PHOTON_FSR2=1)
  target_include_directories(proxy_shaderapi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(proxy_shaderapi PRIVATE ${PHOTON_FSR2_LIB} -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic)
  message(STATUS "proxy: FSR 2 built in (${PHOTON_FSR2_LIB})")
endif()
