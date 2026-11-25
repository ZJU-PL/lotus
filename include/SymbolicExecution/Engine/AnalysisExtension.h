/** @file AnalysisExtension.h @brief Extension hooks decoupling bug checkers from core symbolic state. */
#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"

#include <memory>

namespace SymbolicExecution {

class AnalysisState;
class PathCondSolver;

/// Per-function summary payload owned by an extension (e.g., pending bug queries).
class SummaryExtension {
public:
  virtual ~SummaryExtension() = default;

  /// Rewrites all SMT-backed state inside this summary onto NewSolver.
  virtual void translate(PathCondSolver *NewSolver) = 0;
};

/// Hook interface invoked by AnalysisState during intraprocedural transfer and summary import/export.
class StateExtension {
public:
  virtual ~StateExtension() = default;

  /// Invoked after operand normalization and before executing Inst's state transfer.
  virtual void onPreInstruction(llvm::Instruction *Inst,
                                const AnalysisState &State) {
    (void)Inst;
    (void)State;
  }

  /// Invoked after Inst's symbolic state and taint updates complete.
  virtual void onPostInstruction(llvm::Instruction *Inst,
                                 const AnalysisState &State) = 0;

  /// Invoked during call-site summary import after caller/callee mappings and taint are applied.
  virtual void onCallSummary(llvm::CallInst *Inst, llvm::Function *Callee,
                             const SummaryExtension *CalleeSummary,
                             const AnalysisState &State) = 0;

  /// Invoked during summary finalization to discharge local queries and export residual summary data.
  virtual std::unique_ptr<SummaryExtension>
  finalizeSummary(const AnalysisState &State) = 0;
};

} // namespace SymbolicExecution
