//===----------------------------------------------------------------------===//
//
// AnalysisDriver orchestrates whole-module symbolic execution.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/AnalysisDriver.h"

#include "SymbolicExecution/Models/MemoryAPI.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"
#include "Utils/Parallel/ThreadPool.h"

#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>
#include <numeric>
#include <unordered_map>
#include <vector>

using namespace llvm;
using namespace SymbolicExecution;

namespace {

void topsortCFG(std::vector<BasicBlock *> &Sorted, Function *F) {
  if (!F)
    return;

  std::unordered_map<BasicBlock *, unsigned> Indegree;
  std::deque<BasicBlock *> Worklist;

  for (BasicBlock &BB : *F) {
    unsigned Count = 0;
    for (auto PI = pred_begin(&BB), PE = pred_end(&BB); PI != PE; ++PI)
      ++Count;
    Indegree[&BB] = Count;
  }

  for (BasicBlock &BB : *F) {
    if (Indegree[&BB] == 0)
      Worklist.push_back(&BB);
  }

  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.front();
    Worklist.pop_front();
    Sorted.push_back(BB);

    for (BasicBlock *Succ : successors(BB)) {
      auto It = Indegree.find(Succ);
      if (It == Indegree.end())
        continue;
      if (It->second > 0)
        --It->second;
      if (It->second == 0)
        Worklist.push_back(Succ);
    }
  }

  for (BasicBlock &BB : *F) {
    if (std::find(Sorted.begin(), Sorted.end(), &BB) == Sorted.end())
      Sorted.push_back(&BB);
  }
}

} // namespace

AnalysisDriver::AnalysisDriver(AnalysisContext &Ctx,
                               const TaintModel &TaintSpec,
                               ExtensionFactory ExtFactory)
    : Ctx(Ctx), TaintSpec(TaintSpec), ExtFactory(std::move(ExtFactory)) {}

void AnalysisDriver::runOnModuleParallel(Module *M) {
  assert(M && Ctx.getDataLayout());
  SolverMgr.init(*Ctx.getDataLayout());

  Ctx.computeCallGraphOrder();
  const auto &FuncSeq = Ctx.getFuncSeq();
  if (FuncSeq.empty())
    return;

  std::vector<GuardedValueFlowGraph *> Worklist;
  Worklist.reserve(FuncSeq.size());

  for (auto Iter = FuncSeq.rbegin(), EIter = FuncSeq.rend(); Iter != EIter;
       ++Iter) {
    Function *CurFunc = *Iter;
    if (CurFunc->isDeclaration()) {
      continue;
    }

    auto *Graph = Ctx.getGraph(CurFunc);
    if (!Graph) {
      continue;
    }

    unsigned FunDepth = Ctx.getFunctionDepth(CurFunc);
    if (FunDepth > AnalysisLimit::FUNC_INLINE_LIMIT_V) {
      llvm::errs() << "Skip function " << CurFunc->getName()
                   << " due to inline threshold!\n";
      continue;
    }

    Worklist.push_back(Graph);
  }

  std::atomic<unsigned> NumRemain(Worklist.size());
  std::mutex ProgressMtx;

  llvm::errs() << "[Progress] Start ... " << NumRemain.load()
               << "functions to run!\n";

  ThreadPool::get()->parallelForEach(
      Worklist, 1,
      [this, &NumRemain, &ProgressMtx](GuardedValueFlowGraph *Graph) {
        Function *CurFunc = Graph->getBaseFunction();
        unsigned FunDepth = Ctx.getFunctionDepth(CurFunc);

        {
          std::lock_guard<std::mutex> Lock(ProgressMtx);
          llvm::errs() << "Running on " << CurFunc->getName()
                       << "(depth=" << FunDepth << ")\n";
        }

        runOnFunction(Graph);

        unsigned Remain = NumRemain.fetch_sub(1) - 1;
        {
          std::lock_guard<std::mutex> Lock(ProgressMtx);
          llvm::errs() << "[Progress] " << Remain << "functions remains!\n";
        }
      });
}

void AnalysisDriver::runOnModule(Module *M) {
  assert(M && Ctx.getDataLayout());
  SolverMgr.init(*Ctx.getDataLayout());

  Ctx.computeCallGraphOrder();
  const auto &FuncSeq = Ctx.getFuncSeq();
  if (FuncSeq.empty())
    return;

  unsigned NumRemain = std::accumulate(
      FuncSeq.begin(), FuncSeq.end(), 0u, [](unsigned Sum, Function *Fun) {
        return Fun->isDeclaration() ? Sum : Sum + 1;
      });

  llvm::errs() << "[Progress] Start ... " << NumRemain << "functions to run!\n";
  for (auto Iter = FuncSeq.rbegin(), EIter = FuncSeq.rend(); Iter != EIter;
       ++Iter) {
    Function *CurFunc = *Iter;
    auto *Graph = Ctx.getGraph(CurFunc);
    if (!Graph) {
      continue;
    }

    unsigned FunDepth = Ctx.getFunctionDepth(CurFunc);
    if (FunDepth > AnalysisLimit::FUNC_INLINE_LIMIT_V) {
      llvm::errs() << "Skip function " << CurFunc->getName()
                   << " due to inline threshold!\n";
    } else {
      llvm::errs() << "Running on " << CurFunc->getName()
                   << "(depth=" << FunDepth << ")\n";
      runOnFunction(Graph);
    }

    llvm::errs() << "[Progress] " << --NumRemain << "functions remains!\n";
  }
}

void AnalysisDriver::addSummary(Function *F, AnalysisSummary Smry) {
  std::lock_guard<std::mutex> Lock(AnalysisMtx);
  AnalysisRes.emplace(F, std::make_unique<AnalysisSummary>(std::move(Smry)));
}

bool AnalysisDriver::hasSummary(Function *F) const {
  std::lock_guard<std::mutex> Lock(AnalysisMtx);
  return AnalysisRes.count(F) != 0;
}

const AnalysisSummary &AnalysisDriver::getSummary(Function *F) const {
  std::lock_guard<std::mutex> Lock(AnalysisMtx);
  return *AnalysisRes.at(F);
}

void AnalysisDriver::releaseMemForFunction(Function *F) {
  std::lock_guard<std::mutex> Lock1(AnalysisMtx);
  if (!AnalysisRes.count(F)) {
    return;
  }

  auto *Smry = AnalysisRes.at(F).get();
  assert(Smry->getFunc() == F);

  if (Smry->isSolverShared()) {
    auto *Solver = Smry->getSmrySolver();
    std::lock_guard<std::mutex> Lock2(Solver->getSolverLock());
    AnalysisRes.erase(F);
  } else {
    AnalysisRes.erase(F);
    SolverMgr.releaseFuncSolver(F);
  }
}

void AnalysisDriver::runOnFunction(GuardedValueFlowGraph *Graph) {
  Function *F = Graph->getBaseFunction();

  if (AllocatorAPI::get(F)) {
    return;
  }

  std::vector<BasicBlock *> TopBBs;
  {
    std::lock_guard<std::mutex> Lock(AnalysisMtx);
    topsortCFG(TopBBs, F);
  }

  std::unique_ptr<StateExtension> Ext = ExtFactory ? ExtFactory(F) : nullptr;
  AnalysisState State(Ctx, TaintSpec, Graph, F, Ext.get());
  for (auto *BB : TopBBs) {
    for (Instruction &I : *BB) {
      State.transfer(&I, *this);
    }
  }

  State.finalizeSummary();
  addSummary(F, AnalysisSummary(std::move(State), SolverMgr));
}
