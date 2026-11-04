cmake_minimum_required(VERSION 3.18)
find_package(LLVM REQUIRED CONFIG)
# GTest should already be found or downloaded by FindGTest.cmake in parent CMakeLists.txt
# Try find_package, but if targets already exist (from add_subdirectory), that's fine too

find_package(GTest QUIET)
if(NOT TARGET GTest::gtest AND NOT TARGET gtest)
    message(FATAL_ERROR "GTest not found. This should have been handled by FindGTest.cmake")
endif()

add_definitions(${LLVM_DEFINITIONS})
add_definitions(-DCMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set(LOTUS_UNIT_TEST_TIMEOUT 60 CACHE STRING
    "Default timeout in seconds for Lotus unit tests registered with CTest")
set(LOTUS_TEST_DISCOVERY_TIMEOUT 60 CACHE STRING
    "Default timeout in seconds for gtest_discover_tests enumeration")

# Analyses load default specs relative to the test working directory. Keep
# out-of-tree test builds independent of the source directory's parent layout.
foreach(spec ptr modref taint)
    configure_file(${PROJECT_SOURCE_DIR}/config/${spec}.spec
        ${CMAKE_BINARY_DIR}/config/${spec}.spec COPYONLY)
endforeach()

# Test sources, CLI regressions, and workflow tests have separate entry points.
include(cmake/RegressionTestHelpers.cmake)
add_subdirectory(unit)
add_subdirectory(regress)
add_subdirectory(integration)
add_test(NAME build_selection
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildSelectionTest.cmake)
add_test(NAME test_labels
    COMMAND ${CMAKE_COMMAND}
        -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestLabelsTest.cmake)
set_tests_properties(build_selection test_labels PROPERTIES
    LABELS "lotus;unit;build" TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT})

# Custom target to run all tests
add_custom_target(run_all_tests 
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom target to run only unit tests.
# Depends on lotus_unit_tests so the test binaries are (re)built first when
# driving `cmake --build <dir> --target run_unit_tests`.
add_custom_target(run_unit_tests
    DEPENDS lotus_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L unit --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(run_component_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L component --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L integration --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
