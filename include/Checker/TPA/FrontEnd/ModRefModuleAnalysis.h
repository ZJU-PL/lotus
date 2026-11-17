#pragma once

#include "Checker/TPA/FrontEnd/ModRefModuleSummary.h"
#include "Checker/TPA/Support/ExternalModels.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace annotation {
class ExternalModRefTable;
} // namespace annotation

namespace tpa {
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {

class ModRefModuleAnalysis {
private:
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  UnmodeledPolicy unmodeledPolicy;
  const annotation::ExternalModRefTable &modRefTable;

  void collectProcedureSummary(const llvm::Function &, ModRefFunctionSummary &);

public:
  ModRefModuleAnalysis(const tpa::SemiSparsePointerAnalysis &p,
                       const annotation::ExternalModRefTable &t,
                       UnmodeledPolicy policy = UnmodeledPolicy::Ignore)
      : ptrAnalysis(p), unmodeledPolicy(policy), modRefTable(t) {}

  ModRefModuleSummary runOnModule(const llvm::Module &module);
};

} // namespace tpa::taint
