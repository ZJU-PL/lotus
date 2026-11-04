#include "Checker/GSAF/Support/CheckerServices.h"

#include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
#include "Checker/GSAF/API/Models.h"
#include "Checker/GSAF/Engine/Checker.h"

#include <llvm/IR/Instructions.h>

namespace lotus::gsaf {

std::unique_ptr<HeapPointerAnalysis> heapAnalysis(llvm::Pass *pass) {
  auto &models = pass->getAnalysis<GSAFModels>();
  auto *module = static_cast<GSAFChecker *>(pass)->getModule();
  std::vector<std::string> allocators, releases;
  models.getAllMemoryAllocs(allocators);
  models.getAllMemoryFrees(releases);
  auto analysis = std::make_unique<HeapPointerAnalysis>();
  analysis->analyze(*module, pass->getAnalysis<DyckAliasAnalysis>(), allocators,
                    releases);
  return analysis;
}

const char *branchConditionFromTo(llvm::BasicBlock *from, llvm::BasicBlock *to) {
  auto *branch = llvm::dyn_cast<llvm::BranchInst>(from->getTerminator());
  if (!branch || !branch->isConditional())
    return "true";
  return branch->getSuccessor(0) == to ? "true" : "false";
}

} // namespace lotus::gsaf
