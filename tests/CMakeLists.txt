cmake_minimum_required(VERSION 3.10)
find_package(LLVM REQUIRED CONFIG)
find_package(GTest REQUIRED)

add_definitions(${LLVM_DEFINITIONS})

set(COMMON_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/utils ${CMAKE_CURRENT_SOURCE_DIR}/../include ${LLVM_INCLUDE_DIRS})
set(COMMON_LIBS ${LLVM_LIBRARIES} GTest::gtest GTest::gtest_main IFDS Annotation CanaryDyckAA LLVMIRReader CanaryPDG)

add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE ${COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE ${COMMON_LIBS})

function(add_lotus_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE ${COMMON_INCLUDES})
    target_link_libraries(${test_name} lotus_test_utils ${COMMON_LIBS})
endfunction()

function(add_lotus_pdg_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_include_directories(${test_name} PRIVATE ${COMMON_INCLUDES})
    # PDG tests need additional LLVM analysis libraries
    target_link_libraries(${test_name} lotus_test_utils ${COMMON_LIBS} LLVMTransformUtils)
endfunction()

add_lotus_test(taint_analysis_test unit/TaintAnalysisTest.cpp)
add_lotus_test(ifds_solver_test unit/IFDSSolverTest.cpp)
add_lotus_test(egraph_test unit/EGraphTest.cpp)
add_lotus_pdg_test(pdg_slicing_test unit/pdg_slicing_test.cpp)

enable_testing()
add_test(NAME TaintAnalysisTest COMMAND taint_analysis_test)
add_test(NAME IFDSSolverTest COMMAND ifds_solver_test)
add_test(NAME EGraphTest COMMAND egraph_test)
add_test(NAME PDGSlicingTest COMMAND pdg_slicing_test)
set_tests_properties(TaintAnalysisTest IFDSSolverTest EGraphTest PDGSlicingTest PROPERTIES TIMEOUT 300 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_custom_target(run_all_tests COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure DEPENDS taint_analysis_test ifds_solver_test egraph_test pdg_slicing_test)
add_custom_target(run_unit_tests COMMAND ${CMAKE_CTEST_COMMAND} -R "Test$" --output-on-failure DEPENDS taint_analysis_test ifds_solver_test egraph_test pdg_slicing_test)

