cmake_minimum_required(VERSION 3.20)

project(ResourceEnvelope
  VERSION 1.0.0
  DESCRIPTION "Facility-level resource envelope runtime for DCCP Tranche 6"
  HOMEPAGE_URL "https://github.com/summonlabs/Resource-Envelope"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RESOURCE_ENVELOPE_BUILD_TESTS "Build the Resource Envelope test suite" ON)
option(RESOURCE_ENVELOPE_BUILD_BENCHMARKS "Build the Resource Envelope benchmark driver" ON)
option(RESOURCE_ENVELOPE_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(RESOURCE_ENVELOPE_ENABLE_SANITIZERS "Enable Address/Undefined sanitizers when the toolchain genuinely supports them" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

# ---------------------------------------------------------------------------
# Compiler capability probes. Probed flags are selected by trying to compile a
# trivial translation unit, so an unsupported flag is never silently passed.
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

set(RESOURCE_ENVELOPE_HARDENING_FLAGS "")
set(RESOURCE_ENVELOPE_SANITIZER_FLAGS "")
set(RESOURCE_ENVELOPE_HAS_ADDRESS_SANITIZER FALSE)

function(re_probe_flag out_var flag)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_QUIET TRUE)
  set(CMAKE_REQUIRED_FLAGS "${flag}")
  check_cxx_source_compiles("int main() { return 0; }" "${out_var}")
  cmake_pop_check_state()
endfunction()

function(re_probe_link_flag out_var flag)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_QUIET TRUE)
  set(CMAKE_REQUIRED_FLAGS "${flag}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${flag}")
  check_cxx_source_compiles("int main() { return 0; }" "${out_var}")
  cmake_pop_check_state()
endfunction()

if(RESOURCE_ENVELOPE_ENABLE_SANITIZERS)
  if(MSVC)
    re_probe_flag(RE_MSVC_ASAN_SUPPORTED "/fsanitize=address")
    if(RE_MSVC_ASAN_SUPPORTED)
      set(RESOURCE_ENVELOPE_SANITIZER_FLAGS "/fsanitize=address")
      set(RESOURCE_ENVELOPE_HAS_ADDRESS_SANITIZER TRUE)
    endif()
  else()
    re_probe_link_flag(RE_GNU_ASAN_SUPPORTED "-fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
    if(RE_GNU_ASAN_SUPPORTED)
      set(RESOURCE_ENVELOPE_SANITIZER_FLAGS "-fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer")
      set(RESOURCE_ENVELOPE_HAS_ADDRESS_SANITIZER TRUE)
    endif()
  endif()
  if(NOT RESOURCE_ENVELOPE_HAS_ADDRESS_SANITIZER)
    message(WARNING "Sanitizers were requested but the active toolchain does not support them; continuing without sanitizer instrumentation.")
  endif()
endif()

if(MSVC)
  re_probe_link_flag(RE_MSVC_CET_SUPPORTED "/guard:cf")
  if(RE_MSVC_CET_SUPPORTED)
    list(APPEND RESOURCE_ENVELOPE_HARDENING_FLAGS "/guard:cf")
  endif()
endif()

if(NOT MSVC)
  re_probe_link_flag(RE_GNU_STACK_PROTECTOR "-fstack-protector-strong")
  if(RE_GNU_STACK_PROTECTOR)
    list(APPEND RESOURCE_ENVELOPE_HARDENING_FLAGS "-fstack-protector-strong")
  endif()
  re_probe_link_flag(RE_GNU_FORTIFY "-D_FORTIFY_SOURCE=2")
  if(RE_GNU_FORTIFY)
    list(APPEND RESOURCE_ENVELOPE_HARDENING_FLAGS "-D_FORTIFY_SOURCE=2")
  endif()
endif()

# ---------------------------------------------------------------------------
# Warning policy: strict, first-party only, no warnings leaked by our headers.
# ---------------------------------------------------------------------------
if(MSVC)
  set(RE_WARN_FLAGS /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline /EHsc /GR- /sdl /wd4127)
  # /wd4127: the canonical encoder deliberately uses constant conditional
  # expressions in template helpers, so the construct is intentional.
  if(RESOURCE_ENVELOPE_WARNINGS_AS_ERRORS)
    list(APPEND RE_WARN_FLAGS /WX)
  endif()
  set(RE_WARN_DEFINITIONS _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  set(RE_WARN_FLAGS
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
    -Wcast-qual -Wcast-align -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wunused -Wundef
    -Wredundant-decls -Wstrict-aliasing=2 -Wwrite-strings -Wpointer-arith)
  if(RESOURCE_ENVELOPE_WARNINGS_AS_ERRORS)
    list(APPEND RE_WARN_FLAGS -Werror)
  endif()
  set(RE_WARN_DEFINITIONS "")
endif()

if(RESOURCE_ENVELOPE_HAS_ADDRESS_SANITIZER)
  list(APPEND RE_WARN_DEFINITIONS RE_SANITIZED)
endif()

# Applies the strict first-party warning policy to one target. The policy is applied
# with PRIVATE scope so that no warning flag, hardening flag or sanitizer flag is ever
# propagated to a consumer of the installed package: warnings are a property of how
# this repository is built, not of its interface.
function(resource_envelope_apply_warnings target)
  target_compile_options(${target} PRIVATE
    ${RE_WARN_FLAGS} ${RESOURCE_ENVELOPE_HARDENING_FLAGS} ${RESOURCE_ENVELOPE_SANITIZER_FLAGS})
  target_compile_definitions(${target} PRIVATE ${RE_WARN_DEFINITIONS})
  target_compile_features(${target} PRIVATE cxx_std_20)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(resource_envelope
  src/core/bytes.cpp
  src/core/status.cpp
  src/core/digest.cpp
  src/core/text.cpp
  src/core/time.cpp
  src/core/quantity.cpp
  src/envelope/dimension.cpp
  src/envelope/envelope.cpp
  src/envelope/canonical.cpp
  src/envelope/record.cpp
  src/eval/evaluate.cpp
  src/store/framing.cpp
  src/store/manifest.cpp
  src/store/file_lock.cpp
  src/store/store_io.cpp
  src/store/store.cpp
  src/service/service.cpp)

add_library(ResourceEnvelope::resource_envelope ALIAS resource_envelope)

target_include_directories(resource_envelope
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

resource_envelope_apply_warnings(resource_envelope)
target_compile_features(resource_envelope PUBLIC cxx_std_20)
set_target_properties(resource_envelope PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME resource_envelope)

if(MSVC)
  # Do not leak our build-tree include paths or the warning interface onto
  # consumers of the installed package.
  target_compile_options(resource_envelope PRIVATE /bigobj)
endif()

find_package(Threads REQUIRED)
target_link_libraries(resource_envelope PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(resource-envelope src/cli/main.cpp src/cli/options.cpp)
add_executable(ResourceEnvelope::cli ALIAS resource-envelope)
target_link_libraries(resource-envelope PRIVATE resource_envelope)
resource_envelope_apply_warnings(resource-envelope)

# ---------------------------------------------------------------------------
# Tests and benchmarks
# ---------------------------------------------------------------------------
if(RESOURCE_ENVELOPE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RESOURCE_ENVELOPE_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS resource_envelope resource-envelope
  EXPORT ResourceEnvelopeTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

install(EXPORT ResourceEnvelopeTargets
  NAMESPACE ResourceEnvelope::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResourceEnvelope)

configure_package_config_file(
  cmake/ResourceEnvelopeConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/ResourceEnvelopeConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResourceEnvelope)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/ResourceEnvelopeConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/ResourceEnvelopeConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/ResourceEnvelopeConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ResourceEnvelope)

install(FILES README.md LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})

message(STATUS "ResourceEnvelope ${PROJECT_VERSION}: warnings-as-errors=${RESOURCE_ENVELOPE_WARNINGS_AS_ERRORS} sanitizers=${RESOURCE_ENVELOPE_HAS_ADDRESS_SANITIZER}")
