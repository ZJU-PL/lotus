/** @file MemoryCloningAnalysis.h @brief Stack allocation privatization and cloning analysis for loops. */
/*
 * Copyright 2026 Lotus contributors
 */
#pragma once

#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace llvm {
class AllocaInst;
class CallInst;
class Instruction;
class Type;
class Value;
} // namespace llvm

namespace noelle {
class DominatorSummary;
} // namespace noelle

namespace lotus {
namespace analysis {
namespace loop {

class ClonableMemoryObject {
public:
  ClonableMemoryObject(llvm::AllocaInst *allocation,
                       uint64_t sizeInBits,
                       LoopStructure *loop,
                       const noelle::DominatorSummary &DS,
                       LoopDependenceGraph *ldg);

  llvm::AllocaInst *getAllocation(void) const;
  uint64_t getAllocationSizeInBits(void) const;
  bool isClonableLocation(void) const;
  bool doPrivateCopiesNeedToBeInitialized(void) const;
  bool mustAliasAMemoryLocationWithinObject(llvm::Value *pointer) const;
  bool isInstructionCastOrGEPOfLocation(llvm::Instruction *inst) const;
  bool isInstructionStoringLocation(llvm::Instruction *inst) const;
  bool isInstructionLoadingLocation(llvm::Instruction *inst) const;
  bool isInstructionUsingLocationWithoutStoring(llvm::Instruction *inst) const;
  void addPointer(llvm::Instruction *inst);
  void addStore(llvm::Instruction *inst);
  void addLoad(llvm::Instruction *inst);
  void addNonStoringUse(llvm::Instruction *inst);
  std::unordered_set<llvm::Instruction *>
  getLocationPointerInstructions(void) const;
  void setClonable(bool clonable);
  void setNeedsInitialization(bool needsInitialization);

private:
  struct OverrideSet {
    llvm::BasicBlock *dominatingBlockOfNonStoringInsts{nullptr};
    std::unordered_set<llvm::Instruction *> subsequentNonStoringInstructions;
    std::unordered_set<llvm::Instruction *> initialStoringInstructions;
  };

  void setObjectScope(llvm::AllocaInst *allocation,
                      LoopStructure *loop,
                      const noelle::DominatorSummary &DS);
  bool identifyStoresAndOtherUsers(LoopStructure *loop,
                                   const noelle::DominatorSummary &DS);
  static bool isMemCpyInstrinsicCall(llvm::CallInst *call);
  bool isThereAMemoryDependenceBetweenLoopIterations(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg,
      const std::unordered_set<llvm::Instruction *> &insts) const;
  bool isThereRAWThroughMemoryBetweenLoopIterations(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg) const;
  bool isThereRAWThroughMemoryBetweenLoopIterations(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg,
      const std::unordered_set<llvm::Instruction *> &insts) const;
  bool isThereRAWThroughMemoryFromOutsideToLoop(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg) const;
  bool isThereRAWThroughMemoryFromOutsideToLoop(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg,
      const std::unordered_set<llvm::Instruction *> &insts) const;
  bool isThereRAWThroughMemoryFromLoopToOutside(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg) const;
  bool isThereRAWThroughMemoryFromLoopToOutside(
      LoopStructure *loop,
      llvm::AllocaInst *allocation,
      LoopDependenceGraph *ldg,
      const std::unordered_set<llvm::Instruction *> &insts) const;
  bool identifyInitialStoringInstructions(LoopStructure *loop,
                                          const noelle::DominatorSummary &DS);
  bool areOverrideSetsFullyCoveringTheAllocationSpace(void) const;
  bool isOverrideSetFullyCoveringTheAllocationSpace(
      OverrideSet *overrideSet) const;

  llvm::AllocaInst *allocation;
  uint64_t sizeInBits;
  LoopStructure *loop;
  bool clonable{false};
  bool scopeWithinLoop{false};
  bool needsInitialization{false};
  llvm::Type *allocatedType{nullptr};
  std::unordered_set<llvm::Instruction *> castsAndGEPs;
  std::unordered_set<llvm::Instruction *> storingInstructions;
  std::unordered_set<llvm::Instruction *> loadInstructions;
  std::unordered_set<llvm::Instruction *> nonStoringInstructions;
  std::vector<std::unique_ptr<OverrideSet>> overrideSets;
};

class MemoryCloningAnalysis {
public:
  MemoryCloningAnalysis(LoopStructure *loop,
                        const noelle::DominatorSummary &DS,
                        LoopDependenceGraph *ldg);

  std::unordered_set<ClonableMemoryObject *>
  getClonableMemoryObjects(void) const;
  std::unordered_set<ClonableMemoryObject *>
  getClonableMemoryObjectsFor(llvm::Instruction *inst) const;

private:
  std::vector<std::unique_ptr<ClonableMemoryObject>> clonableMemoryLocations;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
