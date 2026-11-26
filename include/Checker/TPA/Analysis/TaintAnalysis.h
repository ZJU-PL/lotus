#pragma once

#include "Checker/TPA/Annotation/ExternalTaintTable.h"
#include "Checker/TPA/Support/ExternalModels.h"
#include "Checker/TPA/Support/ProgramPointSet.h"
#include "Checker/TPA/Support/SinkViolationRecord.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintMemo.h"

#include <cstddef>
#include <utility>

namespace llvm {
class Module;
} // namespace llvm
namespace tpa {
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {
class DefUseModule;

/// TPA-native explicit-flow sparse taint baseline, ported from Jia Chen's TPA.
/// The pointer analysis, its program, module and context policy must remain
/// alive and unchanged while this analysis and its results are used.
class TaintAnalysis {
public:
  struct Statistics {
    size_t evaluations = 0;
    size_t valueFacts = 0;
    size_t memoryStates = 0;
    size_t contexts = 0;
    size_t sinks = 0;
    size_t violations = 0;
    size_t topEdges = 0;
    size_t memoryEdges = 0;
  };

private:
  UnmodeledPolicy unmodeledPolicy;
  TaintEnv env;
  TaintMemo memo;
  annotation::ExternalTaintTable extTable;
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  SinkViolationRecord violations;
  ProgramPointSet lossSites;
  std::unordered_set<const llvm::CallBase *> ignoredUnresolvedCalls;
  Statistics stats;

public:
  explicit TaintAnalysis(const tpa::SemiSparsePointerAnalysis &p,
                         UnmodeledPolicy policy = UnmodeledPolicy::Ignore)
      : unmodeledPolicy(policy), ptrAnalysis(p) {}
  void loadExternalTaintTable(const char *fileName) {
    extTable = annotation::ExternalTaintTable::loadFromFile(fileName);
  }
  void setExternalTaintTable(annotation::ExternalTaintTable table) {
    extTable = std::move(table);
  }
  /// Clears earlier results. Returns true if no configured sink policy is
  /// violated.
  bool runOnDefUseModule(const DefUseModule &, bool trackPrecisionLoss = false);
  const auto &getIgnoredUnresolvedCalls() const {
    return ignoredUnresolvedCalls;
  }
  const TaintEnv &getEnv() const { return env; }
  const TaintMemo &getMemo() const { return memo; }
  const SinkViolationRecord &getViolations() const { return violations; }
  const ProgramPointSet &getLossSites() const { return lossSites; }
  const Statistics &getStatistics() const { return stats; }
  void printResults(llvm::raw_ostream &) const;
};

/// Checks the supported IR subset before building/running the baseline.
/// Unsupported instructions are errors, never silently treated as clean.
bool validateModule(const llvm::Module &, llvm::raw_ostream &errors);

/// Reject broken executable IR; recover debug-only errors by stripping debug
/// metadata. Returns success, and reports whether recovery stripped metadata.
bool verifyInputModule(llvm::Module &, llvm::raw_ostream &errors,
                       bool &debugStripped);

/// Run TPA normalization and unify exits before constructing the pointer
/// program. Exit unification is mandatory even when optional prepasses are off.
void prepareModule(llvm::Module &, bool runTPAPrepasses = true);
} // namespace tpa::taint
