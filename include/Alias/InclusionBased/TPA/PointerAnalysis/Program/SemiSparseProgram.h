#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/IteratorRange.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/MapValueIterator.h"

#include <unordered_map>
#include <vector>

namespace tpa {

// Semi-sparse program representation
//
// This class represents the entire program as a collection of CFGs,
// one per function. It's the input to the pointer analysis algorithm.
//
// Semi-Sparse Representation:
// - Only analyzes CFG nodes that affect pointer state
// - Ignores nodes that don't create/use pointers
// - Reduces analysis time while maintaining precision
//
// Components:
// - LLVM Module: The original LLVM IR
// - TypeMap: Type information for memory layout
// - CFG Map: Maps functions to their CFGs
// - Address-taken functions: Functions passed as pointers
class SemiSparseProgram {
private:
  const llvm::Module &module;
  TypeMap typeMap;

  // Function -> CFG mapping (lazy construction)
  using MapType = std::unordered_map<const llvm::Function *, CFG>;
  mutable MapType cfgMap;
  mutable uint32_t nextCfgId = 1;

  // List of functions with their address taken
  // These are potential indirect call targets
  using FunctionList = std::vector<const llvm::Function *>;
  FunctionList addrTakenFuncList;

public:
  // Iterator over all CFGs
  using const_iterator = util::MapValueConstIterator<MapType::const_iterator>;
  // Iterator over address-taken functions
  using addr_taken_const_iterator = FunctionList::const_iterator;

  // Constructor
  SemiSparseProgram(const llvm::Module &m);
  // Access the original LLVM module
  const llvm::Module &getModule() const { return module; }

  // Access type information
  const TypeMap &getTypeMap() const { return typeMap; }
  void setTypeMap(TypeMap &&t) { typeMap = std::move(t); }

  // Get or create CFG for a function
  CFG &getOrCreateCFGForFunction(const llvm::Function &f);
  CFG &getOrCreateCFGForFunction(const llvm::Function &f) const;
  // Get CFG if it exists
  const CFG *getCFGForFunction(const llvm::Function &f) const;
  // Get the entry function's CFG
  const CFG *getEntryCFG() const;

  // Iterate over all CFGs
  const_iterator begin() const { return const_iterator(cfgMap.begin()); }
  const_iterator end() const { return const_iterator(cfgMap.end()); }

  // Iterate over address-taken functions
  addr_taken_const_iterator addr_taken_func_begin() const {
    return addrTakenFuncList.begin();
  }
  addr_taken_const_iterator addr_taken_func_end() const {
    return addrTakenFuncList.end();
  }
  auto addr_taken_funcs() const {
    return util::iteratorRange(addrTakenFuncList.begin(),
                               addrTakenFuncList.end());
  }
};

} // namespace tpa