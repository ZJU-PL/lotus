#pragma once

#include "Checker/TPA/Program/DefUseInstruction.h"

#include <deque>
#include <vector>

#include <llvm/ADT/DenseMap.h>

namespace tpa::taint {

class DefUseFunction {
private:
  const llvm::Function &function;
  // Pointer-stable node storage paired with a flat DenseMap lookup index.
  std::deque<DefUseInstruction> instStorage;
  llvm::DenseMap<const llvm::Instruction *, DefUseInstruction *> instMap;

  DefUseInstruction entryInst;
  std::vector<DefUseInstruction *> exitInsts;
  std::vector<DefUseInstruction *> memoryExitInsts;
  MemoryGroupTable memGroups;
  mutable bool memPredsBuilt = false;

public:
  using NodeType = DefUseInstruction;

  DefUseFunction(const llvm::Function &f);
  DefUseFunction(const DefUseFunction &) = delete;
  DefUseFunction &operator=(const DefUseFunction &) = delete;
  const llvm::Function &getFunction() const { return function; }

  DefUseInstruction *getDefUseInstruction(const llvm::Instruction *);
  const DefUseInstruction *
  getDefUseInstruction(const llvm::Instruction *) const;

  DefUseInstruction *getEntryInst() { return &entryInst; }
  const DefUseInstruction *getEntryInst() const { return &entryInst; }
  const auto &getMemoryExitInsts() const { return memoryExitInsts; }
  const std::vector<DefUseInstruction *> &getExitInsts() const {
    return exitInsts;
  }

  MemoryGroupTable &getMemoryGroups() { return memGroups; }
  const MemoryGroupTable &getMemoryGroups() const { return memGroups; }

  void finalizeMemSuccEdges();
  void ensureMemPreds() const;
};

} // namespace tpa::taint
