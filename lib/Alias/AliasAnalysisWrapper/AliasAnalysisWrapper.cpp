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

AliasAnalysisWrapper::AliasAnalysisWrapper(Module &M, AAType type)
    : _aa_type(type), _module(&M), _initialized(false),
      _llvm_aa(nullptr), _seadsa_aa(nullptr), _sraa(nullptr) {
  initialize();
}

AliasAnalysisWrapper::~AliasAnalysisWrapper() = default;

void AliasAnalysisWrapper::initialize() {
  if (!_module) return;

  auto initAA = [this](auto fn, const char *name) {
    try {
      fn();
      _initialized = true;
    } catch (const std::exception &e) {
      errs() << "AliasAnalysisWrapper: Failed to init " << name << ": " << e.what() << "\n";
    }
  };

  switch (_aa_type) {
  case AAType::Andersen:
  case AAType::Combined:
    initAA([this]{ _andersen_aa = std::make_unique<AndersenAAResult>(*_module); }, "Andersen");
    break;
  case AAType::DyckAA:
    initAA([this]{ _dyck_aa = std::make_unique<DyckAliasAnalysis>(); _dyck_aa->runOnModule(*_module); }, "DyckAA");
    break;
  case AAType::UnderApprox:
    initAA([this]{ _underapprox_aa = std::make_unique<UnderApprox::UnderApproxAA>(*_module); }, "UnderApprox");
    break;
  case AAType::CFLAnders:
    initAA([this]{
      auto TLII = std::make_shared<TargetLibraryInfoImpl>(Triple(_module->getTargetTriple()));
      _cflanders_aa = std::make_unique<CFLAndersAAResult>([TLII](Function &) -> const TargetLibraryInfo & {
        static TargetLibraryInfo TLI(*TLII); return TLI;
      });
    }, "CFLAnders");
    break;
  case AAType::CFLSteens:
    initAA([this]{
      auto TLII = std::make_shared<TargetLibraryInfoImpl>(Triple(_module->getTargetTriple()));
      _cflsteens_aa = std::make_unique<CFLSteensAAResult>([TLII](Function &) -> const TargetLibraryInfo & {
        static TargetLibraryInfo TLI(*TLII); return TLI;
      });
    }, "CFLSteens");
    break;
  default:
    break;
  }
}

AliasResult AliasAnalysisWrapper::query(const Value *v1, const Value *v2) {
  if (!isValidPointerQuery(v1, v2)) return AliasResult::NoAlias;
  return queryBackend(v1, v2);
}

AliasResult AliasAnalysisWrapper::query(const MemoryLocation &loc1, const MemoryLocation &loc2) {
  if (!_initialized) return AliasResult::MayAlias;
  if (_andersen_aa) return _andersen_aa->alias(loc1, loc2);
  if (_llvm_aa) return _llvm_aa->alias(loc1, loc2);
  return query(loc1.Ptr, loc2.Ptr);
}

bool AliasAnalysisWrapper::mayAlias(const Value *v1, const Value *v2) {
  return query(v1, v2) != AliasResult::NoAlias;
}

bool AliasAnalysisWrapper::mustAlias(const Value *v1, const Value *v2) {
  return query(v1, v2) == AliasResult::MustAlias;
}

bool AliasAnalysisWrapper::mayNull(const Value *v) {
  if (!v || !v->getType()->isPointerTy()) return false;
  if (isa<ConstantPointerNull>(v)) return true;
  if (_dyck_aa && _initialized) return _dyck_aa->mayNull(const_cast<Value *>(v));
  return true;
}

bool AliasAnalysisWrapper::getPointsToSet(const Value *ptr, std::vector<const Value *> &ptsSet) {
  if (!ptr || !ptr->getType()->isPointerTy()) return false;
  ptsSet.clear();
  return _andersen_aa && _initialized && _andersen_aa->getPointsToSet(ptr, ptsSet);
}

bool AliasAnalysisWrapper::getAliasSet(const Value *v, std::vector<const Value *> &aliasSet) {
  if (!v || !v->getType()->isPointerTy()) return false;
  aliasSet.clear();
  if (_dyck_aa && _initialized) {
    if (const auto *dyckSet = _dyck_aa->getAliasSet(const_cast<Value *>(v))) {
      aliasSet.assign(dyckSet->begin(), dyckSet->end());
      return true;
    }
  }
  return false;
}

AliasResult AliasAnalysisWrapper::queryBackend(const Value *v1, const Value *v2) {
  if (!_initialized) return AliasResult::MayAlias;

  auto v1s = v1->stripPointerCasts(), v2s = v2->stripPointerCasts();
  if (v1s == v2s) return AliasResult::MustAlias;

  auto mkLoc = [](const Value *v) { return MemoryLocation(v, LocationSize::beforeOrAfterPointer(), AAMDNodes()); };

  if (_andersen_aa) return _andersen_aa->alias(mkLoc(v1s), mkLoc(v2s));
  if (_dyck_aa) return _dyck_aa->mayAlias(const_cast<Value *>(v1s), const_cast<Value *>(v2s)) 
                       ? AliasResult::MayAlias : AliasResult::NoAlias;
  if (_llvm_aa) return _llvm_aa->alias(mkLoc(v1), mkLoc(v2));
  if (_underapprox_aa) return _underapprox_aa->mustAlias(v1, v2) ? AliasResult::MustAlias : AliasResult::NoAlias;
  if (_cflanders_aa) return _cflanders_aa->query(mkLoc(v1), mkLoc(v2));
  if (_cflsteens_aa) return _cflsteens_aa->query(mkLoc(v1), mkLoc(v2));
  if (_seadsa_aa) { SimpleAAQueryInfo AAQI; return _seadsa_aa->alias(mkLoc(v1), mkLoc(v2), AAQI); }
  if (_alloc_aa) return _alloc_aa->canPointToTheSameObject(const_cast<Value *>(v1), const_cast<Value *>(v2))
                        ? AliasResult::MayAlias : AliasResult::NoAlias;
  
  return AliasResult::MayAlias;
}

bool AliasAnalysisWrapper::isValidPointerQuery(const Value *v1, const Value *v2) const {
  return v1 && v2 && v1->getType()->isPointerTy() && v2->getType()->isPointerTy();
}

std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::create(Module &M, AAType type) {
  return std::make_unique<AliasAnalysisWrapper>(M, type);
}

std::unique_ptr<AliasAnalysisWrapper> AliasAnalysisFactory::createAuto(Module &M) {
  return create(M, AAType::Andersen);
}

const char *AliasAnalysisFactory::getTypeName(AAType type) {
  static const char *names[] = {"Andersen", "DyckAA", "BasicAA", "TBAA", "GlobalsAA", "SCEVAA",
                                "CFLAnders", "CFLSteens", "SRAA", "SeaDsa", "AllocAA", "Combined", "UnderApprox"};
  return static_cast<size_t>(type) < sizeof(names)/sizeof(names[0]) ? names[static_cast<size_t>(type)] : "Unknown";
}

