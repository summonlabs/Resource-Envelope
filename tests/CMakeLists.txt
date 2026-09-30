# Unit, integration, property, adversarial and end-to-end tests.
#
# Every executable is registered as its own CTest test, so a failure names the suite
# that failed instead of collapsing into one aggregate result. The sanitizer and
# warnings policies applied to the library are applied to every test target as well.

add_library(resource_envelope_testkit STATIC support/main.cpp)
target_include_directories(resource_envelope_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(resource_envelope_testkit PUBLIC resource_envelope)
resource_envelope_apply_warnings(resource_envelope_testkit)
if(MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  # Iterator debugging and runtime checks stay on in every non-Release build, so the Debug
  # validation is genuinely stronger than the Release validation. Release cannot carry
  # _ITERATOR_DEBUG_LEVEL=2: the Standard Library refuses that combination.
  target_compile_definitions(resource_envelope_testkit PUBLIC _ITERATOR_DEBUG_LEVEL=2)
endif()

set(RESOURCE_ENVELOPE_TEST_SOURCES
  core/test_quantity.cpp
  core/test_digest.cpp
  core/test_text.cpp
  core/test_time.cpp
  eval/test_evaluate.cpp
  integration/test_service_store.cpp
  adversarial/test_hostile_inputs.cpp
  property/test_canonical_invariants.cpp
  recovery/test_crash_recovery.cpp)

foreach(source ${RESOURCE_ENVELOPE_TEST_SOURCES})
  get_filename_component(name ${source} NAME_WE)
  get_filename_component(group ${source} DIRECTORY)
  set(target "re_test_${group}_${name}")
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE resource_envelope_testkit)
  resource_envelope_apply_warnings(${target})
  add_test(NAME ${group}.${name} COMMAND ${target})
  set_tests_properties(${group}.${name} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

