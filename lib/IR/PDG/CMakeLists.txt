# Program Dependency Graph (PDG) CMake configuration
#
# This file configures the build for the PDG module, which implements
# a comprehensive program dependency graph for LLVM-based analysis.
# 

add_library(CanaryPDG STATIC
  Core/CallWrapper.cpp
  Core/ControlDependencyGraph.cpp
  Core/DataDependencyGraph.cpp
  Core/FunctionWrapper.cpp
  Core/Graph.cpp
  Core/PDGCallGraph.cpp
  Core/PDGNode.cpp
  Core/ProgramDependencyGraph.cpp
  Core/Tree.cpp
  QueryLanguage/Execution/CypherExecutor.cpp
  QueryLanguage/Execution/CypherResult.cpp
  QueryLanguage/Parser/CypherParser.cpp
  Analysis/Query.cpp
  Analysis/QueryCore.cpp
  Analysis/QuerySupport.cpp
  Analysis/SliceQuery.cpp
  Analysis/DependenceQuery.cpp
  Analysis/DataFlowQuery.cpp
  Analysis/TransformQuery.cpp
  Analysis/DiffQuery.cpp
  Analysis/SummaryQuery.cpp
  Analysis/ImpactQuery.cpp
  Analysis/ResourceFlowQuery.cpp
  Analysis/RuleQuery.cpp
  Analysis/ValueFacts.cpp
  Analysis/LibraryModels.cpp
  Analysis/FunctionFacts.cpp
  Analysis/BoundsQuery.cpp
  Analysis/ArithmeticQuery.cpp
  Analysis/ApiQuery.cpp
  Analysis/ProtocolQuery.cpp
  Analysis/CallContractQuery.cpp
  Analysis/CppLibraryModels.cpp
  Analysis/LifetimeQuery.cpp
  Analysis/StateQuery.cpp
  Analysis/TaintModels.cpp
  Analysis/TaintQuery.cpp
  Analysis/PropertySpec.cpp
  Support/DebugInfoUtils.cpp
  Support/GraphWriter.cpp
  Support/PDGUtils.cpp
)

target_link_libraries(CanaryPDG PUBLIC CanaryControlDependence)
