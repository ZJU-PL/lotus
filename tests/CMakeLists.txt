# Lotus Testing Infrastructure
# Based on PhASAR testing framework, adapted for Lotus IFDS engine

cmake_minimum_required(VERSION 3.14)

# Find required packages
find_package(LLVM REQUIRED CONFIG)
find_package(GTest REQUIRED)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/utils)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)
include_directories(${LLVM_INCLUDE_DIRS})

# Add LLVM definitions
add_definitions(${LLVM_DEFINITIONS})

# Compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test configuration
set(LOTUS_BUILD_DIR ${CMAKE_BINARY_DIR})
set(LOTUS_SRC_DIR ${CMAKE_SOURCE_DIR})

# Create test data directory
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/test_data)

# Copy test data files
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test_data/
     DESTINATION ${CMAKE_BINARY_DIR}/tests/test_data/)

# ============================================================================
# Test Utilities Library
# ============================================================================

add_library(lotus_test_utils STATIC
    utils/TestConfig.h
    utils/SourceLocationEntry.h
    utils/IFDSTestFixture.h
)

target_link_libraries(lotus_test_utils
    ${LLVM_LIBRARIES}
    GTest::gtest
    GTest::gtest_main
)

# ============================================================================
# Unit Tests
# ============================================================================

# Taint Analysis Tests
add_executable(taint_analysis_test
    unit/TaintAnalysisTest.cpp
)

target_link_libraries(taint_analysis_test
    lotus_test_utils
    sparta_ifds
    dyck_aa
    ${LLVM_LIBRARIES}
    GTest::gtest
    GTest::gtest_main
)

# Reaching Definitions Tests (placeholder)
add_executable(reaching_definitions_test
    unit/ReachingDefinitionsTest.cpp
)

target_link_libraries(reaching_definitions_test
    lotus_test_utils
    sparta_ifds
    dyck_aa
    ${LLVM_LIBRARIES}
    GTest::gtest
    GTest::gtest_main
)

# ============================================================================
# Integration Tests
# ============================================================================

add_executable(ifds_integration_test
    integration/IFDSIntegrationTest.cpp
)

target_link_libraries(ifds_integration_test
    lotus_test_utils
    sparta_ifds
    dyck_aa
    ${LLVM_LIBRARIES}
    GTest::gtest
    GTest::gtest_main
)

# ============================================================================
# Test Discovery and Execution
# ============================================================================

# Enable testing
enable_testing()

# Add tests to CTest
add_test(NAME TaintAnalysisTest COMMAND taint_analysis_test)
add_test(NAME ReachingDefinitionsTest COMMAND reaching_definitions_test)
add_test(NAME IFDSIntegrationTest COMMAND ifds_integration_test)

# Set test properties
set_tests_properties(TaintAnalysisTest PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(ReachingDefinitionsTest PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(IFDSIntegrationTest PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ============================================================================
# Custom Targets
# ============================================================================

# Target to run all tests
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS taint_analysis_test reaching_definitions_test ifds_integration_test
    COMMENT "Running all Lotus IFDS tests"
)

# Target to run unit tests only
add_custom_target(run_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "Test$" --output-on-failure
    DEPENDS taint_analysis_test reaching_definitions_test
    COMMENT "Running unit tests"
)

# Target to run integration tests only
add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "Integration" --output-on-failure
    DEPENDS ifds_integration_test
    COMMENT "Running integration tests"
)

# Target to generate test coverage (if enabled)
option(LOTUS_ENABLE_COVERAGE "Enable test coverage reporting" OFF)
if(LOTUS_ENABLE_COVERAGE)
    find_program(GCOV_PATH gcov)
    find_program(LCOV_PATH lcov)
    find_program(GENHTML_PATH genhtml)
    
    if(GCOV_PATH AND LCOV_PATH AND GENHTML_PATH)
        add_custom_target(coverage
            COMMAND ${LCOV_PATH} --directory . --capture --output-file coverage.info
            COMMAND ${LCOV_PATH} --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage.info.cleaned
            COMMAND ${GENHTML_PATH} -o coverage coverage.info.cleaned
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating test coverage report"
        )
    endif()
endif()

# ============================================================================
# Test Data Compilation
# ============================================================================

# Function to compile C++ test files to LLVM IR
function(compile_test_to_llvm cpp_file output_ll)
    get_filename_component(file_name ${cpp_file} NAME_WE)
    get_filename_component(file_dir ${cpp_file} DIRECTORY)
    
    add_custom_command(
        OUTPUT ${output_ll}
        COMMAND ${CMAKE_CXX_COMPILER} -S -emit-llvm -g -O0 -Xclang -disable-O0-optnone
                -I${CMAKE_CURRENT_SOURCE_DIR}/../include
                ${cpp_file} -o ${output_ll}
        DEPENDS ${cpp_file}
        COMMENT "Compiling ${cpp_file} to LLVM IR"
        VERBATIM
    )
endfunction()

# Compile test data files
compile_test_to_llvm(
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data/taint/simple_taint.cpp
    ${CMAKE_BINARY_DIR}/tests/test_data/taint/simple_taint.ll
)

compile_test_to_llvm(
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data/taint/taint_sanitization.cpp
    ${CMAKE_BINARY_DIR}/tests/test_data/taint/taint_sanitization.ll
)

compile_test_to_llvm(
    ${CMAKE_CURRENT_SOURCE_DIR}/test_data/taint/taint_branching.cpp
    ${CMAKE_BINARY_DIR}/tests/test_data/taint/taint_branching.ll
)

# Make sure test data is compiled before running tests
add_dependencies(taint_analysis_test simple_taint.ll taint_sanitization.ll taint_branching.ll)
