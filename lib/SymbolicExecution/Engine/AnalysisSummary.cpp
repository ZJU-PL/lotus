//===----------------------------------------------------------------------===//
//
// Interprocedural function summary construction and translation.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/AnalysisSummary.h"

#include "SymbolicExecution/Engine/AnalysisState.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"
#include "SymbolicExecution/Solver/SummarySolverManager.h"

#include <cassert>
#include <mutex>
#include <utility>

using namespace SymbolicExecution;

namespace {

template <typename MapTy, typename FuncTy>
void forEachMap(MapTy &M, FuncTy Proc) {
  for (auto &P : M) {
    Proc(P.first, P.second);
  }
}

} // namespace

void AnalysisSummary::translate() {
  assert(SolverShared);

  std::lock_guard<std::mutex> LK(SmrySolver->getSolverLock());

  forEachMap(OutSymbolicValMap,
             [&](const ProgramValuePtr &, GuardedSymbolicValSet &V) {
               V.translate(SmrySolver);
             });

  forEachMap(EscapeAllocToSizes,
             [&](const ProgramValuePtr &, GuardedSymbolicValSet &V) {
               V.translate(SmrySolver);
             });

  forEachMap(OutputPts, [&](const ProgramValuePtr &, PtsSet &V) {
    V.translate(SmrySolver);
  });

  StrState.translate(SmrySolver);
  TaintSmry.translate(SmrySolver);

  if (ExtSummary) {
    ExtSummary->translate(SmrySolver);
  }
}

AnalysisSummary::AnalysisSummary(AnalysisState &&State,
                                 SummarySolverManager &SolverMgr)
    : Func(State.F), Graph(State.Graph),
      OutSymbolicValMap(std::move(State.OutSymbolicValMap)),
      EscapeAllocToSizes(std::move(State.EscapeAllocToSizes)),
      OutputPts(std::move(State.OutputPts)),
      UnknownSyms(std::move(State.UnknownSyms)),
      StrState(std::move(State.StrState)),
      TaintSmry(std::move(State.TaintSmry)),
      ExtSummary(std::move(State.FinalizedExt)) {
  if (!State.hasSolver()) {
    SmrySolver = nullptr;
    SolverShared = false;
  } else if (SolverMgr.isFuncSolverFull()) {
    SmrySolver = SolverMgr.getSharedSmrySolver();
    SolverShared = true;
    translate();
  } else {
    SmrySolver = SolverMgr.registerFuncSolver(Func, State.releaseSolver());
    SolverShared = false;
  }
}
