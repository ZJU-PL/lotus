add_lotus_test_suite(tpa_taint_tests
  SOURCES TPATaintTest.cpp
  LINK_LIBS TPATaint TPATransforms LLVMAsmParser LLVMAnalysis LLVMTransformUtils
  COMPILE_DEFINITIONS TPA_TAINT_TEST_INPUTS="${CMAKE_CURRENT_SOURCE_DIR}/Inputs"
  TEST_KIND component)

if(TARGET lotus-check)
  add_dependencies(tpa_taint_tests lotus-check)
  add_test(NAME tpa_taint_cli COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:lotus-check>
    -DINPUTS=${CMAKE_CURRENT_SOURCE_DIR}/Inputs
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/RunCLI.cmake)
  set_tests_properties(tpa_taint_cli PROPERTIES
    LABELS "lotus;checker;component" TIMEOUT 60)
endif()
