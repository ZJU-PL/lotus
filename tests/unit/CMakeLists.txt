include(${CMAKE_CURRENT_LIST_DIR}/UnitTestHelpers.cmake)

foreach(subsystem Alias Analysis Checker CFL Concurrency Dataflow Fuzzing IR
                  Security Solvers SymbolicExecution Transform Utils Verification)
  string(TOLOWER "${subsystem}" selection_name)
  if(selection_name IN_LIST LOTUS_SELECTED_TEST_ROOTS)
    add_subdirectory(${subsystem})
  endif()
endforeach()

# Aggregate build entry point for all unit-test executables:
# `cmake --build <dir> --target lotus_unit_tests` builds just the tests.
get_property(_lotus_unit_test_executables GLOBAL PROPERTY
    LOTUS_UNIT_TEST_EXECUTABLES)
if(_lotus_unit_test_executables)
    list(REMOVE_DUPLICATES _lotus_unit_test_executables)
endif()
add_custom_target(lotus_unit_tests DEPENDS ${_lotus_unit_test_executables})
unset(_lotus_unit_test_executables)
