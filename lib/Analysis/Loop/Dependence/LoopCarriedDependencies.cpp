/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Dependence/LoopCarriedDependencies.h"

#include "Analysis/CFG/Dominator.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

#include <cassert>
#include <queue>
#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

void LoopCarriedDependencies::setLoopCarriedDependencies(
    LoopTree *loopNode,
    const noelle::DominatorSummary &DS,
    LoopDependenceGraph &loopDG) {
  for (auto *edge : loopDG.getEdges()) {
    edge->setLoopCarried(false);
  }

  for (auto *edge : loopDG.getEdges()) {
    if (isALoopCarriedDependence(loopNode, DS, edge)) {
      edge->setLoopCarried(true);
    }
  }
}

bool LoopCarriedDependencies::isALoopCarriedDependence(
    LoopTree *loopNode,
    const noelle::DominatorSummary &DS,
    LoopDependenceEdge *edge) {
  auto *topLoop = loopNode->getLoop();
  assert(topLoop != nullptr);
  auto *topLoopHeader = topLoop->getHeader();
  auto *topLoopHeaderBranch = topLoopHeader->getTerminator();

  auto *producer = edge->getSrc()->getValue();
  auto *consumer = edge->getDst()->getValue();
  auto *producerI = llvm::dyn_cast_or_null<llvm::Instruction>(producer);
  auto *consumerI = llvm::dyn_cast_or_null<llvm::Instruction>(consumer);
  if (producerI == nullptr || consumerI == nullptr) {
    return false;
  }

  auto *producerLoop = loopNode->getInnermostLoopThatContains(producerI);
  auto *consumerLoop = loopNode->getInnermostLoopThatContains(consumerI);
  if (producerLoop == nullptr || consumerLoop == nullptr) {
    return false;
  }

  if (edge->getKind() == LoopDependenceEdgeKind::Control &&
      producerLoop != loopNode->getLoop() &&
      consumerLoop != loopNode->getLoop()) {
    return false;
  }

  bool sameElementSameIteration = true;
  if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
    llvm::Value *producerPointer = nullptr;
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(producerI)) {
      producerPointer = load->getPointerOperand();
    } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(producerI)) {
      producerPointer = store->getPointerOperand();
    }

    llvm::Value *consumerPointer = nullptr;
    if (auto *load = llvm::dyn_cast<llvm::LoadInst>(consumerI)) {
      consumerPointer = load->getPointerOperand();
    } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(consumerI)) {
      consumerPointer = store->getPointerOperand();
    }

    if (producerPointer == nullptr || consumerPointer == nullptr ||
        producerPointer != consumerPointer) {
      sameElementSameIteration = false;
    } else if (auto *pointerInst =
                   llvm::dyn_cast<llvm::Instruction>(producerPointer)) {
      if (topLoop->isIncluded(pointerInst)) {
        sameElementSameIteration = false;
      }
    } else {
      sameElementSameIteration = false;
    }
  }

  if (!sameElementSameIteration) {
    return true;
  }

  if (producerI == consumerI || !DS.DT.dominates(producerI, consumerI)) {
    if (edge->getKind() == LoopDependenceEdgeKind::Variable) {
      auto *producerBlock = producerI->getParent();
      auto *consumerBlock = consumerI->getParent();
      auto mustReachConsumerBeforeHeader =
          !canBasicBlockReachHeaderBeforeOther(*consumerLoop, producerBlock,
                                               consumerBlock);
      if (mustReachConsumerBeforeHeader) {
        return false;
      }

      if (DS.DT.dominates(consumerI, producerI) &&
          DS.DT.dominates(topLoopHeaderBranch, consumerI) &&
          llvm::isa<llvm::PHINode>(consumerI)) {
        return false;
      }
    }
    return true;
  }

  return false;
}

std::set<LoopDependenceEdge *>
LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
    const LoopStructure &loop,
    LoopTree *loopNode,
    LoopDependenceGraph &loopDG) {
  std::set<LoopDependenceEdge *> edges;
  for (auto *edge : loopDG.getEdges()) {
    if (!edge->isLoopCarried()) {
      continue;
    }
    auto *consumerI =
        llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
    if (consumerI == nullptr) {
      continue;
    }
    if (loopNode->getInnermostLoopThatContains(consumerI) != &loop) {
      continue;
    }
    edges.insert(edge);
  }
  return edges;
}

std::set<LoopDependenceEdge *>
LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
    const LoopStructure &loop,
    LoopTree *loopNode,
    LoopSCCDAG &sccdag) {
  std::set<LoopDependenceEdge *> edges;
  assert(sccdag.getLoopDependenceGraph() != nullptr);
  for (auto *scc : sccdag.getSCCs()) {
    for (auto *edge : scc->getEdges()) {
      if (!edge->isLoopCarried()) {
        continue;
      }
      auto *consumerI =
          llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
      if (consumerI == nullptr ||
          loopNode->getInnermostLoopThatContains(consumerI) != &loop) {
        continue;
      }
      auto *producerI =
          llvm::dyn_cast_or_null<llvm::Instruction>(edge->getSrc()->getValue());
      if (producerI == nullptr ||
          loopNode->getInnermostLoopThatContains(producerI) == nullptr) {
        continue;
      }
      edges.insert(edge);
    }
  }
  return edges;
}

bool LoopCarriedDependencies::canBasicBlockReachHeaderBeforeOther(
    const LoopStructure &loop,
    llvm::BasicBlock *from,
    llvm::BasicBlock *other) {
  assert(loop.isIncluded(from) && loop.isIncluded(other));
  if (from == other) {
    return true;
  }

  auto *header = loop.getHeader();
  const auto &exitsVector = loop.getLoopExitBasicBlocks();
  std::set<llvm::BasicBlock *> exits(exitsVector.begin(), exitsVector.end());
  std::queue<llvm::BasicBlock *> queue;
  std::unordered_set<llvm::BasicBlock *> enqueued;
  queue.push(from);
  enqueued.insert(from);
  bool reachedOther = false;

  while (!queue.empty()) {
    auto *current = queue.front();
    queue.pop();
    if (current == header) {
      return true;
    }
    if (exits.count(current) != 0) {
      continue;
    }
    if (current == other) {
      reachedOther = true;
      continue;
    }
    for (auto *succ : llvm::successors(current)) {
      if (!enqueued.insert(succ).second) {
        continue;
      }
      queue.push(succ);
    }
  }

  assert(reachedOther);
  return false;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
