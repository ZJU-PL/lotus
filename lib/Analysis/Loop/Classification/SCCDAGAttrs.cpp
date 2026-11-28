/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Classification/SCCDAGAttrs.h"

#include "Analysis/CFG/Dominator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

SCCDAGAttrs::SCCDAGAttrs(bool enableFloatAsReal,
                         LoopDependenceGraph *loopDG,
                         LoopSCCDAG *loopSCCDAG,
                         LoopTree *loopNode,
                         InductionVariableManager &IV,
                         const noelle::DominatorSummary &DS,
                         MemoryCloningAnalysis *sharedMemoryCloning)
    : enableFloatAsReal{enableFloatAsReal}, loopDG{loopDG}, sccdag{loopSCCDAG} {
  this->collectLoopCarriedDependencies(loopNode);
  if (sharedMemoryCloning != nullptr) {
    this->memoryCloningAnalysis = sharedMemoryCloning;
  } else {
    this->ownedMemoryCloningAnalysis = std::make_unique<MemoryCloningAnalysis>(
        loopNode->getLoop(), DS, loopDG);
    this->memoryCloningAnalysis = this->ownedMemoryCloningAnalysis.get();
  }

  std::set<InductionVariable *> ivs;
  std::set<InductionVariable *> loopGoverningIVs;
  for (auto *loop : loopNode->getLoops()) {
    auto loopIVs = IV.getInductionVariables(*loop);
    ivs.insert(loopIVs.begin(), loopIVs.end());
    auto *giv = IV.getLoopGoverningInductionVariable(*loop);
    if (giv != nullptr) {
      loopGoverningIVs.insert(giv->getInductionVariable());
    }
  }

  for (auto *scc : loopSCCDAG->getAllSCCs()) {
    std::unique_ptr<GenericSCC> info;
    if (this->checkIfIndependent(scc)) {
      info = std::make_unique<LoopIterationSCC>(scc, loopNode->getLoop());
    } else {
      auto periodic = this->checkIfPeriodic(scc, loopNode);
      if (std::get<0>(periodic)) {
        info = std::make_unique<PeriodicVariableSCC>(
            scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc],
            std::get<1>(periodic), std::get<2>(periodic), std::get<3>(periodic),
            std::get<4>(periodic));
      } else {
        auto ivSet = this->checkIfSCCOnlyContainsInductionVariables(
            scc, loopNode, ivs, loopGoverningIVs);
        if (!ivSet.empty()) {
          info = std::make_unique<LinearInductionVariableSCC>(
              scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc],
              ivSet);
        } else if (auto variable = this->checkIfReducible(scc, loopNode)) {
          info = std::make_unique<BinaryReductionSCC>(
              scc, loopNode->getLoop(), this->sccToLoopCarriedDependencies[scc],
              variable.get());
        } else {
          auto recomputable = this->checkIfRecomputable(scc, loopNode);
          if (!recomputable.empty()) {
            info = std::make_unique<UnknownClosedFormSCC>(
                scc, loopNode->getLoop(),
                this->sccToLoopCarriedDependencies[scc], recomputable);
          } else {
            auto clonable =
                this->checkIfClonableByUsingLocalMemory(scc, loopNode);
            if (!clonable.empty()) {
              info = std::make_unique<StackObjectClonableSCC>(
                  scc, loopNode->getLoop(),
                  this->sccToLoopCarriedDependencies[scc], clonable);
            } else {
              info = std::make_unique<LoopCarriedUnknownSCC>(
                  scc, loopNode->getLoop(),
                  this->sccToLoopCarriedDependencies[scc]);
            }
          }
        }
      }
    }
    this->sccToInfo[scc] = std::move(info);
  }
}

void SCCDAGAttrs::collectLoopCarriedDependencies(LoopTree *loopNode) {
  for (auto *loop : loopNode->getLoops()) {
    auto edges = LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
        *loop, loopNode, *this->sccdag);
    for (auto *edge : edges) {
      auto *producerScc = this->sccdag->getSCC(edge->getSrc()->getValue());
      auto *consumerScc = this->sccdag->getSCC(edge->getDst()->getValue());
      if (producerScc != nullptr) {
        this->sccToLoopCarriedDependencies[producerScc].insert(edge);
      }
      if (consumerScc != nullptr) {
        this->sccToLoopCarriedDependencies[consumerScc].insert(edge);
      }
    }
  }
}

std::unique_ptr<LoopCarriedVariable> SCCDAGAttrs::checkIfReducible(
    LoopSCC *scc,
    LoopTree *loopNode) {
  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return nullptr;
  }
  std::unordered_set<llvm::PHINode *> carriedPhis;
  auto *rootHeader = loopNode->getLoop()->getHeader();
  for (auto *dep : it->second) {
    if (dep->getKind() == LoopDependenceEdgeKind::Memory) {
      return nullptr;
    }

    auto *producer = dep->getSrc()->getValue();
    if (dep->getKind() == LoopDependenceEdgeKind::Control) {
      if (scc->isInternal(producer)) {
        return nullptr;
      }
      continue;
    }

    auto *consumer =
        llvm::dyn_cast_or_null<llvm::PHINode>(dep->getDst()->getValue());
    if (consumer == nullptr) {
      return nullptr;
    }
    if (scc->fetchNode(consumer) != nullptr &&
        consumer->getParent() == rootHeader) {
      carriedPhis.insert(consumer);
    }
  }
  if (carriedPhis.size() != 1) {
    return nullptr;
  }
  auto *phi = *carriedPhis.begin();
  auto variable = std::make_unique<LoopCarriedVariable>(
      *loopNode->getLoop(), loopNode, *this->loopDG, *this->sccdag, *scc, phi);
  if (!variable->isEvolutionReducibleAcrossLoopIterations()) {
    return nullptr;
  }
  if (!this->enableFloatAsReal &&
      (phi->getType()->isFloatTy() || phi->getType()->isDoubleTy())) {
    return nullptr;
  }
  return variable;
}

std::tuple<bool, llvm::Value *, llvm::Value *, llvm::Value *, llvm::PHINode *>
SCCDAGAttrs::checkIfPeriodic(LoopSCC *scc, LoopTree *loopNode) {
  auto notPeriodic =
      std::make_tuple(false, nullptr, nullptr, nullptr, nullptr);
  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return notPeriodic;
  }
  if (scc->numberOfInstructions() != 2) {
    return notPeriodic;
  }
  for (auto *dep : it->second) {
    if (!dep->isLoopCarried() ||
        dep->getKind() == LoopDependenceEdgeKind::Control) {
      continue;
    }

    auto *to =
        llvm::dyn_cast_or_null<llvm::PHINode>(dep->getDst()->getValue());
    auto *from = dep->getSrc()->getValue();
    if (to == nullptr || from == nullptr || to->getNumIncomingValues() != 2) {
      return notPeriodic;
    }

    if (auto *fromPhi = llvm::dyn_cast<llvm::PHINode>(from)) {
      bool fromHasExternalUsers = false;
      bool toHasExternalUsers = false;
      for (auto *user : from->users()) {
        if (user != to && llvm::isa<llvm::Instruction>(user)) {
          fromHasExternalUsers = true;
        }
      }
      for (auto *user : to->users()) {
        if (user != from && llvm::isa<llvm::Instruction>(user)) {
          toHasExternalUsers = true;
        }
      }
      if (fromHasExternalUsers && toHasExternalUsers) {
        return notPeriodic;
      }

      int fromInitialIdx = (fromPhi->getIncomingValue(0) == to) ? 1 : 0;
      int toInitialIdx = (to->getIncomingValue(0) == from) ? 1 : 0;
      if (fromPhi->getIncomingBlock(fromInitialIdx) !=
              loopNode->getLoop()->getPreHeader() ||
          to->getIncomingBlock(toInitialIdx) !=
              loopNode->getLoop()->getPreHeader()) {
        return notPeriodic;
      }

      llvm::Value *initial = nullptr;
      llvm::Value *secondaryInitial = nullptr;
      llvm::PHINode *accumulator = nullptr;
      if (fromHasExternalUsers) {
        initial = fromPhi->getIncomingValue(fromInitialIdx);
        secondaryInitial = to->getIncomingValue(toInitialIdx);
        accumulator = fromPhi;
      } else {
        initial = to->getIncomingValue(toInitialIdx);
        secondaryInitial = fromPhi->getIncomingValue(fromInitialIdx);
        accumulator = to;
      }

      auto *initialCI = llvm::dyn_cast<llvm::ConstantInt>(initial);
      auto *secondaryCI = llvm::dyn_cast<llvm::ConstantInt>(secondaryInitial);
      if (initialCI == nullptr || secondaryCI == nullptr) {
        return notPeriodic;
      }

      auto *period = llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(to->getContext()), 2);
      auto *step = llvm::ConstantExpr::getSub(secondaryCI, initialCI);
      return std::make_tuple(true, initial, period, step, accumulator);
    }

    auto *fromInst = llvm::dyn_cast<llvm::Instruction>(from);
    if (fromInst == nullptr) {
      return notPeriodic;
    }

    llvm::Value *initial = (to->getIncomingValue(0) == from)
                               ? to->getIncomingValue(1)
                               : to->getIncomingValue(0);
    llvm::Value *period = nullptr;
    llvm::Value *step = nullptr;
    bool found = false;
    switch (fromInst->getOpcode()) {
    case llvm::Instruction::Xor: {
      period = llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(fromInst->getContext()), 2);
      auto *fromOperand = fromInst->getOperand(1);
      auto *fromCI = llvm::dyn_cast<llvm::ConstantInt>(fromOperand);
      auto *initialCI = llvm::dyn_cast<llvm::ConstantInt>(initial);
      if (fromCI != nullptr && initialCI != nullptr && initialCI->isZero() &&
          fromCI->isOne()) {
        step = fromOperand;
        found = true;
      }
      break;
    }
    case llvm::Instruction::Sub: {
      period = llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(fromInst->getContext()), 2);
      auto *fromOperand = fromInst->getOperand(0);
      auto *fromCI = llvm::dyn_cast<llvm::ConstantInt>(fromOperand);
      auto *initialCI = llvm::dyn_cast<llvm::ConstantInt>(initial);
      if (fromCI != nullptr && fromCI->isZero() && initialCI != nullptr) {
        auto sign = initialCI->isNegative() ? 1 : -1;
        step = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(fromInst->getContext()),
            2 * sign * initialCI->getSExtValue());
        found = true;
      }
      break;
    }
    default:
      return notPeriodic;
    }

    if (found) {
      return std::make_tuple(true, initial, period, step, nullptr);
    }
  }
  return notPeriodic;
}

bool SCCDAGAttrs::checkIfIndependent(LoopSCC *scc) {
  return this->sccToLoopCarriedDependencies.find(scc) ==
         this->sccToLoopCarriedDependencies.end();
}

std::set<InductionVariable *>
SCCDAGAttrs::checkIfSCCOnlyContainsInductionVariables(
    LoopSCC *scc,
    LoopTree *loopNode,
    const std::set<InductionVariable *> &IVs,
    const std::set<InductionVariable *> &loopGoverningIVs) const {
  std::set<InductionVariable *> contained;
  std::set<llvm::Instruction *> containedInsts;
  for (auto *iv : IVs) {
    if (scc->isInternal(iv->getLoopEntryPHI())) {
      contained.insert(iv);
      auto all = iv->getAllInstructions();
      containedInsts.insert(all.begin(), all.end());
    }
  }
  if (contained.empty()) {
    return {};
  }

  for (auto *iv : contained) {
    if (loopGoverningIVs.count(iv) == 0) {
      continue;
    }
    auto *ivLoop =
        loopNode->getInnermostLoopThatContains(iv->getLoopEntryPHI());
    assert(ivLoop != nullptr);
    const auto &exitBlocks = ivLoop->getLoopExitBasicBlocks();
    LoopGoverningInductionVariable attribution(loopNode->getLoop(), *iv, *scc,
                                               exitBlocks);
    if (!attribution.isSCCContainingIVWellFormed()) {
      return {};
    }
    if (auto *cmp =
            attribution.getHeaderCompareInstructionToComputeExitCondition()) {
      containedInsts.insert(cmp);
    }
    if (auto *br = attribution.getHeaderBrInst()) {
      containedInsts.insert(br);
    }
    if (auto *condInst =
            llvm::dyn_cast<llvm::Instruction>(attribution.getExitConditionValue())) {
      containedInsts.insert(condInst);
    }
    auto derivation = attribution.getConditionValueDerivation();
    containedInsts.insert(derivation.begin(), derivation.end());
  }

  for (const auto &pair : scc->internalNodePairs()) {
    auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(pair.first);
    if (inst != nullptr && containedInsts.count(inst) == 0) {
      return {};
    }
  }
  return contained;
}

std::set<llvm::Instruction *> SCCDAGAttrs::checkIfRecomputable(
    LoopSCC *scc,
    LoopTree *loopNode) const {
  for (auto *edge : scc->getEdges()) {
    if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
      return {};
    }
  }

  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return {};
  }
  std::set<llvm::Instruction *> values;
  auto *topLoop = loopNode->getLoop();
  for (auto *dep : it->second) {
    if (dep->getKind() == LoopDependenceEdgeKind::Memory) {
      return {};
    }
    auto *src =
        llvm::dyn_cast_or_null<llvm::Instruction>(dep->getSrc()->getValue());
    auto *dst =
        llvm::dyn_cast_or_null<llvm::Instruction>(dep->getDst()->getValue());
    if (src == nullptr || dst == nullptr) {
      return {};
    }
    if (loopNode->getInnermostLoopThatContains(src) == topLoop ||
        loopNode->getInnermostLoopThatContains(dst) == topLoop) {
      return {};
    }
    values.insert(src);
  }
  return values;
}

std::set<ClonableMemoryObject *>
SCCDAGAttrs::checkIfClonableByUsingLocalMemory(LoopSCC *scc, LoopTree *) const {
  auto it = this->sccToLoopCarriedDependencies.find(scc);
  if (it == this->sccToLoopCarriedDependencies.end()) {
    return {};
  }

  std::set<ClonableMemoryObject *> locations;
  for (auto *dependency : it->second) {
    auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(
        dependency->getSrc()->getValue());
    if (inst == nullptr) {
      return {};
    }

    auto locs = this->memoryCloningAnalysis->getClonableMemoryObjectsFor(inst);
    if (locs.empty()) {
      return {};
    }

    locations.insert(locs.begin(), locs.end());
  }

  return locations;
}

GenericSCC *SCCDAGAttrs::getSCCAttrs(LoopSCC *scc) const {
  auto it = this->sccToInfo.find(scc);
  return it == this->sccToInfo.end() ? nullptr : it->second.get();
}

std::set<LoopCarriedSCC *>
SCCDAGAttrs::getSCCsWithLoopCarriedDependencies(void) const {
  std::set<LoopCarriedSCC *> result;
  for (const auto &pair : this->sccToLoopCarriedDependencies) {
    if (auto *attrs =
            dynamic_cast<LoopCarriedSCC *>(this->getSCCAttrs(pair.first))) {
      result.insert(attrs);
    }
  }
  return result;
}

std::set<LoopCarriedSCC *>
SCCDAGAttrs::getSCCsWithLoopCarriedDataDependencies(void) const {
  std::set<LoopCarriedSCC *> result;
  for (const auto &pair : this->sccToLoopCarriedDependencies) {
    bool hasData = false;
    for (auto *dep : pair.second) {
      if (dep->getKind() != LoopDependenceEdgeKind::Control) {
        hasData = true;
        break;
      }
    }
    if (!hasData) {
      continue;
    }
    if (auto *attrs =
            dynamic_cast<LoopCarriedSCC *>(this->getSCCAttrs(pair.first))) {
      result.insert(attrs);
    }
  }
  return result;
}

std::set<LoopCarriedSCC *>
SCCDAGAttrs::getSCCsWithLoopCarriedControlDependencies(void) const {
  std::set<LoopCarriedSCC *> result;
  for (const auto &pair : this->sccToLoopCarriedDependencies) {
    bool hasControl = false;
    for (auto *dep : pair.second) {
      if (dep->getKind() == LoopDependenceEdgeKind::Control) {
        hasControl = true;
        break;
      }
    }
    if (!hasControl) {
      continue;
    }
    if (auto *attrs =
            dynamic_cast<LoopCarriedSCC *>(this->getSCCAttrs(pair.first))) {
      result.insert(attrs);
    }
  }
  return result;
}

std::unordered_set<GenericSCC *>
SCCDAGAttrs::getSCCsOfKind(GenericSCC::SCCKind kind) const {
  std::unordered_set<GenericSCC *> result;
  for (const auto &pair : this->sccToInfo) {
    if (pair.second != nullptr && pair.second->getKind() == kind) {
      result.insert(pair.second.get());
    }
  }
  return result;
}

bool SCCDAGAttrs::isLoopGovernedBySCC(LoopSCC *governingSCC) const {
  std::queue<LoopSCC *> toTraverse;
  std::unordered_set<LoopSCC *> visited;
  std::set<LoopSCC *> topLevelLoopCarriedSCCs;

  for (auto *scc : this->sccdag->getSCCs()) {
    if (scc->getPredecessors().empty()) {
      toTraverse.push(scc);
    }
  }

  while (!toTraverse.empty()) {
    auto *scc = toTraverse.front();
    toTraverse.pop();
    if (!visited.insert(scc).second) {
      continue;
    }

    auto *info = this->getSCCAttrs(scc);
    if (info == nullptr) {
      continue;
    }
    if (info->getKind() == GenericSCC::LOOP_ITERATION) {
      for (auto *succ : scc->getSuccessors()) {
        toTraverse.push(succ);
      }
      continue;
    }

    topLevelLoopCarriedSCCs.insert(scc);
  }

  return topLevelLoopCarriedSCCs.size() == 1 &&
         *topLevelLoopCarriedSCCs.begin() == governingSCC;
}

std::set<uint32_t> SCCDAGAttrs::getLiveOutVariablesThatAreNotReducable(
    LoopEnvironment *env) const {
  std::set<uint32_t> result;
  if (env == nullptr) {
    return result;
  }

  for (auto envID : env->getEnvIDsOfLiveOutVars()) {
    auto *producer = env->getProducer(envID);
    auto *scc = this->sccdag->getSCC(producer);
    if (scc == nullptr) {
      result.insert(envID);
      continue;
    }
    auto *info = this->getSCCAttrs(scc);
    if (info == nullptr ||
        (info->getKind() != GenericSCC::BINARY_REDUCTION &&
         info->getKind() != GenericSCC::LOOP_ITERATION)) {
      result.insert(envID);
    }
  }

  return result;
}

bool SCCDAGAttrs::isSCCContainedInSubloop(LoopTree *loopNode,
                                          LoopSCC *scc) const {
  if (loopNode == nullptr || scc == nullptr) {
    return false;
  }
  auto *topLoop = loopNode->getLoop();
  for (const auto &pair : scc->internalNodePairs()) {
    auto *inst = llvm::dyn_cast<llvm::Instruction>(pair.first);
    if (inst == nullptr ||
        loopNode->getInnermostLoopThatContains(inst) == topLoop) {
      return false;
    }
  }
  return true;
}

std::pair<SCCDAGAttrs::SCCParentMap, SCCDAGAttrs::SCCEdgeMap>
SCCDAGAttrs::computeSCCDAGWhenSCCsAreIgnored(
    std::function<bool(GenericSCC *)> ignoreSCC) const {
  SCCParentMap parentsViaIgnored;
  SCCEdgeMap edgesViaIgnored;

  auto addIncomingNodes = [&](std::queue<LoopSCC *> &queue, LoopSCC *scc) {
    for (auto *edge : scc->getIncomingEdges()) {
      if (edge == nullptr || edge->getSrc() == nullptr) {
        continue;
      }
      queue.push(edge->getSrc());
      edgesViaIgnored[scc].insert(edge);
    }
  };

  for (auto *childSCC : this->sccdag->getSCCs()) {
    std::queue<LoopSCC *> nodesToCheck;
    std::unordered_set<LoopSCC *> analyzed;

    analyzed.insert(childSCC);
    addIncomingNodes(nodesToCheck, childSCC);

    while (!nodesToCheck.empty()) {
      auto *scc = nodesToCheck.front();
      nodesToCheck.pop();
      if (scc == nullptr) {
        continue;
      }

      auto *sccInfo = this->getSCCAttrs(scc);
      parentsViaIgnored[childSCC].insert(scc);
      if (sccInfo == nullptr || !ignoreSCC(sccInfo)) {
        continue;
      }
      if (!analyzed.insert(scc).second) {
        continue;
      }
      addIncomingNodes(nodesToCheck, scc);
    }
  }

  return std::make_pair(std::move(parentsViaIgnored),
                        std::move(edgesViaIgnored));
}

LoopSCCDAG *SCCDAGAttrs::getSCCDAG(void) const { return this->sccdag; }

void SCCDAGAttrs::print(llvm::raw_ostream &stream) const {
  for (auto *scc : this->sccdag->getSCCs()) {
    auto *info = this->getSCCAttrs(scc);
    stream << "SCC " << scc->getID() << ": ";
    if (info == nullptr) {
      stream << "unknown-info\n";
      continue;
    }
    stream << static_cast<int>(info->getKind()) << "\n";
    for (const auto &pair : scc->internalNodePairs()) {
      if (pair.first == nullptr) {
        continue;
      }
      stream << "  ";
      pair.first->print(stream);
      stream << "\n";
    }
  }
}

} // namespace loop
} // namespace analysis
} // namespace lotus
