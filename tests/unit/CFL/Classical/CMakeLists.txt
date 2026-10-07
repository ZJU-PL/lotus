# Tests mirroring lib/CFL/Classical: core grammar/graph, the solver session,
# and the individual solver engines.
lotus_collect_test_sources(cfl_tests
  CoreTest.cpp
  ArchitectureTest.cpp
  AdaptersTest.cpp
  CatIeoceEngineTest.cpp
  CertCFLEngineTest.cpp
  EndpointQuotientAdvancedTest.cpp
  EndpointQuotientEngineTest.cpp
  EndpointQuotientStagingTest.cpp
  SkewedTabulationEngineTest.cpp
  SubcubicAATest.cpp)

# CLI/regression coverage for the lotus-cfl-solve, lotus-cfl-alias, and
# lotus-cfl-vf drivers built from this subsystem.
include(${CMAKE_CURRENT_LIST_DIR}/CLI.cmake)
