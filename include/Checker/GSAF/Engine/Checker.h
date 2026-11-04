#pragma once

#include "Analysis/CFG/CFGReachability.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "Checker/GSAF/API/Models.h"
#include "Checker/GSAF/API/VulnerabilityRegistry.h"
#include "Checker/GSAF/API/VulnerabilityWrapper.h"
#include "Checker/GSAF/Engine/Summaries.h"
#include "Checker/GSAF/Support/Options.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAAWrapper.h"
#include "Utils/LLVM/PackedTypeLayout.h"
#include "Utils/Parallel/ThreadPool.h"
#include "Utils/Parallel/ThreadSafe.h"

#include <map>
#include <memory>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/Error.h>

namespace lotus::gsaf {

template <typename T> using TraceList = lotus::ThreadSafeVector<T>;

struct GSAFSummary {
  std::map<const gvfg::GuardedValueFlowNode *,
           std::set<InputSummary *, trace_summary_cmp>, ObjectLess>
      InSmrys;
  std::map<const gvfg::GuardedValueFlowNode *,
           std::set<OutputSummary *, trace_summary_cmp>, ObjectLess>
      OutSmrys;
  std::map<const gvfg::GuardedValueFlowReturnNode *, SymbolicSummary *,
           ObjectLess>
      RetSmrys;
  std::map<const gvfg::GuardedValueFlowNode *,
           std::set<InputSummary *, trace_summary_cmp>, ObjectLess>
      TaintSourceWrapperSmrys;
  ~GSAFSummary();
};

class SMTAdvisor4Inlining : public SMTRenamingAdvisor {
public:
  bool prune(const SMTExpr &expression) override;
  bool rename(const SMTExpr &expression) override;
};

class GSAFChecker : public llvm::ModulePass {
  friend class FunctionAnalyzer;

  struct FunctionInfo {
    llvm::DominatorTree DT;
    llvm::PostDominatorTree PDT;
    CFGReachability Reachability;
    bool HasNonBackEdgeCaller = false;
    explicit FunctionInfo(llvm::Function &function)
        : DT(function), PDT(function), Reachability(function) {}
  };

  llvm::Module *Module = nullptr;
  gvfg::GuardedValueFlowGraphBuilderPass *GraphBuilder = nullptr;
  std::unique_ptr<PackedTypeLayout> DL;
  GSAFModels *Models = nullptr;
  DebugInfoAnalysis DebugInfo;
  std::unique_ptr<ir_expression::IRExpressionRenderer> Renderer;
  std::map<const llvm::Function *, std::unique_ptr<FunctionInfo>> Functions;
  std::shared_ptr<VulnerabilityWrapper> Vuln;
  std::map<const llvm::Function *, GSAFSummary *> FuncSmryMap;
  std::map<const llvm::Function *,
           TraceList<std::pair<int, std::shared_ptr<VulnerabilityTrace>>>>
      FuncTraceMap;
  SMTAdvisor4Inlining SMTAdvisor;
  ThreadPool::TaskGroup ReportTasks;

public:
  static char ID;
  explicit GSAFChecker(std::shared_ptr<VulnerabilityWrapper> vulnerabilities);
  ~GSAFChecker() override;
  bool runOnModule(llvm::Module &module) override;
  void getAnalysisUsage(llvm::AnalysisUsage &usage) const override;
  llvm::Module *getModule() const { return Module; }
  PackedTypeLayout &getPackedLayout() { return *DL; }
  DebugInfoAnalysis &getDebugInfo() { return DebugInfo; }
  ir_expression::IRExpressionRenderer &getRenderer() { return *Renderer; }
  gvfg::GuardedValueFlowGraph *getGraph(llvm::Function *function) const {
    return graphFor(GraphBuilder, function);
  }
  bool isReachable(llvm::Instruction *from, llvm::Instruction *to) const {
    if (!from || !to || from->getFunction() != to->getFunction())
      return false;
    return Functions.at(from->getFunction())->Reachability.reachable(from, to);
  }
  const llvm::DominatorTree &getDomTree(const llvm::Function *function) const {
    return Functions.at(function)->DT;
  }
  const llvm::PostDominatorTree &
  getPostDomTree(const llvm::Function *function) const {
    return Functions.at(function)->PDT;
  }
  bool hasNonBackEdgeCaller(const llvm::Function *function) const;
  std::vector<std::shared_ptr<VulnerabilityTrace>>
  traces(const std::shared_ptr<Vulnerability> &vulnerability) const;
};

class GSAFFunctionWorker {
protected:
  GSAFChecker *Parent;
  llvm::Function *F;
  GSAFFunctionWorker(GSAFChecker *parent, llvm::Function *function)
      : Parent(parent), F(function) {}

public:
  virtual ~GSAFFunctionWorker() = default;
  virtual void run() = 0;
  virtual void handleException() {}
};

/// Join completed policy traces according to the composite's dependencies.
void buildMultiVulnerability(
    const std::shared_ptr<MultiVulnerability> &vulnerability,
    const std::map<std::shared_ptr<Vulnerability>, GSAFChecker *> &checkers);

} // namespace lotus::gsaf
