add_library(smoke_dll SHARED smoke_dll.c smoke_dll.def)
set_target_properties(smoke_dll PROPERTIES PREFIX "" OUTPUT_NAME "smoke_dll")
add_executable(smoke smoke_exe.c)
