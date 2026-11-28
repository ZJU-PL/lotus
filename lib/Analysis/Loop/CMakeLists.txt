add_library(CanaryLoopStructure STATIC
  LoopStructure.cpp
)
target_link_libraries(CanaryLoopStructure
  PUBLIC
    LLVMAnalysis
)

set(CanaryLoopAnalysis_SOURCES
  LoopForest.cpp
  LoopEnvironment.cpp
  LoopContent.cpp
  FunctionLoopAnalyses.cpp
  LoopNestingGraph.cpp
  Dependence/LoopDependenceGraph.cpp
  Dependence/LoopDependenceBuilder.cpp
  Dependence/LoopSCCDAG.cpp
  Dependence/LoopCarriedDependencies.cpp
  Scalar/LoopInvariants.cpp
  Scalar/InductionVariable.cpp
  Scalar/InductionVariables.cpp
  Scalar/LoopGoverningInductionVariable.cpp
  Scalar/ScalarEvolutionReferencer.cpp
  Memory/LoopIterationSpaceAnalysis.cpp
  Memory/MemoryCloningAnalysis.cpp
  Classification/LoopCarriedVariable.cpp
  Classification/LoopSCCClassification.cpp
  Classification/SCCDAGAttrs.cpp
)

add_library(CanaryLoopAnalysis STATIC ${CanaryLoopAnalysis_SOURCES})
target_link_libraries(CanaryLoopAnalysis
  PUBLIC
    CanaryLoopStructure
  PRIVATE
    CanaryCFG
    CanaryPDGCore
    CanaryPDGConstruction
    AliasSpecManager
    LLVMAnalysis
)
