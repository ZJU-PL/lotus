#pragma once

#include "Annotation/APISpec.h"
#include "Annotation/Taint/TaintConfigParser.h"

#include <map>
#include <set>

#include <llvm/IR/Instructions.h>
#include <llvm/Pass.h>

namespace lotus::gsaf {

/// Engine-specific model view. Contracts and taint selectors are owned by
/// Lotus's existing annotation stores; this pass provides their analysis life
/// cycle.
class GSAFModels : public llvm::ModulePass {
  APISpec API;
  TaintConfig Taint;
  std::map<std::string, std::vector<int>> SourceArguments;
  std::map<std::string, std::vector<int>> SinkArguments;
  const FunctionSpec *spec(const llvm::Function *function) const;
  static llvm::Function *callee(llvm::Value *value);

public:
  static char ID;
  static const FunctionSpec *builtinSpec(llvm::Function *function);
  const APISpec &api() const { return API; }
  GSAFModels();
  bool runOnModule(llvm::Module &module) override;
  void getAnalysisUsage(llvm::AnalysisUsage &usage) const override {
    usage.setPreservesAll();
  }

  bool is_malloc_function(llvm::Function *function) const;
  bool is_new_function(llvm::Function *function) const;
  bool is_free_function(llvm::Function *function) const;
  bool is_delete_function(llvm::Function *function) const;
  bool isAllocFunc(llvm::Function *function) const;
  bool isFreeFunc(llvm::Function *function) const;
  bool is_pure_lib_function(llvm::Function *function) const;
  bool isPureLib(llvm::Function *function) const;
  bool isHeapAllocSite(llvm::Value *value) const;
  bool isHeapFreeSite(llvm::Value *value) const;
  bool isHeapReallocSite(llvm::Value *value) const;
  bool isStackAllocSite(llvm::Value *value) const;
  bool isGlobalMemory(llvm::Value *value) const;
  bool isConcreteMemory(llvm::Value *value) const;
  std::vector<int> getHeapAllocSize(llvm::Value *value) const;
  int getAllMemoryAllocs(std::vector<std::string> &names) const;
  int getAllMemoryFrees(std::vector<std::string> &names) const;
  int getMatchedFreesForMalloc(const std::string &name,
                               std::vector<std::string> &frees) const;
  int getAllMallocFreePairs(
      std::map<std::string, std::set<std::string>> &pairs) const;

  bool isFileOpenSite(llvm::Value *value) const;
  bool isFileCloseSite(llvm::Value *value) const;
  int getAllFilePtrOpenClosePairs(
      std::map<std::string, std::set<std::string>> &pairs) const;
  bool isBufferAccessFunc(llvm::Function *function) const;
  int getBufferAccessPattern(llvm::Function *function) const;
  bool isPotentialSrcFunction(llvm::Function *function) const;

  bool isFunctionRetAsSource(const llvm::Function *function) const;
  bool isFunctionArgAsSource(const llvm::Function *function) const;
  bool isFunctionAsSink(const llvm::Function *function) const;
  const std::vector<int> *
  getTaintSourceArguments(llvm::Function *function) const;
  const std::vector<int> *getTaintSinkArguments(llvm::Function *function) const;
  bool isException(const std::pair<llvm::Function *, int> &source,
                   const std::pair<llvm::Function *, int> &sink) const;
};

} // namespace lotus::gsaf
