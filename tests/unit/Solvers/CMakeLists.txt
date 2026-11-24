add_subdirectory(EGraph)
add_subdirectory(Datalog)
add_subdirectory(Automata)

set(LOTUS_SOLVER_SMT_TEST_SOURCES SymAbsTest.cpp SMTSamplerTest.cpp)
set(LOTUS_SOLVER_SMT_TEST_LIBS SMTSampler SymAbs ${Z3_LIBRARIES})
set(LOTUS_SOLVER_SMT_TEST_DEFINITIONS)

if(LOTUS_ENABLE_SMT_STABILIZER)
  list(APPEND LOTUS_SOLVER_SMT_TEST_SOURCES SMTStabilizerTest.cpp)
  list(APPEND LOTUS_SOLVER_SMT_TEST_LIBS LotusSMTStabilizer)
  list(APPEND LOTUS_SOLVER_SMT_TEST_DEFINITIONS
      "SMT_STABILIZER_TEST_INPUT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/SMTStabilizerInputs\"")
endif()

lotus_collect_test_sources(solvers_tests ${LOTUS_SOLVER_SMT_TEST_SOURCES})

# All solver test suites are linked into one binary.
set(LOTUS_SOLVERS_TEST_LIBS
    SMTSampler
    SymAbs
    LotusDatalogFrontend
    LotusEGraph
    ${Z3_LIBRARIES})
if(LOTUS_ENABLE_SMT_STABILIZER)
  list(APPEND LOTUS_SOLVERS_TEST_LIBS LotusSMTStabilizer)
endif()

add_lotus_collected_test_suite(solvers_tests
  LINK_LIBS ${LOTUS_SOLVERS_TEST_LIBS}
  COMPILE_DEFINITIONS ${LOTUS_SOLVER_SMT_TEST_DEFINITIONS})

# Preserve the established build entry point.
add_custom_target(solver_tests DEPENDS solvers_tests)
