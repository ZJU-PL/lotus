/**
 * @file ResultUtils.cpp
 * @brief Result utilities for Sparrow-AA.
 *
 * This file provides utility functions for printing and querying Andersen's
 * pointer analysis results, including points-to sets and alias relationships.
 * Useful for debugging and result inspection.
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/SparrowAA/ResultUtils.h"

#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/AndersenAA.h"

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ModuleSlotTracker.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace sparrow_aa {

ValueNameCache buildValueNameCache(const Module &M) {
  ValueNameCache cache;
  ModuleSlotTracker MST(&M);
  auto recordUnnamed = [&](const Value *V) {
    if (V->hasName())
      return;
    std::string rendered;
    raw_string_ostream rso(rendered);
    V->printAsOperand(rso, false, MST);
    rso.flush();
    cache.try_emplace(V, std::move(rendered));
  };

  for (const GlobalVariable &GV : M.globals())
    recordUnnamed(&GV);
  for (const Function &F : M) {
    recordUnnamed(&F);
    if (F.isDeclaration())
      continue;
    MST.incorporateFunction(F);
    for (const Argument &A : F.args())
      if (A.getType()->isPointerTy())
        recordUnnamed(&A);
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB)
        if (I.getType()->isPointerTy())
          recordUnnamed(&I);
  }
  return cache;
}

/**
 * @brief Print a value's name or representation to an output stream.
 */
static void printValue(const Value *V, raw_ostream &OS,
                       ModuleSlotTracker *MST = nullptr,
                       const ValueNameCache *NameCache = nullptr) {
  if (V->hasName()) {
    OS << V->getName();
    return;
  }
  if (NameCache) {
    auto it = NameCache->find(V);
    if (it != NameCache->end()) {
      OS << it->second;
      return;
    }
  }
  if (MST)
    V->printAsOperand(OS, false, *MST);
  else
    V->printAsOperand(OS, false);
}

static void printPointsToSetImpl(const Value *V, Andersen &Anders,
                                 raw_ostream &OS, ModuleSlotTracker *MST,
                                 const ValueNameCache *NameCache) {
  if (!V->getType()->isPointerTy())
    return;

  std::vector<const Value *> ptsSet;
  if (!Anders.getPointsToSet(V, ptsSet)) {
    OS << "  ";
    printValue(V, OS, MST, NameCache);
    OS << " points to unknown\n";
    return;
  }
  if (ptsSet.empty()) {
    OS << "  ";
    printValue(V, OS, MST, NameCache);
    OS << " points to nothing\n";
    return;
  }

  OS << "  ";
  printValue(V, OS, MST, NameCache);
  OS << " points to " << ptsSet.size() << " location(s):\n";
  for (const Value *T : ptsSet) {
    OS << "    - ";
    printValue(T, OS, MST, NameCache);
    if (isa<GlobalVariable>(T))
      OS << " [global]";
    else if (isa<AllocaInst>(T))
      OS << " [stack]";
    else if (isa<CallInst>(T) || isa<InvokeInst>(T))
      OS << " [heap]";
    else if (isa<Function>(T))
      OS << " [function]";
    OS << "\n";
  }
}

void printPointsToSet(const Value *V, Andersen &Anders, raw_ostream &OS) {
  printPointsToSetImpl(V, Anders, OS, nullptr, nullptr);
}

void printPointsToSet(const Value *V, Andersen &Anders, raw_ostream &OS,
                      ModuleSlotTracker &MST) {
  printPointsToSetImpl(V, Anders, OS, &MST, nullptr);
}

void printPointsToSet(const Value *V, Andersen &Anders, raw_ostream &OS,
                      const ValueNameCache &NameCache) {
  printPointsToSetImpl(V, Anders, OS, nullptr, &NameCache);
}

/**
 * @brief Perform alias queries for all pointer pairs in a module.
 *
 * Queries the alias relationship between all pairs of pointers (up to a limit)
 * and prints the results along with a summary of alias statistics.
 *
 * @param M The module to analyze
 * @param AAResult The alias analysis result
 * @param OS The output stream
 */
void performAliasQueries(Module &M, AndersenAAResult &AAResult,
                         raw_ostream &OS) {
  OS << "\n=== Alias Query Results ===\n\n";

  std::vector<const Value *> Pointers;
  for (const GlobalVariable &GV : M.globals())
    if (GV.getType()->isPointerTy())
      Pointers.push_back(&GV);
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB)
        if (I.getType()->isPointerTy())
          Pointers.push_back(&I);
  }

  OS << "Total pointers: " << Pointers.size() << "\n\n";

  ModuleSlotTracker MST(&M);
  unsigned Counts[3] = {0};
  const char *Names[] = {"NoAlias", "MayAlias", "MustAlias"};

  for (size_t i = 0; i < Pointers.size() && i < 20; ++i) {
    for (size_t j = i + 1; j < Pointers.size() && j < 20; ++j) {
      AliasResult R = AAResult.alias(
          MemoryLocation(Pointers[i], LocationSize::beforeOrAfterPointer()),
          MemoryLocation(Pointers[j], LocationSize::beforeOrAfterPointer()));
      int idx = (R == AliasResult::MayAlias)    ? 1
                : (R == AliasResult::MustAlias) ? 2
                                                : 0;
      Counts[idx]++;

      if (R != AliasResult::NoAlias) {
        OS << "  ";
        Pointers[i]->printAsOperand(OS, false, MST);
        OS << " and ";
        Pointers[j]->printAsOperand(OS, false, MST);
        OS << " -> " << Names[idx] << "\n";
      }
    }
  }

  OS << "\n--- Summary ---\n";
  for (int i = 0; i < 3; ++i)
    OS << Names[i] << ": " << Counts[i] << "\n";
}

} // namespace sparrow_aa
