find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME pdg_example_queries
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_examples.py"
    --tool "$<TARGET_FILE:lotus-ir-pdg-query>")
lotus_label_regression_tests(ir)
