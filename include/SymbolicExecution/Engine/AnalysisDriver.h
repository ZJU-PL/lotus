/** @file AnalysisDriver.h @brief Whole-module symbolic execution orchestrator. */
#pragma once

#include "SymbolicExecution/Engine/AnalysisExtension.h"
#include "SymbolicExecution/Engine/AnalysisState.h"
#include "SymbolicExecution/Engine/AnalysisSummary.h"
#include "SymbolicExecution/Models/TaintModel.h"
#include "SymbolicExecution/Solver/SummarySolverManager.h"
#include "SymbolicExecution/Support/AnalysisContext.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace SymbolicExecution {

/// Drives whole-function and whole-module symbolic execution.
///
/// Owns per-function summaries and their summary solver pool, schedules
/// analysis in bottom-up call-graph order, and invokes optional per-function
/// StateExtension hooks (such as bug checkers).
class AnalysisDriver {
public:
  using ExtensionFactory =
      std::function<std::unique_ptr<StateExtension>(llvm::Function *)>;

  AnalysisDriver(AnalysisContext &Ctx, const TaintModel &TaintSpec,
                 ExtensionFactory ExtFactory = nullptr);

  void runOnFunction(GuardedValueFlowGraph *Graph, bool NeedsSummary = true);
  void runOnModuleParallel(llvm::Module *M);
  void runOnModule(llvm::Module *M);

  void addSummary(llvm::Function *F, AnalysisSummary Smry);
  bool hasSummary(llvm::Function *F) const;
  const AnalysisSummary &getSummary(llvm::Function *F) const;

private:
  AnalysisContext &Ctx;
  const TaintModel &TaintSpec;
  ExtensionFactory ExtFactory;

  // SolverMgr must outlive AnalysisRes because summaries hold SMTExprs owned by
  // solvers stored in SolverMgr.
  SummarySolverManager SolverMgr;
  mutable std::mutex AnalysisMtx;
  std::unordered_map<llvm::Function *, std::unique_ptr<AnalysisSummary>>
      AnalysisRes;

  void releaseMemForFunction(llvm::Function *F);
};

} // namespace SymbolicExecution
