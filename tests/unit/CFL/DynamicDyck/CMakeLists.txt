# Tests mirroring lib/CFL/DynamicDyck: the weighted-quotient solver plus the
# primary-component refinement on top of it.
lotus_collect_test_sources(cfl_tests
  WeightedQuotientTest.cpp
  PrimaryComponentIntegrationTest.cpp)

add_subdirectory(PrimaryComponent)
