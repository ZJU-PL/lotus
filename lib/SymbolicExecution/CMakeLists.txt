set(CanarySymbolicExecution_SOURCES
  Support/AnalysisContext.cpp
  Support/AnalysisLimit.cpp
  Support/HashUtil.cpp
  Domain/BigInteger.cpp
  Domain/ProgramVar.cpp
  Domain/PropertyInteger.cpp
  Domain/PropertySym.cpp
  Domain/PropertyValue.cpp
  Domain/SymbolicMemory.cpp
  Models/MemoryAPI.cpp
  Models/TaintModel.cpp
  Solver/ConstraintRepr.cpp
  Solver/PathCondSolver.cpp
  Solver/SummarySolverManager.cpp
  Engine/AnalysisDriver.cpp
  Engine/AnalysisState.cpp
  Engine/AnalysisStateSummary.cpp
  Engine/AnalysisStateTaint.cpp
  Engine/AnalysisSummary.cpp
  Engine/CStringState.cpp
  Engine/GuardedValue.cpp
  Checks/ArithmeticChecks.cpp
  Checks/MemoryChecks.cpp
  Checks/Query.cpp
  Checks/QueryEngine.cpp
  Integration/SymbolicExecutionWrapper.cpp
)

add_library(CanarySymbolicExecution STATIC ${CanarySymbolicExecution_SOURCES})

target_link_libraries(CanarySymbolicExecution PUBLIC
  CanaryGuardedValueFlow
  CanaryFormats
  CanarySMT
  CanaryParallel
  CheckerFramework
  LLVMCore
)
