/**
 * @file UnderApproxAA.cpp
 * @brief Implementation of under-approximation alias analysis
 *
 * This file implements the UnderApproxAA class which provides syntactic
 * pattern matching for definite alias relationships. It is an under-
 * approximation that only reports MustAlias when certain clear patterns
 * are detected.
 */

#include "Alias/UnderApproxAA/UnderApproxAA.h"
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace UnderApprox;

//===----------------------------------------------------------------------===//
// UnderApproxAA Implementation
//===----------------------------------------------------------------------===//

UnderApproxAA::UnderApproxAA(Module &M) : _module(M) {}

UnderApproxAA::~UnderApproxAA() {}

AliasResult UnderApproxAA::query(const Value *v1, const Value *v2)
{
  if (!isValidPointerQuery(v1, v2))
    return AliasResult::NoAlias;

  // Simple under-approximation using syntactic pattern matching
  // This is the ad-hoc approach previously used in DataDependencyGraph
  // It only returns MustAlias for certain clear syntactic patterns
  
  // Strip pointer casts for better matching
  const Value *stripped_v1 = v1->stripPointerCasts();
  const Value *stripped_v2 = v2->stripPointerCasts();
  
  // Quick check: same value must alias
  if (stripped_v1 == stripped_v2)
    return AliasResult::MustAlias;
  
  // Check bit cast: if v1 is a bitcast of v2, they must alias
  if (const BitCastInst *bci = dyn_cast<BitCastInst>(stripped_v1))
  {
    if (bci->getOperand(0) == stripped_v2)
      return AliasResult::MustAlias;
  }
  
  // Symmetric check for bitcast
  if (const BitCastInst *bci = dyn_cast<BitCastInst>(stripped_v2))
  {
    if (bci->getOperand(0) == stripped_v1)
      return AliasResult::MustAlias;
  }
  
  // Handle load instruction patterns
  // If v1 is a load from address A, and v2 is stored to A or also loaded from A,
  // they must alias
  if (const LoadInst *li = dyn_cast<LoadInst>(stripped_v1))
  {
    const Value *load_addr = li->getPointerOperand();
    
    // Check all users of the load address
    for (const User *user : load_addr->users())
    {
      // Case 1: v2 is the value stored to the same address
      if (const StoreInst *si = dyn_cast<StoreInst>(user))
      {
        if (si->getPointerOperand() == load_addr)
        {
          if (si->getValueOperand() == stripped_v2)
            return AliasResult::MustAlias;
        }
      }
      
      // Case 2: v2 is another load from the same address
      if (const LoadInst *alias_load_inst = dyn_cast<LoadInst>(user))
      {
        if (alias_load_inst == stripped_v2)
          return AliasResult::MustAlias;
      }
    }
  }
  
  // Symmetric check: if v2 is a load
  if (const LoadInst *li = dyn_cast<LoadInst>(stripped_v2))
  {
    const Value *load_addr = li->getPointerOperand();
    
    for (const User *user : load_addr->users())
    {
      if (const StoreInst *si = dyn_cast<StoreInst>(user))
      {
        if (si->getPointerOperand() == load_addr)
        {
          if (si->getValueOperand() == stripped_v1)
            return AliasResult::MustAlias;
        }
      }
    }
  }
  
  // Default: we don't know (NoAlias for under-approximation)
  // Under-approximation only reports MustAlias when certain, otherwise NoAlias
  return AliasResult::NoAlias;
}

AliasResult UnderApproxAA::alias(const MemoryLocation &loc1,
                                 const MemoryLocation &loc2)
{
  return query(loc1.Ptr, loc2.Ptr);
}

bool UnderApproxAA::mustAlias(const Value *v1, const Value *v2)
{
  AliasResult result = query(v1, v2);
  return result == AliasResult::MustAlias;
}

bool UnderApproxAA::isValidPointerQuery(const Value *v1, const Value *v2) const
{
  if (!v1 || !v2)
    return false;

  if (!v1->getType()->isPointerTy() || !v2->getType()->isPointerTy())
    return false;

  return true;
}

