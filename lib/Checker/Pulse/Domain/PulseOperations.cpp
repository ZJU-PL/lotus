#include "Checker/Pulse/Domain/PulseOperations.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseTaint.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace pulse {

std::atomic<unsigned> AbstractValueFactory::global_next_id_{1u};

//===----------------------------------------------------------------------===//
// PulseOperations
//
// Defines the operational semantics over `AbductiveDomain`:
// - `eval` maps an LLVM value/expression to an abstract address (plus history).
// - Heap reads may *abduce* missing edges/attrs into the precondition to keep
//   witness paths sound (biabduction).
// - For access paths (GEP), we model projections precisely enough to avoid
//   conflation (critical for sound incorrectness).
//===----------------------------------------------------------------------===//

namespace {
static bool isNullPointerConstantValue(const llvm::Value *v) {
  if (!v)
    return false;
  if (llvm::isa<llvm::ConstantPointerNull>(v))
    return true;
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
    if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0)))
        return CI->isZero();
    }
  }
  if (auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(v)) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I2P->getOperand(0)))
      return CI->isZero();
  }
  return false;
}

static std::optional<int64_t> getI64Constant(const llvm::Value *v) {
  if (!v)
    return std::nullopt;
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    if (CI->getBitWidth() <= 64)
      return CI->getSExtValue();
  }
  return std::nullopt;
}

static const llvm::Function *
extractFunctionFromConstant(const llvm::Constant *C) {
  if (!C)
    return nullptr;
  const llvm::Value *stripped = C->stripPointerCastsAndAliases();
  return llvm::dyn_cast_or_null<llvm::Function>(stripped);
}

static bool maybeMarkStructInitialized(AbductiveDomain &astate,
                                       AbstractValue struct_av,
                                       const llvm::StructType *ST,
                                       const llvm::DataLayout *DL,
                                       const llvm::Instruction *loc = nullptr,
                                       unsigned depth = 0) {
  if (!ST)
    return false;
  AbstractValue c_struct = astate.getCanonical(struct_av);
  if (!astate.getPostAttrs().has(c_struct, Attribute::Uninitialized)) {
    return true;
  }
  const unsigned num_elems = ST->getNumElements();
  if (num_elems == 0 || num_elems > 16)
    return false;
  const llvm::StructLayout *SL =
      (DL && ST->isSized())
          ? DL->getStructLayout(const_cast<llvm::StructType *>(ST))
          : nullptr;
  for (unsigned i = 0; i < num_elems; ++i) {
    uint64_t field_off = SL ? SL->getElementOffset(i) : 0;
    const Address *edge =
        astate.getPostHeap().findEdge(c_struct, Access::field(i, field_off));
    if (!edge) {
      return false;
    }
    AbstractValue fc = astate.getCanonical(edge->addr);
    if (astate.getPostAttrs().has(fc, Attribute::Uninitialized) &&
        !astate.hasWrittenTo(fc)) {
      if (depth < 4) {
        if (const auto *sub_st =
                llvm::dyn_cast<llvm::StructType>(ST->getElementType(i))) {
          if (maybeMarkStructInitialized(astate, fc, sub_st, DL, loc,
                                         depth + 1)) {
            continue;
          }
        }
      }
      return false;
    }
  }
  astate.getPostAttrs().remove(c_struct, Attribute::Uninitialized);
  astate.markWrittenTo(c_struct, loc);
  return true;
}

static const llvm::Constant *
extractConstantAtByteOffset(const llvm::Constant *C, uint64_t byte_off,
                            const llvm::DataLayout &DL, unsigned depth = 0) {
  if (!C || depth > 8)
    return nullptr;
  if (byte_off == 0 && C->getType()->isPointerTy()) {
    return C;
  }
  llvm::Type *Ty = C->getType();
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(Ty)) {
    const llvm::StructLayout *SL = DL.getStructLayout(ST);
    unsigned idx = SL->getElementContainingOffset(byte_off);
    if (idx < ST->getNumElements()) {
      uint64_t elem_off = SL->getElementOffset(idx);
      if (const llvm::Constant *elem = C->getAggregateElement(idx)) {
        return extractConstantAtByteOffset(elem, byte_off - elem_off, DL,
                                           depth + 1);
      }
    }
    return nullptr;
  }
  if (auto *AT = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
    uint64_t elem_size = DL.getTypeAllocSize(AT->getElementType());
    if (elem_size == 0)
      return nullptr;
    uint64_t idx = byte_off / elem_size;
    if (idx < AT->getNumElements()) {
      if (const llvm::Constant *elem =
              C->getAggregateElement(static_cast<unsigned>(idx))) {
        return extractConstantAtByteOffset(elem, byte_off - idx * elem_size, DL,
                                           depth + 1);
      }
    }
    return nullptr;
  }
  return nullptr;
}

static bool parseVtableDynamicType(const std::string &dyn_ty,
                                   std::string &out_gv_name,
                                   uint64_t &out_base_off) {
  static constexpr const char kPrefix[] = "__vtable__:";
  constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
  if (dyn_ty.compare(0, kPrefixLen, kPrefix) != 0)
    return false;
  size_t sep = dyn_ty.rfind(':');
  if (sep == std::string::npos || sep <= kPrefixLen)
    return false;
  out_gv_name = dyn_ty.substr(kPrefixLen, sep - kPrefixLen);
  try {
    out_base_off = std::stoull(dyn_ty.substr(sep + 1));
  } catch (...) {
    return false;
  }
  return true;
}
} // namespace

//===----------------------------------------------------------------------===//
// AbstractValueFactory Implementation
//===----------------------------------------------------------------------===//

AbstractValue AbstractValueFactory::getOrCreate(const llvm::Value *v) {
  auto it = value_map_.find(v);
  if (it != value_map_.end()) {
    return it->second;
  }

  const bool is_ptr = v && v->getType() && v->getType()->isPointerTy();
  const llvm::Value *stripped = is_ptr ? v->stripPointerCasts() : nullptr;

  if (mustAliasFn_ && is_ptr) {
    // O(1) syntactic must-alias fast path: pointer casts and zero-offset GEPs
    // sharing the same underlying value are guaranteed must-alias in LLVM IR
    // without triggering expensive interprocedural UnderApprox::EquivDB runs.
    if (stripped) {
      auto sit = stripped_ptr_map_.find(stripped);
      if (sit != stripped_ptr_map_.end()) {
        AbstractValue av = sit->second.second;
        value_map_[v] = av;
        return av;
      }
    }
    // Identical GEP expressions in the same function must-alias in O(1).
    if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
      const llvm::Function *parent_fn = GEP->getFunction();
      auto &fn_ptrs = ptr_values_by_fn_[parent_fn];
      constexpr size_t kMaxGepScan = 32;
      const size_t start =
          fn_ptrs.size() > kMaxGepScan ? fn_ptrs.size() - kMaxGepScan : 0;
      for (size_t i = fn_ptrs.size(); i > start; --i) {
        if (const auto *other =
                llvm::dyn_cast<llvm::GetElementPtrInst>(fn_ptrs[i - 1].first)) {
          if (GEP->isIdenticalTo(other)) {
            AbstractValue av = fn_ptrs[i - 1].second;
            value_map_[v] = av;
            return av;
          }
        }
      }
    }
  }

  unsigned id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  // 0 is reserved for the default-constructed AbstractValue.
  if (id == 0u) {
    id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  }
  AbstractValue av(v, id);
  value_map_[v] = av;
  if (is_ptr) {
    if (stripped) {
      stripped_ptr_map_.emplace(stripped, std::make_pair(v, av));
    }
    if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(v)) {
      ptr_values_by_fn_[GEP->getFunction()].emplace_back(v, av);
    }
  }
  return av;
}

AbstractValue AbstractValueFactory::get(const llvm::Value *v) const {
  auto it = value_map_.find(v);
  assert(it != value_map_.end() && "Value not found in factory");
  return it->second;
}

bool AbstractValueFactory::has(const llvm::Value *v) const {
  return value_map_.find(v) != value_map_.end();
}

AbstractValue AbstractValueFactory::createFresh(const llvm::Value *hint) {
  // Create a fresh abstract value (for unknown/allocated memory)
  // In a full implementation, we'd track this properly
  unsigned id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  if (id == 0u) {
    id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  }
  AbstractValue av(hint, id);
  if (hint) {
    value_map_[hint] = av;
  }
  return av;
}

//===----------------------------------------------------------------------===//
// PulseOperations Implementation
//===----------------------------------------------------------------------===//

bool PulseOperations::isNullConstantSource(const Address &addr) {
  // Check if the original LLVM value is a null constant
  if (const llvm::Value *v = addr.addr.getValue()) {
    if (isNullPointerConstantValue(v))
      return true;
  }

  // Check ValueHistory for Store events where a null constant was stored
  for (const auto &event : addr.history.getEvents()) {
    if (event.kind == ValueHistory::EventKind::Store && event.location) {
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(event.location)) {
        const llvm::Value *stored_value = SI->getValueOperand();
        if (isNullPointerConstantValue(stored_value)) {
          return true;
        }
        // Also check if the stored value came from a CallInst returning null
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(stored_value)) {
          if (CI->getType()->isPointerTy()) {
            // Check if the CallInst itself is a null constant (shouldn't
            // happen, but be safe)
            if (llvm::isa<llvm::ConstantPointerNull>(CI)) {
              return true;
            }
            // Check if the called function returns null constant
            if (auto *F = CI->getCalledFunction()) {
              // Check if the function returns null constant
              for (const auto &BB : *F) {
                if (auto *RI =
                        llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
                  if (RI->getNumOperands() > 0) {
                    const llvm::Value *ret_val = RI->getReturnValue();
                    if (ret_val && ret_val->getType()->isPointerTy()) {
                      if (isNullPointerConstantValue(ret_val)) {
                        return true;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    // Check for FunctionCall events that returned null
    if (event.kind == ValueHistory::EventKind::FunctionCall && event.location) {
      if (auto *CI = llvm::dyn_cast<llvm::CallInst>(event.location)) {
        if (CI->getType()->isPointerTy()) {
          if (auto *F = CI->getCalledFunction()) {
            // Check if the function returns null constant
            for (const auto &BB : *F) {
              if (auto *RI =
                      llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
                if (RI->getNumOperands() > 0) {
                  const llvm::Value *ret_val = RI->getReturnValue();
                  if (isNullPointerConstantValue(ret_val)) {
                    return true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  return false;
}

std::optional<Address> PulseOperations::eval(AbductiveDomain &astate,
                                              const llvm::Value *exp,
                                              const llvm::Instruction *loc,
                                              const llvm::BasicBlock *pred) {
  // Global storage (GlobalVariable/Function/GlobalAlias): treat as stable
  // memory root. Mark with Attribute::Global so we can prove stack escapes like
  // `store %stack_ptr, @global` without guessing.
  if (exp && exp->getType()->isPointerTy() &&
      llvm::isa<llvm::GlobalValue>(exp)) {
    AbstractValue gv_av = factory_->getOrCreate(exp);
    astate.getPostAttrs().add(gv_av, Attribute::Global);
    astate.registerPreAddress(gv_av);
    if (exp->hasName()) {
      astate.setDecompilerName(gv_av, exp->getName().str());
      if (llvm::isa<llvm::Function>(exp)) {
        // Track concrete function pointer target as dynamic type for indirect
        // call resolution & DynamicTypeNeeded specialization.
        astate.setDynamicType(gv_av, exp->getName().str());
      }
    }
    // Materialize static initializers for constant global variables (strings,
    // integers, pointers, null pointers) if not already initialized in heap.
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(exp)) {
      if (GV->hasInitializer() &&
          !astate.getPostHeap().findEdge(gv_av,
                                         Access(AccessKind::Dereference))) {
        const llvm::Constant *init = GV->getInitializer();
        if (const auto *CDS = llvm::dyn_cast<llvm::ConstantDataSequential>(init)) {
          if (CDS->isCString()) {
            (void)astate.getPathFormula().addStringConstant(
                gv_av, CDS->getAsCString().str());
          }
        } else if (GV->isConstant()) {
          if (const auto *CI = llvm::dyn_cast<llvm::ConstantInt>(init)) {
            if (CI->getBitWidth() <= 64) {
              AbstractValue iv = factory_->getOrCreate(CI);
              astate.getPathFormula().addIntegerConstraint(iv);
              int64_t val = CI->getSExtValue();
              (void)astate.getPathFormula().addBounds(iv, val, val);
              astate.getPostHeap().addEdge(gv_av,
                                           Access(AccessKind::Dereference),
                                           Address(iv));
            }
          } else if (llvm::isa<llvm::ConstantPointerNull>(init)) {
            AbstractValue nv = factory_->createFresh(init);
            astate.getPostAttrs().add(nv, Attribute::Null);
            (void)astate.getPathFormula().addNull(nv);
            astate.getPostHeap().addEdge(gv_av, Access(AccessKind::Dereference),
                                         Address(nv));
          } else if (const auto *target_gv = llvm::dyn_cast_or_null<llvm::GlobalValue>(
                         init->stripPointerCastsAndAliases())) {
            if (auto target_opt = eval(astate, target_gv, loc, pred)) {
              astate.getPostHeap().addEdge(gv_av,
                                           Access(AccessKind::Dereference),
                                           *target_opt);
            }
          }
        }
      }
    }
    return std::optional<Address>(Address(gv_av));
  }

  // Pointer-typed null constants.
  if (isNullPointerConstantValue(exp)) {
    AbstractValue null_addr = factory_->createFresh(exp);
    Address addr(null_addr);
    astate.getPostAttrs().add(null_addr, Attribute::Null);
    astate.getPathFormula().addNull(null_addr);
    return std::optional<Address>(addr);
  }

  // Integer constants: treat as integers, not null pointers.
  if (auto c = getI64Constant(exp)) {
    AbstractValue av = factory_->getOrCreate(exp);
    astate.getPathFormula().addIntegerConstraint(av);
    if (exp->getType()->isIntegerTy()) {
      astate.getPathFormula().setBitWidth(av,
                                          exp->getType()->getIntegerBitWidth());
    }
    (void)astate.getPathFormula().addBounds(av, *c, *c);
    return std::optional<Address>(Address(av));
  }

  // Floating-point constants: safely convert any LLVM FP semantics (half,
  // float, double, x86_fp80, fp128) to IEEEdouble before recording.
  if (auto *CFP = llvm::dyn_cast_or_null<llvm::ConstantFP>(exp)) {
    AbstractValue av = factory_->getOrCreate(exp);
    llvm::APFloat apf = CFP->getValueAPF();
    bool loses_info = false;
    apf.convert(llvm::APFloat::IEEEdouble(),
                llvm::APFloat::rmNearestTiesToEven, &loses_info);
    double val = apf.convertToDouble();
    if (std::isfinite(val)) {
      (void)astate.getPathFormula().addFloatConstant(av, val);
    }
    return std::optional<Address>(Address(av));
  }

  if (auto *addr = astate.getPostStack().find(exp)) {
    // Canonicalize the address
    AbstractValue canon_addr = astate.getCanonical(addr->addr);
    Address result(canon_addr);
    result.history = addr->history;

    // IMPORTANT: When loading a pointer value, preserve its attributes
    // (like Invalid, Null, etc.) so they can be checked when the pointer is
    // used The attributes are already on canon_addr, so they'll be checked in
    // readDeref

    return std::optional<Address>(result);
  }

  // Handle bitcast, inttoptr, and ptrtoint instructions preserving identity.
  if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(exp)) {
    auto src_opt = eval(astate, BC->getOperand(0), loc, pred);
    if (src_opt) {
      AbstractValue src_canon = astate.getCanonical(src_opt->addr);
      Address result(src_canon);
      result.history = src_opt->history;
      return std::optional<Address>(result);
    }
  }
  if (auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(exp)) {
    auto src_opt = eval(astate, I2P->getOperand(0), loc, pred);
    if (src_opt) {
      AbstractValue src_canon = astate.getCanonical(src_opt->addr);
      Address result(src_canon);
      result.history = src_opt->history;
      return std::optional<Address>(result);
    }
  }
  if (auto *P2I = llvm::dyn_cast<llvm::PtrToIntInst>(exp)) {
    auto src_opt = eval(astate, P2I->getOperand(0), loc, pred);
    if (src_opt) {
      AbstractValue src_canon = astate.getCanonical(src_opt->addr);
      Address result(src_canon);
      result.history = src_opt->history;
      return std::optional<Address>(result);
    }
  }

  // Handle bitcast operator (constant expressions)
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(exp)) {
    if (CE->getOpcode() == llvm::Instruction::BitCast) {
      auto src_opt = eval(astate, CE->getOperand(0), loc, pred);
      if (src_opt) {
        AbstractValue src_canon = astate.getCanonical(src_opt->addr);
        Address result(src_canon);
        result.history = src_opt->history;
        // Attributes are preserved through canonical value
        return std::optional<Address>(result);
      }
    }
  }

  // GEP (GetElementPtr) materializes an access path into an aggregate/array.
  //
  // Sound incorrectness principle: never silently conflate distinct
  // projections.
  // - Struct indices must be constant (LLVM requirement). If they are not,
  //   something is off; we conservatively bail out instead of inventing a
  //   projection that could hide bugs.
  // - For array/pointer indices, record the index as an integer in the path
  //   formula (and keep exact bounds for constants). This supports "proved"
  //   checks later without committing to may-alias reasoning.
  if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(exp)) {
    auto base_opt = eval(astate, GEP->getPointerOperand(), loc, pred);
    if (!base_opt)
      return std::nullopt;

    // Check if base pointer is null or invalid before indexing
    AbstractValue base_canon = astate.getCanonical(base_opt->addr);
    const bool base_is_stack =
        astate.getPostAttrs().has(base_canon, Attribute::Stack);
    const bool base_is_global =
        astate.getPostAttrs().has(base_canon, Attribute::Global);
    // For NPD checker, only check Null attribute (set only by null constants)
    if (astate.getPostAttrs().has(base_canon, Attribute::Null) &&
        !astate.getPathFormula().isNonNull(base_canon)) {
      // Base is null (from null constant) - return None to signal error (will
      // be caught by caller)
      return std::nullopt;
    }
    if (astate.getPostAttrs().has(base_canon, Attribute::Invalid)) {
      // Base is invalid (use-after-free) - return None to signal error
      return std::nullopt;
    }

    Address base = *base_opt;
    AbstractValue cur = base.addr;
    // Reuse base_canon from above (already computed for null/invalid checks)
    bool base_is_uninitialized =
        astate.getPostAttrs().has(base_canon, Attribute::Uninitialized);

    // Best-effort: DataLayout lets us compute element stride for array
    // indexing. Stride is stored into the access key to avoid collapsing
    // different element types onto the same access path.
    const llvm::DataLayout *DL = (loc && loc->getModule())
                                     ? &loc->getModule()->getDataLayout()
                                     : nullptr;

    // Bounds tracking: use allocation root size + signed offset when known so
    // one-past-the-end pointers (`end = buff + N`) stepped back (`end - 1`)
    // return cleanly in-bounds while dereferencing `end` remains OOB.
    AbstractValue alloc_root = astate.getAllocationRoot(base_canon);
    std::optional<uint64_t> root_size_opt = astate.getAllocationSize(alloc_root);
    std::optional<int64_t> base_off_opt = astate.getAllocationOffset(base_canon);
    if (!base_off_opt && base_canon == alloc_root && root_size_opt) {
      base_off_opt = 0;
    }
    std::optional<uint64_t> alloc_size_opt =
        astate.getAllocationSize(base_canon);
    bool offset_known = true;
    int64_t total_offset = 0;
    bool oob_proven = false;

    unsigned op_index = 1;
    for (auto GTI = llvm::gep_type_begin(GEP), E = llvm::gep_type_end(GEP);
         GTI != E; ++GTI, ++op_index) {
      llvm::Value *idx = const_cast<llvm::Value *>(GEP->getOperand(op_index));
      Access acc;

      if (llvm::StructType *ST = GTI.getStructTypeOrNull()) {
        auto *C = llvm::dyn_cast<llvm::ConstantInt>(idx);
        if (!C) {
          // Struct indexing must be constant; bail out rather than conflate.
          return std::nullopt;
        }
        uint64_t field = C->getZExtValue();
        if (field >= ST->getNumElements()) {
          // Invalid field access. Be conservative: treat as unknown.
          return std::nullopt;
        }
        uint64_t field_off = 0;
        if (DL && ST->isSized()) {
          field_off = DL->getStructLayout(ST)->getElementOffset(field);
        }
        acc = Access::field(static_cast<unsigned>(field), field_off);

        if (offset_known && DL && ST->isSized()) {
          if (field_off >
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            offset_known = false;
          } else {
            total_offset += static_cast<int64_t>(field_off);
          }
        } else {
          offset_known = false;
        }
      } else {
        auto const_idx_opt = getI64Constant(idx);
        AbstractValue idxAv;
        if (!const_idx_opt) {
          idxAv = factory_->getOrCreate(idx);
          astate.getPathFormula().addIntegerConstraint(idxAv);
          const_idx_opt = astate.getPathFormula().asConstantInt(idxAv);
        }

        // First GEP index 0 (`gep T, T* %p, 0, ...`) is pointer identity
        // (`%p + 0 == %p`). Skipping a synthetic ArrayIndex(0) hop unifies
        // struct GEPs with InsertValue/ExtractValue, field-sensitive taint,
        // bitcasts, and interprocedural constructor summaries.
        if (op_index == 1 && const_idx_opt && *const_idx_opt == 0) {
          continue;
        }

        uint64_t stride_bytes = 0;
        llvm::Type *indexed = GTI.getIndexedType();
        if (DL && indexed && indexed->isSized()) {
          stride_bytes = DL->getTypeAllocSize(indexed);
        }
        if (const_idx_opt) {
          acc = Access::constArrayIndex(*const_idx_opt, stride_bytes);
        } else {
          acc = Access::arrayIndex(astate.getCanonical(idxAv), stride_bytes);
        }

        if (offset_known && stride_bytes > 0) {
          if (const_idx_opt) {
            __int128 off = static_cast<__int128>(*const_idx_opt) *
                           static_cast<__int128>(stride_bytes);
            if (off > std::numeric_limits<int64_t>::max() ||
                off < std::numeric_limits<int64_t>::min()) {
              offset_known = false;
            } else {
              __int128 sum = static_cast<__int128>(total_offset) + off;
              if (sum > std::numeric_limits<int64_t>::max() ||
                  sum < std::numeric_limits<int64_t>::min()) {
                offset_known = false;
              } else {
                total_offset = static_cast<int64_t>(sum);
              }
            }
          } else {
            // Never assume a symbolic index is in bounds merely because the
            // allocation size is known. Use existing path constraints to prove
            // either safety or an OOB witness; otherwise keep the offset
            // unknown.
            if (alloc_size_opt && offset_known) {
              uint64_t alloc_size = *alloc_size_opt;
              if (total_offset >= 0 &&
                  static_cast<uint64_t>(total_offset) < alloc_size) {
                uint64_t remaining =
                    alloc_size - static_cast<uint64_t>(total_offset);
                if (remaining > 0) {
                  uint64_t max_index = (remaining - 1) / stride_bytes;
                  auto lower = astate.getPathFormula().getLowerBound(idxAv);
                  auto upper = astate.getPathFormula().getUpperBound(idxAv);
                  if ((lower && *lower > static_cast<int64_t>(max_index)) ||
                      (upper && *upper < 0)) {
                    oob_proven = true;
                  }
                }
              }
            }
            offset_known = false;
          }
        } else {
          offset_known = false;
        }
      }

      if (auto *target = astate.getPostHeap().findEdge(cur, acc)) {
        cur = target->addr;
        continue;
      }
      AbstractValue fresh = factory_->createFresh(GEP);
      Address targetAddr(fresh);
      targetAddr.history = base.history;
      if (loc)
        targetAddr.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                                    loc->getFunction());
      astate.getPostHeap().addEdge(cur, acc, targetAddr);
      astate.abduceToPre(cur, acc, targetAddr);
      astate.abduceAttrToPre(cur, Attribute::Allocated);
      if (base_is_stack) {
        astate.getPostAttrs().add(fresh, Attribute::Stack);
      }
      if (base_is_global) {
        astate.getPostAttrs().add(fresh, Attribute::Global);
      }

      // Field-level initialization tracking: if base struct is uninitialized,
      // mark the field as uninitialized too
      if (base_is_uninitialized) {
        AbstractValue fresh_canon = astate.getCanonical(fresh);
        astate.getPostAttrs().add(fresh_canon, Attribute::Uninitialized);
      }

      cur = fresh;
    }

    // Pure zero-offset pointer identity GEP (`gep T, T* %p, 0`) returns base.
    AbstractValue cur_canon = astate.getCanonical(cur);
    if (cur_canon == base_canon) {
      Address gepAddr(base_canon);
      gepAddr.history = base.history;
      return std::optional<Address>(gepAddr);
    }

    // Base pointer must be valid if it comes from the precondition. Pointer
    // arithmetic alone allows one-past-the-end pointers (e.g. `end - 1`).
    if (astate.isInPre(base_canon) && !astate.isAllocated(base_canon)) {
      Trace t = Trace::fromValueHistory(base.history);
      if (loc) {
        t.addEvent(loc, "Pointer arithmetic / field access");
      }
      astate.abduceMustBeValid(base_canon, std::move(t), std::nullopt,
                               /*allow_one_past_end=*/true);
    }
    // Resolve constant global vtable / dispatch-table pointers (e.g., C++
    // @_ZTV... vtables or static function-pointer arrays/structs) so virtual
    // dispatch `vtable[slot](this, ...)` resolves directly AND carries a
    // `__vtable__:<gv>:<offset>` dynamic type for interprocedural specialization.
    if (offset_known && total_offset >= 0 && DL) {
      const llvm::GlobalVariable *vtable_gv = nullptr;
      uint64_t abs_byte_off = static_cast<uint64_t>(total_offset);
      if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(
              GEP->getPointerOperand()->stripPointerCasts())) {
        if (GV->isConstant() && GV->hasInitializer()) {
          vtable_gv = GV;
        }
      } else if (auto dyn_ty = astate.getDynamicType(base_canon)) {
        std::string gv_name;
        uint64_t base_off = 0;
        if (parseVtableDynamicType(*dyn_ty, gv_name, base_off) && loc &&
            loc->getModule()) {
          if (const auto *GV = loc->getModule()->getNamedGlobal(gv_name)) {
            if (GV->isConstant() && GV->hasInitializer()) {
              vtable_gv = GV;
              abs_byte_off = base_off + static_cast<uint64_t>(total_offset);
            }
          }
        }
      }

      if (vtable_gv) {
        AbstractValue cur_c = astate.getCanonical(cur);
        if (vtable_gv->hasName()) {
          astate.setDynamicType(cur_c, "__vtable__:" +
                                           vtable_gv->getName().str() + ":" +
                                           std::to_string(abs_byte_off));
        }
        if (const llvm::Constant *elem = extractConstantAtByteOffset(
                vtable_gv->getInitializer(), abs_byte_off, *DL)) {
          if (const llvm::Function *target_fn =
                  extractFunctionFromConstant(elem)) {
            if (auto fn_addr = eval(astate, target_fn, loc, pred)) {
              astate.getPostHeap().addEdge(
                  cur_c, Access(AccessKind::Dereference), *fn_addr);
              astate.getPostAttrs().remove(cur_c, Attribute::Uninitialized);
            }
          }
        }
      }
    }

    // Propagate known constant string suffix across constant-offset byte GEPs
    // (e.g. `gep inbounds ([6 x i8], [6 x i8]* @.str, i64 0, i64 0)`).
    if (offset_known && total_offset >= 0) {
      if (auto base_str = astate.getPathFormula().asConstantString(base_canon)) {
        size_t byte_off = static_cast<size_t>(total_offset);
        if (byte_off <= base_str->size()) {
          (void)astate.getPathFormula().addStringConstant(
              astate.getCanonical(cur), base_str->substr(byte_off));
        }
      }
    }

    // Check symbolic dynamic-size allocation OOB (e.g. `alloca i8, i64 %n` or
    // `malloc(%n)` indexed by `idx >= %n`).
    if (!alloc_size_opt && !oob_proven) {
      if (auto sym_sz = astate.getSymbolicAllocationSize(base_canon)) {
        if (GEP->getNumIndices() >= 1) {
          const llvm::Value *last_idx =
              GEP->getOperand(GEP->getNumOperands() - 1);
          if (last_idx->getType()->isIntegerTy()) {
            AbstractValue idx_av = astate.getCanonical(factory_->getOrCreate(last_idx));
            auto ge_sz = astate.getPathFormula().impliesComparison(
                idx_av, std::nullopt, ">=", *sym_sz, std::nullopt);
            auto lt_zero = astate.getPathFormula().impliesComparison(
                idx_av, std::nullopt, "<", std::nullopt, 0);
            if ((ge_sz && *ge_sz) || (lt_zero && *lt_zero)) {
              oob_proven = true;
            }
          }
        }
      }
    }

    // Structured access-path decompiler name for derived GEP cell.
    if (auto base_name = astate.getDecompilerName(base_canon)) {
      std::string derived_name = *base_name;
      unsigned idx_op = 1;
      for (auto GTI = llvm::gep_type_begin(GEP), E = llvm::gep_type_end(GEP);
           GTI != E; ++GTI, ++idx_op) {
        const llvm::Value *idx = GEP->getOperand(idx_op);
        if (GTI.getStructTypeOrNull()) {
          if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
            derived_name += ".field" + std::to_string(C->getZExtValue());
          }
        } else {
          if (auto c = getI64Constant(idx)) {
            if (idx_op != 1 || *c != 0) {
              derived_name += "[" + std::to_string(*c) + "]";
            }
          } else if (idx->hasName()) {
            derived_name += "[" + idx->getName().str() + "]";
          }
        }
      }
      if (!astate.getDecompilerName(cur).has_value()) {
        astate.setDecompilerName(cur, derived_name);
      }
    }

    // If we can prove an out-of-bounds offset (using absolute offset from the
    // allocation root when available so `*(end - 1)` returns in-bounds), mark
    // or clear OutOfBounds accordingly.
    if (root_size_opt && base_off_opt && offset_known) {
      __int128 abs_off =
          static_cast<__int128>(*base_off_opt) + static_cast<__int128>(total_offset);
      if (abs_off >= std::numeric_limits<int64_t>::min() &&
          abs_off <= std::numeric_limits<int64_t>::max()) {
        int64_t off64 = static_cast<int64_t>(abs_off);
        astate.setAllocationOffset(cur_canon, off64);
        const uint64_t root_size = *root_size_opt;
        if (off64 < 0 || static_cast<uint64_t>(off64) >= root_size) {
          oob_proven = true;
        } else {
          astate.getPostAttrs().remove(cur_canon, Attribute::OutOfBounds);
          uint64_t remaining = root_size - static_cast<uint64_t>(off64);
          if (remaining > 0) {
            astate.setAllocationSize(cur_canon, remaining);
          }
        }
      }
    } else if (alloc_size_opt && offset_known) {
      const uint64_t alloc_size = *alloc_size_opt;
      if (total_offset < 0 ||
          static_cast<uint64_t>(total_offset) >= alloc_size) {
        oob_proven = true;
      } else {
        uint64_t remaining = alloc_size - static_cast<uint64_t>(total_offset);
        if (remaining > 0) {
          astate.setAllocationSize(cur_canon, remaining);
        }
      }
    }

    if (oob_proven) {
      astate.getPostAttrs().add(cur_canon, Attribute::OutOfBounds);
    }
    astate.setAllocationRoot(cur_canon, alloc_root);
    if (base_is_stack) {
      astate.getPostAttrs().add(cur_canon, Attribute::Stack);
    }
    if (base_is_global) {
      astate.getPostAttrs().add(cur_canon, Attribute::Global);
    }
    Address gepAddr(cur_canon);
    gepAddr.history = base.history;
    if (loc)
      gepAddr.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                               loc->getFunction());
    return std::optional<Address>(gepAddr);
  }

  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(exp)) {
    if (!pred)
      return std::nullopt;
    // Check if pred is actually a predecessor of the PHI node
    bool is_predecessor = false;
    for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
      if (Phi->getIncomingBlock(i) == pred) {
        is_predecessor = true;
        break;
      }
    }
    if (!is_predecessor) {
      // The incoming edge is part of the witness. Guessing another edge would
      // combine a value from one execution with the state of another.
      return std::nullopt;
    }
    const llvm::Value *incoming = Phi->getIncomingValueForBlock(pred);
    return eval(astate, incoming, loc, nullptr);
  }

  if (llvm::isa<llvm::Instruction>(exp)) {
    AbstractValue av = factory_->getOrCreate(exp);
    return std::optional<Address>(Address(av));
  }

  AbstractValue av = factory_->getOrCreate(exp);
  return std::optional<Address>(Address(av));
}

std::pair<OperationResult, std::optional<Address>>
PulseOperations::evalDeref(AbductiveDomain &astate, Address ptr,
                           const llvm::Instruction *loc) {
  // Canonicalize pointer
  AbstractValue canon_ptr = astate.getCanonical(ptr.addr);

  // PRIORITY: Check Invalid FIRST (UseAfterFree is more severe than
  // NullDereference) This prevents false positives where we report
  // NullDereference when UseAfterFree is correct
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    return {OperationResult::UseAfterFree, std::nullopt};
  }

  // Check path-sensitive nullness first. A branch-established null pointer is
  // a concrete witness even when the original value was a formal parameter.
  if (astate.getPathFormula().isNull(canon_ptr)) {
    return {OperationResult::NullDereference, std::nullopt};
  }

  // Null attributes originate from concrete null values. A contradictory
  // non-null path condition takes precedence over a stale attribute.
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Null)) {
    if (!astate.getPathFormula().isNonNull(canon_ptr)) {
      return {OperationResult::NullDereference, std::nullopt};
    }
  }

  // Try to find edge in heap (using canonical value)
  Access deref(AccessKind::Dereference);
  if (auto *target = astate.getPostHeap().findEdge(canon_ptr, deref)) {
    // Canonicalize target
    AbstractValue canon_target = astate.getCanonical(target->addr);

    // PROPAGATE ATTRIBUTES: Copy attributes from stored value to loaded value
    // This ensures that if we stored an Invalid pointer, loading it gives
    // Invalid
    auto attrs = astate.getPostAttrs().get(canon_target);
    for (Attribute attr : attrs) {
      // Attributes are already on canon_target, they will be checked by caller
    }

    Address result(canon_target);
    result.history = target->history;
    return {OperationResult::Success, result};
  }

  // Not in post heap: abduce to pre (biabduction)
  AbstractValue fresh = factory_->createFresh();
  Address target(fresh);
  target.history = ptr.history;
  target.history.addEvent(ValueHistory::EventKind::Allocation, loc,
                          loc ? loc->getFunction() : nullptr);

  // Add to post heap (using canonical value)
  astate.getPostHeap().addEdge(canon_ptr, deref, target);

  // Abduce to pre
  astate.abduceToPre(canon_ptr, deref, target);
  astate.abduceAttrToPre(canon_ptr, Attribute::Allocated);

  return {OperationResult::Success, std::optional<Address>(target)};
}

OperationResult PulseOperations::checkAddrAccess(AbductiveDomain &astate,
                                                 Address addr,
                                                 const llvm::Instruction *loc) {
  // Canonicalize address
  AbstractValue canon_addr = astate.getCanonical(addr.addr);

  // PRIORITY ORDER: Invalid > Null > Uninitialized
  // Check Invalid FIRST (UseAfterFree is more severe and prevents false
  // positives)
  if (astate.getPostAttrs().has(canon_addr, Attribute::Invalid)) {
    return OperationResult::UseAfterFree;
  }

  if (astate.getPostAttrs().has(canon_addr, Attribute::OutOfBounds)) {
    return OperationResult::OutOfBounds;
  }

  // Skip null dereference check for free() calls - free(NULL) is safe in C
  bool is_free_call = false;
  if (loc) {
    if (auto *CI = llvm::dyn_cast<llvm::CallInst>(loc)) {
      if (auto *F = CI->getCalledFunction()) {
        if (F->getName() == "free") {
          is_free_call = true;
        }
      }
    }
  }

  // Check null (using formula first, then attributes)
  // Path formula is more precise (path-sensitive)
  // Only check null if Invalid is NOT present (already checked above)
  // Skip null check for free() calls - free(NULL) is safe
  if (!is_free_call) {
    if (astate.getPathFormula().isNull(canon_addr)) {
      return OperationResult::NullDereference;
    }

    // Null attributes originate from concrete null values. A contradictory
    // non-null path condition takes precedence over a stale attribute.
    if (astate.getPostAttrs().has(canon_addr, Attribute::Null)) {
      if (!astate.getPathFormula().isNonNull(canon_addr)) {
        return OperationResult::NullDereference;
      }
    }
  }

  // Check uninitialized (for reads) - lowest priority
  // Only check if Invalid is NOT present
  if (astate.getPostAttrs().has(canon_addr, Attribute::Uninitialized)) {
    return OperationResult::UninitializedRead;
  }

  // Record MustBeValid precondition for accesses to pre-addresses (unless this
  // is free(ptr), where free(NULL) is valid in C).
  if (!is_free_call && astate.isInPre(canon_addr) &&
      !astate.isAllocated(canon_addr)) {
    Trace t = Trace::fromValueHistory(addr.history);
    if (loc) {
      t.addEvent(loc, "Memory access");
    }
    astate.abduceMustBeValid(canon_addr, std::move(t));
  }

  return OperationResult::Success;
}

void PulseOperations::allocate(AbductiveDomain &astate, AbstractValue addr,
                               const llvm::Instruction *loc,
                               AllocatorKind kind) {
  AbstractValue canon = astate.getCanonical(addr);
  astate.getPostAttrs().add(canon, Attribute::Allocated);
  astate.getPostAttrs().remove(canon, Attribute::Invalid);
  astate.getPostAttrs().remove(canon, Attribute::Uninitialized);
  astate.setAllocationRoot(canon, canon);
  astate.setAllocationOffset(canon, 0);
  Trace alloc_trace;
  if (loc) {
    alloc_trace.addEvent(loc, "Allocated");
  }
  astate.setAllocationInfo(canon,
                           AllocationInfo(kind, loc, std::move(alloc_trace)));
}

void PulseOperations::invalidate(AbductiveDomain &astate, Address addr,
                                 const llvm::Instruction *loc,
                                 InvalidationKind kind) {
  AbstractValue canon = astate.getCanonical(addr.addr);
  AbstractValue allocation_root = astate.getAllocationRoot(canon);

  // Invalidate the canonical address
  astate.getPostAttrs().add(canon, Attribute::Invalid);
  astate.getPostAttrs().remove(canon, Attribute::Allocated);
  astate.removeAllocationInfo(canon);
  // Clear allocation size for freed memory.
  astate.setAllocationSize(canon, 0);
  astate.setInvalidationInfo(canon, kind, loc);
  // Heap edges are keyed by canonical abstract values.
  astate.getPostHeap().removeEdges(canon);

  std::set<AbstractValue> same_allocation;
  for (const auto &kv : astate.getAllocationRoots()) {
    if (astate.getAllocationRoot(kv.first) == allocation_root) {
      same_allocation.insert(astate.getCanonical(kv.first));
    }
  }
  for (AbstractValue derived : same_allocation) {
    astate.getPostAttrs().add(derived, Attribute::Invalid);
    astate.getPostAttrs().remove(derived, Attribute::Allocated);
    astate.removeAllocationInfo(derived);
    astate.setAllocationSize(derived, 0);
    astate.setInvalidationInfo(derived, kind, loc);
    astate.getPostHeap().removeEdges(derived);
  }

  // Also invalidate aliases - find all values that canonicalize to the same
  // value This handles cases where q = p; delete p; *q (aliased_uaf) Since
  // canonicalization already groups aliases, we just need to ensure all values
  // with the same canonical value are invalidated The canonical value itself is
  // already invalidated above

  // Propagate invalidation to aliases across stack and heap keys/values.
  std::set<AbstractValue> alias_keys;

  for (const auto &stack_kv : astate.getPostStack().getMap()) {
    AbstractValue stack_canon = astate.getCanonical(stack_kv.second.addr);
    if (stack_canon == canon) {
      alias_keys.insert(stack_canon);
    }
  }

  for (const auto &edge_kv : astate.getPostHeap().getEdges()) {
    AbstractValue source_canon = astate.getCanonical(edge_kv.first);
    if (source_canon == canon) {
      alias_keys.insert(source_canon);
    }
    for (const auto &access_kv : edge_kv.second) {
      AbstractValue target_canon = astate.getCanonical(access_kv.second.addr);
      if (target_canon == canon) {
        alias_keys.insert(target_canon);
      }
    }
  }

  for (AbstractValue alias : alias_keys) {
    astate.getPostAttrs().add(alias, Attribute::Invalid);
    astate.getPostAttrs().remove(alias, Attribute::Allocated);
    astate.setAllocationSize(alias, 0);
    astate.getPostHeap().removeEdges(alias);
  }
}

OperationResult PulseOperations::writeDeref(AbductiveDomain &astate,
                                            Address ptr, Address value,
                                            const llvm::Instruction *loc) {
  // PRIORITY: Check Invalid FIRST before other checks
  AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    return OperationResult::UseAfterFree;
  }

  // Check access (will check Null, Uninitialized, etc.)
  auto result = checkAddrAccess(astate, ptr, loc);
  if (result == OperationResult::UninitializedRead) {
    // A store initializes its destination; the previous contents are not read.
    result = OperationResult::Success;
  }
  if (result != OperationResult::Success) {
    return result;
  }

  // A successful write through an otherwise unknown pointer requires valid
  // storage in the caller. Record that requirement without inventing a heap
  // read of the previous contents.
  if (!astate.getPostAttrs().has(canon_ptr, Attribute::Allocated) &&
      !astate.getPostAttrs().has(canon_ptr, Attribute::Stack) &&
      !astate.getPostAttrs().has(canon_ptr, Attribute::Global)) {
    astate.abduceAttrToPre(canon_ptr, Attribute::Allocated);
    Trace write_trace = Trace::fromValueHistory(ptr.history);
    if (loc) {
      write_trace.addEvent(loc, "Write dereference");
    }
    astate.abduceMustBeValid(canon_ptr, std::move(write_trace));
  }

  // Canonicalize values (canon_ptr already defined above)
  AbstractValue canon_value = astate.getCanonical(value.addr);
  Address canon_value_addr(canon_value);
  canon_value_addr.history = value.history;

  // Write to heap (using canonical values)
  Access deref(AccessKind::Dereference);
  astate.getPostHeap().addEdge(canon_ptr, deref, canon_value_addr);
  Trace wt_trace;
  if (loc) {
    wt_trace.addEvent(loc, "Written to memory");
  }
  astate.markWrittenTo(canon_ptr, loc, std::move(wt_trace));

  // Propagate taint: if value is tainted, propagate to memory location
  TaintOperations::propagateThroughStore(astate, canon_value, canon_ptr, loc);

  // Initialize if needed
  initialize(astate, canon_ptr);

  // Interprocedural initialization: when writing through a pointer parameter,
  // clear Uninitialized from the pointed-to memory location
  AbstractValue pointed_canon = astate.getCanonical(canon_value_addr.addr);
  astate.getPostAttrs().remove(pointed_canon, Attribute::Uninitialized);

  if (astate.getPostAttrs().has(canon_ptr, Attribute::Stack)) {
    astate.getPostAttrs().remove(canon_ptr, Attribute::Uninitialized);
  }

  // If writing a struct field via GEP (including nested GEP/bitcast chains
  // such as ABI `coerce.dive`), propagate initialization to enclosing structs
  // whenever all fields of a struct have now been initialized.
  if (const auto *SI = llvm::dyn_cast_or_null<llvm::StoreInst>(loc)) {
    const llvm::DataLayout *DL =
        SI->getModule() ? &SI->getModule()->getDataLayout() : nullptr;
    llvm::SmallVector<const llvm::GEPOperator *, 6> gep_chain;
    const llvm::Value *cur_p = SI->getPointerOperand()->stripPointerCasts();
    while (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(cur_p)) {
      gep_chain.push_back(GEP);
      if (gep_chain.size() >= 6)
        break;
      cur_p = GEP->getPointerOperand()->stripPointerCasts();
    }
    if (!gep_chain.empty()) {
      const llvm::Value *root_v = gep_chain.back()->getPointerOperand()->stripPointerCasts();
      const Address *root_slot = astate.getPostStack().find(root_v);
      if (!root_slot) {
        root_slot = astate.getPostStack().find(gep_chain.back()->getPointerOperand());
      }
      if (root_slot) {
        AbstractValue walk_av = astate.getCanonical(root_slot->addr);
        llvm::SmallVector<std::pair<AbstractValue, const llvm::StructType *>, 8>
            enclosing_structs;
        bool walk_ok = true;
        for (auto g_it = gep_chain.rbegin();
             g_it != gep_chain.rend() && walk_ok; ++g_it) {
          const llvm::GEPOperator *GEP = *g_it;
          unsigned op_idx = 1;
          for (auto GTI = llvm::gep_type_begin(GEP), E = llvm::gep_type_end(GEP);
               GTI != E && walk_ok; ++GTI, ++op_idx) {
            const llvm::Value *idx = GEP->getOperand(op_idx);
            if (const auto *ST = GTI.getStructTypeOrNull()) {
              enclosing_structs.emplace_back(walk_av, ST);
              auto *C = llvm::dyn_cast<llvm::ConstantInt>(idx);
              if (!C || C->getZExtValue() >= ST->getNumElements()) {
                walk_ok = false;
                break;
              }
              unsigned fidx = static_cast<unsigned>(C->getZExtValue());
              uint64_t foff =
                  (DL && ST->isSized())
                      ? DL->getStructLayout(const_cast<llvm::StructType *>(ST))
                            ->getElementOffset(fidx)
                      : 0;
              const Address *next = astate.getPostHeap().findEdge(
                  walk_av, Access::field(fidx, foff));
              if (!next) {
                walk_ok = false;
                break;
              }
              walk_av = astate.getCanonical(next->addr);
            } else {
              auto c_opt = getI64Constant(idx);
              if (!c_opt) {
                c_opt = astate.getPathFormula().asConstantInt(
                    astate.getCanonical(factory_->getOrCreate(idx)));
              }
              if (op_idx == 1 && c_opt && *c_opt == 0) {
                continue;
              }
              if (!c_opt) {
                // Writing to a symbolic/loop-varying array index initializes
                // elements that cannot be soundly proven uninitialized later
                // (under-approximating Pulse semantics). Constant indices do NOT
                // clear the base array so partial constant init is still caught.
                AbstractValue walk_root = astate.getAllocationRoot(walk_av);
                astate.getPostAttrs().remove(walk_av, Attribute::Uninitialized);
                astate.getPostAttrs().remove(walk_root, Attribute::Uninitialized);
              }
              uint64_t stride_bytes = 0;
              llvm::Type *indexed = GTI.getIndexedType();
              if (DL && indexed && indexed->isSized()) {
                stride_bytes = DL->getTypeAllocSize(indexed);
              }
              Access acc = c_opt
                               ? Access::constArrayIndex(*c_opt, stride_bytes)
                               : Access::arrayIndex(
                                     astate.getCanonical(
                                         factory_->getOrCreate(idx)),
                                     stride_bytes);
              const Address *next = astate.getPostHeap().findEdge(walk_av, acc);
              if (!next) {
                walk_ok = false;
                break;
              }
              walk_av = astate.getCanonical(next->addr);
            }
          }
        }
        for (auto it = enclosing_structs.rbegin();
             it != enclosing_structs.rend(); ++it) {
          maybeMarkStructInitialized(astate, it->first, it->second, DL, loc);
        }
      }
    }
  }

  return OperationResult::Success;
}

std::pair<OperationResult, std::optional<Address>>
PulseOperations::readDeref(AbductiveDomain &astate, Address ptr,
                           const llvm::Instruction *loc) {
  // PRIORITY: Check Invalid FIRST before other checks
  AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    return {OperationResult::UseAfterFree, std::nullopt};
  }

  // Before reporting UninitializedRead on a struct alloca (e.g., ABI-coerced
  // `bitcast %retval to <2 x float>*; load` after all struct fields were
  // initialized), check whether all fields of the struct have been written.
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Uninitialized)) {
    const llvm::DataLayout *DL =
        (loc && loc->getModule()) ? &loc->getModule()->getDataLayout() : nullptr;
    if (const auto *AI =
            llvm::dyn_cast_or_null<llvm::AllocaInst>(canon_ptr.getValue())) {
      if (const auto *ST =
              llvm::dyn_cast<llvm::StructType>(AI->getAllocatedType())) {
        maybeMarkStructInitialized(astate, canon_ptr, ST, DL, loc);
      }
    }
    if (astate.getPostAttrs().has(canon_ptr, Attribute::Uninitialized)) {
      if (const auto *LI = llvm::dyn_cast_or_null<llvm::LoadInst>(loc)) {
        if (const auto *ST = llvm::dyn_cast<llvm::StructType>(LI->getType())) {
          maybeMarkStructInitialized(astate, canon_ptr, ST, DL, loc);
        }
      }
    }
  }

  // Check access (will check Null, OutOfBounds, Uninitialized, and abduce
  // MustBeValid for pre-addresses)
  auto result = checkAddrAccess(astate, ptr, loc);
  if (result != OperationResult::Success) {
    return {result, std::nullopt};
  }

  // Reading through a pre-address that has not been written in this procedure
  // requires the caller to have initialized it.
  if (astate.isInPre(canon_ptr) && !astate.hasWrittenTo(canon_ptr) &&
      !astate.isAllocated(canon_ptr)) {
    Trace init_trace = Trace::fromValueHistory(ptr.history);
    if (loc) {
      init_trace.addEvent(loc, "Read from memory");
    }
    astate.abduceMustBeInitialized(canon_ptr, std::move(init_trace));
  }

  // Evaluate dereference
  auto deref_result = evalDeref(astate, ptr, loc);

  if (deref_result.first == OperationResult::Success && deref_result.second) {
    AbstractValue canon_value = astate.getCanonical(deref_result.second->addr);
    if (astate.getPostAttrs().has(canon_value, Attribute::Uninitialized)) {
      return {OperationResult::UninitializedRead, std::nullopt};
    }
  }

  // Propagate taint: if memory location is tainted, propagate to loaded value
  if (deref_result.first == OperationResult::Success && deref_result.second) {
    // canon_ptr already defined above
    AbstractValue canon_value = astate.getCanonical(deref_result.second->addr);
    TaintOperations::propagateThroughLoad(astate, canon_ptr, canon_value, loc);
  }

  return deref_result;
}

void PulseOperations::initialize(AbductiveDomain &astate, AbstractValue addr) {
  astate.getPostAttrs().remove(addr, Attribute::Uninitialized);
}

OperationResult PulseOperations::checkNull(AbductiveDomain &astate,
                                           Address addr,
                                           const llvm::Instruction *loc) {
  (void)loc;
  if (astate.getPostAttrs().has(addr.addr, Attribute::Null)) {
    return OperationResult::NullDereference;
  }
  return OperationResult::Success;
}

std::pair<OperationResult, std::optional<Address>>
PulseOperations::shallowCopy(AbductiveDomain &astate, Address source,
                             const llvm::Instruction *loc) {
  // Check source is valid
  auto result = checkAddrAccess(astate, source, loc);
  if (result != OperationResult::Success) {
    return {result, std::nullopt};
  }

  // Create fresh address for copy
  AbstractValue copy_addr = factory_->createFresh();
  Address copy(copy_addr);
  copy.history = source.history;
  if (loc) {
    copy.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                          loc->getFunction());
  }

  // Copy all edges from source to copy
  AbstractValue canon_source = astate.getCanonical(source.addr);
  const auto &edges = astate.getPostHeap().getEdges();
  auto it = edges.find(canon_source);
  if (it != edges.end()) {
    for (const auto &edge_kv : it->second) {
      astate.getPostHeap().addEdge(copy_addr, edge_kv.first, edge_kv.second);
    }
  }

  // Copy attributes (except allocation - copy is a new allocation)
  const auto &attrs = astate.getPostAttrs().get(canon_source);
  for (Attribute attr : attrs) {
    if (attr != Attribute::Allocated) {
      astate.getPostAttrs().add(copy_addr, attr);
    }
  }

  return {OperationResult::Success, std::optional<Address>(copy)};
}

std::pair<OperationResult, std::optional<Address>>
PulseOperations::deepCopy(AbductiveDomain &astate, Address source,
                          const llvm::Instruction *loc, unsigned depth_max) {
  // Check source is valid
  auto result = checkAddrAccess(astate, source, loc);
  if (result != OperationResult::Success) {
    return {result, std::nullopt};
  }

  // Create fresh address for copy
  AbstractValue copy_addr = factory_->createFresh();
  Address copy(copy_addr);
  copy.history = source.history;
  if (loc) {
    copy.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                          loc->getFunction());
  }

  // Recursively copy edges up to depth_max
  AbstractValue canon_source = astate.getCanonical(source.addr);
  deepCopyRecursive(astate, canon_source, copy_addr, depth_max, 0);

  return {OperationResult::Success, std::optional<Address>(copy)};
}

void PulseOperations::deepCopyRecursive(AbductiveDomain &astate,
                                        AbstractValue source,
                                        AbstractValue target,
                                        unsigned depth_max,
                                        unsigned current_depth) {
  if (depth_max > 0 && current_depth >= depth_max) {
    // At max depth, do shallow copy
    const auto &edges = astate.getPostHeap().getEdges();
    auto it = edges.find(source);
    if (it != edges.end()) {
      for (const auto &edge_kv : it->second) {
        astate.getPostHeap().addEdge(target, edge_kv.first, edge_kv.second);
      }
    }
    return;
  }

  // Deep copy: recursively copy all edges
  const auto &edges = astate.getPostHeap().getEdges();
  auto it = edges.find(source);
  if (it != edges.end()) {
    for (const auto &edge_kv : it->second) {
      AbstractValue fresh_target = factory_->createFresh();
      Address target_addr(fresh_target);
      target_addr.history = edge_kv.second.history;
      astate.getPostHeap().addEdge(target, edge_kv.first, target_addr);

      // Recursively copy
      deepCopyRecursive(astate, edge_kv.second.addr, fresh_target, depth_max,
                        current_depth + 1);
    }
  }
}

void PulseOperations::copySubgraph(AbductiveDomain &astate, AbstractValue src,
                                   AbstractValue dst, unsigned max_depth) {
  AbstractValue c_src = astate.getCanonical(src);
  AbstractValue c_dst = astate.getCanonical(dst);
  if (c_src == c_dst) {
    return;
  }
  std::set<AbstractValue> visited;
  std::function<void(AbstractValue, AbstractValue, unsigned)> copy_rec =
      [&](AbstractValue s, AbstractValue d, unsigned depth) {
        if (depth >= max_depth || !visited.insert(s).second) {
          return;
        }
        auto it = astate.getPostHeap().getEdges().find(s);
        if (it == astate.getPostHeap().getEdges().end()) {
          return;
        }
        // Snapshot edges before mutating post_heap_.
        auto edges_copy = it->second;
        for (const auto &ekv : edges_copy) {
          const Access &acc = ekv.first;
          const Address &src_target = ekv.second;
          AbstractValue c_src_target = astate.getCanonical(src_target.addr);
          // For nested Field/ArrayIndex sub-cells that themselves have outgoing
          // edges (e.g. addressable struct fields from GEP), allocate/reuse a
          // destination sub-cell and recurse. For leaf values (e.g. pointer or
          // scalar fields in SSA aggregates or Dereference targets), copy the
          // Address directly to preserve its abstract value and attributes.
          const auto src_child_it =
              astate.getPostHeap().getEdges().find(c_src_target);
          const bool has_child_edges =
              src_child_it != astate.getPostHeap().getEdges().end() &&
              !src_child_it->second.empty();
          if ((acc.kind == AccessKind::Field ||
               acc.kind == AccessKind::ArrayIndex) &&
              has_child_edges && depth + 1 <= max_depth) {
            AbstractValue dst_cell;
            if (const Address *existing =
                    astate.getPostHeap().findEdge(d, acc)) {
              dst_cell = astate.getCanonical(existing->addr);
            } else {
              dst_cell = factory_->createFresh();
              Address cell_addr(dst_cell);
              cell_addr.history = src_target.history;
              astate.getPostHeap().addEdge(d, acc, cell_addr);
              astate.setAllocationRoot(dst_cell, astate.getAllocationRoot(d));
            }
            astate.getPostAttrs().remove(dst_cell, Attribute::Uninitialized);
            astate.markWrittenTo(dst_cell, nullptr);
            copy_rec(c_src_target, dst_cell, depth + 1);
          } else {
            astate.getPostHeap().addEdge(d, acc, src_target);
          }
        }
      };
  copy_rec(c_src, c_dst, 0);
}

void PulseOperations::havoc(AbductiveDomain &astate, Address addr,
                            const llvm::Instruction *loc) {
  // Remove all edges from the address (model unknown effects)
  AbstractValue canon_addr = astate.getCanonical(addr.addr);
  astate.getPostHeap().removeEdges(canon_addr);

  // Preserve provenance/resource facts while forgetting value-specific facts.
  AttributeSet preserved;
  for (Attribute attr : astate.getPostAttrs().get(canon_addr)) {
    switch (attr) {
    case Attribute::Allocated:
    case Attribute::Stack:
    case Attribute::Global:
    case Attribute::FileHandle:
    case Attribute::Lock:
    case Attribute::AsyncResource:
      preserved.insert(attr);
      break;
    case Attribute::Invalid:
    case Attribute::Uninitialized:
    case Attribute::Null:
    case Attribute::OutOfBounds:
    case Attribute::Tainted:
      break;
    }
  }
  astate.getPostAttrs().clear(canon_addr);
  for (Attribute attr : preserved) {
    astate.getPostAttrs().add(canon_addr, attr);
  }
  astate.setAllocationSize(canon_addr, 0);

  // Record UnknownEffect and WrittenTo so downstream summaries and leak checks
  // know this address passed through an unknown call.
  std::string callee_name = "<unknown>";
  if (loc) {
    if (auto *CB = llvm::dyn_cast<llvm::CallBase>(loc)) {
      if (const llvm::Function *F = CB->getCalledFunction()) {
        callee_name = F->getName().str();
      } else {
        callee_name = "<indirect>";
      }
    }
  }
  UnknownEffectInfo ue;
  ue.callee = std::move(callee_name);
  ue.location = loc;
  ue.history = addr.history;
  astate.setUnknownEffect(canon_addr, std::move(ue));
  astate.markWrittenTo(canon_addr, loc);
}

OperationResult
PulseOperations::checkAddressEscape(AbductiveDomain &astate, Address addr,
                                    const llvm::Function *current_function,
                                    const llvm::Instruction *loc) {
  (void)current_function;
  (void)loc;
  // Check if address is from stack (local variable)
  // If it escapes (e.g., returned, stored to global, passed to function),
  // it should be heap-allocated

  AbstractValue canon_addr = astate.getCanonical(addr.addr);

  // Sound incorrectness: only treat as escaping stack address when provable.
  if (astate.getPostAttrs().has(canon_addr, Attribute::Stack)) {
    return OperationResult::InvalidAccess;
  }

  return OperationResult::Success;
}

} // namespace pulse
