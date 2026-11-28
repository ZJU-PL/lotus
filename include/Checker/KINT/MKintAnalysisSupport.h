/** @file MKintAnalysisSupport.h @brief Analysis support utilities for MKint integer checker. */
#pragma once

#include "Checker/KINT/MKintPass.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <z3++.h>

namespace kint {

inline std::atomic<uint64_t> g_obj_mem_id{0};
inline std::atomic<uint64_t> g_summary_expr_id{0};

inline z3::expr bvValFromAPInt(z3::context &ctx, const llvm::APInt &value) {
  llvm::SmallString<64> decimal;
  value.toString(decimal, 10, /*Signed=*/false, /*formatAsCLiteral=*/false);
  Z3_sort sort = Z3_mk_bv_sort(ctx, value.getBitWidth());
  Z3_ast ast = Z3_mk_numeral(ctx, decimal.c_str(), sort);
  return z3::to_expr(ctx, ast);
}

inline const char *bugTypeToString(interr t) {
  switch (t) {
  case interr::NONE:
    return "none";
  case interr::INT_OVERFLOW:
    return "integer overflow";
  case interr::DIV_BY_ZERO:
    return "divide by zero";
  case interr::BAD_SHIFT:
    return "bad shift";
  case interr::ARRAY_OOB:
    return "array index out of bound";
  case interr::DEAD_TRUE_BR:
    return "impossible true branch";
  case interr::DEAD_FALSE_BR:
    return "impossible false branch";
  default:
    return "unknown";
  }
}

inline std::optional<uint64_t> getConstantU64(const llvm::Value *v) {
  if (!v)
    return std::nullopt;
  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    if (ci->getValue().getActiveBits() <= 64)
      return ci->getZExtValue();
  }
  return std::nullopt;
}

inline std::optional<uint64_t>
getTypeAllocSizeSafe(const llvm::DataLayout *dl, llvm::Type *ty) {
  if (!dl || !ty || !ty->isSized())
    return std::nullopt;
  llvm::TypeSize ts = dl->getTypeAllocSize(ty);
  if (ts.isScalable())
    return std::nullopt;
  return ts.getFixedSize();
}

inline std::optional<uint64_t>
getTypeStoreSizeSafe(const llvm::DataLayout *dl, llvm::Type *ty) {
  if (!dl || !ty || !ty->isSized())
    return std::nullopt;
  llvm::TypeSize ts = dl->getTypeStoreSize(ty);
  if (ts.isScalable())
    return std::nullopt;
  return ts.getFixedSize();
}

inline z3::expr boolToBv1(const z3::expr &b) {
  return z3::ite(b, b.ctx().bv_val(1, 1), b.ctx().bv_val(0, 1));
}

inline bool isBuiltinZeroArityExpr(const z3::expr &expr) {
  Z3_context rawCtx = expr.ctx();
  Z3_ast ast = expr;
  if (Z3_get_ast_kind(rawCtx, ast) != Z3_APP_AST)
    return false;
  if (Z3_is_numeral_ast(rawCtx, ast))
    return true;
  Z3_app app = Z3_to_app(rawCtx, ast);
  if (Z3_get_app_num_args(rawCtx, app) != 0)
    return false;
  switch (Z3_get_decl_kind(rawCtx, Z3_get_app_decl(rawCtx, app))) {
  case Z3_OP_TRUE:
  case Z3_OP_FALSE:
    return true;
  default:
    return false;
  }
}

inline void collectFreeConstants(const z3::expr &expr,
                                 llvm::DenseSet<Z3_ast> &visitedNodes,
                                 llvm::DenseSet<Z3_ast> &visitedConsts,
                                 std::vector<z3::expr> &outConstants) {
  Z3_ast ast = expr;
  if (!visitedNodes.insert(ast).second)
    return;

  Z3_context rawCtx = expr.ctx();
  switch (Z3_get_ast_kind(rawCtx, ast)) {
  case Z3_APP_AST: {
    Z3_app app = Z3_to_app(rawCtx, ast);
    const unsigned numArgs = Z3_get_app_num_args(rawCtx, app);
    if (numArgs == 0 && !isBuiltinZeroArityExpr(expr)) {
      if (visitedConsts.insert(ast).second)
        outConstants.push_back(expr);
      return;
    }
    for (unsigned i = 0; i < numArgs; ++i) {
      collectFreeConstants(
          z3::to_expr(expr.ctx(), Z3_get_app_arg(rawCtx, app, i)),
          visitedNodes, visitedConsts, outConstants);
    }
    return;
  }
  case Z3_QUANTIFIER_AST:
    collectFreeConstants(
        z3::to_expr(expr.ctx(), Z3_get_quantifier_body(rawCtx, ast)),
        visitedNodes, visitedConsts, outConstants);
    return;
  default:
    return;
  }
}

inline bool computeWithOverflow(
    const llvm::WithOverflowInst *woi, z3::solver &solver,
    const std::function<z3::expr(const llvm::Value *)> &getInt,
    z3::expr &outResult, z3::expr &outOverflowBool) {
  if (!woi || woi->arg_size() < 2 ||
      !woi->getArgOperand(0)->getType()->isIntegerTy() ||
      !woi->getArgOperand(1)->getType()->isIntegerTy())
    return false;
  auto lhs = getInt(woi->getArgOperand(0));
  auto rhs = getInt(woi->getArgOperand(1));

  switch (woi->getIntrinsicID()) {
  case llvm::Intrinsic::uadd_with_overflow:
    outResult = lhs + rhs;
    outOverflowBool = !z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/false);
    return true;
  case llvm::Intrinsic::usub_with_overflow:
    outResult = lhs - rhs;
    outOverflowBool = !z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/false);
    return true;
  case llvm::Intrinsic::umul_with_overflow:
    outResult = lhs * rhs;
    outOverflowBool = !z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/false);
    return true;
  case llvm::Intrinsic::sadd_with_overflow:
    outResult = lhs + rhs;
    outOverflowBool = (!z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/true) ||
                       !z3::bvadd_no_underflow(lhs, rhs));
    return true;
  case llvm::Intrinsic::ssub_with_overflow:
    outResult = lhs - rhs;
    outOverflowBool = (!z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/true) ||
                       !z3::bvsub_no_overflow(lhs, rhs));
    return true;
  case llvm::Intrinsic::smul_with_overflow:
    outResult = lhs * rhs;
    outOverflowBool = (!z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/true) ||
                       !z3::bvmul_no_underflow(lhs, rhs));
    return true;
  default:
    break;
  }
  outResult = solver.ctx().bv_val(0, 1);
  outOverflowBool = solver.ctx().bool_val(false);
  return false;
}

} // namespace kint
