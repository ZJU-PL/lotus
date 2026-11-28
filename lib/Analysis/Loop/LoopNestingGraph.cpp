/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopNestingGraph.h"

#include "IR/PDG/Construction/PDGCallGraph.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <cassert>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

std::unordered_set<const llvm::Function *>
collectReachableFunctions(pdg::PDGCallGraph &callGraph,
                          llvm::Function *entryFunction) {
  std::unordered_set<const llvm::Function *> reachableFunctions;
  if (entryFunction == nullptr) {
    return reachableFunctions;
  }

  std::queue<const llvm::Function *> worklist;
  worklist.push(entryFunction);
  reachableFunctions.insert(entryFunction);
  while (!worklist.empty()) {
    auto *current = worklist.front();
    worklist.pop();
    auto *currentNode =
        callGraph.getNode(*const_cast<llvm::Function *>(current));
    if (currentNode == nullptr) {
      continue;
    }

    for (auto *succ : currentNode->getOutNeighbors()) {
      auto *callee = llvm::dyn_cast_or_null<llvm::Function>(succ->getValue());
      if (callee == nullptr || !reachableFunctions.insert(callee).second) {
        continue;
      }
      worklist.push(callee);
    }
  }

  return reachableFunctions;
}

std::map<const llvm::Function *, std::unordered_set<LoopStructure *>>
collectOutermostLoops(std::vector<LoopStructure *> const &allLoops) {
  std::map<const llvm::Function *, std::unordered_set<LoopStructure *>>
      outermostLoops;
  for (auto *loop : allLoops) {
    if (loop->getNestingLevel() == 1) {
      outermostLoops[loop->getFunction()].insert(loop);
    }
  }
  return outermostLoops;
}

} // namespace

std::set<LoopNestingGraphEdge *>
LoopNestingGraphLoopNode::getIncomingEdges(void) const {
  return this->incomingEdges;
}

std::set<LoopNestingGraphEdge *>
LoopNestingGraphLoopNode::getOutgoingEdges(void) const {
  return this->outgoingEdges;
}

void LoopNestingGraphLoopNode::addIncomingEdge(LoopNestingGraphEdge *edge) {
  this->incomingEdges.insert(edge);
}

void LoopNestingGraphLoopNode::addOutgoingEdge(LoopNestingGraphEdge *edge) {
  this->outgoingEdges.insert(edge);
}

LoopNestingGraphLoopLoopEdge *LoopNestingGraphLoopNode::getNestingEdgeTo(
    LoopNestingGraphLoopNode *target) const {
  for (auto *edge : this->outgoingEdges) {
    auto *loopEdge = dynamic_cast<LoopNestingGraphLoopLoopEdge *>(edge);
    if (loopEdge != nullptr && loopEdge->getDst() == target) {
      return loopEdge;
    }
  }
  return nullptr;
}

void LoopNestingGraphLoopLoopEdge::addSubEdge(
    LoopNestingGraphInstructionLoopEdge *edge) {
  this->subEdges.push_back(edge);
}

std::vector<LoopNestingGraphInstructionLoopEdge *>
LoopNestingGraphLoopLoopEdge::getSubEdges(void) const {
  return this->subEdges;
}

LoopNestingGraph::LoopNestingGraph(
    std::vector<LoopStructure *> const &loops) {
  this->ownedLoopNodes.reserve(loops.size());
  for (auto *loop : loops) {
    auto node = std::make_unique<LoopNestingGraphLoopNode>(loop);
    auto *rawNode = node.get();
    this->ownedLoopNodes.push_back(std::move(node));
    this->loops[loop] = rawNode;
  }
}

LoopNestingGraph::~LoopNestingGraph() = default;

std::unordered_set<LoopNestingGraphLoopNode *>
LoopNestingGraph::getLoopNodes(void) const {
  std::unordered_set<LoopNestingGraphLoopNode *> nodes;
  for (const auto &pair : this->loops) {
    nodes.insert(pair.second);
  }
  return nodes;
}

std::unordered_set<LoopNestingGraphEdge *>
LoopNestingGraph::getEdges(void) const {
  std::unordered_set<LoopNestingGraphEdge *> edgeSet;
  for (const auto &pair : this->edges) {
    for (auto *edge : pair.second) {
      edgeSet.insert(edge);
    }
  }
  return edgeSet;
}

LoopNestingGraphLoopNode *LoopNestingGraph::getLoopNode(
    LoopStructure *loop) const {
  auto it = this->loops.find(loop);
  return it == this->loops.end() ? nullptr : it->second;
}

LoopNestingGraphLoopNode *LoopNestingGraph::getEntryNode(
    llvm::Function *entryFunction) const {
  for (const auto &pair : this->loops) {
    if (pair.first->getFunction() != entryFunction) {
      continue;
    }
    if (pair.second->getIncomingEdges().empty()) {
      return pair.second;
    }
  }
  return nullptr;
}

void LoopNestingGraph::createEdge(LoopStructure *from,
                                  llvm::CallBase *callInst,
                                  LoopStructure *child,
                                  bool isMust) {
  auto fromIt = this->loops.find(from);
  assert(fromIt != this->loops.end());
  this->fetchOrCreateEdge(fromIt->second, callInst, child, isMust);
}

LoopNestingGraphLoopLoopEdge *LoopNestingGraph::fetchOrCreateEdge(
    LoopNestingGraphLoopNode *fromNode,
    llvm::CallBase *callInst,
    LoopStructure *child,
    bool isMust) {
  auto *toNode = this->loops.at(child);

  LoopNestingGraphInstructionNode *instNode = nullptr;
  if (callInst != nullptr) {
    auto &slot = this->instructionNodes[callInst];
    if (slot == nullptr) {
      auto ownedInst =
          std::make_unique<LoopNestingGraphInstructionNode>(callInst);
      slot = ownedInst.get();
      this->ownedInstructionNodes.push_back(std::move(ownedInst));
    }
    instNode = slot;
  }

  auto ownedSubEdge = std::make_unique<LoopNestingGraphInstructionLoopEdge>(
      instNode, toNode, isMust);
  auto *subEdge = ownedSubEdge.get();
  this->ownedEdges.push_back(std::move(ownedSubEdge));

  auto *existing = fromNode->getNestingEdgeTo(toNode);
  if (existing == nullptr) {
    auto ownedLoopEdge =
        std::make_unique<LoopNestingGraphLoopLoopEdge>(fromNode, toNode, isMust);
    auto *newEdge = ownedLoopEdge.get();
    this->ownedEdges.push_back(std::move(ownedLoopEdge));
    this->edges[fromNode].insert(newEdge);
    fromNode->addOutgoingEdge(newEdge);
    toNode->addIncomingEdge(newEdge);
    newEdge->addSubEdge(subEdge);
    return newEdge;
  }

  if (isMust) {
    existing->setMust();
  }
  existing->addSubEdge(subEdge);
  return existing;
}

std::unique_ptr<LoopNestingGraph> LoopNestingGraph::buildFromAnalyses(
    std::vector<FunctionLoopAnalyses *> const &analyses,
    llvm::Module &module,
    llvm::Function *entryFunction) {
  pdg::PDGCallGraph callGraph;
  callGraph.build(module);

  auto reachableFunctions = collectReachableFunctions(callGraph, entryFunction);

  std::vector<LoopStructure *> allLoops;
  for (auto *analysis : analyses) {
    if (entryFunction != nullptr &&
        reachableFunctions.count(analysis->getFunction()) == 0) {
      continue;
    }
    auto loops = analysis->getLoopStructures();
    allLoops.insert(allLoops.end(), loops.begin(), loops.end());
  }

  auto graph = std::make_unique<LoopNestingGraph>(allLoops);

  for (auto *analysis : analyses) {
    auto *forest = analysis->getLoopForest();
    if (forest == nullptr) {
      continue;
    }
    for (auto *tree : forest->getTrees()) {
      tree->visitPreOrder([&](LoopTree *node, uint32_t) -> bool {
        if (node->getParent() != nullptr) {
          graph->createEdge(node->getParent()->getLoop(), nullptr,
                            node->getLoop(), true);
        }
        return false;
      });
    }
  }

  auto outermostLoops = collectOutermostLoops(allLoops);

  for (auto *analysis : analyses) {
    auto *callerFunction = analysis->getFunction();
    auto *callerForest = analysis->getLoopForest();
    if (callerFunction == nullptr || callerForest == nullptr) {
      continue;
    }
    if (entryFunction != nullptr &&
        reachableFunctions.count(callerFunction) == 0) {
      continue;
    }

    for (auto &bb : *callerFunction) {
      for (auto &inst : bb) {
        auto *callBase = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (callBase == nullptr) {
          continue;
        }

        auto *loopNode = callerForest->getInnermostLoopThatContains(callBase);
        if (loopNode == nullptr) {
          continue;
        }
        auto *parentLoop = loopNode->getLoop();

        if (auto *callee = callBase->getCalledFunction()) {
          if (entryFunction != nullptr &&
              reachableFunctions.count(callee) == 0) {
            continue;
          }
          auto it = outermostLoops.find(callee);
          if (it == outermostLoops.end()) {
            continue;
          }
          for (auto *outermostLoop : it->second) {
            graph->createEdge(parentLoop, callBase, outermostLoop, true);
          }
          continue;
        }

        auto candidates = callGraph.getIndirectCallCandidates(*callBase, module);
        for (auto *callee : candidates) {
          if (entryFunction != nullptr &&
              reachableFunctions.count(callee) == 0) {
            continue;
          }
          auto it = outermostLoops.find(callee);
          if (it == outermostLoops.end()) {
            continue;
          }
          for (auto *outermostLoop : it->second) {
            graph->createEdge(parentLoop, callBase, outermostLoop, false);
          }
        }
      }
    }
  }

  return graph;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
