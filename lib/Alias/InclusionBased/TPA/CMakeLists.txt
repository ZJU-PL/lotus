add_subdirectory(Transforms)
add_subdirectory(Util)

set(PointerAnalysisSourceCodes
  PointerAnalysis/Analysis/GlobalPointerAnalysis.cpp
  PointerAnalysis/Analysis/SemiSparsePointerAnalysis.cpp
  Context/AdaptiveContext.cpp
  Context/Context.cpp
  Context/ContextPolicy.cpp
  Context/KLimitContext.cpp
  PointerAnalysis/Engine/ExternalCallAnalysis.cpp
  PointerAnalysis/Engine/Initializer.cpp
  PointerAnalysis/Engine/ParallelSolver.cpp
  PointerAnalysis/Engine/SemiSparsePropagator.cpp
  PointerAnalysis/Engine/StorePruner.cpp
  PointerAnalysis/Engine/TransferFunction.cpp
  PointerAnalysis/Engine/Transfer/AllocTransfer.cpp
  PointerAnalysis/Engine/Transfer/CallTransfer.cpp
  PointerAnalysis/Engine/Transfer/CopyTransfer.cpp
  PointerAnalysis/Engine/Transfer/EntryTransfer.cpp
  PointerAnalysis/Engine/Transfer/LoadTransfer.cpp
  PointerAnalysis/Engine/Transfer/OffsetTransfer.cpp
  PointerAnalysis/Engine/Transfer/ReturnTransfer.cpp
  PointerAnalysis/Engine/Transfer/StoreTransfer.cpp
  PointerAnalysis/FrontEnd/Type/ArrayLayoutAnalysis.cpp
  PointerAnalysis/FrontEnd/CFG/CFGBuilder.cpp
  PointerAnalysis/FrontEnd/CFG/CFGSimplifier.cpp
  PointerAnalysis/FrontEnd/CFG/FunctionTranslator.cpp
  PointerAnalysis/FrontEnd/CFG/InstructionTranslator.cpp
  PointerAnalysis/FrontEnd/Type/PointerLayoutAnalysis.cpp
  PointerAnalysis/FrontEnd/CFG/PriorityAssigner.cpp
  PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.cpp
  PointerAnalysis/FrontEnd/Type/StructCastAnalysis.cpp
  PointerAnalysis/FrontEnd/Type/TypeAnalysis.cpp
  PointerAnalysis/FrontEnd/Type/TypeCollector.cpp
  PointerAnalysis/MemoryModel/Type/ArrayLayout.cpp
  PointerAnalysis/MemoryModel/MemoryManager.cpp
  PointerAnalysis/MemoryModel/Type/PointerLayout.cpp
  PointerAnalysis/MemoryModel/PointerManager.cpp
  PointerAnalysis/MemoryModel/Type/TypeLayout.cpp
  Util/IO/PointerAnalysis/CFGNodePrinter.cpp
  Util/IO/Context/ContextPrinter.cpp
  Util/IO/PointerAnalysis/MemoryPrinter.cpp
  Util/IO/PointerAnalysis/NodePrinter.cpp
  Util/IO/PointerAnalysis/WriteDotFile.cpp
  PointerAnalysis/Precision/PrecisionLossTracker.cpp
  PointerAnalysis/Precision/ValueDependenceTracker.cpp
  PointerAnalysis/Program/CFG/CFG.cpp
  PointerAnalysis/Program/SemiSparseProgram.cpp
  PointerAnalysis/Support/PtsSet.cpp
  PointerAnalysis/Support/Store.cpp
  PointerAnalysis/StaticFields.cpp
)

add_library(FSCSPointerAnalysis STATIC ${PointerAnalysisSourceCodes})
target_link_libraries(FSCSPointerAnalysis PUBLIC Annotation)
find_package(Threads REQUIRED)
target_link_libraries(FSCSPointerAnalysis PRIVATE Threads::Threads)
add_library(Lotus::TPA ALIAS FSCSPointerAnalysis)
