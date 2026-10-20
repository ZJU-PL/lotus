# Workflow tests require the launchers configured by scripts/ikos.
if(NOT LOTUS_ENABLE_IKOS_API_SPEC OR
   NOT "verification" IN_LIST LOTUS_SELECTED_TEST_ROOTS)
  return()
endif()

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(LOTUS_IKOS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/third-party/ikos-api-spec")

add_test(NAME ikos_api_spec_regressions
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${LOTUS_IKOS_SOURCE_DIR}/tool/api-spec/tests" -v)
add_test(NAME ikos_api_spec_integration
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/integration/ikos_api_spec.py"
    --bin-dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    --llvm-bin "${LOTUS_IKOS_LLVM_BIN}")
set_tests_properties(ikos_api_spec_regressions ikos_api_spec_integration PROPERTIES
  ENVIRONMENT "LOTUS_IKOS_LLVM_BIN=${LOTUS_IKOS_LLVM_BIN};LOTUS_IKOS_BIN=${CMAKE_RUNTIME_OUTPUT_DIRECTORY};LOTUS_IKOS_HAS_APRON=${LOTUS_IKOS_ENABLE_APRON};PYTHONDONTWRITEBYTECODE=1"
  LABELS "lotus;verification;integration;ikos-api-spec" TIMEOUT 180)
if(LOTUS_ENABLE_IKOS)
  add_test(NAME ikos_analyzer_integration
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/integration/ikos_analyzer.py"
      --bin-dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      --llvm-bin "${LOTUS_IKOS_LLVM_BIN}")
  set_tests_properties(ikos_analyzer_integration PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    LABELS "lotus;verification;integration;ikos" TIMEOUT 240)
endif()
