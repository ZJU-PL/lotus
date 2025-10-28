/**
 * @file AliasAnalysisWrapper.cpp
 * @brief Implementation of unified alias analysis wrapper
 *
 * This file implements the AliasAnalysisWrapper class which provides a unified
 * interface for querying alias information from various alias analysis backends.
 */

#include "Alias/AliasAnalysisWrapper.h"
#include "Alias/Andersen/AndersenAA.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/UnderApproxAA/UnderApproxAA.h"
#include "Alias/CFLAA/CFLAndersAliasAnalysis.h"
#include "Alias/CFLAA/CFLSteensAliasAnalysis.h"
#include "Alias/seadsa/SeaDsaAliasAnalysis.hh"
#include "Alias/AllocAA/AllocAA.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus;

//===----------------------------------------------------------------------===//
// AliasAnalysisWrapper Implementation
//===----------------------------------------------------------------------===//

AliasAnalysisWrapper::AliasAnalysisWrapper(Module &M, AAType type)
    : _aa_type(type), _module(&M), _initialized(false),
      _llvm_aa(nullptr), _sraa(nullptr), _seadsa_aa(nullptr)
{
  initialize();
}

AliasAnalysisWrapper::~AliasAnalysisWrapper()
{
  // Note: _llvm_aa and _seadsa_aa are managed externally if provided via pass manager
  // Owned unique_ptrs are automatically cleaned up
}

void AliasAnalysisWrapper::initialize()
{
  if (!_module)
  {
    errs() << "AliasAnalysisWrapper: No module provided\n";
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
      errs() << "AliasAnalysisWrapper: Initialized with Andersen's analysis\n";
    }
    catch (const std::exception &e)
    {
      errs() << "AliasAnalysisWrapper: Failed to initialize Andersen AA: " 
             << e.what() << "\n";
    }
    break;

  case AAType::DyckAA:
    // Initialize Dyck AA
    try
    {
      _dyck_aa = std::make_unique<DyckAliasAnalysis>();
      // runOnModule returns false for analysis passes (no modification)
      // but it still performs the analysis
      _dyck_aa->runOnModule(*_module);
      _initialized = true;
      errs() << "AliasAnalysisWrapper: Initialized with Dyck AA\n";
    }
    catch (const std::exception &e)
    {
      errs() << "AliasAnalysisWrapper: Failed to initialize Dyck AA: " 
             << e.what() << "\n";
      _dyck_aa.reset();
    }
    break;

  case AAType::BasicAA:
  case AAType::TBAA:
  case AAType::GlobalsAA:
  case AAType::SCEVAA:
    // LLVM AA variants need to be provided externally via pass manager
    errs() << "AliasAnalysisWrapper: LLVM AA (BasicAA/TBAA/GlobalsAA/SCEVAA) requires pass manager setup\n";
    break;

  case AAType::UnderApprox:
    // Simple syntactic pattern matching - always available
    try
    {
      _underapprox_aa = std::make_unique<UnderApprox::UnderApproxAA>(*_module);
      _initialized = true;
      errs() << "AliasAnalysisWrapper: Initialized with under-approximation (syntactic)\n";
    }
    catch (const std::exception &e)
    {
      errs() << "AliasAnalysisWrapper: Failed to initialize UnderApprox AA: " 
             << e.what() << "\n";
    }
    break;

  case AAType::CFLAnders:
    // Initialize CFL-Anders analysis
    try
    {
      // Create a TargetLibraryInfoImpl for the module
      auto TLII = std::make_shared<TargetLibraryInfoImpl>(Triple(_module->getTargetTriple()));
      auto getTLI = [TLII](Function &F) -> const TargetLibraryInfo & {
        static TargetLibraryInfo TLI(*TLII);
        return TLI;
      };
      _cflanders_aa = std::make_unique<CFLAndersAAResult>(getTLI);
      _initialized = true;
      errs() << "AliasAnalysisWrapper: Initialized with CFL-Anders analysis\n";
    }
    catch (const std::exception &e)
    {
      errs() << "AliasAnalysisWrapper: Failed to initialize CFL-Anders AA: " 
             << e.what() << "\n";
    }
    break;

  case AAType::CFLSteens:
    // Initialize CFL-Steens analysis
    try
    {
      // Create a TargetLibraryInfoImpl for the module
      auto TLII = std::make_shared<TargetLibraryInfoImpl>(Triple(_module->getTargetTriple()));
      auto getTLI = [TLII](Function &F) -> const TargetLibraryInfo & {
        static TargetLibraryInfo TLI(*TLII);
        return TLI;
      };
      _cflsteens_aa = std::make_unique<CFLSteensAAResult>(getTLI);
      _initialized = true;
      errs() << "AliasAnalysisWrapper: Initialized with CFL-Steens analysis\n";
    }
    catch (const std::exception &e)
    {
      errs() << "AliasAnalysisWrapper: Failed to initialize CFL-Steens AA: " 
             << e.what() << "\n";
    }
    break;

  case AAType::SRAA:
    // SRAA needs pass manager setup
    errs() << "AliasAnalysisWrapper: SRAA requires pass manager setup\n";
    break;

  case AAType::SeaDsa:
    // SeaDSA needs pass manager setup
    errs() << "AliasAnalysisWrapper: SeaDSA requires pass manager setup\n";
    break;

  case AAType::AllocAA:
    // AllocAA requires callbacks for ScalarEvolution, LoopInfo, and CallGraph
    errs() << "AliasAnalysisWrapper: AllocAA requires ScalarEvolution, LoopInfo, and CallGraph callbacks\n";
    break;

  case AAType::Combined:
    // Try to initialize multiple analyses
    try
    {
      _andersen_aa = std::make_unique<AndersenAAResult>(*_module);
      _initialized = true;
      errs() << "AliasAnalysisWrapper: Initialized with combined analysis (Andersen)\n";
    }
    catch (const std::exception &e)
    {
      errs() << "AliasAnalysisWrapper: Failed to initialize combined AA: " 
             << e.what() << "\n";
    }
    break;
  }
}

AliasResult AliasAnalysisWrapper::query(const Value *v1, const Value *v2)
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
    return queryUnderApprox(v1, v2);
  case AAType::CFLAnders:
    return queryCFLAnders(v1, v2);
  case AAType::CFLSteens:
    return queryCFLSteens(v1, v2);
  case AAType::SRAA:
    return querySRAA(v1, v2);
  case AAType::SeaDsa:
    return querySeaDsa(v1, v2);
  case AAType::AllocAA:
    return queryAllocAA(v1, v2);
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

AliasResult AliasAnalysisWrapper::query(const MemoryLocation &loc1, 
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

bool AliasAnalysisWrapper::mayAlias(const Value *v1, const Value *v2)
{
  AliasResult result = query(v1, v2);
  return result != AliasResult::NoAlias;
}

bool AliasAnalysisWrapper::mustAlias(const Value *v1, const Value *v2)
{
  AliasResult result = query(v1, v2);
  return result == AliasResult::MustAlias;
}

bool AliasAnalysisWrapper::mayNull(const Value *v)
{
  if (!v || !v->getType()->isPointerTy())
    return false;

  // Check for constant null
  if (isa<ConstantPointerNull>(v))
    return true;

  // Use Dyck AA if available
  if (_aa_type == AAType::DyckAA && _dyck_aa && _initialized)
  {
    return _dyck_aa->mayNull(const_cast<Value *>(v));
  }

  // Conservative: may be null
  return true;
}

bool AliasAnalysisWrapper::getPointsToSet(const Value *ptr, 
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

bool AliasAnalysisWrapper::getAliasSet(const Value *v, 
                                       std::vector<const Value *> &aliasSet)
{
  if (!v || !v->getType()->isPointerTy())
    return false;

  aliasSet.clear();

  if (_aa_type == AAType::DyckAA && _dyck_aa && _initialized)
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

AliasResult AliasAnalysisWrapper::queryAndersen(const Value *v1, const Value *v2)
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

AliasResult AliasAnalysisWrapper::queryDyck(const Value *v1, const Value *v2)
{
  if (!_dyck_aa || !_initialized)
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

AliasResult AliasAnalysisWrapper::queryLLVM(const Value *v1, const Value *v2)
{
  if (!_llvm_aa)
    return AliasResult::MayAlias;

  MemoryLocation loc1(v1, LocationSize::beforeOrAfterPointer(), AAMDNodes());
  MemoryLocation loc2(v2, LocationSize::beforeOrAfterPointer(), AAMDNodes());

  return _llvm_aa->alias(loc1, loc2);
}

AliasResult AliasAnalysisWrapper::queryUnderApprox(const Value *v1, const Value *v2)
{
  if (!_underapprox_aa || !_initialized)
    return AliasResult::NoAlias;

  return _underapprox_aa->mustAlias(v1, v2) ? AliasResult::MustAlias
                                            : AliasResult::NoAlias;
}

AliasResult AliasAnalysisWrapper::queryCFLAnders(const Value *v1, const Value *v2)
{
  if (!_cflanders_aa || !_initialized)
    return AliasResult::MayAlias;

  MemoryLocation loc1(v1, LocationSize::beforeOrAfterPointer(), AAMDNodes());
  MemoryLocation loc2(v2, LocationSize::beforeOrAfterPointer(), AAMDNodes());

  return _cflanders_aa->query(loc1, loc2);
}

AliasResult AliasAnalysisWrapper::queryCFLSteens(const Value *v1, const Value *v2)
{
  if (!_cflsteens_aa || !_initialized)
    return AliasResult::MayAlias;

  MemoryLocation loc1(v1, LocationSize::beforeOrAfterPointer(), AAMDNodes());
  MemoryLocation loc2(v2, LocationSize::beforeOrAfterPointer(), AAMDNodes());

  return _cflsteens_aa->query(loc1, loc2);
}

AliasResult AliasAnalysisWrapper::querySRAA(const Value *v1, const Value *v2)
{
  // Suppress unused parameter warnings
  (void)v1;
  (void)v2;
  
  // SRAA (StrictRelations) is defined in an anonymous namespace and can only
  // be used through the pass manager. Users should set up the pass externally.
  if (!_sraa)
    return AliasResult::MayAlias;

  // Note: This requires the SRAA pass to be properly registered and accessible
  // The alias() method call would need proper setup through LLVM's pass infrastructure
  return AliasResult::MayAlias;
}

AliasResult AliasAnalysisWrapper::querySeaDsa(const Value *v1, const Value *v2)
{
  if (!_seadsa_aa)
    return AliasResult::MayAlias;

  MemoryLocation loc1(v1, LocationSize::beforeOrAfterPointer(), AAMDNodes());
  MemoryLocation loc2(v2, LocationSize::beforeOrAfterPointer(), AAMDNodes());

  // SeaDSA alias() method requires AAQueryInfo parameter
  // We use SimpleAAQueryInfo which has a default constructor
  SimpleAAQueryInfo AAQI;
  return _seadsa_aa->alias(loc1, loc2, AAQI);
}

AliasResult AliasAnalysisWrapper::queryAllocAA(const Value *v1, const Value *v2)
{
  if (!_alloc_aa)
    return AliasResult::MayAlias;

  // AllocAA has a different API - it uses canPointToTheSameObject
  // which returns a bool rather than AliasResult
  bool canAlias = _alloc_aa->canPointToTheSameObject(const_cast<Value *>(v1), 
                                                      const_cast<Value *>(v2));
  
  // If they cannot point to the same object, they don't alias
  if (!canAlias)
    return AliasResult::NoAlias;
  
  // Otherwise, they may alias (conservative)
  return AliasResult::MayAlias;
}

bool AliasAnalysisWrapper::isValidPointerQuery(const Value *v1, const Value *v2) const
{
  if (!v1 || !v2)
    return false;

  if (!v1->getType()->isPointerTy() || !v2->getType()->isPointerTy())
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
// AliasAnalysisFactory Implementation
//===----------------------------------------------------------------------===//

std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::create(Module &M, AAType type)
{
  return std::make_unique<AliasAnalysisWrapper>(M, type);
}

std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::createAuto(Module &M)
{
  // Auto-select based on module size or other heuristics
  // For now, default to Andersen which is a good balance of precision and performance
  errs() << "AliasAnalysisFactory: Auto-selecting Andersen's analysis\n";
  return create(M, AAType::Andersen);
}

const char *AliasAnalysisFactory::getTypeName(AAType type)
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
  case AAType::CFLAnders:
    return "CFLAnders";
  case AAType::CFLSteens:
    return "CFLSteens";
  case AAType::SRAA:
    return "SRAA";
  case AAType::SeaDsa:
    return "SeaDsa";
  case AAType::AllocAA:
    return "AllocAA";
  case AAType::Combined:
    return "Combined";
  case AAType::UnderApprox:
    return "UnderApprox";
  default:
    return "Unknown";
  }
}

