/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Scalar/InductionVariables.h"
#include "Analysis/Loop/Scalar/ScalarEvolutionReferencer.h"

#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Instructions.h"

#include <cassert>

namespace lotus {
namespace analysis {
namespace loop {

InductionVariableManager::InductionVariableManager(
    LoopTree *loopNode,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    llvm::LoopInfo &LI,
    LoopSCCDAG &sccdag,
    LoopEnvironment &loopEnvironment,
    bool enableExtendedRecognition)
    : loop{loopNode} {
  assert(this->loop != nullptr);

  auto *loopToAnalyze = this->loop->getLoop();
  assert(loopToAnalyze != nullptr);
  auto &F = *loopToAnalyze->getHeader()->getParent();
  detail::ScalarEvolutionReferentialExpander referentialExpander(SE, F);

  for (auto *subOrRootLoop : this->loop->getLoops()) {
    auto &owned = this->ownedIVs[subOrRootLoop];
    auto *header = subOrRootLoop->getHeader();
    auto *preHeader = subOrRootLoop->getPreHeader();
    if (header == nullptr) {
      continue;
    }

    auto *llvmLoop = LI.getLoopFor(header);
    for (auto &phi : header->phis()) {
      llvm::InductionDescriptor ID{};
      bool llvmDeterminedValidIV = false;
      bool llvmLoopValidForInductionAnalysis =
          preHeader != nullptr && (phi.getBasicBlockIndex(preHeader) >= 0) &&
          (llvmLoop != nullptr);
      if (llvmLoopValidForInductionAnalysis &&
          llvm::InductionDescriptor::isInductionPHI(&phi, llvmLoop, &SE, ID)) {
        llvmDeterminedValidIV = true;
      } else if (phi.getType()->isFloatingPointTy() && llvmLoop != nullptr &&
                 llvm::InductionDescriptor::isFPInductionPHI(&phi, llvmLoop,
                                                             &SE, ID)) {
        llvmDeterminedValidIV = true;
      }

      auto *sccContainingIV = sccdag.getSCC(&phi);
      bool noelleDeterminedValidIV = true;
      std::unique_ptr<InductionVariable> IV;
      if (!SE.isSCEVable(phi.getType())) {
        noelleDeterminedValidIV = false;
      } else {
        auto *scev = SE.getSCEV(&phi);
        if (scev == nullptr) {
          noelleDeterminedValidIV = false;
        } else if (scev->getSCEVType() != llvm::SCEVTypes::scAddRecExpr) {
          noelleDeterminedValidIV = false;
          bool foundOnePHI = false;
          llvm::PHINode *internalPHI = nullptr;
          if (sccContainingIV != nullptr) {
            sccContainingIV->iterateOverInstructions(
                [&](llvm::Instruction *inst) -> bool {
                  if (llvm::isa<llvm::PHINode>(inst) && inst != &phi &&
                      SE.getSCEV(inst)->getSCEVType() ==
                          llvm::SCEVTypes::scAddRecExpr &&
                      this->loop->isIncludedInItsSubLoops(inst)) {
                    if (!foundOnePHI) {
                      foundOnePHI = true;
                      internalPHI = llvm::cast<llvm::PHINode>(inst);
                    } else {
                      foundOnePHI = false;
                      return true;
                    }
                  }
                  return false;
                });
          }

          if (foundOnePHI) {
            auto *subloop =
                this->loop->getInnermostLoopThatContains(internalPHI);
            if (subloop != nullptr &&
                subloop->getLoopExitBasicBlocks().size() == 1) {
              auto *subloopHeader = subloop->getHeader();
              auto *llvmSubloop =
                  subloopHeader->getUniqueSuccessor() == nullptr
                      ? LI.getLoopFor(subloopHeader)
                      : nullptr;
              if (llvmSubloop != nullptr) {
                auto exactTripCount = SE.getSmallConstantTripCount(llvmSubloop);
                if (exactTripCount != 0) {
                  IV = std::make_unique<InductionVariable>(
                      subOrRootLoop, IVM, SE,
                      static_cast<int64_t>(exactTripCount), &phi,
                      std::unordered_set<llvm::PHINode *>({internalPHI}),
                      sccContainingIV, loopEnvironment, referentialExpander);
                }
              }
            }
          }
        }
      }

      if (enableExtendedRecognition && !IV &&
          (noelleDeterminedValidIV || llvmDeterminedValidIV)) {
        llvm::Value *startValue =
            llvmDeterminedValidIV ? ID.getStartValue() : nullptr;
        if (!llvmDeterminedValidIV) {
          const auto &bbs = subOrRootLoop->getBasicBlocks();
          for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
            if (bbs.find(phi.getIncomingBlock(i)) == bbs.end()) {
              startValue = phi.getIncomingValue(i);
              break;
            }
          }
        }

        const llvm::SCEV *stepSCEV =
            llvmDeterminedValidIV ? ID.getStep() : nullptr;
        llvm::Value *singleStepValue =
            llvmDeterminedValidIV ? ID.getConstIntStepValue() : nullptr;
        if (singleStepValue == nullptr && stepSCEV != nullptr) {
          if (auto *constant = llvm::dyn_cast<llvm::SCEVConstant>(stepSCEV)) {
            singleStepValue = constant->getValue();
          } else if (auto *unknown =
                         llvm::dyn_cast<llvm::SCEVUnknown>(stepSCEV)) {
            auto *value = unknown->getValue();
            if (subOrRootLoop->isLoopInvariant(value)) {
              singleStepValue = value;
            }
          }
        }

        std::unordered_set<llvm::PHINode *> stepPHIs{&phi};
        auto collected = InductionVariable::collectIVInstructions(
            *subOrRootLoop, sccContainingIV, &phi, true);
        auto derivedInstructions =
            InductionVariable::collectDerivedSCEVInstructions(
                *subOrRootLoop, IVM, SE, collected.instructions, true);

        if (stepSCEV == nullptr && !stepPHIs.empty()) {
          auto *stepSCEVPHI = *stepPHIs.begin();
          if (SE.getSCEV(stepSCEVPHI)->getSCEVType() ==
              llvm::SCEVTypes::scAddRecExpr) {
            stepSCEV =
                llvm::cast<llvm::SCEVAddRecExpr>(SE.getSCEV(stepSCEVPHI))
                    ->getStepRecurrence(SE);
            if (singleStepValue == nullptr) {
              if (auto *constant =
                      llvm::dyn_cast<llvm::SCEVConstant>(stepSCEV)) {
                singleStepValue = constant->getValue();
              } else if (auto *unknown =
                             llvm::dyn_cast<llvm::SCEVUnknown>(stepSCEV)) {
                auto *value = unknown->getValue();
                if (subOrRootLoop->isLoopInvariant(value)) {
                  singleStepValue = value;
                }
              }
            }
          }
        }
        if (startValue != nullptr && singleStepValue != nullptr &&
            stepSCEV != nullptr) {
          IV = std::make_unique<InductionVariable>(
              subOrRootLoop, sccContainingIV, &phi, startValue, stepSCEV,
              singleStepValue, std::move(stepPHIs), std::move(collected.phis),
              std::move(collected.nonPHIInstructions),
              std::move(collected.instructions),
              std::move(derivedInstructions));
        }
      }

      if (!IV && noelleDeterminedValidIV && sccContainingIV != nullptr) {
        IV = std::make_unique<InductionVariable>(
            subOrRootLoop, IVM, SE, 1, &phi,
            std::unordered_set<llvm::PHINode *>({&phi}), sccContainingIV,
            loopEnvironment, referentialExpander);
      } else if (!IV && llvmDeterminedValidIV && sccContainingIV != nullptr) {
        IV = std::make_unique<InductionVariable>(
            subOrRootLoop, IVM, SE, &phi, sccContainingIV, loopEnvironment,
            referentialExpander, ID);
      }

      if (!IV || IV->getStepSCEV() == nullptr) {
        continue;
      }

      owned.push_back(std::move(IV));
    }

    for (auto &iv : owned) {
      auto *ivSCC = iv->getSCC();
      if (ivSCC == nullptr) {
        continue;
      }
      auto candidate = std::make_unique<LoopGoverningInductionVariable>(
          subOrRootLoop, *iv, *ivSCC, subOrRootLoop->getLoopExitBasicBlocks());
      if (!candidate->isSCCContainingIVWellFormed()) {
        continue;
      }
      this->governingIVs[subOrRootLoop] = candidate.get();
      this->ownedGoverningIVs.push_back(std::move(candidate));
    }
  }
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(void) const {
  return this->getInductionVariables(*this->loop->getLoop());
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(llvm::Instruction *inst) const {
  std::unordered_set<InductionVariable *> result;
  if (inst == nullptr) {
    return result;
  }

  for (const auto &loopIVPair : this->ownedIVs) {
    for (const auto &ownedIV : loopIVPair.second) {
      auto *iv = ownedIV.get();
      if (iv != nullptr && iv->isIVInstruction(inst)) {
        result.insert(iv);
      }
    }
  }
  return result;
}

std::unordered_set<InductionVariable *>
InductionVariableManager::getInductionVariables(LoopStructure &loop) const {
  std::unordered_set<InductionVariable *> ivs;
  auto it = this->ownedIVs.find(&loop);
  if (it == this->ownedIVs.end()) {
    return ivs;
  }
  for (const auto &owned : it->second) {
    ivs.insert(owned.get());
  }
  return ivs;
}

InductionVariable *InductionVariableManager::getInductionVariable(
    LoopStructure &loop,
    llvm::Instruction *inst) const {
  if (inst == nullptr) {
    return nullptr;
  }
  for (auto *iv : this->getInductionVariables(loop)) {
    if (iv != nullptr && iv->isIVInstruction(inst)) {
      return iv;
    }
  }
  return nullptr;
}

bool InductionVariableManager::doesContributeToComputeAnInductionVariable(
    llvm::Instruction *inst) const {
  return !this->getInductionVariables(inst).empty();
}

InductionVariable *InductionVariableManager::getDerivingInductionVariable(
    LoopStructure &loop,
    llvm::Instruction *derivedInstruction) const {
  if (derivedInstruction == nullptr) {
    return nullptr;
  }
  for (auto *iv : this->getInductionVariables(loop)) {
    if (iv != nullptr &&
        iv->isDerivedFromIVInstructions(derivedInstruction)) {
      return iv;
    }
  }
  return nullptr;
}

LoopGoverningInductionVariable *
InductionVariableManager::getLoopGoverningInductionVariable(void) const {
  return this->getLoopGoverningInductionVariable(*this->loop->getLoop());
}

LoopGoverningInductionVariable *
InductionVariableManager::getLoopGoverningInductionVariable(
    LoopStructure &loop) const {
  auto it = this->governingIVs.find(&loop);
  return it == this->governingIVs.end() ? nullptr : it->second;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
