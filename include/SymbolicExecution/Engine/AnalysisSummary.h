/** @file AnalysisSummary.h @brief Interprocedural function summary exported by symbolic execution. */
#pragma once

#include "SymbolicExecution/Engine/AnalysisExtension.h"
#include "SymbolicExecution/Engine/CStringState.h"
#include "SymbolicExecution/Engine/GuardedValue.h"
#include "SymbolicExecution/Engine/TaintState.h"

#include "llvm/IR/Function.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace SymbolicExecution {

class AnalysisState;
class SummarySolverManager;

/// Interprocedural summary exported from one analyzed function.
class AnalysisSummary {
  friend class AnalysisState;

public:
  AnalysisSummary(AnalysisState &&State, SummarySolverManager &SolverMgr);
  AnalysisSummary(AnalysisSummary &&) noexcept = default;
  AnalysisSummary &operator=(AnalysisSummary &&) noexcept = default;
  AnalysisSummary(const AnalysisSummary &) = delete;
  AnalysisSummary &operator=(const AnalysisSummary &) = delete;

  bool isSolverShared() const { return SolverShared; }
  llvm::Function *getFunc() const { return Func; }
  PathCondSolver *getSmrySolver() const { return SmrySolver; }
  lotus::gvfg::GuardedValueFlowGraph *getGraph() const { return Graph; }

  const std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> &
  getOutSymbolicValMap() const {
    return OutSymbolicValMap;
  }

  const std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> &
  getEscapeInfo() const {
    return EscapeAllocToSizes;
  }

  const TaintSummary &getTaintSmry() const { return TaintSmry; }
  const std::unordered_set<ProgramValuePtr> &getUnknownSyms() const {
    return UnknownSyms;
  }
  const CStringState &getStrState() const { return StrState; }
  const SummaryExtension *getExtension() const { return ExtSummary.get(); }

private:
  PathCondSolver *SmrySolver = nullptr;
  bool SolverShared = false;

  llvm::Function *Func = nullptr;
  lotus::gvfg::GuardedValueFlowGraph *Graph = nullptr;

  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> OutSymbolicValMap;
  std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> EscapeAllocToSizes;
  std::unordered_map<ProgramValuePtr, PtsSet> OutputPts;
  std::unordered_set<ProgramValuePtr> UnknownSyms;
  CStringState StrState;
  TaintSummary TaintSmry;
  std::unique_ptr<SummaryExtension> ExtSummary;

  void translate();
};

} // namespace SymbolicExecution
