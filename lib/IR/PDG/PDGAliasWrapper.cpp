/**
 * @file PDGAliasWrapper.cpp
 * @brief Implementation of unified alias analysis wrapper for PDG construction
 *
 * This file implements the PDGAliasWrapper class which provides a unified
 * interface for querying alias information from various alias analysis backends.
 * It integrates Andersen's analysis, Dyck AA, and LLVM's built-in AA.
 */

#include "IR/PDG/PDGAliasWrapper.h"
#include "Alias/Andersen/AndersenAA.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pdg;

//===----------------------------------------------------------------------===//
// PDGAliasWrapper Implementation
//===----------------------------------------------------------------------===//

PDGAliasWrapper::PDGAliasWrapper(Module &M, AAType type)
    : _aa_type(type), _module(&M), _initialized(false),
      _dyck_aa(nullptr), _llvm_aa(nullptr)
{
  initialize();
}

PDGAliasWrapper::~PDGAliasWrapper()
{
  // Note: _dyck_aa is managed by LLVM pass manager, don't delete
  // _llvm_aa is also managed externally
}

void PDGAliasWrapper::initialize()
{
  if (!_module)
  {
    errs() << "PDGAliasWrapper: No module provided\n";
    return;
  }

  switch (_aa_type)
  {
  case AAType::Andersen:
    // Initialize Andersen's analysis
    try
    {
      _andersen_aa = std::make_unique<AndersenAAResult>(*_module);
      _initialized = true;
      errs() << "PDGAliasWrapper: Initialized with Andersen's analysis\n";
    }
    catch (const std::exception &e)
    {
      errs() << "PDGAliasWrapper: Failed to initialize Andersen AA: " 
             << e.what() << "\n";
    }
    break;

  case AAType::DyckAA:
    // DyckAA needs to be run as a pass first
    // We'll create it lazily when needed
    errs() << "PDGAliasWrapper: Dyck AA requires pass manager setup\n";
    errs() << "PDGAliasWrapper: Please run DyckAliasAnalysis pass before using\n";
    break;

  case AAType::BasicAA:
  case AAType::TBAA:
  case AAType::GlobalsAA:
  case AAType::SCEVAA:
    // LLVM AA variants need to be provided externally via pass manager
    errs() << "PDGAliasWrapper: LLVM AA (BasicAA/TBAA/GlobalsAA/SCEVAA) requires pass manager setup\n";
    break;

  case AAType::UnderApprox:
    // Simple syntactic pattern matching - always available, no initialization needed
    _initialized = true;
    errs() << "PDGAliasWrapper: Initialized with under-approximation (syntactic)\n";
    break;

  case AAType::Combined:
    // Try to initialize multiple analyses
    try
    {
      _andersen_aa = std::make_unique<AndersenAAResult>(*_module);
      _initialized = true;
      errs() << "PDGAliasWrapper: Initialized with combined analysis (Andersen)\n";
    }
    catch (const std::exception &e)
    {
      errs() << "PDGAliasWrapper: Failed to initialize combined AA: " 
             << e.what() << "\n";
    }
    break;
  }
}

AliasResult PDGAliasWrapper::query(const Value *v1, const Value *v2)
{
  if (!isValidPointerQuery(v1, v2))
    return AliasResult::NoAlias;

  switch (_aa_type)
  {
  case AAType::Andersen:
    return queryAndersen(v1, v2);
  case AAType::DyckAA:
    return queryDyck(v1, v2);
  case AAType::BasicAA:
  case AAType::TBAA:
  case AAType::GlobalsAA:
  case AAType::SCEVAA:
    return queryLLVM(v1, v2);
  case AAType::UnderApprox:
    return queryUnderApproximate(v1, v2);
  case AAType::Combined:
    // Use conservative approach: if any analysis says MayAlias, return MayAlias
    {
      AliasResult result = queryAndersen(v1, v2);
      if (result == AliasResult::MayAlias)
        return result;
      // Add more analyses here if available
      return result;
    }
  default:
    return AliasResult::MayAlias; // Conservative default
  }
}

AliasResult PDGAliasWrapper::query(const MemoryLocation &loc1, 
                                   const MemoryLocation &loc2)
{
  if (!_initialized)
    return AliasResult::MayAlias;

  switch (_aa_type)
  {
  case AAType::Andersen:
    if (_andersen_aa)
      return _andersen_aa->alias(loc1, loc2);
    break;
  case AAType::BasicAA:
  case AAType::TBAA:
  case AAType::GlobalsAA:
  case AAType::SCEVAA:
    if (_llvm_aa)
      return _llvm_aa->alias(loc1, loc2);
    break;
  default:
    // Fall back to value-based query
    return query(loc1.Ptr, loc2.Ptr);
  }

  return AliasResult::MayAlias;
}

bool PDGAliasWrapper::mayAlias(const Value *v1, const Value *v2)
{
  AliasResult result = query(v1, v2);
  return result != AliasResult::NoAlias;
}

bool PDGAliasWrapper::mustAlias(const Value *v1, const Value *v2)
{
  AliasResult result = query(v1, v2);
  return result == AliasResult::MustAlias;
}

bool PDGAliasWrapper::mayNull(const Value *v)
{
  if (!v || !v->getType()->isPointerTy())
    return false;

  // Check for constant null
  if (isa<ConstantPointerNull>(v))
    return true;

  // Use Dyck AA if available
  if (_aa_type == AAType::DyckAA && _dyck_aa)
  {
    return _dyck_aa->mayNull(const_cast<Value *>(v));
  }

  // Conservative: may be null
  return true;
}

bool PDGAliasWrapper::getPointsToSet(const Value *ptr, 
                                     std::vector<const Value *> &ptsSet)
{
  if (!ptr || !ptr->getType()->isPointerTy())
    return false;

  ptsSet.clear();

  if (_aa_type == AAType::Andersen && _andersen_aa && _initialized)
  {
    return _andersen_aa->getPointsToSet(ptr, ptsSet);
  }

  return false;
}

bool PDGAliasWrapper::getAliasSet(const Value *v, 
                                  std::vector<const Value *> &aliasSet)
{
  if (!v || !v->getType()->isPointerTy())
    return false;

  aliasSet.clear();

  if (_aa_type == AAType::DyckAA && _dyck_aa)
  {
    const std::set<Value *> *dyckSet = _dyck_aa->getAliasSet(const_cast<Value *>(v));
    if (dyckSet)
    {
      for (Value *val : *dyckSet)
      {
        aliasSet.push_back(val);
      }
      return true;
    }
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Private query methods for different backends
//===----------------------------------------------------------------------===//

AliasResult PDGAliasWrapper::queryAndersen(const Value *v1, const Value *v2)
{
  if (!_andersen_aa || !_initialized)
    return AliasResult::MayAlias;

  // Strip pointer casts for better precision
  const Value *stripped_v1 = v1->stripPointerCasts();
  const Value *stripped_v2 = v2->stripPointerCasts();

  // Quick check: same value must alias
  if (stripped_v1 == stripped_v2)
    return AliasResult::MustAlias;

  // Create memory locations for the query
  MemoryLocation loc1(stripped_v1, LocationSize::beforeOrAfterPointer(), AAMDNodes());
  MemoryLocation loc2(stripped_v2, LocationSize::beforeOrAfterPointer(), AAMDNodes());

  return _andersen_aa->alias(loc1, loc2);
}

AliasResult PDGAliasWrapper::queryDyck(const Value *v1, const Value *v2)
{
  if (!_dyck_aa)
    return AliasResult::MayAlias;

  // Strip pointer casts
  const Value *stripped_v1 = v1->stripPointerCasts();
  const Value *stripped_v2 = v2->stripPointerCasts();

  // Quick check: same value must alias
  if (stripped_v1 == stripped_v2)
    return AliasResult::MustAlias;

  // Dyck AA provides mayAlias which is conservative
  bool aliases = _dyck_aa->mayAlias(const_cast<Value *>(stripped_v1), 
                                    const_cast<Value *>(stripped_v2));
  
  if (aliases)
    return AliasResult::MayAlias;
  else
    return AliasResult::NoAlias;
}

AliasResult PDGAliasWrapper::queryLLVM(const Value *v1, const Value *v2)
{
  if (!_llvm_aa)
    return AliasResult::MayAlias;

  MemoryLocation loc1(v1, LocationSize::beforeOrAfterPointer(), AAMDNodes());
  MemoryLocation loc2(v2, LocationSize::beforeOrAfterPointer(), AAMDNodes());

  return _llvm_aa->alias(loc1, loc2);
}

AliasResult PDGAliasWrapper::queryUnderApproximate(const Value *v1, const Value *v2)
{
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

bool PDGAliasWrapper::isValidPointerQuery(const Value *v1, const Value *v2) const
{
  if (!v1 || !v2)
    return false;

  if (!v1->getType()->isPointerTy() || !v2->getType()->isPointerTy())
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
// PDGAliasFactory Implementation
//===----------------------------------------------------------------------===//

std::unique_ptr<PDGAliasWrapper> PDGAliasFactory::create(Module &M, AAType type)
{
  return std::make_unique<PDGAliasWrapper>(M, type);
}

std::unique_ptr<PDGAliasWrapper> PDGAliasFactory::createAuto(Module &M)
{
  // Auto-select based on module size or other heuristics
  // For now, default to Andersen which is a good balance of precision and performance
  errs() << "PDGAliasFactory: Auto-selecting Andersen's analysis\n";
  return create(M, AAType::Andersen);
}

const char *PDGAliasFactory::getTypeName(AAType type)
{
  switch (type)
  {
  case AAType::Andersen:
    return "Andersen";
  case AAType::DyckAA:
    return "DyckAA";
  case AAType::BasicAA:
    return "BasicAA";
  case AAType::TBAA:
    return "TBAA";
  case AAType::GlobalsAA:
    return "GlobalsAA";
  case AAType::SCEVAA:
    return "SCEVAA";
  case AAType::Combined:
    return "Combined";
  case AAType::UnderApprox:
    return "UnderApprox";
  default:
    return "Unknown";
  }
}
