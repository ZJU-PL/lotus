#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <unordered_set>

namespace llvm {
class Function;
class Value;
} // namespace llvm

namespace tpa::taint {

class ProgramPoint;
class TrackerGlobalState;
class TrackerWorkList;

class TrackerTransferFunction {
private:
  TrackerGlobalState &trackerState;
  TrackerWorkList &workList;

  using ValueSet = std::unordered_set<const llvm::Value *>;
  using MemorySet = std::unordered_set<const tpa::MemoryObject *>;

  void evalExternalCallInst(const ProgramPoint &, const llvm::Function *);

  ValueSet evalAllOperands(const ProgramPoint &);
  MemorySet evalPtsSet(const ProgramPoint &, tpa::PtsSet, const TaintStore &);
  ValueSet evalStore(const ProgramPoint &);
  MemorySet evalLoad(const ProgramPoint &, const TaintStore &);
  void evalCallInst(const ProgramPoint &);

  void evalEntryInst(const ProgramPoint &);
  void evalInst(const ProgramPoint &, const TaintStore *);

public:
  TrackerTransferFunction(TrackerGlobalState &g, TrackerWorkList &w)
      : trackerState(g), workList(w) {}

  void eval(const ProgramPoint &);
};

} // namespace tpa::taint
