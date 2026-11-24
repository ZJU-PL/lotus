# IFDS/IDE Unit Tests

lotus_collect_test_sources(dataflow_tests
  PhasarMigrationTest.cpp
  LibCSummaryTest.cpp
  DFISolverTest.cpp
  AnalysisSessionTest.cpp
  SharedAnalysisServicesTest.cpp
  IFDSFrameworkCoreTest.cpp
  IFDSSolverTest.cpp
  TaintAnalysisTest.cpp
  IFDSSolverInvariantsTest.cpp
  PathAwareSolverTest.cpp
  ReturnSiteAndConfigTest.cpp
  IDEEdgeFunctionTest.cpp
  IDEConstantPropagationTest.cpp
  IDEJumpFunctionJoinTest.cpp
  IDENewAnalysesTest.cpp
  IDEExtendedTaintMemoryAnalysisTest.cpp
  IterativeIDESolverTest.cpp
  LLVMFlowHelpersTest.cpp
  LLVMFlowHelpersMappingTest.cpp
  IFDSClientAliasFlowTest.cpp
  IFDSSolverAliasIntegrationTest.cpp)
