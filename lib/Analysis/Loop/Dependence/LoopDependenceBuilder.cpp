/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Dependence/LoopDependenceBuilder.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"
#include "Analysis/CFG/Dominator.h"
#include "Analysis/Loop/Dependence/LoopCarriedDependencies.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/Memory/LoopIterationSpaceAnalysis.h"
#include "Analysis/Loop/Memory/MemoryCloningAnalysis.h"
#include "Analysis/Loop/Scalar/InductionVariables.h"
#include "Analysis/Loop/Scalar/LoopInvariants.h"
#include "IR/PDG/Core/Graph.h"
#include "llvm/IR/CFG.h"

#include <algorithm>
#include <cassert>
#include <queue>
#include <set>
#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

bool canInstructionReachWithinSameIteration(const LoopStructure *loopStructure,
                                            llvm::Instruction *from,
                                            llvm::Instruction *to) {
  if (loopStructure == nullptr || from == nullptr || to == nullptr) {
    return false;
  }
  if (!loopStructure->isIncluded(from) || !loopStructure->isIncluded(to)) {
    return false;
  }

  if (from->getParent() == to->getParent()) {
    bool seenFrom = false;
    for (auto &inst : *from->getParent()) {
      if (&inst == from) {
        seenFrom = true;
        continue;
      }
      if (seenFrom && &inst == to) {
        return true;
      }
    }
    return false;
  }

  std::queue<llvm::BasicBlock *> worklist;
  std::unordered_set<llvm::BasicBlock *> visited;
  worklist.push(from->getParent());
  visited.insert(from->getParent());
  auto *header = loopStructure->getHeader();

  while (!worklist.empty()) {
    auto *current = worklist.front();
    worklist.pop();
    for (auto *succ : llvm::successors(current)) {
      if (!loopStructure->isIncluded(succ) || succ == header) {
        continue;
      }
      if (succ == to->getParent()) {
        return true;
      }
      if (visited.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  return false;
}

bool isReadOnlyInstruction(const llvm::Instruction *instruction) {
  if (instruction == nullptr) {
    return false;
  }
  if (llvm::isa<llvm::LoadInst>(instruction)) {
    return true;
  }
  auto *call = llvm::dyn_cast<llvm::CallBase>(instruction);
  if (call == nullptr) {
    return !instruction->mayWriteToMemory();
  }
  return !call->mayWriteToMemory();
}

bool removeReadOnlyImportedMemoryDependences(LoopDependenceGraph &graph) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (edge->getKind() != LoopDependenceEdgeKind::Memory ||
        edge->getOrigin() != LoopDependenceEdgeOrigin::ImportedPDG) {
      continue;
    }

    auto *producer =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getSrc()->getValue());
    auto *consumer =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr) {
      continue;
    }

    if (edge->getMemoryKind() == LoopDependenceMemoryKind::ReadOnly ||
        (isReadOnlyInstruction(producer) && isReadOnlyInstruction(consumer))) {
      edgesToRemove.push_back(edge);
    }
  }

  for (auto *edge : edgesToRemove) {
    edge->setLoopCarried(false);
    graph.removeEdge(edge);
  }
  return !edgesToRemove.empty();
}

std::vector<llvm::Value *> collectLoopInternalValues(
    LoopStructure *loopStructure) {
  std::vector<llvm::Value *> values;
  if (loopStructure == nullptr) {
    return values;
  }

  for (auto *block : loopStructure->getBasicBlocks()) {
    for (auto &instruction : *block) {
      if (LoopDependenceGraph::shouldIgnoreInstruction(&instruction)) {
        continue;
      }
      values.push_back(&instruction);
    }
  }

  return values;
}

bool removeLoopCarriedDependencesProvedDisjoint(
    LoopDependenceGraph &graph,
    LoopStructure *loopStructure,
    LoopIterationSpaceAnalysis &analysis) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *from =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getSrc()->getValue());
    auto *to =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
    if (from == nullptr || to == nullptr) {
      continue;
    }
    if (canInstructionReachWithinSameIteration(loopStructure, from, to)) {
      continue;
    }
    if (!analysis
             .areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                 from, to)) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
  return !edgesToRemove.empty();
}

bool isThreadSafeLibraryCall(llvm::CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  if (callee == nullptr || !callee->empty()) {
    return false;
  }

  auto &specManager = LoopDependenceBuilder::getAliasSpecManager();
  if (specManager.isAllocator(callee) || specManager.isDeallocator(callee) ||
      specManager.getCategory(callee) ==
          lotus::alias::FunctionCategory::Reallocator) {
    return true;
  }

  static const std::set<std::string> threadSafeFallback{"malloc", "calloc",
                                                        "realloc", "free"};
  return threadSafeFallback.count(callee->getName().str()) != 0;
}

bool isPseudoRandomValueGenerator(llvm::CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  return callee != nullptr && callee->getName() == "rand";
}

bool removePRVGMemoryDependences(LoopDependenceGraph &graph) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (edge == nullptr || edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *src =
        llvm::dyn_cast_or_null<llvm::CallBase>(edge->getSrc()->getValue());
    auto *dst =
        llvm::dyn_cast_or_null<llvm::CallBase>(edge->getDst()->getValue());
    if (!isPseudoRandomValueGenerator(src) &&
        !isPseudoRandomValueGenerator(dst)) {
      continue;
    }
    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
  return !edgesToRemove.empty();
}

bool removeThreadSafeLibraryDependences(LoopDependenceGraph &graph) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *producer =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getSrc()->getValue());
    auto *consumer =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr || producer != consumer) {
      continue;
    }

    auto *call = llvm::dyn_cast<llvm::CallInst>(producer);
    if (!isThreadSafeLibraryCall(call)) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
  return !edgesToRemove.empty();
}

bool removeMemoryCloningNegatedDependences(LoopDependenceGraph &graph,
                                           MemoryCloningAnalysis &analysis) {
  std::vector<LoopDependenceEdge *> edgesToRemove;
  for (auto *edge : graph.getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }

    auto *producer =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getSrc()->getValue());
    auto *consumer =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
    if (producer == nullptr || consumer == nullptr) {
      continue;
    }

    auto locationsProducer = analysis.getClonableMemoryObjectsFor(producer);
    auto locationsConsumer = analysis.getClonableMemoryObjectsFor(consumer);
    if (locationsProducer.empty() || locationsConsumer.empty()) {
      continue;
    }

    bool removable = false;
    for (auto *locationProducer : locationsProducer) {
      for (auto *locationConsumer : locationsConsumer) {
        if (locationProducer->getAllocation() !=
            locationConsumer->getAllocation()) {
          continue;
        }
        bool producerStores =
            locationProducer->isInstructionStoringLocation(producer);
        bool consumerStores =
            locationConsumer->isInstructionStoringLocation(consumer);
        bool producerLoads =
            locationProducer->isInstructionLoadingLocation(producer);
        bool consumerLoads =
            locationConsumer->isInstructionLoadingLocation(consumer);

        bool isRAW = producerStores && consumerLoads;
        bool isWAR = producerLoads && consumerStores;
        bool isWAW = producerStores && consumerStores;
        if (isRAW || isWAR || isWAW) {
          removable = true;
          break;
        }
      }
      if (removable) {
        break;
      }
    }

    if (!removable) {
      continue;
    }

    edge->setLoopCarried(false);
    edgesToRemove.push_back(edge);
  }

  for (auto *edge : edgesToRemove) {
    graph.removeEdge(edge);
  }
  return !edgesToRemove.empty();
}

} // namespace

LoopDependenceBuilder::GraphBundle::~GraphBundle() = default;

lotus::alias::AliasSpecManager &LoopDependenceBuilder::getAliasSpecManager(
    void) {
  static lotus::alias::AliasSpecManager manager;
  return manager;
}

void LoopDependenceBuilderOptions::addLoopAwareRefinementPass(
    LoopDependenceRefinementPass *pass) {
  if (pass == nullptr) {
    return;
  }
  if (std::find(this->loopAwareRefinementPasses.begin(),
                this->loopAwareRefinementPasses.end(),
                pass) == this->loopAwareRefinementPasses.end()) {
    this->loopAwareRefinementPasses.push_back(pass);
  }
}

void LoopDependenceBuilderOptions::removeLoopAwareRefinementPass(
    LoopDependenceRefinementPass *pass) {
  this->loopAwareRefinementPasses.erase(
      std::remove(this->loopAwareRefinementPasses.begin(),
                  this->loopAwareRefinementPasses.end(), pass),
      this->loopAwareRefinementPasses.end());
}

void LoopDependenceBuilder::captureSnapshot(GraphBundle &bundle,
                                            const std::string &phase,
                                            const LoopDependenceGraph &graph) {
  bundle.debugSnapshots.emplace_back(phase, graph.renderStable());
}

LoopDependenceBuilder::GraphBundle
LoopDependenceBuilder::buildBaseLoopDependenceGraph(LoopTree *loopNode,
                                                   pdg::ProgramGraph &pdg) {
  GraphBundle bundle;
  bundle.graph = std::unique_ptr<LoopDependenceGraph>(new LoopDependenceGraph());
  auto &graph = *bundle.graph;
  graph.initialize(loopNode, pdg);

  auto *loopStructure = graph.getLoopStructure();
  assert(loopStructure != nullptr);

  auto loopInternalValues = collectLoopInternalValues(loopStructure);
  std::vector<llvm::Instruction *> loopInstructions;
  std::unordered_set<llvm::Value *> loopInternalValueSet;
  loopInstructions.reserve(loopInternalValues.size());
  for (auto *value : loopInternalValues) {
    loopInternalValueSet.insert(value);
    if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
      loopInstructions.push_back(instruction);
    }
  }

  std::sort(loopInstructions.begin(), loopInstructions.end(),
            [](llvm::Instruction *lhs, llvm::Instruction *rhs) {
              if (lhs->getParent()->getName() != rhs->getParent()->getName()) {
                return lhs->getParent()->getName() <
                       rhs->getParent()->getName();
              }
              return LoopDependenceGraph::describeValue(lhs) <
                     LoopDependenceGraph::describeValue(rhs);
            });

  for (auto *instruction : loopInstructions) {
    auto *pdgNode = pdg.getNode(*instruction);
    graph.fetchOrCreateNode(instruction, pdgNode, true);
  }

  std::unordered_set<pdg::Edge *> seenEdges;
  for (auto *instruction : loopInstructions) {
    auto *pdgNode = pdg.getNode(*instruction);
    if (pdgNode == nullptr) {
      continue;
    }

    for (auto *edge : pdgNode->getOutEdgeSet()) {
      graph.importEdgeIfIncluded(edge, /*linkToExternal=*/true,
                                 loopInternalValueSet, seenEdges);
    }
    for (auto *edge : pdgNode->getInEdgeSet()) {
      graph.importEdgeIfIncluded(edge, /*linkToExternal=*/true,
                                 loopInternalValueSet, seenEdges);
    }
  }

  captureSnapshot(bundle, "base", graph);
  bundle.sccdag = std::make_unique<LoopSCCDAG>(graph);
  return bundle;
}

std::unique_ptr<LoopDependenceGraph>
LoopDependenceBuilder::createInternalSubgraph(const LoopDependenceGraph &graph) {
  std::vector<llvm::Value *> loopInternals;
  loopInternals.reserve(graph.internalNodePairs().size());
  for (const auto &pair : graph.internalNodePairs()) {
    loopInternals.push_back(pair.first);
  }
  return graph.createSubgraphFromValues(loopInternals,
                                        /*linkToExternal=*/false);
}

std::unique_ptr<LoopSCCDAG>
LoopDependenceBuilder::computeSCCDAGWithOnlyVariableAndControlDependences(
    const LoopDependenceGraph &graph) {
  auto internalGraph = LoopDependenceBuilder::createInternalSubgraph(graph);
  if (internalGraph == nullptr) {
    return nullptr;
  }

  auto variableControlGraph =
      internalGraph->createSubgraph(/*includeControl=*/true,
                                    /*includeVariable=*/true,
                                    /*includeMemory=*/false);
  if (variableControlGraph == nullptr) {
    return nullptr;
  }
  return std::make_unique<LoopSCCDAG>(*variableControlGraph);
}

LoopDependenceBuilder::GraphBundle
LoopDependenceBuilder::refineLoopDependenceGraph(
    std::unique_ptr<LoopDependenceGraph> graph,
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    const noelle::DominatorSummary &DS,
    const LoopDependenceBuilderOptions &options) {
  GraphBundle bundle;
  bundle.graph = std::move(graph);
  auto &resolvedGraph = *bundle.graph;
  auto *loopNode = resolvedGraph.getLoopHierarchyStructures();
  assert(loopNode != nullptr);

  LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                      resolvedGraph);

  if (options.assumePseudoRandomValueGeneratorsNonDeterministic &&
      removePRVGMemoryDependences(resolvedGraph)) {
    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                        resolvedGraph);
  }

  if (options.enableAffineIterationSpaceRefinement) {
    InvariantManager temporaryInvariants(loopNode->getLoop(), &resolvedGraph);
    auto temporaryInternalGraph = createInternalSubgraph(resolvedGraph);
    auto ivGraph = temporaryInternalGraph->createSubgraph(
        /*includeControl=*/true, /*includeVariable=*/true,
        /*includeMemory=*/false);
    auto ivSCCDAG = std::make_unique<LoopSCCDAG>(*ivGraph);
    LoopEnvironment temporaryEnvironment(
        &resolvedGraph, loopNode->getLoop()->getLoopExitBasicBlocks());
    InductionVariableManager temporaryIVs(
        loopNode, temporaryInvariants, SE, LI, *ivSCCDAG, temporaryEnvironment);
    LoopIterationSpaceAnalysis temporaryIterationSpace(loopNode, temporaryIVs,
                                                       SE);

    if (removeLoopCarriedDependencesProvedDisjoint(
            resolvedGraph, loopNode->getLoop(), temporaryIterationSpace)) {
      LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                          resolvedGraph);
    }

    if (options.loopAwareRefinementPasses.empty()) {
      bundle.ivGraph = std::move(ivGraph);
      bundle.ivSCCDAG = std::move(ivSCCDAG);
    }
  }
  captureSnapshot(bundle, "after_affine", resolvedGraph);

  bool dirtyLoopCarried = false;
  if (options.enableLoopAwareDependenceAnalyses) {
    if (options.hasLoopAwareDependenceBackend &&
        removeReadOnlyImportedMemoryDependences(resolvedGraph)) {
      dirtyLoopCarried = true;
    }

    for (auto *pass : options.loopAwareRefinementPasses) {
      if (pass == nullptr) {
        continue;
      }
      if (dirtyLoopCarried) {
        LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                            resolvedGraph);
        dirtyLoopCarried = false;
      }
      pass->refine(resolvedGraph, loopNode, SE, LI, DS);
      dirtyLoopCarried = true;
    }
  }
  if (dirtyLoopCarried) {
    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                        resolvedGraph);
    dirtyLoopCarried = false;
  }
  captureSnapshot(bundle, "after_loop_aware", resolvedGraph);

  if (options.enableMemoryCloningRefinement) {
    bundle.memoryCloningAnalysis = std::make_unique<MemoryCloningAnalysis>(
        loopNode->getLoop(), DS, &resolvedGraph);
    if (removeMemoryCloningNegatedDependences(resolvedGraph,
                                              *bundle.memoryCloningAnalysis)) {
      dirtyLoopCarried = true;
    }
  }
  captureSnapshot(bundle, "after_memory_cloning", resolvedGraph);

  if (options.enableThreadSafeLibraryRefinement) {
    if (dirtyLoopCarried) {
      LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                          resolvedGraph);
      dirtyLoopCarried = false;
    }
    if (removeThreadSafeLibraryDependences(resolvedGraph)) {
      dirtyLoopCarried = true;
    }
  }
  if (dirtyLoopCarried) {
    LoopCarriedDependencies::setLoopCarriedDependencies(loopNode, DS,
                                                        resolvedGraph);
  }
  captureSnapshot(bundle, "after_thread_safe_library", resolvedGraph);

  bundle.sccdag = std::make_unique<LoopSCCDAG>(resolvedGraph);
  captureSnapshot(bundle, "final", resolvedGraph);

  return bundle;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
