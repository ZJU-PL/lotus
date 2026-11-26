#include "Analysis/Summary/Purity/PurityAttrInferencePass.h"

#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace lotus {
namespace analysis {
namespace purity {

char PurityAttrInferencePass::ID = 0;

namespace {

bool inferAttributesForFunction(Function &function,
                                const FunctionPurityAnalysis &analysis) {
  if (function.isDeclaration()) {
    return false;
  }

  const PurityKind purity = analysis.getPurity(&function);
  switch (purity) {
  case PurityKind::Const: {
    bool changed = false;
    if (function.hasFnAttribute(Attribute::ReadOnly)) {
      function.removeFnAttr(Attribute::ReadOnly);
      changed = true;
    }
    if (!function.hasFnAttribute(Attribute::ReadNone)) {
      function.addFnAttr(Attribute::ReadNone);
      changed = true;
    }
    return changed;
  }
  case PurityKind::Pure:
    if (function.hasFnAttribute(Attribute::ReadNone) ||
        function.hasFnAttribute(Attribute::ReadOnly)) {
      return false;
    }
    function.addFnAttr(Attribute::ReadOnly);
    return true;
  case PurityKind::Impure:
  case PurityKind::Unknown:
    return false;
  }

  llvm_unreachable("unexpected PurityKind");
}

} // namespace

bool inferPurityAttributes(Module &module,
                           const FunctionPurityAnalysis &analysis) {
  bool changed = false;
  for (Function &function : module) {
    changed |= inferAttributesForFunction(function, analysis);
  }
  return changed;
}

bool PurityAttrInferencePass::runOnModule(Module &module) {
  FunctionPurityAnalysis analysis(module);
  analysis.run();
  return inferPurityAttributes(module, analysis);
}

} // namespace purity
} // namespace analysis
} // namespace lotus

static llvm::RegisterPass<lotus::analysis::purity::PurityAttrInferencePass> X(
    "infer-purity-attrs",
    "Infer readnone/readonly attributes from purity analysis");

namespace lotus {
namespace analysis {
namespace purity {

llvm::Pass *createPurityAttrInferencePass() {
  return new PurityAttrInferencePass();
}

} // namespace purity
} // namespace analysis
} // namespace lotus
