#pragma once

#include "Checker/TPA/FrontEnd/ReachingDefMap.h"

namespace llvm {
class Instruction;
class Function;
} // namespace llvm

namespace annotation {
class ExternalModRefTable;
} // namespace annotation

namespace tpa {
class MemoryLocation;
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {

class ModRefModuleSummary;

using ReachingDefModuleMap = ReachingDefMap<llvm::Instruction>;

class ReachingDefModuleAnalysis {
private:
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  const ModRefModuleSummary &summaryMap;
  const annotation::ExternalModRefTable &modRefTable;

public:
  ReachingDefModuleAnalysis(const tpa::SemiSparsePointerAnalysis &p,
                            const ModRefModuleSummary &s,
                            const annotation::ExternalModRefTable &t)
      : ptrAnalysis(p), summaryMap(s), modRefTable(t) {}

  ReachingDefModuleMap runOnFunction(const llvm::Function &func);
};

} // namespace tpa::taint
