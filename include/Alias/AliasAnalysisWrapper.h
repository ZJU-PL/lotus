/// Unified wrapper for alias analysis - supports multiple AA backends
#pragma once

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <memory>

class AndersenAAResult;
class DyckAliasAnalysis;
class AllocAA;

namespace llvm { class CFLAndersAAResult; class CFLSteensAAResult; }
namespace seadsa { class SeaDsaAAResult; }
namespace UnderApprox { class UnderApproxAA; }

namespace lotus {

enum class AAType {
  Andersen, DyckAA, BasicAA, TBAA, GlobalsAA, SCEVAA,
  CFLAnders, CFLSteens, SRAA, SeaDsa, AllocAA, Combined, UnderApprox
};

class AliasAnalysisWrapper {
public:
  AliasAnalysisWrapper(llvm::Module &M, AAType type = AAType::Andersen);
  ~AliasAnalysisWrapper();

  llvm::AliasResult query(const llvm::Value *v1, const llvm::Value *v2);
  llvm::AliasResult query(const llvm::MemoryLocation &loc1, const llvm::MemoryLocation &loc2);
  
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mustAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mayNull(const llvm::Value *v);
  
  bool getPointsToSet(const llvm::Value *ptr, std::vector<const llvm::Value *> &ptsSet);
  bool getAliasSet(const llvm::Value *v, std::vector<const llvm::Value *> &aliasSet);
  
  AAType getType() const { return _aa_type; }
  bool isInitialized() const { return _initialized; }

private:
  void initialize();
  llvm::AliasResult queryBackend(const llvm::Value *v1, const llvm::Value *v2);
  bool isValidPointerQuery(const llvm::Value *v1, const llvm::Value *v2) const;

  AAType _aa_type;
  llvm::Module *_module;
  bool _initialized;

  std::unique_ptr<AndersenAAResult> _andersen_aa;
  std::unique_ptr<DyckAliasAnalysis> _dyck_aa;
  std::unique_ptr<UnderApprox::UnderApproxAA> _underapprox_aa;
  std::unique_ptr<llvm::CFLAndersAAResult> _cflanders_aa;
  std::unique_ptr<llvm::CFLSteensAAResult> _cflsteens_aa;
  std::unique_ptr<AllocAA> _alloc_aa;
  
  llvm::AAResults *_llvm_aa;
  seadsa::SeaDsaAAResult *_seadsa_aa;
  void *_sraa;
};

class AliasAnalysisFactory {
public:
  static std::unique_ptr<AliasAnalysisWrapper> create(llvm::Module &M, AAType type);
  static std::unique_ptr<AliasAnalysisWrapper> createAuto(llvm::Module &M);
  static const char *getTypeName(AAType type);
};

} // namespace lotus

