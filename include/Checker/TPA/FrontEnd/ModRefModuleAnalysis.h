#pragma once

#include "Checker/TPA/FrontEnd/ModRefModuleSummary.h"

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
  const annotation::ExternalModRefTable &modRefTable;

  void collectProcedureSummary(const llvm::Function &, ModRefFunctionSummary &);

public:
  ModRefModuleAnalysis(const tpa::SemiSparsePointerAnalysis &p,
                       const annotation::ExternalModRefTable &t)
      : ptrAnalysis(p), modRefTable(t) {}

  ModRefModuleSummary runOnModule(const llvm::Module &module);
};

} // namespace tpa::taint
