#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/CallGraph.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Checker/TPA/Support/ExternalModels.h"
#include "Checker/TPA/Support/SinkSignature.h"

#include <unordered_set>

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

class TaintGlobalState {
private:
  // The program
  const DefUseModule &duModule;
  UnmodeledPolicy unmodeledPolicy;

  // Pointer Analysis
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;

  // External taink annotations
  const annotation::ExternalTaintTable &extTaintTable;

  // The environment
  TaintEnv &env;

  // The global memo
  TaintMemo &memo;

  // The call graph
  using CallGraphType = tpa::CallGraph<ProgramPoint, tpa::FunctionContext>;
  CallGraphType callGraph;

  // Taint sinks
  using SinkSet = std::unordered_set<SinkSignature>;
  SinkSet sinkSet;
  std::unordered_set<const llvm::CallBase *> ignoredUnresolvedCalls;

public:
  TaintGlobalState(const DefUseModule &m,
                   const tpa::SemiSparsePointerAnalysis &p,
                   const annotation::ExternalTaintTable &t, TaintEnv &e,
                   TaintMemo &mm, UnmodeledPolicy policy)
      : duModule(m), unmodeledPolicy(policy), ptrAnalysis(p), extTaintTable(t),
        env(e), memo(mm) {}

  UnmodeledPolicy getUnmodeledPolicy() const { return unmodeledPolicy; }

  const DefUseModule &getDefUseModule() const { return duModule; }

  const tpa::SemiSparsePointerAnalysis &getPointerAnalysis() const {
    return ptrAnalysis;
  }
  const annotation::ExternalTaintTable &getExternalTaintTable() const {
    return extTaintTable;
  }

  TaintEnv &getEnv() { return env; }
  const TaintEnv &getEnv() const { return env; }

  TaintMemo &getMemo() { return memo; }
  const TaintMemo &getMemo() const { return memo; }

  CallGraphType &getCallGraph() { return callGraph; }
  const CallGraphType &getCallGraph() const { return callGraph; }

  void recordIgnoredUnresolvedCall(const llvm::CallBase *call) {
    ignoredUnresolvedCalls.insert(call);
  }
  const auto &getIgnoredUnresolvedCalls() const {
    return ignoredUnresolvedCalls;
  }

  SinkSet &getSinks() { return sinkSet; }
  const SinkSet &getSinks() const { return sinkSet; }
};

} // namespace tpa::taint