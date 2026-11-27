#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Alias/InclusionBased/TPA/Util/CallCompatibility.h"
#include "Annotation/Pointer/ExternalPointerTable.h"

#include <algorithm>
#include <cstddef>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

namespace context {
class Context;
} // namespace context

namespace tpa {

// CRTP pointer analysis base class
//
// This template class provides common functionality for pointer analysis
// implementations using the Curiously Recurring Template Pattern (CRTP).
// Subclasses implement specific pointer analysis algorithms while this base
// class provides common interfaces and utilities.
//
// Key Responsibilities:
// 1. Memory and pointer management via PointerManager and MemoryManager
// 2. External pointer table loading for modeling library functions
// 3. Points-to set queries with optional context sensitivity
// 4. Indirect call target resolution
//
// Design:
// - Uses CRTP for static polymorphism - SubClass implements getPtsSetImpl()
// - PointerManager handles mapping (context, value) -> Pointer objects
// - MemoryManager handles memory object allocation and management
// - External pointer table models library functions with annotations
//
// Usage Pattern:
//   class MyAnalysis : public PointerAnalysis<MyAnalysis> {
//     PtsSet getPtsSetImpl(const Pointer* ptr) const { ... }
//   };
//
template <typename SubClass> class PointerAnalysis {
protected:
  // Manages pointer SSA variables and their mapping to LLVM values
  PointerManager ptrManager;
  // Manages memory objects and their allocation sites
  MemoryManager memManager;
  // External pointer table for modeling library function effects
  annotation::ExternalPointerTable extTable;
  // Cached list of address-taken functions in the module so universal indirect
  // call queries do not rescan all functions in the module on every call site.
  mutable const llvm::Module *cachedAddrTakenModule = nullptr;
  mutable std::vector<const llvm::Function *> cachedAddrTakenFuncs;

  // Helper method to extract function targets from a points-to set
  // If the set contains the universal object, all address-taken functions are
  // potential targets Otherwise, only functions whose allocation site is in the
  // set are targets
  void getCallees(const llvm::Instruction *inst, PtsSet pSet,
                  std::vector<const llvm::Function *> &funcs) const {
    if (pSet.has(MemoryManager::getUniversalObject())) {
      const auto *module = inst->getParent()->getParent()->getParent();
      if (cachedAddrTakenModule != module) {
        cachedAddrTakenFuncs.clear();
        for (auto const &f : *module)
          if (f.hasAddressTaken())
            cachedAddrTakenFuncs.push_back(&f);
        cachedAddrTakenModule = module;
      }
      funcs.insert(funcs.end(), cachedAddrTakenFuncs.begin(),
                   cachedAddrTakenFuncs.end());
    } else {
      for (const auto *obj : pSet) {
        auto &allocSite = obj->getAllocSite();
        if (allocSite.getAllocType() == AllocSiteTag::Function)
          funcs.push_back(allocSite.getFunction());
      }
    }
  }

public:
  PointerAnalysis() = default;

  PointerAnalysis(const PointerAnalysis &) = delete;
  PointerAnalysis(PointerAnalysis &&) noexcept = default;
  PointerAnalysis &operator=(const PointerAnalysis &) = delete;
  PointerAnalysis &operator=(PointerAnalysis &&) = delete;

  // Access the pointer manager
  const PointerManager &getPointerManager() const { return ptrManager; }
  // Access the memory manager
  const MemoryManager &getMemoryManager() const { return memManager; }

  // Load external pointer table from file
  // The external pointer table describes pointer effects of library functions
  // Parameters: extFileName - path to the external pointer table file
  void loadExternalPointerTable(const char *extFileName) {
    extTable = annotation::ExternalPointerTable::loadFromFile(extFileName);
  }

  // Get points-to set for a pointer (internal dispatch)
  // Subclass must implement getPtsSetImpl()
  PtsSet getPtsSet(const Pointer *ptr) const {
    return static_cast<const SubClass *>(this)->getPtsSetImpl(ptr);
  }

  // Get points-to set for a value in a specific context
  // This is the main query interface for context-sensitive analysis
  // Parameters: ctx - the calling context, val - the LLVM value to query
  // Returns: the points-to set of the pointer (ctx, val)
  PtsSet getPtsSet(const context::Context *ctx, const llvm::Value *val) const {
    assert(ctx != nullptr && val != nullptr);

    auto ptr = ptrManager.getPointer(ctx, val->stripPointerCasts());
    if (ptr == nullptr)
      return PtsSet::getEmptySet();

    return getPtsSet(ptr);
  }

  // Get points-to set for a value (context-insensitive)
  // Merges points-to sets from all contexts where the value appears
  // Parameters: val - the LLVM value to query
  // Returns: union of points-to sets across all contexts
  PtsSet getPtsSet(const llvm::Value *val) const {
    assert(val != nullptr);

    auto ptrs = ptrManager.getPointersWithValue(val->stripPointerCasts());
    if (ptrs.empty())
      return PtsSet::getEmptySet();

    std::vector<PtsSet> pSets;
    pSets.reserve(ptrs.size());

    for (auto ptr : ptrs)
      pSets.emplace_back(getPtsSet(ptr));

    return PtsSet::mergeAll(pSets);
  }

  // Get possible callee functions for an indirect call
  // Parameters: inst - the call instruction, ctx - optional context
  // Returns: compatible indirect targets; explicit named callees are retained.
  // filteredTargets optionally receives this query's rejected candidate count.
  std::vector<const llvm::Function *>
  getCallees(const llvm::Instruction *inst,
             const context::Context *ctx = nullptr,
             std::size_t *filteredTargets = nullptr) const {
    std::vector<const llvm::Function *> ret;
    if (filteredTargets)
      *filteredTargets = 0;

    const auto *callBase = llvm::dyn_cast<llvm::CallBase>(inst);
    if (!callBase)
      return ret;

    if (auto *f = llvm::dyn_cast<llvm::Function>(
            callBase->getCalledOperand()->stripPointerCasts())) {
      ret.push_back(f);
    } else {
      auto *funPtrVal = callBase->getCalledOperand();
      assert(funPtrVal != nullptr);

      auto pSet =
          ctx == nullptr ? getPtsSet(funPtrVal) : getPtsSet(ctx, funPtrVal);
      getCallees(inst, pSet, ret);
      const auto before = ret.size();
      ret.erase(std::remove_if(ret.begin(), ret.end(),
                               [&](const llvm::Function *candidate) {
                                 return !compatibleCallTarget(*callBase,
                                                              *candidate);
                               }),
                ret.end());
      if (filteredTargets)
        *filteredTargets = before - ret.size();
    }

    return ret;
  }
};

} // namespace tpa