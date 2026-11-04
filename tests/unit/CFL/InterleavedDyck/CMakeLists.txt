# Tests mirroring lib/CFL/InterleavedDyck (SPDS, LCL, AffineSPDS, MCFL, Unary,
# and StagedBounds engines).
lotus_collect_test_sources(cfl_tests
  AffineSPDSTest.cpp
  LCLTest.cpp
  MCFLTest.cpp
  SolverComparisonTest.cpp
  SPDSTest.cpp
  SPDSValueFlowTest.cpp
  StagedBoundsEngineTest.cpp
  StagedBoundsTest.cpp
  UnaryAdaptiveTest.cpp
  UnaryFixedCounterTest.cpp)
