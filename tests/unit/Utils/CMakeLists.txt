# Utility Unit Tests

add_subdirectory(Parallel)

lotus_collect_test_sources(utils_tests
  UtilsTestMain.cpp
  GraphSlicerTest.cpp
  IntervalReachabilityTest.cpp
  LLVMCallUtilsTest.cpp
  LLVMFunctionUtilsTest.cpp
  StringUtilsTest.cpp
  PathExpressionsTest.cpp
  HierarchicalClustererTest.cpp
  Graph/DotWriterTest.cpp)

set(LOTUS_UTILS_TEST_INCLUDE_DIRS)
set(LOTUS_UTILS_TEST_LIBS
    CanaryADT
    CanaryGraphWriter
    CanaryICFG
    SVFG
    CanaryLLVMUtils
    CanaryFormats
    CanaryParallel)
if(LOTUS_ENABLE_CFL)
  list(APPEND LOTUS_UTILS_TEST_LIBS CanaryCSIndexFLARE)
endif()

if(TARGET Crab)
  lotus_collect_test_sources(utils_tests CrabMachineIntDomainTest.cpp)
  list(APPEND LOTUS_UTILS_TEST_INCLUDE_DIRS
      ${CMAKE_SOURCE_DIR}/third-party/crab/include)
  list(APPEND LOTUS_UTILS_TEST_LIBS Crab)
else()
  message(STATUS "Skipping CrabMachineIntDomainTest: Crab target is not available")
endif()

add_lotus_collected_test_suite(utils_tests
  CUSTOM_MAIN
  LINK_LIBS ${LOTUS_UTILS_TEST_LIBS}
  INCLUDE_DIRS ${LOTUS_UTILS_TEST_INCLUDE_DIRS})
