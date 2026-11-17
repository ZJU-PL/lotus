#include "Checker/TPA/FrontEnd/ModRefModuleAnalysis.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Program/Reachability.h"
#include "Utils/LLVM/FIFOWorkList.h"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;
using namespace tpa;

namespace tpa::taint {

using RevCallMapType =
    std::unordered_map<const Function *, util::VectorSet<const Instruction *>>;

namespace {

inline bool isLocalStackLocation(const MemoryObject *loc, const Function *f) {
  auto const &allocSite = loc->getAllocSite();

  switch (allocSite.getAllocType()) {
  // NullObj and UniversalObj are counted as local stack location because we
  // don't want them to appear in the mod-ref summaries
  case AllocSiteTag::Null:
  case AllocSiteTag::Universal:
    return true;
  case AllocSiteTag::Global:
  case AllocSiteTag::Function:
  case AllocSiteTag::Heap:
    return false;
  case AllocSiteTag::Stack: {
    if (const auto *allocInst = dyn_cast<AllocaInst>(allocSite.getLocalValue()))
      return allocInst->getParent()->getParent() == f;
    else
      return false;
  }
  }
}

bool updateSummary(ModRefFunctionSummary &callerSummary,
                   const ModRefFunctionSummary &calleeSummary,
                   const Function *caller) {
  bool changed = false;

  for (const auto *value : calleeSummary.value_reads())
    changed |= callerSummary.addValueRead(value);
  for (const auto *loc : calleeSummary.mem_reads()) {
    if (!isLocalStackLocation(loc, caller))
      changed |= callerSummary.addMemoryRead(loc);
  }
  for (const auto *loc : calleeSummary.mem_writes()) {
    if (!isLocalStackLocation(loc, caller))
      changed |= callerSummary.addMemoryWrite(loc);
  }

  return changed;
}

void updateRevCallGraph(RevCallMapType &revCallGraph, const Function &f,
                        const SemiSparsePointerAnalysis &ptrAnalysis) {
  for (const auto *bb : llvm::depth_first(&f.getEntryBlock())) {
    for (auto const &inst : *bb) {
      auto *cs = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (!cs)
        continue;

      auto callTgts = ptrAnalysis.getCallees(cs);
      for (const auto *callee : callTgts)
        revCallGraph[callee].insert(cs);
    }
  }
}

class SummaryInstVisitor : public InstVisitor<SummaryInstVisitor> {
private:
  const SemiSparsePointerAnalysis &ptrAnalysis;

  ModRefFunctionSummary &summary;

public:
  SummaryInstVisitor(const SemiSparsePointerAnalysis &p,
                     ModRefFunctionSummary &s)
      : ptrAnalysis(p), summary(s) {}

  void visitLoadInst(LoadInst &loadInst) {
    auto *ptrOp = loadInst.getPointerOperand();
    if (isa<GlobalValue>(ptrOp))
      summary.addValueRead(ptrOp->stripPointerCasts());
    auto pSet = ptrAnalysis.getPtsSet(ptrOp);
    for (const auto *obj : pSet) {
      if (!isLocalStackLocation(obj, loadInst.getParent()->getParent()))
        summary.addMemoryRead(obj);
    }
  }

  void visitStoreInst(StoreInst &storeInst) {
    auto *storeSrc = storeInst.getValueOperand();
    auto *storeDst = storeInst.getPointerOperand();

    if (isa<GlobalValue>(storeSrc))
      summary.addValueRead(storeSrc->stripPointerCasts());
    if (isa<GlobalValue>(storeDst))
      summary.addValueRead(storeDst->stripPointerCasts());

    auto pSet = ptrAnalysis.getPtsSet(storeDst);
    for (const auto *obj : pSet) {
      if (!isLocalStackLocation(obj, storeInst.getParent()->getParent()))
        summary.addMemoryWrite(obj);
    }
  }

  void visitInstruction(Instruction &inst) {
    for (auto &use : inst.operands()) {
      auto *value = use.get();
      if (isa<GlobalValue>(value))
        summary.addValueRead(value->stripPointerCasts());
    }
  }
};

bool addExternalMemoryWrite(const Value *v, ModRefFunctionSummary &summary,
                            const Function *caller,
                            const SemiSparsePointerAnalysis &ptrAnalysis,
                            bool isReachableMemory) {
  auto changed = false;
  auto pSet = ptrAnalysis.getPtsSet(v);
  for (const auto *loc : pSet) {
    if (isLocalStackLocation(loc, caller))
      continue;
    if (isReachableMemory) {
      for (const auto *oLoc :
           ptrAnalysis.getMemoryManager().getReachableMemoryObjects(loc))
        changed |= summary.addMemoryWrite(oLoc);
    } else {
      changed |= summary.addMemoryWrite(loc);
    }
  }
  return changed;
}

bool updateSummaryForModEffect(const Instruction *inst,
                               ModRefFunctionSummary &summary,
                               const SemiSparsePointerAnalysis &ptrAnalysis,
                               const ModRefEffect &modEffect) {
  assert(modEffect.isModEffect());

  auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  assert(cs);
  const auto *caller = inst->getParent()->getParent();

  auto const &pos = modEffect.getPosition();
  if (pos.isReturnPosition())
    return addExternalMemoryWrite(inst, summary, caller, ptrAnalysis,
                                  modEffect.onReachableMemory());
  else {
    auto const &argPos = pos.getAsArgPosition();
    unsigned idx = argPos.getArgIndex();

    if (!argPos.isAfterArgPosition())
      return addExternalMemoryWrite(cs->getArgOperand(idx)->stripPointerCasts(),
                                    summary, caller, ptrAnalysis,
                                    modEffect.onReachableMemory());
    else {
      bool changed = false;
      for (auto i = idx, e = cs->arg_size(); i < e; ++i)
        changed |= addExternalMemoryWrite(
            cs->getArgOperand(i)->stripPointerCasts(), summary, caller,
            ptrAnalysis, modEffect.onReachableMemory());
      return changed;
    }
  }
}

bool addExternalMemoryRead(const Value *v, ModRefFunctionSummary &summary,
                           const Function *caller,
                           const SemiSparsePointerAnalysis &ptrAnalysis,
                           bool isReachableMemory) {
  bool changed = false;
  auto pSet = ptrAnalysis.getPtsSet(v);
  for (const auto *loc : pSet) {
    if (isLocalStackLocation(loc, caller))
      continue;
    if (isReachableMemory) {
      for (const auto *oLoc :
           ptrAnalysis.getMemoryManager().getReachableMemoryObjects(loc))
        changed |= summary.addMemoryRead(oLoc);
    } else {
      changed |= summary.addMemoryRead(loc);
    }
  }
  return changed;
}

bool updateSummaryForRefEffect(const Instruction *inst,
                               ModRefFunctionSummary &summary,
                               const SemiSparsePointerAnalysis &ptrAnalysis,
                               const ModRefEffect &refEffect) {
  assert(refEffect.isRefEffect());

  auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  assert(cs);
  const auto *caller = inst->getParent()->getParent();

  auto const &pos = refEffect.getPosition();
  assert(!pos.isReturnPosition() &&
         "It doesn't make any sense to ref a return position!");

  auto const &argPos = pos.getAsArgPosition();
  unsigned idx = argPos.getArgIndex();

  if (!argPos.isAfterArgPosition())
    return addExternalMemoryRead(cs->getArgOperand(idx)->stripPointerCasts(),
                                 summary, caller, ptrAnalysis,
                                 refEffect.onReachableMemory());
  else {
    bool changed = false;
    for (auto i = idx, e = cs->arg_size(); i < e; ++i) {
      changed |= addExternalMemoryRead(
          cs->getArgOperand(i)->stripPointerCasts(), summary, caller,
          ptrAnalysis, refEffect.onReachableMemory());
    }
    return changed;
  }
}

bool updateSummaryForExternalCall(const Instruction *inst, const Function *f,
                                  ModRefFunctionSummary &summary,
                                  const SemiSparsePointerAnalysis &ptrAnalysis,
                                  const ExternalModRefTable &modRefTable) {
  const auto *modRefSummary = modRefTable.lookup(f->getName());
  if (modRefSummary == nullptr) {
    errs() << modRefTable.size() << "\n";
    errs() << "Missing entry in ModRefTable: " << f->getName() << "\n";
    llvm_unreachable("Consider adding the function to modref annotations");
  }

  bool changed = false;
  for (auto const &effect : *modRefSummary) {
    if (effect.isModEffect())
      changed |= updateSummaryForModEffect(inst, summary, ptrAnalysis, effect);
    else
      changed |= updateSummaryForRefEffect(inst, summary, ptrAnalysis, effect);
  }
  return changed;
}

void propagateSummary(ModRefModuleSummary &moduleSummary,
                      const RevCallMapType &revCallGraph,
                      const SemiSparsePointerAnalysis &ptrAnalysis,
                      const ExternalModRefTable &modRefTable) {
  auto workList = util::FIFOWorkList<const Function *>();

  // Enqueue all callees at the beginning
  for (auto const &mapping : revCallGraph)
    workList.enqueue(mapping.first);

  // Each function "push"es its info back into its callers
  while (!workList.empty()) {
    const auto *f = workList.dequeue();

    auto itr = revCallGraph.find(f);
    if (itr == revCallGraph.end())
      continue;
    auto &callerSet = itr->second;

    if (f->isDeclaration()) {
      for (const auto *callSite : callerSet) {
        const auto *caller = callSite->getParent()->getParent();
        auto &callerSummary = moduleSummary.getSummary(caller);
        if (updateSummaryForExternalCall(callSite, f, callerSummary,
                                         ptrAnalysis, modRefTable))
          workList.enqueue(caller);
      }
    } else {
      auto &calleeSummary = moduleSummary.getSummary(f);
      for (const auto *callSite : callerSet) {
        const auto *caller = callSite->getParent()->getParent();
        auto &callerSummary = moduleSummary.getSummary(caller);
        if (updateSummary(callerSummary, calleeSummary, caller))
          workList.enqueue(caller);
      }
    }
  }
}

} // end of anonymous namespace

void ModRefModuleAnalysis::collectProcedureSummary(
    const Function &f, ModRefFunctionSummary &summary) {
  SummaryInstVisitor summaryVisitor(ptrAnalysis, summary);
  for (const auto *bb : llvm::depth_first(&f.getEntryBlock()))
    summaryVisitor.visit(*const_cast<BasicBlock *>(bb));
}

ModRefModuleSummary ModRefModuleAnalysis::runOnModule(const Module &module) {
  ModRefModuleSummary moduleSummary;
  RevCallMapType revCallGraph;

  for (const auto *function : reachableFunctions(module, ptrAnalysis)) {
    const auto &f = *function;
    collectProcedureSummary(f, moduleSummary.getSummary(&f));
    updateRevCallGraph(revCallGraph, f, ptrAnalysis);
  }

  propagateSummary(moduleSummary, revCallGraph, ptrAnalysis, modRefTable);

  return moduleSummary;
}

} // namespace tpa::taint