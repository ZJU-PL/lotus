#include "Checker/TPA/FrontEnd/DefUseModuleBuilder.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"
#include "Checker/TPA/FrontEnd/ModRefModuleAnalysis.h"
#include "Checker/TPA/FrontEnd/ReachingDefModuleAnalysis.h"
#include "Checker/TPA/Program/Reachability.h"

#include <cstddef>

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
      // InlineAsm is neither an LLVM Constant nor a parameter. Calls without
      // SSA inputs still need an initial evaluation under the ignore policy.
      if (const auto *call = dyn_cast<CallBase>(&inst))
        if (call->isInlineAsm())
          entryInst->insertTopLevelEdge(duFunc.getDefUseInstruction(call));
      if (isa<LandingPadInst>(inst))
        entryInst->insertTopLevelEdge(duFunc.getDefUseInstruction(&inst));
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

} // namespace

void DefUseModuleBuilder::buildMemLevelEdges(
    DefUseFunction &duFunc, const ModRefModuleSummary &summaryMap) {
  ReachingDefModuleAnalysis rdAnalysis(ptrAnalysis, summaryMap, modRefTable,
                                       unmodeledPolicy, enableMemoryGrouping);
  rdAnalysis.buildMemoryDefUseEdges(duFunc);
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
  auto reachable = reachableFunctions(module, ptrAnalysis, unmodeledPolicy);
  DefUseModule duModule(module, reachable);

  // Obtain mod ref summary first
  auto moduleSummary =
      ModRefModuleAnalysis(ptrAnalysis, modRefTable, unmodeledPolicy)
          .runOnModule(module, reachable);

  for (auto &duFunc : duModule)
    buildDefUseFunction(duFunc, moduleSummary);

  return duModule;
}

} // namespace tpa::taint
