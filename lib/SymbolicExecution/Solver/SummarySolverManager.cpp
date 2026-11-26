//===----------------------------------------------------------------------===//
//
// Solver pool backing interprocedural function summaries.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Solver/SummarySolverManager.h"

#include <cassert>
#include <utility>

namespace SymbolicExecution {

void SummarySolverManager::init(const llvm::DataLayout &DL, unsigned Num) {
  assert(Num > 0);
  std::lock_guard<std::mutex> LK(Mtx);

  FuncSolvers.clear();
  NextSolverIdx = 0;
  Solvers.resize(Num);
  for (unsigned Idx = 0; Idx < Num; ++Idx) {
    Solvers[Idx] = std::make_unique<PathCondSolver>(DL);
  }
}

PathCondSolver *SummarySolverManager::getSharedSmrySolver() {
  std::lock_guard<std::mutex> LK(Mtx);

  auto *Res = Solvers[NextSolverIdx].get();
  NextSolverIdx = (NextSolverIdx + 1) % Solvers.size();
  return Res;
}

PathCondSolver *SummarySolverManager::registerFuncSolver(
    const llvm::Function *Func, std::unique_ptr<PathCondSolver> Solver) {
  std::lock_guard<std::mutex> LK(Mtx);

  auto *Raw = Solver.get();
  FuncSolvers[Func] = std::move(Solver);
  return Raw;
}

void SummarySolverManager::releaseFuncSolver(const llvm::Function *Func) {
  std::lock_guard<std::mutex> LK(Mtx);
  FuncSolvers.erase(Func);
}

} // namespace SymbolicExecution
