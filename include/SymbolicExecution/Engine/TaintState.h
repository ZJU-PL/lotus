/** @file TaintState.h @brief Taint facts and trace steps tracked by symbolic execution. */
#pragma once

#include "SymbolicExecution/Engine/GuardedValue.h"

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <unordered_map>
#include <vector>

namespace SymbolicExecution {

class TaintValSet : public GuardedSet<TaintValSet, ProgramValuePtr,
                                      &AnalysisLimit::TAINT_VAL_SET_LIMIT_V> {};

class TaintStep {
public:
  enum TaintStepKind { TAINT_STEP_SOURCE, TAINT_STEP_CALL, TAINT_STEP_PROP };

  TaintStep(TaintStepKind TK, llvm::Instruction *Inst, llvm::Value *V1 = nullptr,
            llvm::Value *V2 = nullptr)
      : TK(TK), Inst(Inst), V1(V1), V2(V2) {}

  TaintStepKind TK;
  llvm::Instruction *Inst;
  llvm::Value *V1;
  llvm::Value *V2;
};

/// Summary of taint facts exported from one analyzed function.
class TaintSummary {
  friend class AnalysisState;
  friend class AnalysisSummary;

public:
  TaintSummary(const TaintValSet &TaintedFormals,
               const TaintValSet &TaintedRets,
               const std::unordered_map<ProgramValuePtr, std::vector<TaintStep>>
                   &TaintedSteps)
      : TaintedFormals(TaintedFormals), TaintedRets(TaintedRets),
        TaintedSteps(TaintedSteps) {}

  TaintSummary() = default;

  std::vector<TaintStep> getTaintSteps(const ProgramValuePtr &V) const {
    return TaintedSteps.at(V);
  }

private:
  TaintValSet TaintedFormals;
  TaintValSet TaintedRets;
  std::unordered_map<ProgramValuePtr, std::vector<TaintStep>> TaintedSteps;

  void translate(PathCondSolver *NewSolver) {
    TaintedFormals.translate(NewSolver);
    TaintedRets.translate(NewSolver);
  }
};

} // namespace SymbolicExecution
