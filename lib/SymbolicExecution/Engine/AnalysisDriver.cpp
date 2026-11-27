//===----------------------------------------------------------------------===//
//
// AnalysisDriver orchestrates whole-module symbolic execution.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/AnalysisDriver.h"

#include "SymbolicExecution/Models/MemoryAPI.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"
#include "Utils/Parallel/ThreadPool.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;
using namespace SymbolicExecution;

namespace {

void topsortCFG(std::vector<BasicBlock *> &Sorted, Function *F) {
  if (!F || F->empty())
    return;

  ReversePostOrderTraversal<Function *> RPOT(F);
  Sorted.reserve(F->size());
  for (BasicBlock *BB : RPOT) {
    Sorted.push_back(BB);
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

  // Track exact downstream summary consumers so completed callee summaries and
  // their Z3 solver contexts can be reclaimed as soon as all later callers
  // finish, preserving identical interprocedural behavior with bounded memory.
  std::vector<Function *> OrderedFuncs;
  OrderedFuncs.reserve(FuncSeq.size());
  std::unordered_map<Function *, size_t> FuncOrderIdx;
  for (auto Iter = FuncSeq.rbegin(), EIter = FuncSeq.rend(); Iter != EIter;
       ++Iter) {
    Function *CurFunc = *Iter;
    auto *Graph = Ctx.getGraph(CurFunc);
    if (!Graph)
      continue;
    if (Ctx.getFunctionDepth(CurFunc) <= AnalysisLimit::FUNC_INLINE_LIMIT_V) {
      FuncOrderIdx.emplace(CurFunc, OrderedFuncs.size());
      OrderedFuncs.push_back(CurFunc);
    }
  }

  std::unordered_map<Function *, std::vector<Function *>> LaterCallees;
  std::unordered_map<Function *, unsigned> RemainingConsumers;
  for (size_t CallerIdx = 0; CallerIdx < OrderedFuncs.size(); ++CallerIdx) {
    Function *Caller = OrderedFuncs[CallerIdx];
    std::unordered_set<Function *> SeenCallees;
    for (BasicBlock &BB : *Caller) {
      for (Instruction &I : BB) {
        if (!isa<CallInst>(&I))
          continue;
        Function *Callee = getCallee(&I);
        if (!Callee || Callee == Caller || !SeenCallees.insert(Callee).second)
          continue;
        auto It = FuncOrderIdx.find(Callee);
        if (It != FuncOrderIdx.end() && It->second < CallerIdx) {
          LaterCallees[Caller].push_back(Callee);
          ++RemainingConsumers[Callee];
        }
      }
    }
  }

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
      auto ConsIt = RemainingConsumers.find(CurFunc);
      const bool NeedsSummary =
          (ConsIt != RemainingConsumers.end() && ConsIt->second > 0);
      runOnFunction(Graph, NeedsSummary);

      auto CalleeIt = LaterCallees.find(CurFunc);
      if (CalleeIt != LaterCallees.end()) {
        for (Function *Callee : CalleeIt->second) {
          auto RemIt = RemainingConsumers.find(Callee);
          if (RemIt != RemainingConsumers.end() && RemIt->second > 0) {
            if (--RemIt->second == 0) {
              releaseMemForFunction(Callee);
            }
          }
        }
      }
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
    if (Solver) {
      std::lock_guard<std::mutex> Lock2(Solver->getSolverLock());
      AnalysisRes.erase(F);
    } else {
      AnalysisRes.erase(F);
    }
  } else {
    AnalysisRes.erase(F);
    SolverMgr.releaseFuncSolver(F);
  }
}

void AnalysisDriver::runOnFunction(GuardedValueFlowGraph *Graph,
                                   bool NeedsSummary) {
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
  AnalysisState State(Ctx, TaintSpec, Graph, F, Ext.get(), NeedsSummary);
  for (auto *BB : TopBBs) {
    for (Instruction &I : *BB) {
      State.transfer(&I, *this);
    }
  }

  State.finalizeSummary();
  Ext.reset();
  if (NeedsSummary) {
    addSummary(F, AnalysisSummary(std::move(State), SolverMgr));
  }
}
