# The benchmark driver measures completed operations only. It is not part of the installed
# package and it is not registered as a test.
add_executable(resource-envelope-benchmark benchmark_main.cpp)
target_link_libraries(resource-envelope-benchmark PRIVATE resource_envelope)
resource_envelope_apply_warnings(resource-envelope-benchmark)
set_target_properties(resource-envelope-benchmark PROPERTIES CXX_EXTENSIONS OFF)
