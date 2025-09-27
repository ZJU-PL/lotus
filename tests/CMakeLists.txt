# Lotus Testing Infrastructure
cmake_minimum_required(VERSION 3.10)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

# Find GTest
find_package(GTest REQUIRED)
set(GTEST_LIBRARIES GTest::gtest GTest::gtest_main)

# Configuration
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(${LLVM_DEFINITIONS})

# Common include directories and libraries
set(COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/utils ${CMAKE_CURRENT_SOURCE_DIR}/../include ${LLVM_INCLUDE_DIRS})
set(COMMON_LIBS 
    ${LLVM_LIBRARIES} 
    ${GTEST_LIBRARIES} 
    IFDS 
    Checker 
    CanaryDyckAA
    LLVMCore
    LLVMIRReader
    LLVMSupport
)

# Test data setup
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/test_data)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test_data/ DESTINATION ${CMAKE_BINARY_DIR}/tests/test_data/)

# Test utilities library (header-only)
add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE ${COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE ${COMMON_LIBS})

# Function to create test executables
function(add_lotus_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(${test_name} lotus_test_utils ${COMMON_LIBS})
endfunction()

# Create all tests
add_lotus_test(taint_analysis_test unit/TaintAnalysisTest.cpp)

# Test discovery and execution
enable_testing()

# Add test to CTest
add_test(NAME TaintAnalysisTest COMMAND taint_analysis_test)
set_tests_properties(TaintAnalysisTest PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom targets
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS taint_analysis_test
    COMMENT "Running all Lotus IFDS tests"
)

add_custom_target(run_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "Test$" --output-on-failure
    DEPENDS taint_analysis_test 
    COMMENT "Running unit tests"
)

# Test data compilation
function(compile_test_to_llvm cpp_file output_ll)
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
set(TAINT_TEST_FILES
    simple_taint.cpp
    taint_sanitization.cpp
    taint_branching.cpp
)

# Create custom targets for test data compilation
foreach(test_file IN LISTS TAINT_TEST_FILES)
    get_filename_component(file_name ${test_file} NAME_WE)
    set(output_ll ${CMAKE_BINARY_DIR}/tests/test_data/taint/${file_name}.ll)
    compile_test_to_llvm(
        ${CMAKE_CURRENT_SOURCE_DIR}/test_data/taint/${test_file}
        ${output_ll}
    )
    add_custom_target(${file_name}_ll DEPENDS ${output_ll})
    add_dependencies(taint_analysis_test ${file_name}_ll)
endforeach()
