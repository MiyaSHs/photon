add_executable(proxyhost proxyhost.c vlgtest.c ../../src/ambient/amb_bake.c ../../src/ambient/amb_clip.c)
# The mock's engine.dll for the photon_ambient 1 runs: in its own directory, because the game's real one sits next to the proxy in the test bin dir
add_library(fake_engine SHARED fake_engine.c)
set_target_properties(fake_engine PROPERTIES PREFIX "" OUTPUT_NAME "engine"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_engine"
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_engine"
                      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_engine")
