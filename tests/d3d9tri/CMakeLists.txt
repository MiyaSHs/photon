# D3D9 clear+triangle+readback test; run with scripts/run_d3d9tri.sh (DXVK on lavapipe).
add_executable(d3d9tri d3d9tri.c)
target_link_libraries(d3d9tri PRIVATE d3d9)
