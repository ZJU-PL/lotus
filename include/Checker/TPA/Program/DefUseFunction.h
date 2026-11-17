#pragma once

#include "Checker/TPA/Program/DefUseInstruction.h"

#include <vector>

namespace tpa::taint {

class DefUseFunction {
private:
  const llvm::Function &function;
  std::unordered_map<const llvm::Instruction *, DefUseInstruction> instMap;

  DefUseInstruction entryInst;
  std::vector<DefUseInstruction *> exitInsts;

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
  const std::vector<DefUseInstruction *> &getExitInsts() const {
    return exitInsts;
  }
};

} // namespace tpa::taint
