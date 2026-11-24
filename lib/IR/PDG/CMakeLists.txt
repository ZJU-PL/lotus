# Keep the graph representation usable without construction, semantic rules,
# or the Cypher front end. Link the layer a consumer actually uses.
add_library(CanaryPDGCore STATIC
  Core/CallWrapper.cpp
  Core/FunctionWrapper.cpp
  Core/Graph.cpp
  Core/PDGNode.cpp
  Core/Tree.cpp
  Support/DebugInfoUtils.cpp
  Support/DebugOptions.cpp
  Support/PDGUtils.cpp
)
target_link_libraries(CanaryPDGCore PUBLIC LLVMCore LLVMSupport)

add_library(CanaryPDGConstruction STATIC
  Core/ControlDependencyGraph.cpp
  Core/DataDependencyGraph.cpp
  Core/GraphBuilder.cpp
  Core/PDGCallGraph.cpp
  Core/ProgramDependencyGraph.cpp
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

# Facts and models cooperate in resolving LLVM semantics. They do not depend
# on graph traversal, rule orchestration, or a query-language front end.
add_library(CanaryPDGSemanticSupport STATIC
  Analysis/FunctionFacts.cpp
  Analysis/ValueFacts.cpp
  Analysis/CppLibraryModels.cpp
  Analysis/LibraryModels.cpp
  Analysis/TaintModels.cpp
)
target_link_libraries(CanaryPDGSemanticSupport PUBLIC LLVMAnalysis LLVMCore)

add_library(CanaryPDGSemantics STATIC
  Analysis/ApiQuery.cpp
  Analysis/ArithmeticQuery.cpp
  Analysis/BoundsQuery.cpp
  Analysis/CallContractQuery.cpp
  Analysis/LifetimeQuery.cpp
  Analysis/ProtocolQuery.cpp
  Analysis/StateQuery.cpp
  Analysis/TaintQuery.cpp
)
target_link_libraries(CanaryPDGSemantics PUBLIC
  CanaryPDGCore CanaryPDGSemanticSupport)

add_library(CanaryPDGRules STATIC
  Analysis/RuleCatalog.cpp
  Analysis/RuleQuery.cpp
)
target_link_libraries(CanaryPDGRules PUBLIC CanaryPDGQuery CanaryPDGSemantics)

add_library(CanaryPDGCypher STATIC
  QueryLanguage/CypherExecutor.cpp
  QueryLanguage/CypherResult.cpp
  QueryLanguage/CypherParser.cpp
)
target_link_libraries(CanaryPDGCypher PUBLIC CanaryPDGQuery CanaryPDGSemantics)

add_library(CanaryPDGVisualization STATIC Support/GraphWriter.cpp)
target_link_libraries(CanaryPDGVisualization PUBLIC CanaryPDGConstruction)
