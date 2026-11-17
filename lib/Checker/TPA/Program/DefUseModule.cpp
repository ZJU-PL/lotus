#include "Checker/TPA/Program/DefUseModule.h"

#include "Checker/TPA/Support/ExternalModels.h"

#include <limits>

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace tpa::taint {

DefUseInstruction::DefUseInstruction(const Instruction &i) : inst(&i), rpo(0) {}

DefUseInstruction::DefUseInstruction(const Function *f)
    : inst(f), rpo(std::numeric_limits<size_t>::max()) {}

const Instruction *DefUseInstruction::getInstruction() const {
  return cast<Instruction>(inst);
}

const Function *DefUseInstruction::getFunction() const {
  if (const auto *f = dyn_cast<Function>(inst))
    return f;
  else {
    const auto *i = cast<Instruction>(inst);
    return i->getParent()->getParent();
  }
}

bool DefUseInstruction::isEntryInstruction() const {
  return isa<Function>(inst);
}

bool DefUseInstruction::isCallInstruction() const {
  return isa<CallBase>(inst);
}

bool DefUseInstruction::isReturnInstruction() const {
  return isa<ReturnInst>(inst);
}

DefUseInstruction *
DefUseFunction::getDefUseInstruction(const Instruction *inst) {
  auto itr = instMap.find(inst);
  if (itr != instMap.end())
    return &itr->second;
  else
    return nullptr;
}
const DefUseInstruction *
DefUseFunction::getDefUseInstruction(const Instruction *inst) const {
  auto itr = instMap.find(inst);
  if (itr != instMap.end())
    return &itr->second;
  else
    return nullptr;
}

DefUseFunction::DefUseFunction(const Function &f) : function(f), entryInst(&f) {
  for (const auto *bb : llvm::depth_first(&f.getEntryBlock())) {
    for (auto const &inst : *bb) {
      if (isDebugIntrinsic(inst))
        continue;
      if (const auto *brInst = dyn_cast<BranchInst>(&inst))
        if (brInst->isUnconditional())
          continue;

      auto itr =
          instMap
              .emplace(std::piecewise_construct, std::forward_as_tuple(&inst),
                       std::forward_as_tuple(inst))
              .first;

      if (isa<ReturnInst>(&inst)) {
        exitInsts.push_back(&itr->second);
      }
      if (isa<ReturnInst>(inst) || isa<ResumeInst>(inst))
        memoryExitInsts.push_back(&itr->second);
    }
  }
}

DefUseModule::DefUseModule(
    const Module &m, const std::unordered_set<const Function *> &reachable)
    : module(m), entryFunc(nullptr) {
  for (auto const &f : module) {
    if (f.isDeclaration() || !reachable.count(&f))
      continue;

    auto itr = funMap
                   .emplace(std::piecewise_construct, std::forward_as_tuple(&f),
                            std::forward_as_tuple(f))
                   .first;

    if (f.getName() == "main") {
      assert(entryFunc == nullptr);
      entryFunc = &itr->second;
    }
  }
}

const DefUseFunction &DefUseModule::getDefUseFunction(const Function *f) const {
  auto itr = funMap.find(f);
  assert(itr != funMap.end());
  return itr->second;
}

} // namespace tpa::taint
