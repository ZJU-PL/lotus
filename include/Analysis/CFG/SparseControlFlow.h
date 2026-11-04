#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Analysis/CallGraph/CallGraph.h"

#include <functional>
#include <unordered_map>

namespace lotus {

class SparseLLVMBasedCFG {
public:
  using vgraph_t =
      llvm::SmallDenseMap<const llvm::Instruction *, const llvm::Instruction *>;

  SparseLLVMBasedCFG() noexcept = default;
  explicit SparseLLVMBasedCFG(vgraph_t &&VGraph) noexcept
      : VGraph(std::move(VGraph)) {}

  [[nodiscard]] const llvm::Instruction *
  nextUserOrNull(const llvm::Instruction *FromInstruction) const {
    return VGraph.lookup(FromInstruction);
  }

  [[nodiscard]] const vgraph_t &getGraph() const noexcept { return VGraph; }

private:
  friend class SparseLLVMBasedICFG;
  vgraph_t VGraph;
};

class SparseLLVMControlFlow {
public:
  using AliasCheckFn =
      std::function<bool(const llvm::Value *, const llvm::Value *)>;

  [[nodiscard]] static const llvm::Instruction *
  advanceToNextUser(const llvm::Instruction *Succ, const llvm::Value *Fact,
                    const AliasCheckFn &MayAlias = nullptr);

  [[nodiscard]] static bool
  shouldKeepInst(const llvm::Instruction *Inst, const llvm::Value *Val,
                 const AliasCheckFn &MayAlias = nullptr);

  [[nodiscard]] static llvm::SmallVector<const llvm::Instruction *, 2>
  getNormalSuccsOf(const llvm::Instruction *Inst);

  [[nodiscard]] static bool isStartInst(const llvm::Instruction *Inst);
  [[nodiscard]] static bool isExitInst(const llvm::Instruction *Inst);
  [[nodiscard]] static bool isNoopIntrinsic(const llvm::Instruction *Inst);
};

class SparseLLVMBasedICFG {
public:
  using AliasCheckFn = SparseLLVMControlFlow::AliasCheckFn;

  explicit SparseLLVMBasedICFG(const CallGraph *CG = nullptr,
                               AliasCheckFn MayAlias = nullptr);

  [[nodiscard]] const SparseLLVMBasedCFG &
  getSparseCFG(const llvm::Function *Fun, const llvm::Value *Val) const;

  [[nodiscard]] const llvm::Instruction *
  advanceToNextUser(const llvm::Instruction *Succ,
                    const llvm::Value *Fact) const;

  [[nodiscard]] llvm::SmallVector<const llvm::Instruction *, 2>
  getSparseSuccsOf(const llvm::Instruction *Inst,
                   const llvm::Value *Fact) const;

  [[nodiscard]] const CallGraph *getCallGraph() const noexcept { return CG; }

  [[nodiscard]] bool shouldKeepInst(const llvm::Instruction *Inst,
                                    const llvm::Value *Fact) const {
    return SparseLLVMControlFlow::shouldKeepInst(Inst, Fact, MayAlias);
  }

private:
  struct FVHasher {
    std::size_t
    operator()(const std::pair<const llvm::Function *, const llvm::Value *> &FV)
        const noexcept {
      return std::hash<const llvm::Function *>()(FV.first) ^
             (std::hash<const llvm::Value *>()(FV.second) << 1);
    }
  };

  const CallGraph *CG = nullptr;
  AliasCheckFn MayAlias;
  mutable std::unordered_map<
      std::pair<const llvm::Function *, const llvm::Value *>,
      SparseLLVMBasedCFG, FVHasher>
      Cache;
};

} // namespace lotus
