# Eight flat directories mirror the eight PDG static libraries 1:1.
# Dependencies form a strict DAG; link only the layer a consumer uses.

add_library(CanaryPDGCore STATIC
  Core/CallWrapper.cpp
  Core/DebugInfoUtils.cpp
  Core/FunctionWrapper.cpp
  Core/Graph.cpp
  Core/PDGCommandLineOptions.cpp
  Core/PDGNode.cpp
  Core/PDGUtils.cpp
  Core/Tree.cpp
)
target_link_libraries(CanaryPDGCore PUBLIC LLVMCore LLVMSupport)

add_library(CanaryPDGConstruction STATIC
  Construction/ControlDependencyGraph.cpp
  Construction/DataDependencyGraph.cpp
  Construction/GraphBuilder.cpp
  Construction/PDGCallGraph.cpp
  Construction/ProgramDependencyGraph.cpp
)
target_link_libraries(CanaryPDGConstruction PUBLIC
  CanaryPDGCore CanaryControlDependence CanaryAliasAnalysisWrapper)

add_library(CanaryPDGQuery STATIC
  Query/Criteria.cpp
  Query/DataFlowQuery.cpp
  Query/DependenceQuery.cpp
  Query/DiffQuery.cpp
  Query/ImpactQuery.cpp
  Query/PropertyCriteria.cpp
  Query/PropertySpec.cpp
  Query/QuerySupport.cpp
  Query/QueryTypes.cpp
  Query/ResourceFlowQuery.cpp
  Query/SliceQuery.cpp
  Query/SummaryQuery.cpp
  Query/TransformQuery.cpp
  Query/Traversal.cpp
)
target_link_libraries(CanaryPDGQuery PUBLIC CanaryPDGCore LLVMAnalysis)

# Reusable LLVM facts, C/C++ library models, and shared finding/rule types.
# Independent of graph traversal, rule orchestration, and the Cypher front end.
add_library(CanaryPDGModels STATIC
  Models/CppLibraryModels.cpp
  Models/FunctionFacts.cpp
  Models/LibraryModels.cpp
  Models/TaintModels.cpp
  Models/ValueFacts.cpp
)
target_link_libraries(CanaryPDGModels PUBLIC LLVMAnalysis LLVMCore)

add_library(CanaryPDGAnalyses STATIC
  Analyses/ApiAnalysis.cpp
  Analyses/ArithmeticAnalysis.cpp
  Analyses/BoundsAnalysis.cpp
  Analyses/CallContractAnalysis.cpp
  Analyses/LifetimeAnalysis.cpp
  Analyses/ProtocolAnalysis.cpp
  Analyses/StateAnalysis.cpp
  Analyses/TaintAnalysis.cpp
)
target_link_libraries(CanaryPDGAnalyses PUBLIC
  CanaryPDGCore CanaryPDGModels)

add_library(CanaryPDGRules STATIC
  Rules/RuleCatalog.cpp
  Rules/RuleEngine.cpp
)
target_link_libraries(CanaryPDGRules PUBLIC CanaryPDGQuery CanaryPDGAnalyses)

add_library(CanaryPDGCypher STATIC
  Cypher/CypherExecutor.cpp
  Cypher/CypherParser.cpp
  Cypher/CypherResult.cpp
)
target_link_libraries(CanaryPDGCypher PUBLIC CanaryPDGQuery CanaryPDGAnalyses)

add_library(CanaryPDGVisualization STATIC
  Visualization/GraphWriter.cpp
)
target_link_libraries(CanaryPDGVisualization PUBLIC CanaryPDGConstruction)
