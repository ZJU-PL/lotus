#pragma once

#include "Checker/TPA/Annotation/ExternalTaintTable.h"
#include "Checker/TPA/Support/ProgramPointSet.h"
#include "Checker/TPA/Support/SinkViolationRecord.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintMemo.h"

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
  TaintEnv env;
  TaintMemo memo;
  annotation::ExternalTaintTable extTable;
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  SinkViolationRecord violations;
  ProgramPointSet lossSites;
  Statistics stats;

public:
  explicit TaintAnalysis(const tpa::SemiSparsePointerAnalysis &p)
      : ptrAnalysis(p) {}
  void loadExternalTaintTable(const char *fileName) {
    extTable = annotation::ExternalTaintTable::loadFromFile(fileName);
  }
  void setExternalTaintTable(annotation::ExternalTaintTable table) {
    extTable = std::move(table);
  }
  /// Clears earlier results. Returns true if no configured sink policy is
  /// violated.
  bool runOnDefUseModule(const DefUseModule &, bool trackPrecisionLoss = false);
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

/// Run TPA normalization and unify exits before constructing the pointer
/// program. Exit unification is mandatory even when optional prepasses are off.
void prepareModule(llvm::Module &, bool runTPAPrepasses = true);
} // namespace tpa::taint
