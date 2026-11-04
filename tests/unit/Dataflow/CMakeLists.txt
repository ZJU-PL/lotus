# Dataflow Unit Tests

set(dataflow_test_libs)
foreach(directory IfdsIde Mono WPDS APA DemandAPA NPA VASCO)
  string(TOLOWER "${directory}" group)
  lotus_test_group_enabled(dataflow "${group}" selected)
  if(NOT selected)
    continue()
  endif()
  if(group STREQUAL "demandapa" AND NOT TARGET DemandAPA)
    continue()
  endif()
  add_subdirectory(${directory})
  if(group STREQUAL "ifdside")
    list(APPEND dataflow_test_libs IFDS CanaryAliasAnalysisWrapper)
  elseif(group STREQUAL "mono")
    list(APPEND dataflow_test_libs MONODataFlow)
  elseif(group STREQUAL "wpds")
    list(APPEND dataflow_test_libs WPDSDataFlow wpds++)
  elseif(group STREQUAL "apa")
    list(APPEND dataflow_test_libs APADataFlow LotusEGraph)
  elseif(group STREQUAL "demandapa")
    list(APPEND dataflow_test_libs DemandAPA)
  elseif(group STREQUAL "npa")
    list(APPEND dataflow_test_libs NPADataFlow CanaryAliasAnalysisWrapper CanaryParallel)
  elseif(group STREQUAL "vasco")
    list(APPEND dataflow_test_libs VASCODataFlow CanaryParallel)
  endif()
endforeach()

# All dataflow test sources collected from the subdirectories above are linked
# into one binary so the shared libraries are linked only once.
lotus_collect_test_sources(dataflow_tests DataflowTestMain.cpp)
add_lotus_collected_test_suite(dataflow_tests
  CUSTOM_MAIN
  LINK_LIBS ${dataflow_test_libs})

lotus_test_group_enabled(dataflow vasco vasco_selected)
if(vasco_selected)
  add_test(NAME dataflow_vasco_parallel_harness_workers
    COMMAND $<TARGET_FILE:dataflow_tests> -nworkers=2
      --gtest_filter=VASCOParallelHarnessTest.*)
  set_tests_properties(dataflow_vasco_parallel_harness_workers PROPERTIES
    TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
    LABELS "lotus;component;dataflow;parallel"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
