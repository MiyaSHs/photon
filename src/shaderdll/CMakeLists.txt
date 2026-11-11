# photon_shaders.dll: Photon's base shader DLL, loaded via the proxy's HWConfig slot-73 hook (analysis/10, 11).
set(PHOTON_SHADERDLL_SRC shaderdll.c baseshader.c log.c shaders/photon_debug.c shaders/photon_debug30.c photon_shaders.def)
add_library(photon_shaders SHARED ${PHOTON_SHADERDLL_SRC})
set_target_properties(photon_shaders PROPERTIES PREFIX "" OUTPUT_NAME "photon_shaders")
target_include_directories(photon_shaders PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Test variant for tests/shaderhost only (never installed): adds the filter-test clones (VertexLitGeneric,
# LightmappedGeneric, g_bloom) behind PHOTON_TEST_FILTER=1. The release DLL does not contain them.
add_library(photon_shaders_test SHARED ${PHOTON_SHADERDLL_SRC})
set_target_properties(photon_shaders_test PROPERTIES PREFIX "" OUTPUT_NAME "photon_shaders_test")
target_include_directories(photon_shaders_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(photon_shaders_test PRIVATE PHOTON_TEST_CLONES=1)
