/** @file SummarySolverManager.h @brief Solver pool that owns solvers for function summaries. */
#pragma once

#include "SymbolicExecution/Solver/PathCondSolver.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SymbolicExecution {

/// Pool of solvers backing interprocedural function summaries for one analysis run.
class SummarySolverManager {
public:
  SummarySolverManager() = default;
  ~SummarySolverManager() = default;
  SummarySolverManager(const SummarySolverManager &) = delete;
  SummarySolverManager &operator=(const SummarySolverManager &) = delete;

  void init(const llvm::DataLayout &DL, unsigned Num = 128);
  PathCondSolver *getSharedSmrySolver();
  PathCondSolver *registerFuncSolver(const llvm::Function *Func,
                                     std::unique_ptr<PathCondSolver> Solver);
  void releaseFuncSolver(const llvm::Function *Func);

  bool isFuncSolverFull() const {
    std::lock_guard<std::mutex> LK(Mtx);
    return FuncSolvers.size() >= AnalysisLimit::MAX_FUNC_SOLVER_LIMIT_V;
  }

private:
  mutable std::mutex Mtx;
  unsigned NextSolverIdx = 0;
  std::vector<std::unique_ptr<PathCondSolver>> Solvers;
  std::unordered_map<const llvm::Function *, std::unique_ptr<PathCondSolver>>
      FuncSolvers;
};

} // namespace SymbolicExecution
