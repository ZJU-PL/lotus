#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"

#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/UnifyFunctionExitNodes.h>

namespace tpa::taint {
void prepareModule(llvm::Module &module, bool runTPAPrepasses) {
  if (runTPAPrepasses)
    transform::runPrepassOn(module);
  // The migrated TPA pointer CFG has one exit node per function. Normalize
  // before either analysis so their call/return and memory identities agree.
  llvm::FunctionAnalysisManager manager;
  llvm::UnifyFunctionExitNodesPass unify;
  for (auto &function : module)
    if (!function.isDeclaration())
      unify.run(function, manager);
}
} // namespace tpa::taint
