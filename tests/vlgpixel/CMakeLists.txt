# Photon VLG pixel test (T3.4b): stock vs photon_vlg_ps30 combos on DXVK/lavapipe; run with scripts/run_vlgpixel.sh.
add_executable(vlgpixel vlgpixel.c)
target_include_directories(vlgpixel PRIVATE ${CMAKE_SOURCE_DIR}/src/proxy_shaderapi)
target_link_libraries(vlgpixel PRIVATE d3d9 m)
# Option A pair test (T3.4c2/A1): GMod's vs20+ps20b vs the built photon_vlg_vs20+_ps20b; scripts/run_vlgpixel.sh --pair.
add_executable(vlgpair vlgpair.c)
target_include_directories(vlgpair PRIVATE ${CMAKE_SOURCE_DIR}/src/proxy_shaderapi)
target_link_libraries(vlgpair PRIVATE d3d9 m)
# Bump pair test (lighting plan v2 P2): GMod's bump vs20+ps20b vs the built photon_bump_vs20+_ps20b; scripts/run_vlgpixel.sh --bump.
add_executable(bumppair bumppair.c)
target_include_directories(bumppair PRIVATE ${CMAKE_SOURCE_DIR}/src/proxy_shaderapi)
target_link_libraries(bumppair PRIVATE d3d9 m)
