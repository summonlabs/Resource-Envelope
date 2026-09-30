cmake_minimum_required(VERSION 3.20)
project(ResourceEnvelopeConsumer LANGUAGES CXX)

# The consumer finds the installed package through find_package only. No source directory,
# header path or build tree of the library is referenced anywhere in this project.
find_package(ResourceEnvelope 1.0 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ResourceEnvelope::resource_envelope)
target_compile_features(consumer PRIVATE cxx_std_20)
set_target_properties(consumer PROPERTIES CXX_EXTENSIONS OFF)
