#pragma once

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "IR/GVFG/GuardedValueFlowNodes.h"
#include "Utils/LLVM/PackedTypeLayout.h"
#include "Utils/LLVM/StringUtils.h"

#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>

namespace lotus {
namespace ir_expression {
using namespace llvm;
using namespace std;
inline constexpr int64_t UnknownOffset = std::numeric_limits<int64_t>::max();
inline Function *getEnclosingFunction(Value *value) {
  if (auto *instruction = dyn_cast<Instruction>(value))
    return instruction->getFunction();
  if (auto *argument = dyn_cast<Argument>(value))
    return argument->getParent();
  return dyn_cast<Function>(value);
}
inline Value *getRWPtr(Instruction *instruction) {
  if (auto *load = dyn_cast<LoadInst>(instruction))
    return load->getPointerOperand();
  if (auto *store = dyn_cast<StoreInst>(instruction))
    return store->getPointerOperand();
  return nullptr;
}
inline bool isPseudoArgument(const Value *value) {
  auto *argument = dyn_cast<Argument>(value);
  return argument && !argument->getParent();
}
inline std::string valueToString(const Value *value) {
  std::string text;
  raw_string_ostream out(text);
  value->printAsOperand(out, false);
  return out.str();
}

using namespace llvm;

inline llvm::Type *sequentialType(llvm::Type *type) {
  return type->isArrayTy() || type->isVectorTy() || type->isPointerTy()
             ? type
             : nullptr;
}
inline llvm::Type *sequentialElement(llvm::Type *type) {
  if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
    return array->getElementType();
  if (auto *vector = llvm::dyn_cast<llvm::VectorType>(type))
    return vector->getElementType();
  return type->getPointerElementType();
}

class ConstantVarAnalysis {
public:
  std::function<llvm::Constant *(llvm::Value *, llvm::Function *)> Resolver;
  llvm::Constant *getConstant(llvm::Value *value, llvm::Function *function) {
    return Resolver ? Resolver(value, function)
                    : llvm::dyn_cast<llvm::Constant>(value);
  }
};

class IRExpressionRenderer {
private:
  PackedTypeLayout *TSDL;
  DebugInfoAnalysis *DIA;

  // The cache for the computed offset for every GEP instruction
  std::unordered_map<Value *, std::pair<int64_t, int64_t>> gep_offs_cache;

  std::mutex gep_offs_cache_mtx;

public:
  IRExpressionRenderer(PackedTypeLayout &layout, DebugInfoAnalysis &debug);
  virtual ~IRExpressionRenderer();

public:
  // Return the non-aggregate type of field given a type and an offset
  // The offset should be in presented bits, (NOT bytes)
  Type *get_field_type(Type *ty, int64_t offset);

  // Calculate offset of a GEP/ExtractValue/InsertValue instruction
  // This is important for field-sensitive modeling
  // Return a 2-tuple where it means: <pointer offset, in-struct offset>.
  std::pair<int64_t, int64_t>
  get_gep_offset(GEPOperator *gep_inst, ConstantVarAnalysis *CVA = nullptr);

  int64_t get_extractValue_offset(ExtractValueInst *,
                                  ConstantVarAnalysis *CVA = nullptr);

  int64_t get_insertValue_offset(InsertValueInst *,
                                 ConstantVarAnalysis *CVA = nullptr);

  /*
   * Trace back to base pointer for a chain of GEP/bitcast instructions and
   * calculate the offset. The offset is the accumulation of the constant
   * pointer offsets and the in-struct offset into a struct. Variable offsets
   * are dropped.
   *
   * e.g:
   * %a = load ...
   * %p = gep %a, 1, 2, 3
   * %q = bitcast %p
   * %u = gep %q, 4, 5
   * ------------------------------
   * track_pointer_offset(%u) -> <%a, 15>
   *
   * \p CVA : inspecting the values that can be constants
   * if \p CVA is nullptr, we only track the values of type llvm::constant as
   * constants
   */
  std::pair<Value *, int64_t>
  track_pointer_offset(Value *ptr, bool track_to_top = false,
                       ConstantVarAnalysis *CVA = nullptr);

  /*
   * Synthesize pointer expression starting with a GEP instruction \p ptr.
   * Return a 3-tuple where the first is the base pointer of the GEP chain, the
   * second is the pointer offset, and the third is the pointer expression in
   * string form. The pointer offset will be UnknownOffset if variable is
   * applied to the offset. \p phi_choice: please refer to function comment for
   * "restore_access_path_expr()"
   *
   * e.g.:
   * %q = gep %p, c1
   * %u = gep %q, c2, 2, 3
   * -----------------
   * generate_pointer_offset_expr(%u) -> (%q, UnknownOffset,
   * p[c1+c2].field_at_offset_5)
   */
  std::tuple<Value *, int64_t, std::string> generate_pointer_offset_expr(
      Value *ptr, std::unordered_map<Value *, Value *> *phi_choice = nullptr);

  // Resolve the inlined pointer constant expression
  // Return the base pointer and the offset to the base
  std::tuple<Value *, int64_t, int64_t> resolve_ptr_constexpr(Value *ptr_expr);

public:
  // Restore the access path expression in source code from a GEP instruction,
  // in which: \p is_deref_expr: indicates that \p ptr is used for dereference
  // or not \p phi_choice specifies the value choices of PhiNodes, where
  // phi_choice.first is the PhiNode, and phi_choice.second is the Value choice
  // When choice is null, we do not track PhiNodes in the GEP chain
  std::string restore_access_path_expr(
      Value *ptr, bool is_deref_expr = true,
      std::unordered_map<Value *, Value *> *phi_choice = nullptr);

  // Another way to construct the human readable expression for an access path
  // \p ap. \p base_ptr_name is the customized name for base pointer of the
  // access path. It is used, for example, to display pseudo inputs, which is
  // better to use the real argument name instead of the formal argument.
  std::string restore_access_path_expr(const gvfg::AccessPath &ap,
                                       std::string base_ptr_name = "");

  /*
   * Construct the arithmetic expression by backward traversing the IR.
   * \p depth records current recursive depth.
   * If \p depth == 0, the expression is no longer wrapped by a pair of
   * parenthesis.
   *
   * e.g.:
   * %1 = mul i32 3, %0
   * %3 = add i32 %1, %2
   * --------------------
   * restore_arithmetic_expr(%3) = 3*%0+%2
   */
  std::string restore_expr(Value *arith_expr, int depth = 0);

  // restore the callsite expression as func_name(args)
  std::string restore_callsite_expr(llvm::CallBase *cs);

  // Try best to restore the debug expression for val
  // phi_choice: please refer to function comment for
  // "restore_access_path_expr()"
  std::string restore_value_expr(
      Value *val, std::unordered_map<Value *, Value *> *phi_choice = nullptr);

  // Try best to restore the debug expression for val,
  // Try best identifying left-values
  // E.g. %a = alloca(), in restore_value_expr(), we return "a" and in
  // restore_left_value_expr() we return "&a" phi_choice: please refer to
  // function comment for "restore_access_path_expr()"
  std::string restore_left_value_expr(
      Value *val, std::unordered_map<Value *, Value *> *phi_choice = nullptr);

  // Try best to restore the debug expression for val as a right value,
  // We first identify left-values by return %a = alloca() as "&a"
  // Then we get the right value by upgrade the pointer level
  // E.g. %a = alloca(), we return "a" and for %a=load ... we return "*a"
  // phi_choice: please refer to function comment for
  // "restore_access_path_expr()"
  std::string restore_right_value_expr(
      Value *val, std::unordered_map<Value *, Value *> *phi_choice = nullptr);

  // Return the address of a value expr
  // e.g. a => &a , **a => *a
  static void value_to_address_expr(std::string &value_string);

  // Return the value expr corresponding to an address expr
  // e.g. a => *a , &a => a
  static void address_to_value_expr(std::string &address_string);

private:
  // Do real job to compute the offset of single gep/extractvalue/insertvalue
  // instruction
  template <typename UserTy>
  int64_t get_inbound_offset(UserTy *inst, int start_id, Type *start_type,
                             ConstantVarAnalysis *CVA);

  // Trace back the continuous *cast operations from \p cast_val
  // \p ptr_src = true => continue backtracing only if the source value is a
  // pointer
  Value *traceback_cast_chain(Value *cast_val, bool ptr_src = false);

  std::string construct_gep_expr(GEPOperator *gep_inst);

  // Remove the redundant brackets of the given string
  // e.g. ((*a)) => *a
  static void remove_redundant_brackets(std::string &to_refactor);

public:
};

} // namespace ir_expression
} // namespace lotus
