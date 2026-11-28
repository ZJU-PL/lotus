#pragma once

#include "Checker/TPA/FrontEnd/ReachingDefMap.h"
#include "Checker/TPA/Support/ExternalModels.h"

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

class DefUseFunction;
class ModRefModuleSummary;

using ReachingDefModuleMap = ReachingDefMap<llvm::Instruction>;

class ReachingDefModuleAnalysis {
private:
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  UnmodeledPolicy unmodeledPolicy;
  const ModRefModuleSummary &summaryMap;
  const annotation::ExternalModRefTable &modRefTable;
  bool enableMemoryGrouping;

public:
  ReachingDefModuleAnalysis(const tpa::SemiSparsePointerAnalysis &p,
                            const ModRefModuleSummary &s,
                            const annotation::ExternalModRefTable &t,
                            UnmodeledPolicy policy = UnmodeledPolicy::Ignore,
                            bool enableMemoryGrouping = true)
      : ptrAnalysis(p), unmodeledPolicy(policy), summaryMap(s), modRefTable(t),
        enableMemoryGrouping(enableMemoryGrouping) {}

  ReachingDefModuleMap runOnFunction(const llvm::Function &func);

  // Computes exact memory-level reaching definitions at basic-block
  // granularity (with location equivalence-class coalescing) and streams
  // memory def-use edges directly into `duFunc` without materializing
  // per-instruction reaching-definition maps.
  void buildMemoryDefUseEdges(DefUseFunction &duFunc) const;
};

} // namespace tpa::taint
