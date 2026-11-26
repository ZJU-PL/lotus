# SymbolicExecution Unit Tests

add_lotus_test_suite(symbolic_execution_tests
  SOURCES
    SymbolicExecutionDomainTest.cpp
    SymbolicExecutionPipelineTest.cpp
    SymbolicExecutionSpecCompatTest.cpp
    SymbolicExecutionTaintModelTest.cpp
  LINK_LIBS
    CanarySymbolicExecution
    CanaryGuardedValueFlow
    CheckerFramework
    LotusAA
    CanaryGSA
    CanaryParallel
    LLVMAnalysis
    LLVMTransformUtils)
