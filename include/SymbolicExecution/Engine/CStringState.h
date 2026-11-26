/** @file CStringState.h @brief C-string length tracking state for symbolic execution. */
#pragma once

#include "SymbolicExecution/Engine/GuardedValue.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include <unordered_map>

namespace SymbolicExecution {

class AnalysisState;
class AnalysisSummary;

/// Manages symbolic length tracking for C strings.
class CStringState {
  friend class AnalysisSummary;

public:
  static ProgramValuePtr getLenVAtCaller(const ProgramValuePtr &LenV,
                                         llvm::Instruction *CS);
  static PropertyValuePtr getAbsurdStrLen();

  void handleCStrLen(const ProgramValuePtr &Ptr, llvm::Instruction *Loc,
                     const Condition &Cond, AnalysisState &CurState,
                     bool IsDirect);

  GuardedSymbolicValSet getCStrlen(const ProgramValuePtr &Ptr,
                                   llvm::Instruction *Loc, bool IsDirect,
                                   const AnalysisState &CurState) const;

  void onProcessCall(llvm::Instruction *Inst, llvm::Function *Callee,
                     AnalysisState &CurState, const CStringState &Smry);

  const std::unordered_map<ProgramValuePtr, PtsSet> &getLenPts() const {
    return LenPts;
  }

private:
  std::unordered_map<ProgramValuePtr, PtsSet> LenPts;

  ProgramValuePtr getLenVariable(const ProgramValuePtr &Ptr,
                                 llvm::Instruction *Loc,
                                 const AnalysisState &CurState,
                                 bool IsDirect) const;

  GuardedSymbolicValSet computeCStrLength(const ProgramValuePtr &Len,
                                          llvm::Instruction *Loc,
                                          const PtsSet &Pts,
                                          AnalysisState &CurState,
                                          const Condition &Cond);

  GuardedSymbolicValSet computeCStrLength(const ProgramValuePtr &Len,
                                          llvm::Instruction *Loc,
                                          const PTItem &Pt,
                                          AnalysisState &CurState,
                                          const Condition &Cond);

  void translate(PathCondSolver *NewSolver) {
    for (auto &P : LenPts) {
      P.second.translate(NewSolver);
    }
  }
};

} // namespace SymbolicExecution
