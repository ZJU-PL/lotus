/** @file AnalysisContext.h @brief Module-scoped context and stateless LLVM/GVFG helpers. */
#pragma once

#include "IR/GVFG/GuardedValueFlowBuilder.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SymbolicExecution {

using lotus::gvfg::GuardedValueFlowBuilderPass;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowRegionNode;

/// Per-module analysis context replacing global state.
///
/// Owns module layout metadata, GVFG lookup access, and the computed call-graph
/// topological order and depths for a single symbolic execution run.
class AnalysisContext {
public:
  AnalysisContext() = default;
  AnalysisContext(llvm::Module *M, const llvm::DataLayout *DL,
                  GuardedValueFlowBuilderPass *Builder);

  llvm::Module *getModule() const { return M; }
  const llvm::DataLayout *getDataLayout() const { return DL; }
  llvm::Type *getNonPtrType() const { return NonPtrTy; }

  GuardedValueFlowGraph *getGraph(llvm::Function *Func) const;

  void computeCallGraphOrder();
  const std::vector<llvm::Function *> &getFuncSeq() const { return FuncSeq; }
  unsigned getFunctionDepth(llvm::Function *Func) const;
  bool isFunctionTopLevel(llvm::Function *Func) const;

  uint64_t getTypeSizeInBits(llvm::Type *Ty) const;
  uint64_t getTypeStoreSize(llvm::Type *Ty) const;
  uint64_t getTypeStoreSizeInBits(llvm::Type *Ty) const;
  uint64_t getElementOffset(llvm::StructType *St, unsigned Idx) const;

private:
  llvm::Module *M = nullptr;
  const llvm::DataLayout *DL = nullptr;
  GuardedValueFlowBuilderPass *Builder = nullptr;
  llvm::Type *NonPtrTy = nullptr;

  std::vector<llvm::Function *> FuncSeq;
  std::unordered_map<llvm::Function *, unsigned> FunDepthMap;
  std::unordered_map<llvm::Function *, std::unordered_set<llvm::Function *>>
      CallInfoTopDown;
  std::unordered_map<llvm::Function *, std::unordered_set<llvm::Function *>>
      CallInfoBottomUp;
};

/// Stateless LLVM and GVFG query helpers.
llvm::Function *getDirectCallee(const llvm::CallBase *CB);
llvm::Function *getCallee(llvm::Instruction *I);
bool isDefiniteCall(llvm::Instruction *I);
bool isPureLibrary(const llvm::Function *F);
bool isMatchLib(llvm::CallInst *CallI, const std::string &CalleeName,
                const std::string &LibName);

std::vector<std::pair<GuardedValueFlowNode *, GuardedValueFlowRegionNode *>>
getIncomingValuesForLoad(const GuardedValueFlowNode *Node);

template <typename T> llvm::Value *getPointerOperand(T *V) {
  return V->getPointerOperand()->stripPointerCasts();
}

} // namespace SymbolicExecution
