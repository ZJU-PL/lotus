#include "Checker/TPA/FrontEnd/ReachingDefModuleAnalysis.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/FrontEnd/ModRefModuleSummary.h"
#include "Checker/TPA/Program/Reachability.h"
#include "Utils/LLVM/FIFOWorkList.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;
using namespace tpa;

namespace tpa::taint {

namespace {

const Instruction *getNextInstruction(const Instruction *inst) {
  auto itr = inst->getIterator();
  ++itr;
  assert(itr != inst->getParent()->end());
  return &*itr;
}

class EvalVisitor : public InstVisitor<EvalVisitor> {
private:
  const SemiSparsePointerAnalysis &ptrAnalysis;
  UnmodeledPolicy unmodeledPolicy;
  const ModRefModuleSummary &summaryMap;
  const ExternalModRefTable &modRefTable;

  using StoreType = ReachingDefStore<Instruction>;
  StoreType store;

  void evalStore(Instruction &inst, Value *ptr) {
    auto pSet = ptrAnalysis.getPtsSet(ptr);
    bool needWeekUpdate = true;
    if (pSet.size() == 1) {
      const auto *obj = *pSet.begin();
      if (!obj->isSummaryObject()) {
        store.updateBinding(obj, &inst);
        needWeekUpdate = false;
      }
    }
    if (needWeekUpdate) {
      for (const auto *obj : pSet)
        store.insertBinding(obj, &inst);
    }
  }

  void modValue(const Value *v, const Instruction *inst,
                bool isReachableMemory) {
    auto pSet = ptrAnalysis.getPtsSet(v);
    for (const auto *loc : pSet) {
      if (isReachableMemory) {
        for (const auto *oLoc :
             ptrAnalysis.getMemoryManager().getReachableMemoryObjects(loc))
          store.insertBinding(oLoc, inst);
      } else {
        store.insertBinding(loc, inst);
      }
    }
  }

  void evalExternalCall(CallBase *cs, const Function *f) {
    const auto &summary = lookupModRefSummary(modRefTable, *f, unmodeledPolicy);

    for (auto const &effect : summary) {
      if (effect.isModEffect()) {
        auto const &pos = effect.getPosition();
        if (pos.isReturnPosition()) {
          modValue(cs->stripPointerCasts(), cs, effect.onReachableMemory());
        } else {
          auto const &argPos = pos.getAsArgPosition();
          unsigned idx = argPos.getArgIndex();

          if (!argPos.isAfterArgPosition())
            modValue(cs->getArgOperand(idx)->stripPointerCasts(), cs,
                     effect.onReachableMemory());
          else {
            for (auto i = idx, e = cs->arg_size(); i < e; ++i)
              if (cs->getArgOperand(i)->getType()->isPointerTy())
                modValue(cs->getArgOperand(i)->stripPointerCasts(), cs,
                         effect.onReachableMemory());
          }
        }
      }
    }
  }

public:
  EvalVisitor(const SemiSparsePointerAnalysis &p, const ModRefModuleSummary &sm,
              const ExternalModRefTable &e, const StoreType &s,
              UnmodeledPolicy policy)
      : ptrAnalysis(p), unmodeledPolicy(policy), summaryMap(sm), modRefTable(e),
        store(s) {}
  const StoreType &getStore() const { return store; }

  // Default case
  void visitInstruction(Instruction &) {}

  void visitAllocaInst(AllocaInst &allocInst) {
    auto pSet = ptrAnalysis.getPtsSet(&allocInst);
    for (const auto *obj : pSet)
      store.insertBinding(obj, &allocInst);
  }

  void visitStoreInst(StoreInst &storeInst) {
    auto *ptr = storeInst.getPointerOperand();
    evalStore(storeInst, ptr);
  }

  void visitAtomicRMWInst(AtomicRMWInst &inst) {
    evalStore(inst, inst.getPointerOperand());
  }
  void visitAtomicCmpXchgInst(AtomicCmpXchgInst &inst) {
    evalStore(inst, inst.getPointerOperand());
  }

  void visitCallBase(CallBase &call) { visitTaintCall(&call); }

  void visitTaintCall(CallBase *cs) {
    if (isDebugIntrinsic(*cs))
      return;
    auto callTgts = resolveCallTargets(ptrAnalysis, *cs, unmodeledPolicy);
    for (const auto *callee : callTgts) {
      if (callee->isDeclaration()) {
        evalExternalCall(cs, callee);
      } else {
        auto &summary = summaryMap.getSummary(callee);
        for (const auto *loc : summary.mem_writes())
          store.insertBinding(loc, cs);
      }
    }
  }
};

} // end of anonymous namespace

ReachingDefMap<Instruction>
ReachingDefModuleAnalysis::runOnFunction(const Function &func) {
  ReachingDefMap<Instruction> rdMap;

  const auto *entryInst = &func.getEntryBlock().front();
  auto &summary = summaryMap.getSummary(&func);
  auto &initStore = rdMap.getReachingDefStore(entryInst);
  for (const auto *loc : summary.mem_reads())
    initStore.insertBinding(loc, nullptr);
  // Write-only locations still need an entry definition on paths where no
  // write executes, so the caller's prior contents survive those paths.
  for (const auto *loc : summary.mem_writes())
    initStore.insertBinding(loc, nullptr);

  util::FIFOWorkList<const Instruction *> workList;
  workList.enqueue(entryInst);

  while (!workList.empty()) {
    const auto *inst = workList.dequeue();

    EvalVisitor evalVisitor(ptrAnalysis, summaryMap, modRefTable,
                            rdMap.getReachingDefStore(inst), unmodeledPolicy);
    evalVisitor.visit(const_cast<Instruction &>(*inst));

    if (inst->isTerminator()) {
      for (auto i = 0u, e = inst->getNumSuccessors(); i != e; ++i) {
        auto *succInst = &inst->getSuccessor(i)->front();
        if (rdMap.update(succInst, evalVisitor.getStore()))
          workList.enqueue(succInst);
      }
    } else {
      const auto *nextInst = getNextInstruction(inst);
      if (rdMap.update(nextInst, evalVisitor.getStore()))
        workList.enqueue(nextInst);
    }
  }

  return rdMap;
}

} // namespace tpa::taint