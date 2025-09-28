cmake_minimum_required(VERSION 3.10)
find_package(LLVM REQUIRED CONFIG)
find_package(GTest REQUIRED)

set(CMAKE_CXX_STANDARD 17)
add_definitions(${LLVM_DEFINITIONS})

set(COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/utils ${CMAKE_CURRENT_SOURCE_DIR}/../include ${LLVM_INCLUDE_DIRS})
set(COMMON_LIBS ${LLVM_LIBRARIES} GTest::gtest GTest::gtest_main IFDS Annotation CanaryDyckAA LLVMCore LLVMIRReader LLVMSupport)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/test_data)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test_data/ DESTINATION ${CMAKE_BINARY_DIR}/tests/test_data/)

add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE ${COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE ${COMMON_LIBS})

function(add_lotus_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(${test_name} lotus_test_utils ${COMMON_LIBS})
endfunction()

add_lotus_test(taint_analysis_test unit/TaintAnalysisTest.cpp)
add_lotus_test(ifds_solver_test unit/IFDSSolverTest.cpp)

enable_testing()
add_test(NAME TaintAnalysisTest COMMAND taint_analysis_test)
add_test(NAME IFDSSolverTest COMMAND ifds_solver_test)
set_tests_properties(TaintAnalysisTest IFDSSolverTest PROPERTIES TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_custom_target(run_all_tests COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS taint_analysis_test ifds_solver_test)
add_custom_target(run_unit_tests COMMAND ${CMAKE_CTEST_COMMAND} -R "Test$" --output-on-failure DEPENDS taint_analysis_test ifds_solver_test)

function(compile_test_to_llvm cpp_file output_ll)
    add_custom_command(OUTPUT ${output_ll} COMMAND ${CMAKE_CXX_COMPILER} -S -emit-llvm -g -O0 -Xclang -disable-O0-optnone -I${CMAKE_CURRENT_SOURCE_DIR}/../include ${cpp_file} -o ${output_ll} DEPENDS ${cpp_file} VERBATIM)
endfunction()

set(TAINT_TEST_FILES simple_taint.cpp taint_sanitization.cpp taint_branching.cpp)
foreach(test_file IN LISTS TAINT_TEST_FILES)
    get_filename_component(file_name ${test_file} NAME_WE)
    compile_test_to_llvm(${CMAKE_CURRENT_SOURCE_DIR}/test_data/taint/${test_file} ${CMAKE_BINARY_DIR}/tests/test_data/taint/${file_name}.ll)
    add_custom_target(${file_name}_ll DEPENDS ${CMAKE_BINARY_DIR}/tests/test_data/taint/${file_name}.ll)
    add_dependencies(taint_analysis_test ${file_name}_ll)
endforeach()
