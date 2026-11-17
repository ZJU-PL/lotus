#include "Checker/TPA/FrontEnd/DefUseModuleBuilder.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"
#include "Checker/TPA/FrontEnd/ModRefModuleAnalysis.h"
#include "Checker/TPA/FrontEnd/ReachingDefModuleAnalysis.h"
#include "Checker/TPA/Program/Reachability.h"

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;
using namespace tpa;

namespace tpa::taint {

namespace {

void buildTopLevelEdges(DefUseFunction &duFunc) {
  auto const &func = duFunc.getFunction();
  for (auto const &bb : func) {
    for (auto const &inst : bb) {
      auto *duInst = duFunc.getDefUseInstruction(&inst);
      if (!duInst)
        continue;
      for (const auto *user : inst.users()) {
        if (const auto *instUser = dyn_cast<Instruction>(user)) {
          assert(inst.getParent()->getParent() ==
                 instUser->getParent()->getParent());

          auto *duInstUser = duFunc.getDefUseInstruction(instUser);
          if (duInstUser)
            duInst->insertTopLevelEdge(duInstUser);
        }
      }
    }
  }
}

void addExternalDefs(DefUseFunction &duFunc) {
  auto *entryInst = duFunc.getEntryInst();
  for (auto const &bb : duFunc.getFunction()) {
    for (auto const &inst : bb) {
      if (!duFunc.getDefUseInstruction(&inst))
        continue;
      if (const auto *allocInst = dyn_cast<AllocaInst>(&inst)) {
        entryInst->insertTopLevelEdge(duFunc.getDefUseInstruction(allocInst));
        continue;
      }
      if (const auto *retInst = dyn_cast<ReturnInst>(&inst)) {
        auto *retVal = retInst->getReturnValue();
        if (retVal == nullptr || isa<Constant>(retVal)) {
          entryInst->insertTopLevelEdge(duFunc.getDefUseInstruction(retInst));
        }
      }
      for (auto &use : inst.operands()) {
        auto *value = use.get();
        if (isa<Constant>(value) || isa<Argument>(value)) {
          entryInst->insertTopLevelEdge(duFunc.getDefUseInstruction(&inst));
          break;
        }
      }
    }
  }
}

void addExternalUses(DefUseFunction &duFunc,
                     const ReachingDefMap<Instruction> &rdMap,
                     const ModRefFunctionSummary &summary) {
  for (auto *exitInst : duFunc.getExitInsts()) {
    auto &rdStore = rdMap.getReachingDefStore(exitInst->getInstruction());
    for (auto const &mapping : rdStore) {
      const auto *loc = mapping.first;
      if (summary.isMemoryWritten(loc))
        for (auto *inst : mapping.second)
          // A null definition denotes memory supplied at function entry.
          (inst ? duFunc.getDefUseInstruction(inst) : duFunc.getEntryInst())
              ->insertMemLevelEdge(loc, exitInst);
    }
  }
}

void computeNodePriority(DefUseFunction &duFunc) {
  auto &f = duFunc.getFunction();

  size_t currPrio = 1;
  for (const auto &bb : f)
    for (const auto &inst : bb)
      if (auto *node = duFunc.getDefUseInstruction(&inst))
        node->setPriority(currPrio++);
  for (auto itr = po_begin(&f), ite = po_end(&f); itr != ite; ++itr) {
    const auto *bb = *itr;
    for (auto bItr = bb->rbegin(), bIte = bb->rend(); bItr != bIte; ++bItr) {
      auto *duInst = duFunc.getDefUseInstruction(&*bItr);
      if (duInst != nullptr)
        duInst->setPriority(currPrio++);
    }
  }
}

class MemReadVisitor : public llvm::InstVisitor<MemReadVisitor> {
private:
  const SemiSparsePointerAnalysis &ptrAnalysis;
  const ModRefModuleSummary &summaryMap;
  const ExternalModRefTable &modRefTable;

  using RDMapType = ReachingDefMap<Instruction>;
  const RDMapType &rdMap;

  DefUseFunction &duFunc;

  void addMemDefUseEdge(const MemoryObject *obj,
                        const ReachingDefStore<Instruction> &rdStore,
                        DefUseInstruction *dstDuInst) {
    const auto *nodeSet = rdStore.getReachingDefs(obj);
    if (nodeSet == nullptr) {
      duFunc.getEntryInst()->insertMemLevelEdge(obj, dstDuInst);
      return;
    }
    for (const auto *srcInst : *nodeSet) {
      auto *srcDuInst = srcInst == nullptr
                            ? duFunc.getEntryInst()
                            : duFunc.getDefUseInstruction(srcInst);
      srcDuInst->insertMemLevelEdge(obj, dstDuInst);
    }
  }

  void processMemRead(Instruction &inst, Value *ptr, bool isReachableMemory) {
    auto pSet = ptrAnalysis.getPtsSet(ptr);
    if (!pSet.empty()) {
      auto &rdStore = rdMap.getReachingDefStore(&inst);
      auto *dstDuInst = duFunc.getDefUseInstruction(&inst);
      for (const auto *obj : pSet) {
        if (isReachableMemory) {
          for (const auto *oLoc :
               ptrAnalysis.getMemoryManager().getReachableMemoryObjects(obj))
            addMemDefUseEdge(oLoc, rdStore, dstDuInst);
        } else {
          addMemDefUseEdge(obj, rdStore, dstDuInst);
        }
      }
    }
  }

  void processExternalCall(CallBase *cs, const Function *f) {
    const auto *summary = modRefTable.lookup(f->getName());
    if (summary == nullptr) {
      errs() << "Missing entry in ModRefTable: " << f->getName() << "\n";
      llvm_unreachable("Consider adding the function to modref annotations");
    }
    auto &inst = *cs;

    for (auto const &effect : *summary) {
      if (effect.isRefEffect()) {
        auto const &pos = effect.getPosition();
        assert(!pos.isReturnPosition() &&
               "It doesn't make any sense to ref a return position!");

        auto const &argPos = pos.getAsArgPosition();
        unsigned idx = argPos.getArgIndex();

        if (!argPos.isAfterArgPosition())
          processMemRead(inst, cs->getArgOperand(idx),
                         effect.onReachableMemory());
        else {
          for (auto i = idx, e = cs->arg_size(); i < e; ++i)
            processMemRead(inst, cs->getArgOperand(i),
                           effect.onReachableMemory());
        }
      }
    }
  }

public:
  MemReadVisitor(const RDMapType &m, DefUseFunction &f,
                 const SemiSparsePointerAnalysis &p,
                 const ModRefModuleSummary &sm, const ExternalModRefTable &e)
      : ptrAnalysis(p), summaryMap(sm), modRefTable(e), rdMap(m), duFunc(f) {}

  // Default case
  void visitInstruction(Instruction &) {}

  void visitLoadInst(LoadInst &loadInst) {
    processMemRead(loadInst, loadInst.getPointerOperand(), false);
  }

  void visitCallBase(CallBase &call) { visitTaintCall(&call); }

  void visitTaintCall(CallBase *cs) {
    auto *inst = cs;
    auto callTgts = ptrAnalysis.getCallees(cs);
    auto &rdStore = rdMap.getReachingDefStore(inst);
    auto *dstDuInst = duFunc.getDefUseInstruction(inst);
    for (const auto *callee : callTgts) {
      if (callee->isDeclaration()) {
        processExternalCall(cs, callee);
      } else {
        auto &summary = summaryMap.getSummary(callee);
        for (const auto *obj : summary.mem_reads())
          addMemDefUseEdge(obj, rdStore, dstDuInst);
        for (const auto *obj : summary.mem_writes())
          addMemDefUseEdge(obj, rdStore, dstDuInst);
      }
    }
  }
};

} // namespace

void DefUseModuleBuilder::buildMemLevelEdges(
    DefUseFunction &duFunc, const ModRefModuleSummary &summaryMap) {
  // Run the reaching def analysis
  ReachingDefModuleAnalysis rdAnalysis(ptrAnalysis, summaryMap, modRefTable);
  auto rdMap = rdAnalysis.runOnFunction(duFunc.getFunction());

  MemReadVisitor memVisitor(rdMap, duFunc, ptrAnalysis, summaryMap,
                            modRefTable);
  for (const auto *bb :
       llvm::depth_first(&duFunc.getFunction().getEntryBlock()))
    memVisitor.visit(*const_cast<BasicBlock *>(bb));

  // Add mem-level external uses
  auto const &summary = summaryMap.getSummary(&duFunc.getFunction());
  addExternalUses(duFunc, rdMap, summary);
}

void DefUseModuleBuilder::buildDefUseFunction(
    DefUseFunction &duFunc, const ModRefModuleSummary &moduleSummary) {
  // Add top-level edges
  buildTopLevelEdges(duFunc);

  // Add top-level external defs
  addExternalDefs(duFunc);

  // Add memory-level edges
  buildMemLevelEdges(duFunc, moduleSummary);

  // Compute node priorities;
  computeNodePriority(duFunc);
}

DefUseModule DefUseModuleBuilder::buildDefUseModule(const Module &module) {
  if (!validateModule(module, llvm::errs()))
    throw std::invalid_argument("Unsupported module for TPA taint");
  DefUseModule duModule(module, reachableFunctions(module, ptrAnalysis));

  // Obtain mod ref summary first
  auto moduleSummary =
      ModRefModuleAnalysis(ptrAnalysis, modRefTable).runOnModule(module);

  for (auto &duFunc : duModule)
    buildDefUseFunction(duFunc, moduleSummary);

  return duModule;
}

} // namespace tpa::taint
