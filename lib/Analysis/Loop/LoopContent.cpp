/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopContent.h"

#include "Analysis/CFG/Dominator.h"
#include "Analysis/Loop/Classification/SCCDAGAttrs.h"
#include "Analysis/Loop/Dependence/LoopCarriedDependencies.h"
#include "Analysis/Loop/Dependence/LoopDependenceBuilder.h"
#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"
#include "Analysis/Loop/Dependence/LoopSCCDAG.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/Memory/LoopIterationSpaceAnalysis.h"
#include "Analysis/Loop/Memory/MemoryCloningAnalysis.h"
#include "Analysis/Loop/Scalar/InductionVariables.h"
#include "Analysis/Loop/Scalar/LoopInvariants.h"

#include <iterator>

namespace lotus {
namespace analysis {
namespace loop {

LoopContent::LoopContent(LoopTree *loopNode) : loop{loopNode} {}

LoopContent::~LoopContent() = default;

void LoopContent::invalidateAnalysesAfterDependenceGraph(void) {
  this->memoryCloningAnalysis.reset();
  this->invalidateAnalysesAfterScalarAnalyses();
}

void LoopContent::invalidateAnalysesAfterScalarAnalyses(void) {
  this->ivDependenceGraph.reset();
  this->ivSCCDAG.reset();
  this->environment.reset();
  this->invariantManager.reset();
  this->inductionVariables.reset();
  this->iterationSpaceAnalysis.reset();
  this->sccAttrs.reset();
}

void LoopContent::materializeDependenceGraph(pdg::ProgramGraph &pdg) {
  auto bundle =
      LoopDependenceBuilder::buildBaseLoopDependenceGraph(this->loop, pdg);
  this->dependenceGraph = std::move(bundle.graph);
  this->sccdag = std::move(bundle.sccdag);
  this->dependenceGraphDebugDumps = std::move(bundle.debugSnapshots);
  this->invalidateAnalysesAfterDependenceGraph();
}

void LoopContent::buildEnvironment(
    const std::set<llvm::Value *> &excludeValues) {
  assert(this->dependenceGraph != nullptr);
  this->environment = std::make_unique<LoopEnvironment>(
      this->dependenceGraph.get(),
      this->getLoopStructure()->getLoopExitBasicBlocks(), excludeValues);
}

void LoopContent::materializeEnvironment(void) {
  if (this->environment != nullptr) {
    return;
  }
  this->materializeEnvironmentFromMemoryCloning();
}

void LoopContent::materializeLoopCarriedDependencies(
    const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  LoopCarriedDependencies::setLoopCarriedDependencies(this->loop, DS,
                                                      *this->dependenceGraph);
}

void LoopContent::materializeEnvironmentFromMemoryCloning(void) {
  if (this->environment != nullptr) {
    return;
  }

  std::set<llvm::Value *> excludeValues;
  if (this->memoryCloningAnalysis != nullptr) {
    for (auto *memoryObject :
         this->memoryCloningAnalysis->getClonableMemoryObjects()) {
      if (memoryObject == nullptr ||
          memoryObject->doPrivateCopiesNeedToBeInitialized()) {
        continue;
      }
      excludeValues.insert(memoryObject->getAllocation());
    }
  }

  this->buildEnvironment(excludeValues);
}

void LoopContent::materializeInductionVariables(
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    const LoopDependenceBuilderOptions &options) {
  if (this->ivDependenceGraph == nullptr || this->ivSCCDAG == nullptr) {
    auto loopInternalGraph =
        LoopDependenceBuilder::createInternalSubgraph(*this->dependenceGraph);
    assert(loopInternalGraph != nullptr);
    this->ivDependenceGraph =
        loopInternalGraph->createSubgraph(/*includeControl=*/true,
                                          /*includeVariable=*/true,
                                          /*includeMemory=*/false);
    assert(this->ivDependenceGraph != nullptr);
    this->ivSCCDAG = std::make_unique<LoopSCCDAG>(*this->ivDependenceGraph);
  }

  this->inductionVariables = std::make_unique<InductionVariableManager>(
      this->loop, *this->invariantManager, SE, LI, *this->ivSCCDAG,
      *this->environment, options.enableExtendedIVRecognition);
}

void LoopContent::materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                            llvm::LoopInfo &LI,
                                            const noelle::DominatorSummary &DS) {
  this->materializeScalarAnalyses(SE, LI, DS, LoopDependenceBuilderOptions{});
}

void LoopContent::materializeScalarAnalyses(
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    const noelle::DominatorSummary &DS,
    const LoopDependenceBuilderOptions &options) {
  assert(this->dependenceGraph != nullptr);

  auto refined = LoopDependenceBuilder::refineLoopDependenceGraph(
      std::move(this->dependenceGraph), SE, LI, DS, options);
  this->dependenceGraph = std::move(refined.graph);
  this->sccdag = std::move(refined.sccdag);
  this->dependenceGraphDebugDumps.insert(
      this->dependenceGraphDebugDumps.end(),
      std::make_move_iterator(refined.debugSnapshots.begin()),
      std::make_move_iterator(refined.debugSnapshots.end()));

  this->invalidateAnalysesAfterScalarAnalyses();
  if (refined.memoryCloningAnalysis != nullptr) {
    this->memoryCloningAnalysis = std::move(refined.memoryCloningAnalysis);
  } else {
    this->memoryCloningAnalysis = std::make_unique<MemoryCloningAnalysis>(
        this->getLoopStructure(), DS, this->dependenceGraph.get());
  }
  this->ivDependenceGraph = std::move(refined.ivGraph);
  this->ivSCCDAG = std::move(refined.ivSCCDAG);

  this->materializeEnvironmentFromMemoryCloning();
  this->invariantManager = std::make_unique<InvariantManager>(
      this->getLoopStructure(), this->dependenceGraph.get());
  this->materializeInductionVariables(SE, LI, options);
}

void LoopContent::materializeIterationSpaceAnalysis(llvm::ScalarEvolution &SE) {
  assert(this->inductionVariables != nullptr);
  this->iterationSpaceAnalysis = std::make_unique<LoopIterationSpaceAnalysis>(
      this->loop, *this->inductionVariables, SE);
}

void LoopContent::materializeMemoryCloningAnalysis(
    const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  if (this->memoryCloningAnalysis == nullptr) {
    this->memoryCloningAnalysis = std::make_unique<MemoryCloningAnalysis>(
        this->getLoopStructure(), DS, this->dependenceGraph.get());
  }
}

void LoopContent::materializeSCCAttrs(bool enableFloatAsReal,
                                      const noelle::DominatorSummary &DS) {
  assert(this->dependenceGraph != nullptr);
  assert(this->sccdag != nullptr);
  assert(this->inductionVariables != nullptr);
  this->materializeMemoryCloningAnalysis(DS);
  this->sccAttrs = std::make_unique<SCCDAGAttrs>(
      enableFloatAsReal, this->dependenceGraph.get(), this->sccdag.get(),
      this->loop, *this->inductionVariables, DS,
      this->memoryCloningAnalysis.get());
}

} // namespace loop
} // namespace analysis
} // namespace lotus
