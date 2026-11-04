#include "Utils/LLVM/CallUtils.h"
#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "IR/GVFG/GuardedValueFlowNodes.h"
#include "Utils/LLVM/PackedTypeLayout.h"
#include "Utils/LLVM/StringUtils.h"

#include <cinttypes>
#include <functional>
#include <limits>
#include <tuple>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>

namespace lotus {
namespace ir_expression {
using namespace llvm;
using namespace std;

#define DEBUG_TYPE "inst_resolver"

using namespace llvm;
using namespace std;

IRExpressionRenderer::IRExpressionRenderer(PackedTypeLayout &layout,
                                           DebugInfoAnalysis &debug)
    : TSDL(&layout), DIA(&debug) {}
IRExpressionRenderer::~IRExpressionRenderer() = default;

/*
 * start_id: start operand,
 * start_type: start type to extract offset info
 * sym_offs: the container to hold symbolic variables in the offset computation
 */
template <typename UserTy>
int64_t IRExpressionRenderer::get_inbound_offset(UserTy *inst, int start_id,
                                                 Type *start_type,
                                                 ConstantVarAnalysis *CVA) {
  int64_t offset = 0;
  int num_indices = static_cast<int>(inst->getNumOperands());
  int idx = start_id;
  Type *type = start_type;
  Function *f = getEnclosingFunction(inst);

  while (idx < num_indices) {
    Value *vidx = inst->getOperand(idx);
    Constant *const_vidx =
        CVA && f ? CVA->getConstant(vidx, f) : dyn_cast<Constant>(vidx);

    if (auto *CI = dyn_cast<ConstantInt>(const_vidx)) {
      int64_t fieldIdx = CI->getSExtValue();
      if (Type *sequential_type = sequentialType(type)) {
        // TODO: currently, we collapse all the array fields, can be improved
        // later
        type = sequentialElement(sequential_type);
        offset += fieldIdx * static_cast<int64_t>(TSDL->getTypeSizeInBits(type));
      } else if (StructType *struct_type = dyn_cast<StructType>(type)) {
        // getElementOffset works in O(1), it is fast enough and we do not need
        // to cache the result
        int64_t curr_offset =
            static_cast<int64_t>(TSDL->getElementOffsetInBits(struct_type,
                                                              fieldIdx));
        offset += curr_offset;
        type = struct_type->getElementType(fieldIdx);
      } else {
        errs() << "*** Fatal error: GEP into a non-composite type:";
        type->print(errs());
        errs() << " ***\n";
        offset = UnknownOffset;
        break;
      }
    } else {
      // This is a symbolic offset
      if (Type *sequential_type = sequentialType(type)) {
        // offset remains the same. (i.e. should collapse the array field)
        type = sequentialElement(sequential_type);
        //				if (sym_offs)
        //					sym_offs->push_back(vidx);
      } else {
        // collapse all fields of the GEP base type if a symbolic offset is
        // applied to a struct
        errs() << "*** Fatal error: GEP into a struct with non-constant offset "
                  "***\n";
        offset = UnknownOffset;
        break;
      }
    }
    ++idx;
  }

  return offset;
}

static int64_t get_gep_offset_at(GEPOperator *gep_inst, unsigned i,
                                 ConstantVarAnalysis *CVA) {
  int64_t offset = UnknownOffset;
  Value *vidx = gep_inst->getOperand(i);

  Function *f = getEnclosingFunction(gep_inst);
  Constant *const_vidx =
      CVA && f ? CVA->getConstant(vidx, f) : dyn_cast<Constant>(vidx);

  if (const_vidx) {
    if (ConstantInt *ci = dyn_cast<ConstantInt>(const_vidx)) {
      // Call getSExtValue because the inner_offset can be negative
      offset = ci->getSExtValue();
    }
  }

  return offset;
}

static bool has_inbound_offset(GEPOperator *gep_inst) {
  return gep_inst->getNumOperands() >= 3;
}

pair<int64_t, int64_t>
IRExpressionRenderer::get_gep_offset(GEPOperator *gep_inst,
                                     ConstantVarAnalysis *CVA) {
  {
    std::unique_lock<std::mutex> lock(gep_offs_cache_mtx);
    auto iRes = gep_offs_cache.find(gep_inst);
    if (iRes != gep_offs_cache.end()) {
      return iRes->second;
    }
  }

  Value *ptr = gep_inst->getOperand(0);

  Type *ptr_type = ptr->getType();
  Type *base_type = ptr_type;

  if (ptr_type->isPointerTy()) {
    base_type = ptr_type->getPointerElementType();
  } else {
    // Must be a pointer type according to LLVM manual
    errs() << "**× Fatal error: The base value of GEP is not a pointer ***\n";
    return make_pair(UnknownOffset, UnknownOffset);
  }

  // We first collect the pointer offset
  int64_t pointer_offset = get_gep_offset_at(gep_inst, 1, CVA);
  if (pointer_offset != UnknownOffset)
    pointer_offset *= static_cast<int64_t>(TSDL->getTypeSizeInBits(base_type));
  else
    // Suggested again by Andy, we treat symbolic array offsets as offset 0
    pointer_offset = 0;

  // Then we calculate the pointer offset
  int64_t inbound_offset = UnknownOffset;

  if (isa<StructType>(base_type)) {
    /*
     *  Currently, we collapse all the array elements to the index 0 element.
     *  i.e. a[2].b[idx].c.d[7][8] is regarded as a[0].b[0].c.d[0][0]
     */
    const int start_id = 2;
    Type *start_type = base_type;

    inbound_offset = get_inbound_offset(gep_inst, start_id, start_type, CVA);

    // Register the name for field at this inbound_offset
    if (inbound_offset != UnknownOffset) {
      if (gep_inst->hasName()) {
        if (StructType *struct_type = dyn_cast<StructType>(base_type)) {
          DIA->registerFieldName(struct_type, inbound_offset,
                                 gep_inst->getName().str());
        }
      }
    }
  } else if ((isa<ArrayType>(base_type)) || isa<VectorType>(base_type)) {
    // Array and vector types
    if (has_inbound_offset(gep_inst)) {
      Type *seq_type = sequentialType(base_type);
      Type *elem_type = sequentialElement(seq_type);
      inbound_offset = get_gep_offset_at(gep_inst, 2, CVA);
      if (inbound_offset != UnknownOffset)
        inbound_offset *=
            static_cast<int64_t>(TSDL->getTypeSizeInBits(elem_type));
      else
        // Suggested by Andy, we treat symbolic array offsets as offset 0
        inbound_offset = 0;
    }
  } else {
    /*
     * The GEP instruction could be:
     * %arrayidx48 = getelementptr inbounds i32* %80, i64 %indvars.iv6
     * This refers to accessing an array with pointer arithmetics.
     * It only has an outer offset because the base type is not a composite
     * type.
     */
    // Suggested again and again... by Andy, we treat symbolic pointer
    // arithmetic offsets as offset 0
    inbound_offset = 0;
  }

  auto ret_val = make_pair(pointer_offset, inbound_offset);

  {
    std::unique_lock<std::mutex> lock(gep_offs_cache_mtx);
    gep_offs_cache[gep_inst] = ret_val;
  }

  return ret_val;
}

int64_t
IRExpressionRenderer::get_extractValue_offset(ExtractValueInst *ev_inst,
                                              ConstantVarAnalysis *CVA) {
  Value *base = ev_inst->getOperand(0);
  Type *base_type = base->getType();
  if (!isa<Type>(base_type)) {
    /*
     * Extract Value must start from a composite type
     */
    assert(false && "Extracting values from a non-composite type");
  }

  const int start_id = 1;
  Type *start_type = base_type;

  return get_inbound_offset(ev_inst, start_id, start_type, CVA);
}

int64_t IRExpressionRenderer::get_insertValue_offset(InsertValueInst *iv_inst,
                                                     ConstantVarAnalysis *CVA) {
  Value *base = iv_inst->getOperand(0);
  Type *base_type = base->getType();

  if (!isa<Type>(base_type)) {
    /*
     * InsertValue must start from a composite type
     */
    assert(false && "Inserting values to a non-composite type");
  }

  /*
   * operand 1 is the inserted value and the index starts from operand 2
   */
  const int start_id = 2;
  Type *start_type = base_type;

  return get_inbound_offset(iv_inst, start_id, start_type, CVA);
}

Value *IRExpressionRenderer::traceback_cast_chain(Value *cast_val,
                                                  bool ptr_src) {
  while (Instruction *cast_inst = dyn_cast<Instruction>(cast_val)) {
    if (cast_inst->isCast()) {
      Value *src_val = cast_inst->getOperand(0);
      if (ptr_src && !src_val->getType()->isPointerTy()) {
        break;
      }

      cast_val = src_val;
    } else
      break;
  }

  return cast_val;
}

tuple<Value *, int64_t, int64_t>
IRExpressionRenderer::resolve_ptr_constexpr(Value *ptr) {
  int64_t pointer_offset = 0, inbound_offset = 0;
  bool cont_flag;

  while (ConstantExpr *ptr_expr = dyn_cast<ConstantExpr>(ptr)) {
    cont_flag = false;

    if (GEPOperator *gep_op = dyn_cast<GEPOperator>(ptr)) {
      pair<int64_t, int64_t> offset_pack = get_gep_offset(gep_op);

      // It's safe to add the offset directly
      pointer_offset += offset_pack.first;
      inbound_offset += offset_pack.second;
      ptr = gep_op->getPointerOperand();
      cont_flag = true;
    } else if (Instruction::isCast(ptr_expr->getOpcode())) {
      ptr = ptr_expr->getOperand(0);
      cont_flag = true;
    }

    // TODO: Some instructions such as ExtractElement are not handled.
    // Might cause problems and need more investigation.
    if (!cont_flag)
      break;
  }

  return make_tuple(ptr, pointer_offset, inbound_offset);
}

pair<Value *, int64_t>
IRExpressionRenderer::track_pointer_offset(Value *ptr, bool track_to_top,
                                           ConstantVarAnalysis *CVA) {
  int64_t offset = 0;

  if (isa<ConstantExpr>(ptr)) {
    auto off_pack = resolve_ptr_constexpr(ptr);
    ptr = std::get<0>(off_pack);
    offset = std::get<1>(off_pack) + std::get<2>(off_pack);
  } else {
    while (true) {
      Value *ptr_start = ptr;

      while (GEPOperator *gep_inst = dyn_cast<GEPOperator>(ptr)) {
        if (offset != UnknownOffset) {
          pair<int64_t, int64_t> offset_pack = get_gep_offset(gep_inst, CVA);
          if (offset_pack.first == UnknownOffset ||
              offset_pack.second == UnknownOffset) {
            offset = UnknownOffset;
          } else {
            offset += offset_pack.first + offset_pack.second;
          }
        }

        ptr = gep_inst->getPointerOperand();
      }

      /*
       * We test whether the base pointer is pointer type because we deem that:
       * cast from non-pointer type to pointer type should be explicit written
       * in code and thus, we should break the tracing back chain to reflect the
       * source code in fidelity.
       */
      ptr = traceback_cast_chain(ptr, true);

      if (ptr == ptr_start || !track_to_top)
        break;
    }
  }

  return make_pair(ptr, offset);
}

string IRExpressionRenderer::construct_gep_expr(GEPOperator *gep_inst) {
  string gep_expr;

  Value *ptr = gep_inst->getOperand(0);
  Type *ptr_type = ptr->getType();
  Type *base_type = ptr_type;

  if (ptr_type->isPointerTy()) {
    base_type = ptr_type->getPointerElementType();
  } else {
    // Must be a pointer type according to LLVM manual
    errs() << "*** Fatal error: The base value of GEP is not a pointer ***\n";
    return "@GEP@";
  }

  Type *type = base_type;

  // Construct the pointer offset expression
  Value *vidx = gep_inst->getOperand(1);
  if (isa<ConstantInt>(vidx)) {
    ConstantInt *ci = cast<ConstantInt>(vidx);
    int64_t fieldIdx = ci->getSExtValue();
    if (fieldIdx != 0)
      gep_expr = "[" + format_str("%" PRId64, fieldIdx) + "]";
  } else {
    gep_expr = "[" + restore_expr(vidx) + "]";
  }

  int idx = 2;
  int num_indices = static_cast<int>(gep_inst->getNumOperands());

  while (idx < num_indices) {
    Value *vidx = gep_inst->getOperand(idx);

    if (isa<ConstantInt>(vidx)) {
      ConstantInt *ci = cast<ConstantInt>(vidx);
      int64_t fieldIdx = ci->getSExtValue();

      if (isa<ArrayType>(type) || isa<VectorType>(type)) {
        Type *sequential_type = sequentialType(type);
        type = sequentialElement(sequential_type);
        gep_expr += "[" + format_str("%" PRId64, fieldIdx) + "]";
      } else if (StructType *struct_type = dyn_cast<StructType>(type)) {
        // getElementOffset works in O(1), it is fast enough and we do not need
        // to cache the result
        int64_t curr_offset = static_cast<int64_t>(
            TSDL->getElementOffsetInBits(struct_type, fieldIdx));

        if (!gep_expr.empty())
          gep_expr += ".";
        gep_expr += DIA->getFieldName(struct_type, curr_offset, 0);

        type = struct_type->getElementType(fieldIdx);
      } else {
        errs() << "*** Fatal error: GEP into a non-composite type:";
        type->print(errs());
        errs() << " ***\n";
        break;
      }
    } else {
      // This is a symbolic offset
      if (Type *sequential_type = sequentialType(type)) {
        type = sequentialElement(sequential_type);
        gep_expr += "[" + restore_expr(vidx) + "]";
      } else {
        // collapse all fields of the GEP base type if a symbolic offset is
        // applied to a struct
        errs() << "*** Fatal error: GEP into a struct with non-constant offset "
                  "***\n";
        break;
      }
    }

    ++idx;
  }

  LLVM_DEBUG(dbgs() << "GEP: " << DIA->getIRString(gep_inst)
                    << ", EXPR: " << gep_expr << "\n";);
  return gep_expr;
}

tuple<Value *, int64_t, string>
IRExpressionRenderer::generate_pointer_offset_expr(
    Value *ptr, unordered_map<Value *, Value *> *phi_choice) {
  string ap_expr;
  int64_t pointer_offset = 0;

  if (isa<ConstantExpr>(ptr)) {
    // Skip outer bitcast instructions
    while (ConstantExpr *ptr_expr = dyn_cast<ConstantExpr>(ptr)) {
      if (Instruction::isCast(ptr_expr->getOpcode())) {
        ptr = ptr_expr->getOperand(0);
      } else
        break;
    }

    if (GEPOperator *gep_op = dyn_cast<GEPOperator>(ptr)) {
      ap_expr = construct_gep_expr(gep_op);
      auto gep_tuple = resolve_ptr_constexpr(gep_op);
      ptr = std::get<0>(gep_tuple);
      pointer_offset = std::get<1>(gep_tuple);
    }

    // Special case: load a constant string
    if (GlobalVariable *gv = dyn_cast<GlobalVariable>(ptr)) {
      if (gv->hasUniqueInitializer()) {
        Constant *init_expr = gv->getInitializer();
        if (ConstantDataSequential *const_seq =
                dyn_cast<ConstantDataSequential>(init_expr)) {
          if (const_seq->isCString()) {
            ap_expr = const_seq->getAsCString().substr(pointer_offset);
            ptr = nullptr;
          }
        }
      }
    }
  } else {
    // Keep all the symbolic values appeared in the GEPs
    while (GEPOperator *gep_inst = dyn_cast<GEPOperator>(ptr)) {
      pair<int64_t, int64_t> offset_pack = get_gep_offset(gep_inst);
      if (pointer_offset != 0) {
        /*
         * We already see some GEPs add pointer offsets to a pointer.
         * We cannot trace back if current GEP adds inbound offset.
         */
        if (has_inbound_offset(gep_inst))
          break;
      }

      std::string gep_expr = construct_gep_expr(gep_inst);
      if (!ap_expr.empty()) {
        if (ap_expr[0] != '[')
          gep_expr += ".";
        gep_expr += ap_expr;
      }
      ap_expr = std::move(gep_expr);

      if (pointer_offset != UnknownOffset) {
        if (offset_pack.first != UnknownOffset)
          pointer_offset += offset_pack.first;
        else
          pointer_offset = UnknownOffset;
      }

      ptr = gep_inst->getPointerOperand();

      // We also stop on array or vector types to correctly build their
      // multi-dimensional expressions
      Type *base_type = ptr->getType();
      if (isa<PointerType>(base_type))
        base_type = base_type->getPointerElementType();
      if (isa<ArrayType>(base_type) || isa<VectorType>(base_type))
        break;
    }

    if (phi_choice) {
      while (phi_choice->count(ptr) || isa<PHINode>(ptr)) {
        Value *phi_choice_val =
            phi_choice->count(ptr) ? (*phi_choice)[ptr] : nullptr;

        Value *potential_base_val = nullptr;

        if (phi_choice_val) {
          if (!isa<Constant>(phi_choice_val))
            potential_base_val = phi_choice_val;
        } else {
          if (DIA->hasVariableDebugName(ptr)) {
            // We don't track further in case it has a name
            break;
          }

          // TODO: Here we track the first non-constant incoming value.
          //       We should be more specific to the trace.
          //       Yet due to the sparse property of the trace, it is
          //       challenging. Should improve in future.
          PHINode *phi_ptr = dyn_cast<PHINode>(ptr);

          if (phi_ptr) {
            for (unsigned i = 0; i < phi_ptr->getNumIncomingValues(); i++) {
              phi_choice_val = phi_ptr->getIncomingValue(i);
              if (!isa<Constant>(phi_choice_val)) {
                potential_base_val = phi_choice_val;
                break;
              }
            }
          }
        }

        if (!potential_base_val)
          break;

        ptr = potential_base_val;
      }
    }
  }

  return make_tuple(ptr, pointer_offset, ap_expr);
}

static const char *get_binaryOP_symbol(unsigned op_code) {
  const char *op_sym = " ";

  switch (op_code) {
  case Instruction::Add:
  case Instruction::FAdd:
    op_sym = "+";
    break;

  case Instruction::Sub:
  case Instruction::FSub:
    op_sym = "-";
    break;

  case Instruction::Mul:
  case Instruction::FMul:
    op_sym = "*";
    break;

  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
    op_sym = "/";
    break;

  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
    op_sym = "%";
    break;

  case Instruction::Shl:
    op_sym = "<<";
    break;

  case Instruction::LShr:
  case Instruction::AShr:
    op_sym = ">>";
    break;

  case Instruction::And:
    op_sym = "&";
    break;

  case Instruction::Or:
    op_sym = "|";
    break;

  case Instruction::Xor:
    op_sym = "^";
    break;
  }

  return op_sym;
}

static const char *get_cmpOP_symbol(CmpInst::Predicate pred) {
  const char *op_sym = nullptr;

  switch (pred) {
  case CmpInst::Predicate::ICMP_EQ:
  case CmpInst::Predicate::FCMP_OEQ:
  case CmpInst::Predicate::FCMP_UEQ:
    /// equal
    op_sym = "==";
    break;
  case CmpInst::Predicate::ICMP_NE:
  case CmpInst::Predicate::FCMP_ONE:
  case CmpInst::Predicate::FCMP_UNE:
    /// not equal
    op_sym = "!=";
    break;

  case CmpInst::Predicate::ICMP_UGT:
  case CmpInst::Predicate::ICMP_SGT:
  case CmpInst::Predicate::FCMP_OGT:
  case CmpInst::Predicate::FCMP_UGT:
    /// greater than
    op_sym = ">";
    break;

  case CmpInst::Predicate::ICMP_UGE:
  case CmpInst::Predicate::ICMP_SGE:
  case CmpInst::Predicate::FCMP_OGE:
  case CmpInst::Predicate::FCMP_UGE:
    /// greater or equal
    op_sym = ">=";
    break;

  case CmpInst::Predicate::ICMP_ULT:
  case CmpInst::Predicate::ICMP_SLT:
  case CmpInst::Predicate::FCMP_OLT:
  case CmpInst::Predicate::FCMP_ULT:
    /// less than
    op_sym = "<";
    break;
  case CmpInst::Predicate::ICMP_ULE:
  case CmpInst::Predicate::ICMP_SLE:
  case CmpInst::Predicate::FCMP_OLE:
  case CmpInst::Predicate::FCMP_ULE:
    op_sym = "<=";
    /// less or equal
    break;

  case CmpInst::Predicate::FCMP_FALSE:
  case CmpInst::Predicate::FCMP_ORD:
  case CmpInst::Predicate::FCMP_UNO:
  case CmpInst::Predicate::FCMP_TRUE:
  case CmpInst::Predicate::BAD_FCMP_PREDICATE:
  case CmpInst::Predicate::BAD_ICMP_PREDICATE:
    // special
    op_sym = nullptr;
    break;
  }

  return op_sym;
}

string IRExpressionRenderer::restore_callsite_expr(llvm::CallBase *cs) {
  Value *called_value = cs->getCalledOperand();
  string call_expr = "";

  if (called_value) {
    bool is_class_member_function = false;
    Function *callee = cs->getCalledFunction();

    if ((callee && llvm_utils::isClassMemberFunction(*callee)) ||
        llvm_utils::isVirtualCall(cs)) {
      is_class_member_function = true;
    }

    unsigned arg_num = cs->arg_size();

    if (is_class_member_function && arg_num >= 1) {
      string first_arg_expr = restore_expr(cs->getArgOperand(0));
      call_expr = first_arg_expr + "->";
    }

    if (callee) {
      string readable_name = DIA->getDeclaredFunctionName(callee);
      call_expr += readable_name;
    } else {
      if (llvm_utils::isVirtualCall(cs)) {
        call_expr += "<virtual call>";
      } else {
        call_expr += restore_expr(called_value);
      }
    }
    call_expr += "(";

    int arg_start_idx = is_class_member_function ? 1 : 0;

    for (unsigned i = arg_start_idx; i < arg_num; i++) {
      call_expr += restore_expr(cs->getArgOperand(i));
      if (i + 1 != arg_num) {
        call_expr += ",";
      }
    }
    call_expr += ")";
  } else {
    call_expr = "unknownCallSite";
  }

  return call_expr;
}

string IRExpressionRenderer::restore_expr(Value *an_expr, int depth) {
  if (!an_expr)
    return "";

  if (isPseudoArgument(an_expr)) {
    return "PseudoArg";
  }

  string expr_str = "";
  Instruction *expr_inst = dyn_cast<Instruction>(an_expr);

  bool has_dbg_name = DIA->hasVariableDebugName(an_expr);

  LLVM_DEBUG(dbgs() << "Value = " << valueToString(an_expr)
                    << ", has debug name = "
                    << (has_dbg_name ? "true" : "false") << "\n");

  if (expr_inst != nullptr && !has_dbg_name) {
    unsigned op_code = expr_inst->getOpcode();

    if (expr_inst->isBinaryOp()) {
      const char *op_sym = get_binaryOP_symbol(op_code);

      Value *a = expr_inst->getOperand(0);
      Value *b = expr_inst->getOperand(1);

      string expr_str_inner = "";
      if (op_code == Instruction::Xor && b != nullptr && isa<ConstantInt>(b)) {
        // A^True means !a, needs special handling
        ConstantInt *ci_b = dyn_cast<ConstantInt>(b);
        if (ci_b->getZExtValue() == 0) {
          expr_str_inner = restore_expr(a, depth);
        } else {
          expr_str_inner = "!" + restore_expr(a, depth + 1);
        }
      } else {
        expr_str_inner =
            restore_expr(a, depth + 1) + op_sym + restore_expr(b, depth + 1);
      }

      if (depth > 0)
        expr_str = "(" + expr_str_inner + ")";
      else
        expr_str = std::move(expr_str_inner);
    } else if (CmpInst *cmp_inst = dyn_cast<CmpInst>(expr_inst)) {
      CmpInst::Predicate pred = cmp_inst->getPredicate();
      const char *op_sym = get_cmpOP_symbol(pred);
      if (op_sym && cmp_inst->getNumOperands() == 2) {
        Value *a = cmp_inst->getOperand(0);
        Value *b = cmp_inst->getOperand(1);

        string expr_str_inner =
            restore_expr(a, depth + 1) + op_sym + restore_expr(b, depth + 1);

        if (depth > 0)
          expr_str = "(" + expr_str_inner + ")";
        else
          expr_str = std::move(expr_str_inner);
      }
    } else if (SelectInst *select_inst = dyn_cast<SelectInst>(expr_inst)) {
      Value *true_select_val = select_inst->getTrueValue();
      Value *false_select_val = select_inst->getFalseValue();
      Value *cond_val = select_inst->getCondition();
      string expr_str_inner = restore_expr(cond_val, depth + 1) + "?" +
                              restore_expr(true_select_val, depth + 1) + ":" +
                              restore_expr(false_select_val, depth + 1);

      expr_str = "(" + expr_str_inner + ")";

    } else if (CallInst *call_inst = dyn_cast<CallInst>(expr_inst)) {
      llvm::CallBase *cs(call_inst);
      expr_str = restore_callsite_expr(cs);
    } else if (InvokeInst *invoke_inst = dyn_cast<InvokeInst>(expr_inst)) {
      llvm::CallBase *cs(invoke_inst);
      expr_str = restore_callsite_expr(cs);
    } else if (op_code != Instruction::PHI) {
      if (expr_inst->isCast()) {
        // We handle non-pointer cast at this place
        Value *cast_from = expr_inst->getOperand(0);
        expr_str = restore_expr(cast_from, depth);
      } else {
        Value *rw_ptr = getRWPtr(expr_inst);
        if (rw_ptr != nullptr)
          an_expr = rw_ptr;

        // Could result in infinite recursions if loops are no longer flattened
        expr_str =
            restore_access_path_expr(an_expr, !(isa<GlobalVariable>(an_expr) ||
                                                isa<AllocaInst>(an_expr)));
      }
    }
  } else if (isa<ConstantExpr>(an_expr)) {
    // Special case to handle constant strings
    tuple<Value *, int64_t, string> ptr_expr_pack =
        generate_pointer_offset_expr(an_expr);
    Value *base_ptr = std::get<0>(ptr_expr_pack);

    if (base_ptr == nullptr) {
      string ap_expr = std::get<2>(ptr_expr_pack);
      return "\"" + ap_expr + "\"";
    }
  }

  if (expr_str.empty()) {
    bool is_negative_val = false;

    if (ConstantInt *const_int_val = dyn_cast<ConstantInt>(an_expr)) {
      is_negative_val = const_int_val->isNegative();
    } else if (ConstantFP *const_fp_val = dyn_cast<ConstantFP>(an_expr)) {
      is_negative_val = const_fp_val->isNegative();
    }

    if (is_negative_val)
      expr_str += "(";

    expr_str += DIA->getVariableName(an_expr);

    if (is_negative_val)
      expr_str += ")";
  }

  return expr_str;
}

/*
 * Recovering pointer expression along with GEP resolution.
 * \p ptr can only be GEP, bitcast, and load instructions.
 */
string IRExpressionRenderer::restore_access_path_expr(
    Value *ptr, bool is_deref_expr,
    unordered_map<Value *, Value *> *phi_choice) {
  if (!ptr)
    return "nullptr";

  if (isPseudoArgument(ptr)) {
    return "PseudoArg";
  }

  enum APSectionType {
    APCast,         // A type cast such as (int)p
    APFieldExpr,    // A field access expr p->f1.f2
    APArrayExpr,    // An array access expr such as p[3]
    APPrimitivePtr, // Take value from primitive type pointer such as *p
    APConstString,  // A constant string such as "hello"
    APOthers
  };

  /*
   * The fully access path expression consists of several access path sections,
   * where each section is described by a 3-tuple:
   * 1. Type of the AP section;
   * 2. Section dereferenceable flag;
   * 3. Section inbound string based descriptor;
   */
  vector<tuple<APSectionType, bool, string>> ap_sections;
  bool is_straight_casts = true;

  while (!DIA->hasVariableDebugName(ptr)) {
    tuple<Value *, int64_t, string> ptr_expr_pack =
        generate_pointer_offset_expr(ptr, phi_choice);
    Value *base_ptr = std::get<0>(ptr_expr_pack);

    if (base_ptr == nullptr) {
      // Special case to handle constants such as constant strings
      string ap_expr = std::get<2>(ptr_expr_pack);
      return "\"" + ap_expr + "\"";
    }

    LLVM_DEBUG(dbgs() << "ptr = " << DIA->getIRString(ptr)
                      << ", base_ptr = " << DIA->getIRString(base_ptr) << "\n");

    if (base_ptr != ptr) {
      int64_t pointer_offset = std::get<1>(ptr_expr_pack);
      string ap_expr = std::get<2>(ptr_expr_pack);

      APSectionType ap_type = APOthers;

      // Must be a pointer
      Type *base_type = base_ptr->getType();
      if (isa<PointerType>(base_type))
        base_type = base_type->getPointerElementType();

      if (isa<StructType>(base_type)) {
        ap_type = APFieldExpr;
      } else {
        if (pointer_offset == 0 && ap_expr[0] != '[' &&
            !isa<ArrayType>(base_type) && !isa<VectorType>(base_type) &&
            !isa<Constant>(base_ptr)) {
          // Special printout for primitive type pointer dereference
          ap_type = APPrimitivePtr;
          LLVM_DEBUG(dbgs() << "~~~~Found primitive type deref: "
                            << ", section AP = " << ap_expr << "\n");
        } else {
          ap_type = APArrayExpr;
        }
      }

      assert(ap_type != APOthers && "AP section must have a type.");
      ap_sections.push_back(
          make_tuple(ap_type, is_deref_expr, std::move(ap_expr)));
      is_straight_casts = false;
    }

    // Check if the base_ptr is a cast expression
    if (Instruction *base_ptr_inst = dyn_cast<Instruction>(base_ptr)) {
      if (base_ptr_inst->isCast()) {
        Value *cast_from = base_ptr_inst->getOperand(0);

        if (cast_from->getType()->isPointerTy() &&
            !DIA->hasVariableDebugName(base_ptr)) {
          if (!is_straight_casts) {
            // Don't print the leading type casts
            // Most likely they are added by LLVM

            string type_name;
            raw_string_ostream oss(type_name);
            base_ptr_inst->getType()->print(oss);

            type_name = oss.str();

            // string::size_type type_prefix_pos =
            //         type_name.find_first_of("%struct.");
            // int prefix_len = sizeof("%struct.") - 1;

            // if (type_prefix_pos == string::npos) {
            //     type_prefix_pos = type_name.find_first_of("%class.");
            //     prefix_len = sizeof("%class.") - 1;
            // }

            // if (type_prefix_pos != string::npos)
            //     type_name = type_name.substr(type_prefix_pos + prefix_len);

            ap_sections.push_back(
                make_tuple(APCast, false, std::move(type_name)));
          }

          ptr = cast_from;
          continue;
        }
      }
    }

    if (LoadInst *load_inst = dyn_cast<LoadInst>(base_ptr)) {
      if (!DIA->hasVariableDebugName(base_ptr)) {
        // Only follow the load chain
        ptr = load_inst->getPointerOperand();
        is_deref_expr = true;
        continue;
      }
    }

    // Other cases
    // Continue only when base_ptr is another GEP
    if (ptr == base_ptr)
      break;

    ptr = base_ptr;
    if (!isa<GEPOperator>(ptr))
      break;
  }

  // Synthesize the access path
  string full_ap = "";

  if (!DIA->hasVariableDebugName(ptr)) {
    if (isa<SelectInst>(ptr) || isa<BinaryOperator>(ptr) || isa<CmpInst>(ptr)) {
      // Expressions for such instructions are complex and we add a "()" to make
      // it easier to read
      full_ap = "(" + restore_expr(ptr) + ")";
    } else if (isa<CallInst>(ptr) || isa<InvokeInst>(ptr)) {
      full_ap = restore_expr(ptr);
    } else {
      full_ap = DIA->getVariableName(ptr);
    }
  } else {
    full_ap = DIA->getVariableName(ptr);
  }

  if (!is_straight_casts) {
    // We do not construct an expression that consists of casts only.
    // Because compilers always add lots of casts that are not appeared in
    // source code.
    bool base_is_pointer = true;

    if (isa<GlobalVariable>(ptr) || isa<AllocaInst>(ptr)) {
      base_is_pointer = false;
      Type *ptr_type = ptr->getType();
      if (PointerType *base_type = dyn_cast<PointerType>(ptr_type)) {
        if (base_type->getPointerElementType()->isPointerTy())
          base_is_pointer = true;
      }
    }

    int n_sections = static_cast<int>(ap_sections.size());

    for (int i = n_sections - 1; i > -1; --i, base_is_pointer = true) {
      auto &ap_tuple = ap_sections[i];
      APSectionType ap_type = std::get<0>(ap_tuple);
      bool ap_deref = std::get<1>(ap_tuple);
      string ap_expr = std::get<2>(ap_tuple);

      if (ap_type == APCast) {
        string cast_expr = "((";
        cast_expr += ap_expr;
        cast_expr += ")";
        cast_expr += full_ap;
        cast_expr += ")";
        full_ap = std::move(cast_expr);
      } else {
        if (ap_type == APFieldExpr) {
          if (ap_expr[0] != '[')
            full_ap += (base_is_pointer ? "->" : ".");
          full_ap += ap_expr;
        } else if (ap_type == APArrayExpr) {
          full_ap += ap_expr;
        } else if (ap_type == APPrimitivePtr) {
          if (base_is_pointer)
            address_to_value_expr(full_ap);
        }

        if (!ap_deref) {
          value_to_address_expr(full_ap);
        }
      }
    }
  }

  return full_ap;
}

string
IRExpressionRenderer::restore_access_path_expr(const gvfg::AccessPath &ap,
                                               string base_ptr_name) {
  Value *base_ptr = ap.get_base_ptr();
  if (base_ptr == nullptr) {
    return "Special_source";
  }

  Type *cur_type = base_ptr->getType();

  string result;

  if (base_ptr_name.empty()) {
    result = restore_value_expr(base_ptr);
  } else {
    result = base_ptr_name;
  }

  int ap_depth = ap.getDepth();
  for (int idx = 0; idx < ap_depth; idx++) {
    int64_t offset = ap.getOffset(idx);

    if (cur_type->isPointerTy()) {
      // Change pointer types to struct type for finding the struct field
      // accessed by "->"
      PointerType *cur_pointer_type = dyn_cast<PointerType>(cur_type);
      cur_type = cur_pointer_type->getPointerElementType();
    }

    int64_t type_size = static_cast<int64_t>(TSDL->getTypeSizeInBits(cur_type));
    if (type_size <= 0)
      type_size = 1;

    int64_t array_idx = offset / type_size;
    offset = offset % type_size;
    if (offset < 0) {
      offset += type_size;
      array_idx -= 1;
    }

    if (isa<GlobalValue>(base_ptr) && idx == 0) {
      // Global variable is always a pointer in llvm IR
      // and global->f in llvm IR is represented as &global -> 0 -> f
      // We need to skip the "->0" step
      value_to_address_expr(result);
    }

    if (array_idx != 0) {
      result.append(format_str("["
                               "%" PRId64 "]",
                               array_idx));
    }

    if (StructType *cur_struct_type = dyn_cast<StructType>(cur_type)) {
      result.append(array_idx == 0 ? "->" : ".");
      result.append(DIA->getFieldName(cur_struct_type, offset));
      cur_type = get_field_type(cur_type, offset);
    } else if (offset == 0) {
      // a->0 => *(a)
      if (array_idx == 0)
        address_to_value_expr(result);
    } else {
      result.append("+");
      result.append(format_str("%d", offset / 8));
      address_to_value_expr(result);
    }
  }

  return result;
}

// Return the non-aggregate type of field given a type and an offset
Type *IRExpressionRenderer::get_field_type(Type *ty, int64_t offset) {
  if ((!ty->isAggregateType()) || ty->isPointerTy()) {
    // ty is non-aggregate
    LLVM_DEBUG(
        uint64_t ty_size = TSDL->getTypeSizeInBits(ty);
        if (offset % ty_size != 0) { dbgs() << "Unaligned data accessed\n"; });
    if (!ty->isFirstClassType()) {
      // Non-first class type are not handled
      LLVM_DEBUG(dbgs() << "Non-first class type used, but not handled\n";);
      return Type::getInt64Ty(ty->getContext());
    }
    return ty;
  }

  Type *aggregate_type = dyn_cast<Type>(ty);
  uint64_t size = TSDL->getTypeSizeInBits(aggregate_type);

  if (aggregate_type->isArrayTy()) {
    Type *result_type = aggregate_type->getArrayElementType();
    int64_t result_type_size = (int64_t)TSDL->getTypeSizeInBits(result_type);
    if (result_type_size) {
      offset = offset % result_type_size;
      result_type = get_field_type(result_type, offset);
    }
    return result_type;
  }

  // handle the array/vectors
  if (size == 0) {
    // void type loaded, where there should be no values, but w.r.t. memory, it
    // is possible to load something
    return Type::getInt64Ty(ty->getContext());
  }

  int64_t type_offset = static_cast<int64_t>(offset % size);

  unsigned num_fields = aggregate_type->getNumContainedTypes();
  for (unsigned i = 0; i < num_fields; i++) {
    Type *field_type = aggregate_type->getContainedType(i);
    int64_t field_size = (int64_t)TSDL->getTypeSizeInBits(field_type);
    if (type_offset >= field_size) {
      type_offset = type_offset - field_size;
    } else {
      return get_field_type(field_type, type_offset);
    }
  }

  // Actual type size is larger than that analysed by PackedTypeLayout
  LLVM_DEBUG(dbgs() << "Actual type size is larger than that analyzed by "
                       "PackedTypeLayout for type ( "
                    << *aggregate_type << " ) with size " << size
                    << " on offset " << offset << "\n";);

  return Type::getInt64Ty(ty->getContext());
}

string IRExpressionRenderer::restore_value_expr(
    Value *val, unordered_map<Value *, Value *> *phi_choice) {
  if (!val)
    return "";

  // Ordered by priority
  if (isPseudoArgument(val)) {
    // TODO: Lookup the access path for this pseudo argument
    return "PseudoArg";
  } else if (DIA->hasVariableDebugName(val)) {
    return DIA->getVariableName(val);
  } else if (isa<Constant>(val) && !isa<ConstantExpr>(val)) {
    return valueToString(val);
  } else if (LoadInst *load = dyn_cast<LoadInst>(val)) {
    return restore_access_path_expr(load->getPointerOperand(), true,
                                    phi_choice);
  } else if (isa<GEPOperator>(val) || isa<AllocaInst>(val)) {
    return restore_access_path_expr(val, false, phi_choice);
  } else if (StoreInst *store = dyn_cast<StoreInst>(val)) {
    return restore_access_path_expr(store->getPointerOperand(), true,
                                    phi_choice);
  }

  // Default choice
  return restore_expr(val);
}

string IRExpressionRenderer::restore_left_value_expr(
    Value *val, unordered_map<Value *, Value *> *phi_choice) {
  bool is_addr_expr = false;
  if (isa<AllocaInst>(val) || isa<GEPOperator>(val)) {
    is_addr_expr = true;
  }

  return restore_access_path_expr(val, is_addr_expr, phi_choice);
  ;
}

string IRExpressionRenderer::restore_right_value_expr(
    Value *val, unordered_map<Value *, Value *> *phi_choice) {
  return restore_access_path_expr(val, true, phi_choice);
  ;
}

static bool is_single_var_name(string &target, int start_idx, int end_idx) {
  for (int i = start_idx; i < end_idx; i++) {
    if (target[i] == '+' || target[i] == '-') {
      return false;
    }
  }
  return true;
}

void IRExpressionRenderer::value_to_address_expr(string &value_string) {
  remove_redundant_brackets(value_string);
  int length = static_cast<int>(value_string.size());

  if (length == 0) {
    return;
  }

  if (value_string[0] == '*') {
    if (length == 1) {
      // "*" => ""
      value_string = "";
    } else if (length >= 3 && value_string[1] == '(' &&
               value_string[length - 1] == ')') {
      // *(blabla...) => blabla...
      value_string = value_string.substr(2, length - 3);
    } else {
      // &blabla...
      if (is_single_var_name(value_string, 1, length)) {
        // *a => a
        value_string = value_string.substr(1, length - 1);
      } else {
        // *a + b => &(*a +b)
        value_string = "&(" + value_string + ")";
      }
    }
  } else {
    if (length >= 3 && value_string[length - 1] == ']' &&
        value_string[length - 2] == '*' && value_string[length - 3] == '[') {
      // a[*] => a
      value_string = value_string.substr(0, length - 3);
    } else {
      // a => &(a)
      value_string = "&(" + value_string + ")";
    }
  }
}

void IRExpressionRenderer::address_to_value_expr(string &address_string) {
  remove_redundant_brackets(address_string);

  int length = static_cast<int>(address_string.size());

  if (length == 0) {
    return;
  }

  if (address_string[0] == '&') {
    if (length == 1) {
      // "&" => ""
      address_string = "";
    } else if (length >= 3 && address_string[1] == '(' &&
               address_string[length - 1] == ')') {
      // &(blabla...) => blabla...
      address_string = address_string.substr(2, length - 3);
    } else {
      // &blabla...
      if (is_single_var_name(address_string, 1, length)) {
        // &a => a
        address_string = address_string.substr(1, length - 1);
      } else {
        // &a + b => *(&a +b)
        address_string = "*(" + address_string + ")";
      }
    }
  } else {
    address_string = "*(" + address_string + ")";
  }
}

void IRExpressionRenderer::remove_redundant_brackets(string &to_refactor) {
  int front = 0, end = 0;
  int size = static_cast<int>(to_refactor.size());
  for (front = 0; front < size; front++) {
    if (to_refactor[front] != '(') {
      break;
    }
  }

  for (end = 0; end < size; end++) {
    if (to_refactor[size - end - 1] != ')') {
      break;
    }
  }

  int brackets_count = front < end ? front : end;

  if (brackets_count != 0)
    to_refactor = to_refactor.substr(brackets_count, size - 2 * brackets_count);
}
} // namespace ir_expression
} // namespace lotus
