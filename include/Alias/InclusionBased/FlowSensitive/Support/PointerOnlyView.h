/**
 * @file PointerOnlyView.h
 * @brief Filtered SVFG view retaining only pointer-typed operations.
 *
 * SVF constructs a pointer-only SVFG (memSSA.buildPTROnlySVFG) so that scalar
 * loads, stores, and non-pointer operations do not pollute the solver worklist
 * or allocate unused memory state versions.
 *
 * This header provides an upfront FilteredSVFGView over an existing SVFG to
 * restrict sparse and versioned solvers to pointer operations.
 */
#pragma once

#include "IR/GraphView.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>

#include <memory>

namespace lotus::alias::flow_sensitive {

/// Checks if an SVFG node represents a pointer-typed value or memory operation.
inline bool isPointerSVFGNode(const analysis::SVFGNode *node) {
  if (!node)
    return false;
  if (llvm::isa<analysis::BinaryOpSVFGNode>(node) ||
      llvm::isa<analysis::CmpSVFGNode>(node) ||
      llvm::isa<analysis::BranchSVFGNode>(node))
    return false;
  if (const auto *load = llvm::dyn_cast<analysis::LoadSVFGNode>(node)) {
    const llvm::Instruction *inst = load->getInstruction();
    return !inst || inst->getType()->isPointerTy();
  }
  if (const auto *store = llvm::dyn_cast<analysis::StoreSVFGNode>(node)) {
    const llvm::Instruction *inst = store->getInstruction();
    if (!inst)
      return true;
    if (const auto *storeInst = llvm::dyn_cast<llvm::StoreInst>(inst))
      return storeInst->getValueOperand()->getType()->isPointerTy();
    return true;
  }
  if (const auto *phi = llvm::dyn_cast<analysis::IntraPhiSVFGNode>(node)) {
    const llvm::Instruction *inst = phi->getInstruction();
    return !inst || inst->getType()->isPointerTy();
  }
  if (const auto *stmt = llvm::dyn_cast<analysis::StmtSVFGNode>(node)) {
    const llvm::Value *val = stmt->getValue();
    if (val && !val->getType()->isVoidTy())
      return val->getType()->isPointerTy();
  }
  return true;
}

/// Creates an upfront FilteredSVFGView retaining only pointer-typed nodes.
inline std::unique_ptr<analysis::FilteredSVFGView>
createPointerOnlySVFGView(const analysis::SVFG &graph) {
  return std::make_unique<analysis::FilteredSVFGView>(
      graph, [](const analysis::SVFGNode &node) {
        return isPointerSVFGNode(&node);
      });
}

} // namespace lotus::alias::flow_sensitive
