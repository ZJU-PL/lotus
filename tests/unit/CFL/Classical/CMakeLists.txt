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
