#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/CallGraph.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Checker/TPA/Support/ProgramPointSet.h"

namespace annotation {
class ExternalTaintTable;
} // namespace annotation

namespace tpa {
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {

class DefUseModule;
class TaintEnv;
class TaintMemo;

class TrackerGlobalState {
private:
  // The program
  const DefUseModule &duModule;

  // Pointer Analysis
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;

  // External taink annotations
  const annotation::ExternalTaintTable &extTable;

  // The environment
  const TaintEnv &env;

  // The global memo
  const TaintMemo &memo;

  // The call graph
  using CallGraphType = tpa::CallGraph<ProgramPoint, tpa::FunctionContext>;
  const CallGraphType &callGraph;

  ProgramPointSet &imprecisionSources;
  ProgramPointSet visited;

public:
  TrackerGlobalState(const DefUseModule &m,
                     const tpa::SemiSparsePointerAnalysis &p,
                     const annotation::ExternalTaintTable &t, const TaintEnv &e,
                     const TaintMemo &mm, const CallGraphType &cg,
                     ProgramPointSet &pps)
      : duModule(m), ptrAnalysis(p), extTable(t), env(e), memo(mm),
        callGraph(cg), imprecisionSources(pps) {}

  const DefUseModule &getDefUseModule() const { return duModule; }

  const tpa::SemiSparsePointerAnalysis &getPointerAnalysis() const {
    return ptrAnalysis;
  }
  const annotation::ExternalTaintTable &getExternalTaintTable() const {
    return extTable;
  }
  const TaintEnv &getEnv() const { return env; }
  const TaintMemo &getMemo() const { return memo; }
  const CallGraphType &getCallGraph() const { return callGraph; }

  bool insertVisitedLocation(const ProgramPoint &pp) {
    return visited.insert(pp);
  }

  void addImprecisionSource(const ProgramPoint &pp) {
    imprecisionSources.insert(pp);
  }
};

} // namespace tpa::taint
