#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Domain/PulseTaintConfig.h"
#include "Checker/Pulse/Interproc/PulseModels.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

namespace {

static std::string formatTypeName(const llvm::Type *ty) {
  if (!ty)
    return "unknown";
  std::string out;
  llvm::raw_string_ostream os(out);
  ty->print(os);
  return os.str();
}

static bool isExpensiveAggregateType(const llvm::Type *ty,
                                     const llvm::Module *M) {
  if (!ty)
    return false;
  if (!ty->isStructTy() && !ty->isArrayTy()) {
    return false;
  }
  if (const auto *ST = llvm::dyn_cast<llvm::StructType>(ty)) {
    if (ST->hasName()) {
      llvm::StringRef n = ST->getName();
      if (n.contains("string_view") || n.contains("basic_string_view") ||
          n.contains("span") || n.contains("iterator") ||
          n.contains("unique_ptr") || n.contains("weak_ptr") ||
          n.contains("reference_wrapper")) {
        return false;
      }
    }
  }
  if (M && ty->isSized()) {
    return M->getDataLayout().getTypeAllocSize(const_cast<llvm::Type *>(ty)) >=
           16;
  }
  return true;
}

static void modelUnknownCallEffects(PulseOperations &ops,
                                    AbductiveDomain &astate,
                                    const llvm::CallBase *CB,
                                    const llvm::BasicBlock *pred,
                                    AbstractValueFactory &factory) {
  for (unsigned i = 0; i < CB->arg_size(); ++i) {
    const llvm::Value *arg = CB->getArgOperand(i);
    if (!arg->getType()->isPointerTy()) {
      continue;
    }
    auto arg_opt = ops.eval(astate, arg, CB, pred);
    if (!arg_opt) {
      continue;
    }
    ops.havoc(astate, *arg_opt, CB);
  }

  if (!CB->getType()->isVoidTy()) {
    AbstractValue ret_val = factory.createFresh(CB);
    Address ret_addr(ret_val);
    ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CB,
                              CB->getFunction());
    astate.getPostStack().add(CB, ret_addr);
    if (CB->getType()->isIntegerTy()) {
      astate.getPathFormula().addIntegerConstraint(ret_val);
    }
  }
}

static bool isHeapOrResourceAllocator(AllocatorKind kind) {
  return kind == AllocatorKind::CMalloc || kind == AllocatorKind::CRealloc ||
         kind == AllocatorKind::CppNew || kind == AllocatorKind::CppNewArray ||
         kind == AllocatorKind::FileHandle || kind == AllocatorKind::Socket ||
         kind == AllocatorKind::CustomResource;
}

static void clearUnreachableAt(AbductiveDomain &astate, AbstractValue av) {
  AbstractValue root = astate.getAllocationRoot(av);
  auto it = astate.getPostAllocationInfo().find(root);
  if (it != astate.getPostAllocationInfo().end()) {
    it->second.unreachable_at = nullptr;
  }
}

static void markUnreachableAllocationIfDead(AbductiveDomain &astate,
                                            AbstractValue candidate,
                                            const llvm::Instruction *loc) {
  if (!loc) {
    return;
  }
  AbstractValue canon = astate.getCanonical(candidate);
  AbstractValue root = astate.getAllocationRoot(canon);
  auto &alloc_map = astate.getPostAllocationInfo();
  auto it = alloc_map.find(root);
  if (it == alloc_map.end()) {
    it = alloc_map.find(canon);
    if (it == alloc_map.end()) {
      return;
    }
    root = it->first;
  }
  if (!isHeapOrResourceAllocator(it->second.kind) ||
      astate.getPostAttrs().has(root, Attribute::Invalid) ||
      astate.getPostAttrs().has(root, Attribute::Stack) ||
      astate.getPostAttrs().has(root, Attribute::Global)) {
    return;
  }

  // Check if any stack slot (alloca/arg/global) or heap edge still references
  // this allocation or an interior pointer derived from it.
  for (const auto &kv : astate.getPostStack().getMap()) {
    if (!kv.first ||
        (!llvm::isa<llvm::AllocaInst>(kv.first) &&
         !llvm::isa<llvm::Argument>(kv.first) &&
         !llvm::isa<llvm::GlobalValue>(kv.first))) {
      continue;
    }
    AbstractValue v = astate.getCanonical(kv.second.addr);
    if (v == root || astate.getAllocationRoot(v) == root) {
      return;
    }
  }
  for (const auto &node_kv : astate.getPostHeap().getEdges()) {
    for (const auto &edge_kv : node_kv.second) {
      AbstractValue target = astate.getCanonical(edge_kv.second.addr);
      if (target == root || astate.getAllocationRoot(target) == root) {
        return;
      }
    }
  }

  if (!it->second.unreachable_at) {
    it->second.unreachable_at = loc;
  }
}

static PathConditionAtom::Kind
atomKindFromIcmpPred(llvm::ICmpInst::Predicate pred) {
  switch (pred) {
  case llvm::ICmpInst::ICMP_EQ:
    return PathConditionAtom::Kind::Equal;
  case llvm::ICmpInst::ICMP_NE:
    return PathConditionAtom::Kind::NotEqual;
  case llvm::ICmpInst::ICMP_SLT:
  case llvm::ICmpInst::ICMP_ULT:
    return PathConditionAtom::Kind::LessThan;
  case llvm::ICmpInst::ICMP_SLE:
  case llvm::ICmpInst::ICMP_ULE:
    return PathConditionAtom::Kind::LessEqual;
  case llvm::ICmpInst::ICMP_SGT:
  case llvm::ICmpInst::ICMP_UGT:
    return PathConditionAtom::Kind::GreaterThan;
  case llvm::ICmpInst::ICMP_SGE:
  case llvm::ICmpInst::ICMP_UGE:
    return PathConditionAtom::Kind::GreaterEqual;
  default:
    return PathConditionAtom::Kind::Other;
  }
}

static inline std::vector<ExecutionDomain>
singleState(ExecutionDomain &&st) {
  std::vector<ExecutionDomain> out;
  out.reserve(1);
  out.push_back(std::move(st));
  return out;
}

} // namespace

std::vector<ExecutionDomain> PulseChecker::executeInstruction(
    const llvm::Instruction *I, ExecutionDomain exec_state,
    const llvm::BasicBlock *pred, unsigned call_depth) {
  if (exec_state.isStopped() || exec_state.isExceptionRaised())
    return singleState(std::move(exec_state));
  auto *astate = exec_state.getAstate();
  if (!astate)
    return singleState(std::move(exec_state));

  // Fast O(1) feasibility check for straight-line non-branching instructions
  // (avoids re-invoking Z3 on every Load/Store/GEP/Cast/PHI when SMT is on).
  auto pruneInfeasibleFast = [](ExecutionDomain &st) -> bool {
    if (st.isStopped() || st.isExceptionRaised())
      return true;
    AbductiveDomain *a = st.getAstate();
    if (!a)
      return true;
    const PulseFormula &f = a->getPathFormula();
    return !f.isContradiction() && f.isConsistent();
  };

  auto pruneInfeasible = [](ExecutionDomain &st) -> bool {
    if (st.isStopped() || st.isExceptionRaised())
      return true;
    AbductiveDomain *a = st.getAstate();
    if (!a)
      return true;
    return !a->getPathFormula().isUnsat();
  };

  auto pruneSingleState = [&](ExecutionDomain &&st) -> std::vector<ExecutionDomain> {
    if (!pruneInfeasibleFast(st))
      return {};
    return singleState(std::move(st));
  };

  auto pruneStates =
      [&](std::vector<ExecutionDomain> states) -> std::vector<ExecutionDomain> {
    std::vector<ExecutionDomain> out;
    out.reserve(states.size());
    for (auto &st : states) {
      if (pruneInfeasible(st)) {
        out.push_back(std::move(st));
      }
    }
    return out;
  };

  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(I)) {
    if (Sel->getType()->isPointerTy() || Sel->getType()->isIntegerTy()) {
      auto apply_choice = [&](ExecutionDomain &st, const llvm::Value *chosen) {
        auto *a = st.getAstate();
        if (!a)
          return;
        auto chosen_opt = ops_.eval(*a, chosen, I, pred);
        if (chosen_opt) {
          AbstractValue canon = a->getCanonical(chosen_opt->addr);
          Address result(canon);
          result.history = chosen_opt->history;
          result.history.addEvent(ValueHistory::EventKind::Unknown, I,
                                  I->getFunction());
          a->getPostStack().add(Sel, result);
        }
      };

      // Fast path when condition is a compile-time constant boolean.
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(Sel->getCondition())) {
        apply_choice(exec_state,
                     CI->isOne() ? Sel->getTrueValue() : Sel->getFalseValue());
        return singleState(std::move(exec_state));
      }

      // For branchless scalar integer selects (min/max/clamp/bit-twiddling),
      // avoid 2^N disjunctive explosion by joining interval bounds into a
      // single abstract value instead of forking the entire AbductiveDomain.
      if (Sel->getType()->isIntegerTy()) {
        auto tv_opt = ops_.eval(*astate, Sel->getTrueValue(), I, pred);
        auto fv_opt = ops_.eval(*astate, Sel->getFalseValue(), I, pred);
        AbstractValue res_av = factory_.getOrCreate(Sel);
        astate->getPathFormula().addIntegerConstraint(res_av);
        astate->getPathFormula().setBitWidth(
            res_av, Sel->getType()->getIntegerBitWidth());
        if (tv_opt && fv_opt) {
          AbstractValue tc = astate->getCanonical(tv_opt->addr);
          AbstractValue fc = astate->getCanonical(fv_opt->addr);
          if (tc == fc) {
            (void)astate->getPathFormula().addEquality(res_av, tc);
          } else {
            const PulseFormula &pf = astate->getPathFormula();
            auto lo1 = pf.getLowerBound(tc);
            auto lo2 = pf.getLowerBound(fc);
            auto hi1 = pf.getUpperBound(tc);
            auto hi2 = pf.getUpperBound(fc);
            if (lo1 && lo2 && hi1 && hi2) {
              (void)astate->getPathFormula().addBounds(
                  res_av, std::min(*lo1, *lo2), std::max(*hi1, *hi2));
            } else if (lo1 && lo2) {
              (void)astate->getPathFormula().addBounds(
                  res_av, std::min(*lo1, *lo2),
                  std::numeric_limits<int64_t>::max());
            } else if (hi1 && hi2) {
              (void)astate->getPathFormula().addBounds(
                  res_av, std::numeric_limits<int64_t>::min(),
                  std::max(*hi1, *hi2));
            }
            if (pf.isNonNull(tc) && pf.isNonNull(fc)) {
              (void)astate->getPathFormula().addNonNull(res_av);
            }
          }
          TaintOperations::propagate(*astate, tc, res_av, I);
          TaintOperations::propagate(*astate, fc, res_av, I);
        }
        astate->getPostStack().add(Sel, Address(res_av));
        return singleState(std::move(exec_state));
      }

      std::vector<ExecutionDomain> selected_states;
      selected_states.reserve(2);
      auto true_state = assumeCondition(exec_state.clone(), Sel->getCondition(),
                                        true, Sel, pred);
      if (true_state) {
        if (!true_state->isStopped()) {
          apply_choice(*true_state, Sel->getTrueValue());
        }
        selected_states.push_back(std::move(*true_state));
      }
      auto false_state = assumeCondition(std::move(exec_state),
                                         Sel->getCondition(), false, Sel, pred);
      if (false_state) {
        if (!false_state->isStopped()) {
          apply_choice(*false_state, Sel->getFalseValue());
        }
        selected_states.push_back(std::move(*false_state));
      }
      return pruneStates(std::move(selected_states));
    }
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(I)) {
    if (!pred) {
      pred = exec_state.getEntryPred();
    }
    bool is_predecessor = false;
    for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
      if (Phi->getIncomingBlock(i) == pred) {
        is_predecessor = true;
        break;
      }
    }
    if (!is_predecessor) {
      return {};
    }

    const llvm::Value *incoming = Phi->getIncomingValueForBlock(pred);
    auto addr_opt = ops_.eval(*astate, incoming, I, pred);
    if (addr_opt) {
      AbstractValue canon_addr = astate->getCanonical(addr_opt->addr);
      Address canon_result(canon_addr);
      canon_result.history = addr_opt->history;
      canon_result.history.addEvent(ValueHistory::EventKind::Unknown, I,
                                    I->getFunction());
      astate->getPostStack().add(Phi, canon_result);
    }
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(I)) {
    auto gep_opt = ops_.eval(*astate, GEP, GEP, pred);
    if (gep_opt) {
      astate->getPostStack().add(GEP, *gep_opt);
    }
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(I)) {
    auto src_opt = ops_.eval(*astate, BC->getOperand(0), BC, pred);
    if (src_opt) {
      astate->getPostStack().add(BC, *src_opt);
    }
    return pruneSingleState(std::move(exec_state));
  }

  if (llvm::isa<llvm::ZExtInst>(I) || llvm::isa<llvm::SExtInst>(I) ||
      llvm::isa<llvm::TruncInst>(I) || llvm::isa<llvm::IntToPtrInst>(I) ||
      llvm::isa<llvm::PtrToIntInst>(I)) {
    auto src_opt = ops_.eval(*astate, I->getOperand(0), I, pred);
    if (src_opt) {
      astate->getPostStack().add(I, *src_opt);
    }
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *IVI = llvm::dyn_cast<llvm::InsertValueInst>(I)) {
    AbstractValue res_av = factory_.createFresh(IVI);
    Address res_addr(res_av);
    if (auto agg_opt =
            ops_.eval(*astate, IVI->getAggregateOperand(), IVI, pred)) {
      ops_.copySubgraph(*astate, astate->getCanonical(agg_opt->addr), res_av, 3);
    }
    if (auto ins_opt =
            ops_.eval(*astate, IVI->getInsertedValueOperand(), IVI, pred)) {
      AbstractValue cur = res_av;
      auto indices = IVI->getIndices();
      for (size_t idx_i = 0; idx_i < indices.size(); ++idx_i) {
        Access acc(indices[idx_i]);
        if (idx_i + 1 == indices.size()) {
          Address leaf = *ins_opt;
          leaf.history.addEvent(ValueHistory::EventKind::Store, IVI,
                                IVI->getFunction());
          astate->getPostHeap().addEdge(cur, acc, leaf);
        } else if (const Address *existing =
                       astate->getPostHeap().findEdge(cur, acc)) {
          cur = astate->getCanonical(existing->addr);
        } else {
          AbstractValue mid = factory_.createFresh(IVI);
          astate->getPostHeap().addEdge(cur, acc, Address(mid));
          cur = mid;
        }
      }
    }
    astate->getPostStack().add(IVI, res_addr);
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *EVI = llvm::dyn_cast<llvm::ExtractValueInst>(I)) {
    if (auto agg_opt =
            ops_.eval(*astate, EVI->getAggregateOperand(), EVI, pred)) {
      AbstractValue cur = astate->getCanonical(agg_opt->addr);
      ValueHistory hist = agg_opt->history;
      for (unsigned field_idx : EVI->getIndices()) {
        Access acc(field_idx);
        if (const Address *next = astate->getPostHeap().findEdge(cur, acc)) {
          cur = astate->getCanonical(next->addr);
          hist = next->history;
        } else {
          AbstractValue fresh = factory_.createFresh(EVI);
          Address next_addr(fresh);
          next_addr.history = hist;
          astate->getPostHeap().addEdge(cur, acc, next_addr);
          astate->abduceToPre(cur, acc, next_addr);
          cur = fresh;
        }
      }
      Address extracted(cur);
      extracted.history = hist;
      astate->getPostStack().add(EVI, extracted);
    }
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(I)) {
    auto ptr_opt = ops_.eval(*astate, RMW->getPointerOperand(), RMW, pred);
    if (!ptr_opt)
      return pruneSingleState(std::move(exec_state));
    auto read_res = ops_.readDeref(*astate, *ptr_opt, RMW);
    if (read_res.first != OperationResult::Success) {
      Trace trace = Trace::fromValueHistory(ptr_opt->history);
      trace.addEvent(RMW, "Atomic read-modify-write through invalid pointer");
      AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
      if (LatentIssue::isManifest(read_res.first, *astate, canon_ptr)) {
        reportBug(read_res.first, RMW, canon_ptr, trace, astate);
        return {ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()), read_res.first,
            std::move(trace))};
      }
      latent_issues_.emplace_back(
          read_res.first, LatentIssue::issueKindFromResult(read_res.first),
          canon_ptr, RMW, trace.clone());
      return {ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back(), read_res.first, std::move(trace))};
    }
    Address old_addr =
        read_res.second ? *read_res.second : Address(factory_.createFresh(RMW));
    astate->getPostStack().add(RMW, old_addr);

    auto val_opt = ops_.eval(*astate, RMW->getValOperand(), RMW, pred);
    Address new_addr(factory_.createFresh(RMW));
    if (RMW->getOperation() == llvm::AtomicRMWInst::Xchg && val_opt) {
      new_addr = *val_opt;
    } else if (val_opt && RMW->getType()->isIntegerTy()) {
      astate->getPathFormula().addIntegerConstraint(new_addr.addr);
      if (RMW->getOperation() == llvm::AtomicRMWInst::Add) {
        (void)astate->getPathFormula().addArithmeticOperation(
            new_addr.addr, astate->getCanonical(old_addr.addr),
            astate->getCanonical(val_opt->addr), "+");
      } else if (RMW->getOperation() == llvm::AtomicRMWInst::Sub) {
        (void)astate->getPathFormula().addArithmeticOperation(
            new_addr.addr, astate->getCanonical(old_addr.addr),
            astate->getCanonical(val_opt->addr), "-");
      }
    }
    ops_.writeDeref(*astate, *ptr_opt, new_addr, RMW);
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *CX = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(I)) {
    auto ptr_opt = ops_.eval(*astate, CX->getPointerOperand(), CX, pred);
    if (!ptr_opt)
      return pruneSingleState(std::move(exec_state));
    auto read_res = ops_.readDeref(*astate, *ptr_opt, CX);
    if (read_res.first != OperationResult::Success) {
      Trace trace = Trace::fromValueHistory(ptr_opt->history);
      trace.addEvent(CX, "Atomic compare-exchange through invalid pointer");
      AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
      if (LatentIssue::isManifest(read_res.first, *astate, canon_ptr)) {
        reportBug(read_res.first, CX, canon_ptr, trace, astate);
        return {ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()), read_res.first,
            std::move(trace))};
      }
      latent_issues_.emplace_back(
          read_res.first, LatentIssue::issueKindFromResult(read_res.first),
          canon_ptr, CX, trace.clone());
      return {ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back(), read_res.first, std::move(trace))};
    }
    Address old_addr =
        read_res.second ? *read_res.second : Address(factory_.createFresh(CX));
    auto cmp_opt = ops_.eval(*astate, CX->getCompareOperand(), CX, pred);
    auto new_opt = ops_.eval(*astate, CX->getNewValOperand(), CX, pred);

    std::vector<ExecutionDomain> out;
    // Branch 1: compare succeeded -> write new value, return {old, 1}.
    {
      ExecutionDomain succ = exec_state.clone();
      AbductiveDomain *sa = succ.getAstate();
      bool ok = true;
      if (cmp_opt) {
        ok = sa->getPathFormula().addEquality(sa->getCanonical(old_addr.addr),
                                              sa->getCanonical(cmp_opt->addr));
      }
      if (ok && sa->incorporatePendingNewEqs().isOk()) {
        if (new_opt) {
          ops_.writeDeref(*sa, *ptr_opt, *new_opt, CX);
        }
        AbstractValue res_av = factory_.createFresh(CX);
        AbstractValue flag_av = factory_.createFresh(CX);
        sa->getPathFormula().addBounds(flag_av, 1, 1);
        sa->getPostHeap().addEdge(res_av, Access(0u), old_addr);
        sa->getPostHeap().addEdge(res_av, Access(1u), Address(flag_av));
        sa->getPostStack().add(CX, Address(res_av));
        out.push_back(std::move(succ));
      }
    }
    // Branch 2: compare failed -> leave memory unchanged, return {old, 0}.
    {
      ExecutionDomain fail = exec_state.clone();
      AbductiveDomain *fa = fail.getAstate();
      bool ok = true;
      if (cmp_opt) {
        ok = fa->getPathFormula().addDisequality(
            fa->getCanonical(old_addr.addr), fa->getCanonical(cmp_opt->addr));
      }
      if (ok && fa->incorporatePendingNewEqs().isOk()) {
        AbstractValue res_av = factory_.createFresh(CX);
        AbstractValue flag_av = factory_.createFresh(CX);
        fa->getPathFormula().addBounds(flag_av, 0, 0);
        fa->getPostHeap().addEdge(res_av, Access(0u), old_addr);
        fa->getPostHeap().addEdge(res_av, Access(1u), Address(flag_av));
        fa->getPostStack().add(CX, Address(res_av));
        out.push_back(std::move(fail));
      }
    }
    return pruneStates(std::move(out));
  }

  if (auto *IEI = llvm::dyn_cast<llvm::InsertElementInst>(I)) {
    AbstractValue res_av = factory_.createFresh(IEI);
    if (auto vec_opt = ops_.eval(*astate, IEI->getOperand(0), IEI, pred)) {
      ops_.copySubgraph(*astate, astate->getCanonical(vec_opt->addr), res_av, 2);
    }
    if (auto elem_opt = ops_.eval(*astate, IEI->getOperand(1), IEI, pred)) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(IEI->getOperand(2))) {
        astate->getPostHeap().addEdge(
            res_av, Access(static_cast<unsigned>(CI->getZExtValue())),
            *elem_opt);
      }
    }
    astate->getPostStack().add(IEI, Address(res_av));
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *EEI = llvm::dyn_cast<llvm::ExtractElementInst>(I)) {
    if (auto vec_opt = ops_.eval(*astate, EEI->getOperand(0), EEI, pred)) {
      AbstractValue vec_c = astate->getCanonical(vec_opt->addr);
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(EEI->getOperand(1))) {
        Access acc(static_cast<unsigned>(CI->getZExtValue()));
        if (const Address *elem = astate->getPostHeap().findEdge(vec_c, acc)) {
          astate->getPostStack().add(EEI, *elem);
          return pruneSingleState(std::move(exec_state));
        }
      }
    }
    astate->getPostStack().add(EEI, Address(factory_.createFresh(EEI)));
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *SVI = llvm::dyn_cast<llvm::ShuffleVectorInst>(I)) {
    AbstractValue res_av = factory_.createFresh(SVI);
    auto v0_opt = ops_.eval(*astate, SVI->getOperand(0), SVI, pred);
    auto v1_opt = ops_.eval(*astate, SVI->getOperand(1), SVI, pred);
    unsigned v0_elems = 0;
    if (auto *FT = llvm::dyn_cast<llvm::FixedVectorType>(
            SVI->getOperand(0)->getType())) {
      v0_elems = FT->getNumElements();
    }
    auto mask = SVI->getShuffleMask();
    for (unsigned out_i = 0; out_i < mask.size(); ++out_i) {
      int m = mask[out_i];
      if (m < 0)
        continue;
      const Address *src_elem = nullptr;
      if (static_cast<unsigned>(m) < v0_elems && v0_opt) {
        src_elem = astate->getPostHeap().findEdge(
            astate->getCanonical(v0_opt->addr), Access(static_cast<unsigned>(m)));
      } else if (v1_opt && v0_elems > 0) {
        src_elem = astate->getPostHeap().findEdge(
            astate->getCanonical(v1_opt->addr),
            Access(static_cast<unsigned>(m) - v0_elems));
      }
      if (src_elem) {
        astate->getPostHeap().addEdge(res_av, Access(out_i), *src_elem);
      }
    }
    astate->getPostStack().add(SVI, Address(res_av));
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *VAI = llvm::dyn_cast<llvm::VAArgInst>(I)) {
    AbstractValue res_av = factory_.createFresh(VAI);
    if (VAI->getType()->isIntegerTy()) {
      astate->getPathFormula().addIntegerConstraint(res_av);
    }
    astate->getPostStack().add(VAI, Address(res_av));
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *LPI = llvm::dyn_cast<llvm::LandingPadInst>(I)) {
    AbstractValue lp_av = factory_.createFresh(LPI);
    AbstractValue exc_ptr = exec_state.getExceptionValue()
                                ? astate->getCanonical(*exec_state.getExceptionValue())
                                : factory_.createFresh(LPI);
    AbstractValue sel = factory_.createFresh(LPI);
    astate->addNonNull(exc_ptr);
    astate->getPathFormula().addIntegerConstraint(sel);

    const std::string &exc_type = exec_state.getExceptionType();
    if (!exc_type.empty()) {
      astate->setDynamicType(exc_ptr, exc_type);
      std::optional<int64_t> matched_sel;
      for (unsigned i = 0; i < LPI->getNumClauses(); ++i) {
        if (!LPI->isCatch(i))
          continue;
        std::string clause_type =
            detail::normalizeRttiTypeName(LPI->getClause(i));
        if (clause_type.empty()) {
          if (!matched_sel)
            matched_sel = 1;
        } else if (PulseFormula::isSubtypeOf(exc_type, clause_type)) {
          matched_sel = detail::rttiTypeIdForName(clause_type);
          break;
        }
      }
      if (!matched_sel && LPI->isCleanup()) {
        matched_sel = 0;
      }
      if (matched_sel) {
        (void)astate->getPathFormula().addBounds(sel, *matched_sel,
                                                 *matched_sel);
      }
    }

    astate->getPostHeap().addEdge(lp_av, Access(0u), Address(exc_ptr));
    astate->getPostHeap().addEdge(lp_av, Access(1u), Address(sel));
    astate->getPostStack().add(LPI, Address(lp_av));
    return pruneSingleState(std::move(exec_state));
  }

  if (auto *ResI = llvm::dyn_cast<llvm::ResumeInst>(I)) {
    std::optional<AbstractValue> exc_val = exec_state.getExceptionValue();
    std::string exc_type = exec_state.getExceptionType();
    if (auto lp_opt = ops_.eval(*astate, ResI->getValue(), ResI, pred)) {
      AbstractValue lp_c = astate->getCanonical(lp_opt->addr);
      if (const Address *e0 = astate->getPostHeap().findEdge(lp_c, Access(0u))) {
        AbstractValue e0_c = astate->getCanonical(e0->addr);
        exc_val = e0_c;
        if (auto dt = astate->getDynamicType(e0_c)) {
          exc_type = *dt;
        }
      }
    }
    return {ExecutionDomain::exceptionRaised(
        std::make_unique<AbductiveDomain>(astate->clone()), exc_val,
        std::move(exc_type))};
  }

  if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(I))
    return pruneSingleState(handleLoad(LI, std::move(exec_state), pred));

  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(I)) {
    auto result = handleStore(SI, std::move(exec_state), pred);
    if (!result.isStopped() && result.getAstate() &&
        topl_program_.observes(ToplEvent::Kind::ArrayWrite, "#ArrayWrite")) {
      const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
          SI->getPointerOperand()->stripPointerCasts());
      if (gep && gep->getNumIndices()) {
        auto &state = *result.getAstate();
        ToplEvent event;
        event.kind = ToplEvent::Kind::ArrayWrite;
        event.name = "#ArrayWrite";
        event.location = SI;
        event.arguments = {
            toplValue(state, gep->getPointerOperand(), SI, pred),
            toplValue(state, gep->getOperand(gep->getNumOperands() - 1), SI,
                      pred)};
        recordToplEvent(state, std::move(event));
      }
    }
    return pruneSingleState(std::move(result));
  }

  if (auto *CB = llvm::dyn_cast<llvm::CallBase>(I)) {
    const auto *callee = llvm::dyn_cast<llvm::Function>(
        CB->getCalledOperand()->stripPointerCasts());
    const bool observe = callee &&
                         topl_program_.observes(ToplEvent::Kind::Call,
                                                callee->getName());
    ToplEvent event;
    if (observe) {
      event.name = callee->getName().str();
      event.location = CB;
      for (const auto &arg : CB->args())
        event.arguments.push_back(toplValue(*astate, arg.get(), CB, pred));
    }
    auto results = handleCallBase(CB, std::move(exec_state), pred, call_depth);
    if (observe) {
      for (auto &result : results) {
        if (result.isStopped() || result.isExceptionRaised() ||
            !result.getAstate())
          continue;
        ToplEvent completed = event;
        completed.arguments.push_back(
            toplValue(*result.getAstate(), CB, CB, pred));
        recordToplEvent(*result.getAstate(), std::move(completed));
      }
    }
    return pruneStates(std::move(results));
  }

  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(I))
    return pruneSingleState(handleAlloca(AI, std::move(exec_state)));
  if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(I))
    return pruneSingleState(handleReturn(RI, std::move(exec_state)));

  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(I)) {
    if (BO->getType()->isFloatingPointTy()) {
      auto lhs_opt = ops_.eval(*astate, BO->getOperand(0), BO, pred);
      auto rhs_opt = ops_.eval(*astate, BO->getOperand(1), BO, pred);
      if (lhs_opt && rhs_opt) {
        AbstractValue res_av = factory_.getOrCreate(BO);
        AbstractValue lhs_av = astate->getCanonical(lhs_opt->addr);
        AbstractValue rhs_av = astate->getCanonical(rhs_opt->addr);
        astate->getPostStack().add(BO, Address(res_av));
        std::string op;
        switch (BO->getOpcode()) {
        case llvm::Instruction::FAdd:
          op = "+";
          break;
        case llvm::Instruction::FSub:
          op = "-";
          break;
        case llvm::Instruction::FMul:
          op = "*";
          break;
        case llvm::Instruction::FDiv:
          op = "/";
          break;
        default:
          break;
        }
        if (!op.empty()) {
          if (!astate->getPathFormula().addFloatOperation(res_av, lhs_av,
                                                          rhs_av, op)) {
            return {};
          }
        }
      }
      return singleState(std::move(exec_state));
    }
    if (BO->getType()->isIntegerTy()) {
      auto lhs_opt = ops_.eval(*astate, BO->getOperand(0), BO, pred);
      auto rhs_opt = ops_.eval(*astate, BO->getOperand(1), BO, pred);
      if (lhs_opt && rhs_opt) {
        AbstractValue res_av = factory_.getOrCreate(BO);
        AbstractValue lhs_av = astate->getCanonical(lhs_opt->addr);
        AbstractValue rhs_av = astate->getCanonical(rhs_opt->addr);
        unsigned bw = BO->getType()->getIntegerBitWidth();
        astate->getPathFormula().addIntegerConstraint(res_av);
        astate->getPathFormula().addIntegerConstraint(lhs_av);
        astate->getPathFormula().addIntegerConstraint(rhs_av);
        astate->getPathFormula().setBitWidth(res_av, bw);
        astate->getPathFormula().setBitWidth(lhs_av, bw);
        astate->getPathFormula().setBitWidth(rhs_av, bw);
        astate->getPostStack().add(BO, Address(res_av));

        std::string op;
        switch (BO->getOpcode()) {
        case llvm::Instruction::Add:
          op = "+";
          break;
        case llvm::Instruction::Sub:
          op = "-";
          break;
        case llvm::Instruction::Mul:
          op = "*";
          break;
        case llvm::Instruction::SDiv:
        case llvm::Instruction::UDiv:
          op = "/";
          break;
        case llvm::Instruction::SRem:
        case llvm::Instruction::URem:
          op = "%";
          break;
        case llvm::Instruction::And:
          op = "&";
          break;
        case llvm::Instruction::Or:
          op = "|";
          break;
        case llvm::Instruction::Xor:
          op = "^";
          break;
        case llvm::Instruction::Shl:
          op = "<<";
          break;
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
          op = ">>";
          break;
        default:
          break;
        }
        if (!op.empty()) {
          (void)astate->getPathFormula().addArithmeticOperation(res_av, lhs_av,
                                                                rhs_av, op);
        }
      }
    }
    return singleState(std::move(exec_state));
  }

  if (llvm::isa<llvm::ICmpInst>(I) || llvm::isa<llvm::FCmpInst>(I)) {
    const llvm::Instruction *next = I->getNextNode();
    if (auto *BI = next ? llvm::dyn_cast<llvm::BranchInst>(
                              const_cast<llvm::Instruction *>(next))
                        : nullptr) {
      if (BI->isConditional() &&
          BI->getCondition() ==
              static_cast<llvm::Value *>(const_cast<llvm::Instruction *>(I)))
        return singleState(std::move(exec_state));
    }
    return pruneSingleState(handleComparison(I, std::move(exec_state), pred));
  }

  return pruneSingleState(std::move(exec_state));
}

std::vector<ExecutionDomain> PulseChecker::handleLibraryCall(
    const llvm::CallBase *CB, ExecutionDomain exec_state,
    const llvm::BasicBlock *pred, const llvm::Function *resolved_callee) {
  auto result = models_->dispatch(CB, exec_state, pred, resolved_callee);
  if (result.handled) {
    return result.states;
  }
  return {};
}

ExecutionDomain PulseChecker::handleLoad(const llvm::LoadInst *LI,
                                         ExecutionDomain exec_state,
                                         const llvm::BasicBlock *pred) {
  auto *astate = exec_state.getAstate();
  const llvm::Value *ptr_operand = LI->getPointerOperand();

  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ptr_operand)) {
    AbstractValue stack_var = factory_.getOrCreate(AI);
    AbstractValue canon_var = astate->getCanonical(stack_var);

    if (astate->getPostAttrs().has(canon_var, Attribute::Uninitialized)) {
      Trace trace;
      trace.addEvent(LI, "Load from uninitialized variable");
      if (LatentIssue::isManifest(OperationResult::UninitializedRead, *astate,
                                  canon_var)) {
        reportBug(OperationResult::UninitializedRead, LI, canon_var, trace,
                  astate);
        return ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UninitializedRead, std::move(trace));
      } else {
        latent_issues_.emplace_back(OperationResult::UninitializedRead,
                                    LatentIssue::issueKindFromResult(
                                        OperationResult::UninitializedRead),
                                    canon_var, LI, trace.clone());
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back(), OperationResult::UninitializedRead,
            std::move(trace));
      }
    }

    if (auto *stack_addr = astate->getPostStack().find(AI)) {
      AbstractValue loaded_canon = astate->getCanonical(stack_addr->addr);
      if (astate->getPostAttrs().has(loaded_canon, Attribute::Invalid)) {
        Trace trace = Trace::fromValueHistory(stack_addr->history);
        trace.addEvent(LI, "Load of freed pointer");
        if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                    loaded_canon)) {
          reportBug(OperationResult::UseAfterFree, LI, loaded_canon, trace,
                    astate);
          return ExecutionDomain::abortProgram(
              std::make_unique<AbductiveDomain>(astate->clone()),
              OperationResult::UseAfterFree, std::move(trace));
        } else {
          latent_issues_.emplace_back(
              OperationResult::UseAfterFree,
              LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
              loaded_canon, LI, trace.clone());
          return ExecutionDomain::latentAbortProgram(
              std::make_unique<AbductiveDomain>(astate->clone()),
              &latent_issues_.back(), OperationResult::UseAfterFree,
              std::move(trace));
        }
      }
      astate->getPostStack().add(LI, *stack_addr);
    } else {
      AbstractValue fresh = factory_.createFresh(LI);
      Address addr(fresh);
      astate->getPostStack().add(LI, addr);
    }
    return exec_state;
  }

  // Track reads from candidate const-refable parameters.
  if (const auto *Arg =
          llvm::dyn_cast<llvm::Argument>(ptr_operand->stripPointerCasts())) {
    analysis_non_disj_.markParamRead(Arg);
  } else if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(
                 ptr_operand->stripPointerCasts())) {
    if (const auto *Arg = llvm::dyn_cast<llvm::Argument>(
            GEP->getPointerOperand()->stripPointerCasts())) {
      analysis_non_disj_.markParamRead(Arg);
    }
  }

  auto ptr_opt = ops_.eval(*astate, ptr_operand, LI, pred);
  if (!ptr_opt) {
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr_operand)) {
      auto base_opt = ops_.eval(*astate, GEP->getPointerOperand(), LI, pred);
      if (base_opt) {
        AbstractValue base_canon = astate->getCanonical(base_opt->addr);
        if (astate->getPostAttrs().has(base_canon, Attribute::Invalid)) {
          Trace trace = Trace::fromValueHistory(base_opt->history);
          trace.addEvent(LI, "Array access through invalid pointer");
          if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                      base_canon)) {
            reportBug(OperationResult::UseAfterFree, LI, base_canon, trace,
                      astate);
            return ExecutionDomain::abortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                OperationResult::UseAfterFree, std::move(trace));
          } else {
            latent_issues_.emplace_back(
                OperationResult::UseAfterFree,
                LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
                base_canon, LI, trace.clone());
            return ExecutionDomain::latentAbortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                &latent_issues_.back(), OperationResult::UseAfterFree,
                std::move(trace));
          }
        }
        if (astate->getPathFormula().isNull(base_canon) ||
            (astate->getPostAttrs().has(base_canon, Attribute::Null) &&
             !astate->getPathFormula().isNonNull(base_canon))) {
          Trace trace = Trace::fromValueHistory(base_opt->history);
          trace.addEvent(LI, "Array access through null pointer");
          if (LatentIssue::isManifest(OperationResult::NullDereference, *astate,
                                      base_canon)) {
            reportBug(OperationResult::NullDereference, LI, base_canon, trace,
                      astate);
            return ExecutionDomain::abortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                OperationResult::NullDereference, std::move(trace));
          } else {
            latent_issues_.emplace_back(
                OperationResult::NullDereference,
                LatentIssue::issueKindFromResult(
                    OperationResult::NullDereference),
                base_canon, LI, trace.clone());
            return ExecutionDomain::latentAbortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                &latent_issues_.back(), OperationResult::NullDereference,
                std::move(trace));
          }
        }
      }
    }
    return exec_state;
  }

  AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
  if (astate->getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(LI, "Dereference of invalid pointer");
    if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                canon_ptr)) {
      reportBug(OperationResult::UseAfterFree, LI, canon_ptr, trace, astate);
      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          OperationResult::UseAfterFree, std::move(trace));
    } else {
      latent_issues_.emplace_back(
          OperationResult::UseAfterFree,
          LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
          canon_ptr, LI, trace.clone());
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back(), OperationResult::UseAfterFree,
          std::move(trace));
    }
  }

  auto read_result = ops_.readDeref(*astate, *ptr_opt, LI);
  OperationResult result = read_result.first;
  auto value_opt = read_result.second;

  if (result != OperationResult::Success) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(LI, "Load from invalid address");

    if (LatentIssue::isManifest(result, *astate,
                                astate->getCanonical(ptr_opt->addr))) {
      reportBug(result, LI, ptr_opt->addr, trace, astate);
      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()), result,
          std::move(trace));
    } else {
      latent_issues_.emplace_back(result,
                                  LatentIssue::issueKindFromResult(result),
                                  ptr_opt->addr, LI, trace.clone());
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back(), result, std::move(trace));
    }
  }
  if (value_opt) {
    AbstractValue loaded_canon = astate->getCanonical(value_opt->addr);
    if (astate->getPostAttrs().has(loaded_canon, Attribute::Invalid)) {
      Trace trace = Trace::fromValueHistory(value_opt->history);
      trace.addEvent(LI, "Load of invalid/freed pointer value");
      if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                  loaded_canon)) {
        reportBug(OperationResult::UseAfterFree, LI, loaded_canon, trace,
                  astate);
        return ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UseAfterFree, std::move(trace));
      } else {
        latent_issues_.emplace_back(
            OperationResult::UseAfterFree,
            LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
            loaded_canon, LI, trace.clone());
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back(), OperationResult::UseAfterFree,
            std::move(trace));
      }
    }
    astate->getPostStack().add(LI, *value_opt);
  }
  return exec_state;
}

ExecutionDomain PulseChecker::handleStore(const llvm::StoreInst *SI,
                                          ExecutionDomain exec_state,
                                          const llvm::BasicBlock *pred) {
  auto *astate = exec_state.getAstate();
  auto value_opt = ops_.eval(*astate, SI->getValueOperand(), SI, pred);
  if (!value_opt)
    return exec_state;

  auto isReallocResult = [](const Address &addr) {
    for (const auto &event : addr.history.getEvents()) {
      if (event.kind != ValueHistory::EventKind::Allocation ||
          !event.location) {
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallBase>(event.location);
      if (!call)
        continue;
      auto *callee = call->getCalledFunction();
      if (callee && callee->getName() == "realloc") {
        return true;
      }
    }
    return false;
  };

  const llvm::Value *ptr_operand = SI->getPointerOperand();

  auto maybeReportStackEscape = [&](const Address &stored_value,
                                    const Address &dest_ptr) {
    if (!SI->getValueOperand()->getType()->isPointerTy())
      return;
    AbstractValue canon_value = astate->getCanonical(stored_value.addr);
    if (!astate->getPostAttrs().has(canon_value, Attribute::Stack))
      return;
    AbstractValue canon_dest = astate->getCanonical(dest_ptr.addr);
    const bool dest_is_global =
        astate->getPostAttrs().has(canon_dest, Attribute::Global);
    const bool dest_is_heap =
        astate->getPostAttrs().has(canon_dest, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_dest, Attribute::Stack);
    if (!dest_is_global && !dest_is_heap)
      return;

    Trace trace = Trace::fromValueHistory(stored_value.history);
    trace.addEvent(SI, "Storing stack-derived address into non-stack memory");
    auto diag = std::make_unique<StackVariableAddressEscape>(
        SI, canon_value, "Stack address escapes via store",
        "Do not store addresses of local variables into heap/global memory.",
        std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
  };

  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ptr_operand)) {
    AbstractValue stack_var = factory_.getOrCreate(AI);
    AbstractValue canon_var = astate->getCanonical(stack_var);

    astate->getPostAttrs().remove(canon_var, Attribute::Uninitialized);
    astate->markWrittenTo(canon_var, SI);

    AbstractValue canon_value = astate->getCanonical(value_opt->addr);
    if (astate->getPostAttrs().has(canon_value, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_value, Attribute::Invalid)) {
      astate->getPostAttrs().remove(canon_var, Attribute::Invalid);
    }

    std::optional<AbstractValue> overwritten_av;
    if (auto *old_addr = astate->getPostStack().find(AI)) {
      overwritten_av = astate->getCanonical(old_addr->addr);
    }

    if (astate->getPostAttrs().has(canon_value, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_value, Attribute::Invalid) &&
        isReallocResult(*value_opt)) {
      if (auto *old_addr = astate->getPostStack().find(AI)) {
        ops_.invalidate(*astate, *old_addr, SI, InvalidationKind::Realloc);
      }
      astate->getPostAttrs().remove(canon_var, Attribute::Invalid);
    }

    Address stored_addr = *value_opt;
    stored_addr.history.addEvent(ValueHistory::EventKind::Store, SI,
                                 SI->getFunction());
    astate->getPostStack().add(AI, stored_addr);
    clearUnreachableAt(*astate, canon_value);

    // If storing an aggregate SSA value (e.g. built via InsertValueInst) into
    // a stack alloca, mirror its field subgraph onto the alloca address too.
    if (SI->getValueOperand()->getType()->isStructTy() ||
        SI->getValueOperand()->getType()->isArrayTy()) {
      ops_.copySubgraph(*astate, canon_value, canon_var, 3);
    }

    if (overwritten_av && !(*overwritten_av == canon_value)) {
      markUnreachableAllocationIfDead(*astate, *overwritten_av, SI);
    }

    // Track expensive aggregate copies into local stack variables.
    if (isExpensiveAggregateType(SI->getValueOperand()->getType(), module_) &&
        !llvm::isa<llvm::Constant>(SI->getValueOperand())) {
      std::string var_name = AI->hasName() ? AI->getName().str() : "local_copy";
      analysis_non_disj_.recordAggregateCopy(
          SI, canon_var, var_name,
          formatTypeName(SI->getValueOperand()->getType()),
          astate->getTimestampCounter());
    }
    return exec_state;
  }

  auto ptr_opt = ops_.eval(*astate, ptr_operand, SI, pred);
  if (!ptr_opt)
    return exec_state;

  maybeReportStackEscape(*value_opt, *ptr_opt);

  AbstractValue canon_dest = astate->getCanonical(ptr_opt->addr);
  AbstractValue canon_val = astate->getCanonical(value_opt->addr);
  std::optional<AbstractValue> overwritten_heap_av;
  if (const Address *old_edge = astate->getPostHeap().findEdge(
          canon_dest, Access(AccessKind::Dereference))) {
    overwritten_heap_av = astate->getCanonical(old_edge->addr);
  }

  auto res = ops_.writeDeref(*astate, *ptr_opt, *value_opt, SI);
  if (res != OperationResult::Success) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(SI, "Store to invalid address");

    if (LatentIssue::isManifest(res, *astate,
                                astate->getCanonical(ptr_opt->addr))) {
      reportBug(res, SI, ptr_opt->addr, trace, astate);
      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()), res,
          std::move(trace));
    } else {
      latent_issues_.emplace_back(res, LatentIssue::issueKindFromResult(res),
                                  ptr_opt->addr, SI, trace.clone());
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back(), res, std::move(trace));
    }
  }

  clearUnreachableAt(*astate, canon_val);
  if (SI->getValueOperand()->getType()->isStructTy() ||
      SI->getValueOperand()->getType()->isArrayTy()) {
    ops_.copySubgraph(*astate, canon_val, canon_dest, 3);
  }
  if (overwritten_heap_av && !(*overwritten_heap_av == canon_val)) {
    markUnreachableAllocationIfDead(*astate, *overwritten_heap_av, SI);
  }

  if (isExpensiveAggregateType(SI->getValueOperand()->getType(), module_) &&
      !llvm::isa<llvm::Constant>(SI->getValueOperand()) &&
      !llvm::isa<llvm::PHINode>(SI->getValueOperand())) {
    AbstractValue dest_c = astate->getCanonical(ptr_opt->addr);
    std::string var_name =
        astate->getDecompilerName(dest_c).value_or(
            ptr_operand->hasName() ? ptr_operand->getName().str() : "copy");
    analysis_non_disj_.recordAggregateCopy(
        SI, dest_c, var_name, formatTypeName(SI->getValueOperand()->getType()),
        astate->getTimestampCounter());
  }
  return exec_state;
}

std::vector<ExecutionDomain>
PulseChecker::handleCall(const llvm::CallInst *CI, ExecutionDomain exec_state,
                         const llvm::BasicBlock *pred, unsigned call_depth) {
  return handleCallBase(CI, std::move(exec_state), pred, call_depth);
}

std::vector<ExecutionDomain>
PulseChecker::handleCallBase(const llvm::CallBase *CB,
                             ExecutionDomain exec_state,
                             const llvm::BasicBlock *pred,
                             unsigned call_depth) {
  (void)call_depth;
  auto *astate = exec_state.getAstate();
  if (!astate) {
    return singleState(std::move(exec_state));
  }

  // Handle LLVM intrinsics (eh.typeid.for, lifetime, memcpy/memmove/memset).
  if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(
          const_cast<llvm::CallBase *>(CB))) {
    const auto iid = II->getIntrinsicID();
    if (auto *DVI = llvm::dyn_cast<llvm::DbgVariableIntrinsic>(II)) {
      if (const llvm::DILocalVariable *var = DVI->getVariable()) {
        if (const llvm::Value *loc_val = DVI->getVariableLocationOp(0)) {
          if (auto addr_opt = ops_.eval(*astate, loc_val, CB, pred)) {
            astate->setDecompilerName(astate->getCanonical(addr_opt->addr),
                                      var->getName().str());
          }
        }
      }
      return singleState(std::move(exec_state));
    }
    if (iid == llvm::Intrinsic::eh_typeid_for && CB->arg_size() >= 1) {
      std::string rtti_name =
          detail::normalizeRttiTypeName(CB->getArgOperand(0));
      int64_t tid = detail::rttiTypeIdForName(rtti_name);
      AbstractValue ret_val = factory_.createFresh(CB);
      astate->getPathFormula().addIntegerConstraint(ret_val);
      (void)astate->getPathFormula().addBounds(ret_val, tid, tid);
      astate->getPostStack().add(CB, Address(ret_val));
      return singleState(std::move(exec_state));
    }
    if (iid == llvm::Intrinsic::lifetime_start ||
        iid == llvm::Intrinsic::lifetime_end) {
      if (CB->arg_size() >= 2) {
        auto ptr_opt = ops_.eval(*astate, CB->getArgOperand(1), CB, pred);
        if (ptr_opt) {
          AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
          if (iid == llvm::Intrinsic::lifetime_start) {
            astate->getPostAttrs().remove(canon_ptr, Attribute::Invalid);
            astate->getPostAttrs().add(canon_ptr, Attribute::Uninitialized);
          } else {
            ops_.invalidate(*astate, *ptr_opt, CB,
                            InvalidationKind::GoneOutOfScope);
          }
        }
      }
      return singleState(std::move(exec_state));
    }

    if (iid == llvm::Intrinsic::memcpy || iid == llvm::Intrinsic::memmove ||
        iid == llvm::Intrinsic::memset) {
      auto *a = exec_state.getAstate();
      if (!a)
        return singleState(std::move(exec_state));
      auto check_len = [&](const Address &addr, const llvm::Value *len_val,
                           const char *detail) {
        auto *CI_len = llvm::dyn_cast<llvm::ConstantInt>(len_val);
        if (!CI_len || CI_len->isNegative() || CI_len->getBitWidth() > 64)
          return;
        uint64_t len = CI_len->getZExtValue();
        AbstractValue canon = a->getCanonical(addr.addr);
        auto size_opt = a->getAllocationSize(canon);
        if (size_opt && len > *size_opt) {
          Trace trace = Trace::fromValueHistory(addr.history);
          trace.addEvent(CB, detail);
          reportBug(OperationResult::OutOfBounds, CB, canon, trace, a);
          a->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      };

      auto dest_opt = ops_.eval(*a, CB->getArgOperand(0), CB, pred);
      if (!dest_opt)
        return singleState(std::move(exec_state));
      if (iid == llvm::Intrinsic::memset) {
        check_len(*dest_opt, CB->getArgOperand(2),
                  "memset writes beyond destination buffer");
        auto val_opt = ops_.eval(*a, CB->getArgOperand(1), CB, pred);
        if (val_opt) {
          ops_.writeDeref(*a, *dest_opt, *val_opt, CB);
        }
        a->getPostStack().add(CB, *dest_opt);
        auto *dest_val = CB->getArgOperand(0);
        while (dest_val) {
          if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(dest_val)) {
            AbstractValue base_canon = a->getCanonical(factory_.getOrCreate(AI));
            a->getPostAttrs().remove(base_canon, Attribute::Uninitialized);
            a->markWrittenTo(base_canon, CB);
            break;
          }
          if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(dest_val)) {
            dest_val = BC->getOperand(0);
          } else if (auto *GEP =
                         llvm::dyn_cast<llvm::GetElementPtrInst>(dest_val)) {
            dest_val = GEP->getPointerOperand();
          } else {
            break;
          }
        }
        return singleState(std::move(exec_state));
      }

      auto src_opt = ops_.eval(*a, CB->getArgOperand(1), CB, pred);
      if (src_opt) {
        check_len(*dest_opt, CB->getArgOperand(2),
                  "memcpy/memmove writes beyond destination buffer");
        check_len(*src_opt, CB->getArgOperand(2),
                  "memcpy/memmove reads beyond source buffer");
        AbstractValue src_c = a->getCanonical(src_opt->addr);
        AbstractValue dst_c = a->getCanonical(dest_opt->addr);
        const bool src_invalid_or_null =
            a->getPostAttrs().has(src_c, Attribute::Invalid) ||
            a->getPostAttrs().has(src_c, Attribute::Null) ||
            a->getPathFormula().isNull(src_c);
        auto src_read = ops_.readDeref(*a, *src_opt, CB);
        if (src_read.first == OperationResult::Success) {
          AbstractValue val_to_write =
              src_read.second ? src_read.second->addr : factory_.createFresh(CB);
          ops_.writeDeref(*a, *dest_opt, Address(val_to_write), CB);
        }
        if (!src_invalid_or_null) {
          ops_.copySubgraph(*a, src_c, dst_c, 3);
          TaintOperations::propagate(*a, src_c, dst_c, CB);
          a->getPostStack().add(CB, *dest_opt);
        }
      }
      // Track expensive struct/array memcpy into local stack alloca as copy.
      auto *dest_val = CB->getArgOperand(0);
      while (dest_val) {
        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(dest_val)) {
          AbstractValue base_canon = a->getCanonical(factory_.getOrCreate(AI));
          a->getPostAttrs().remove(base_canon, Attribute::Uninitialized);
          if (isExpensiveAggregateType(AI->getAllocatedType(), module_)) {
            std::string var_name =
                AI->hasName() ? AI->getName().str() : "local_copy";
            analysis_non_disj_.recordAggregateCopy(
                CB, base_canon, var_name,
                formatTypeName(AI->getAllocatedType()),
                a->getTimestampCounter());
          }
          break;
        }
        if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(dest_val)) {
          dest_val = BC->getOperand(0);
        } else if (auto *GEP =
                       llvm::dyn_cast<llvm::GetElementPtrInst>(dest_val)) {
          dest_val = GEP->getPointerOperand();
        } else {
          break;
        }
      }
      return singleState(std::move(exec_state));
    }
  }

  const llvm::Function *F = CB->getCalledFunction();
  if (!F) {
    if (const auto *direct = llvm::dyn_cast<llvm::Function>(
            CB->getCalledOperand()->stripPointerCasts())) {
      F = direct;
    }
  }
  if (!F) {
    // Resolve indirect calls (function pointers / vtable slots) through the
    // abstract heap and dynamic types; if the callee is an unconstrained formal
    // in pre, mark DynamicTypeNeeded for caller-driven specialization.
    auto callee_opt = ops_.eval(*astate, CB->getCalledOperand(), CB, pred);
    if (callee_opt) {
      AbstractValue canon_callee = astate->getCanonical(callee_opt->addr);
      if (const auto *fn =
              llvm::dyn_cast_or_null<llvm::Function>(canon_callee.getValue())) {
        F = fn;
      } else if (auto dyn_ty = astate->getDynamicType(canon_callee)) {
        if (module_) {
          F = module_->getFunction(*dyn_ty);
        }
      }
      if (!F && astate->isInPre(canon_callee)) {
        astate->addNeedDynamicTypeSpecialization(canon_callee);
      }
      // For virtual dispatch (`%vt = load %vptr; %slot = gep %vt, k; %fn = load %slot`),
      // also mark the vtable pointer AV (`%vt`) if it comes from pre so callers
      // with concrete `__vtable__:<gv>:<off>` specialize the callee.
      if (!F) {
        const llvm::Value *cur_v = CB->getCalledOperand()->stripPointerCasts();
        for (unsigned hops = 0; hops < 4 && cur_v; ++hops) {
          if (const auto *LI = llvm::dyn_cast<llvm::LoadInst>(cur_v)) {
            cur_v = LI->getPointerOperand()->stripPointerCasts();
          } else if (const auto *GEP =
                         llvm::dyn_cast<llvm::GEPOperator>(cur_v)) {
            cur_v = GEP->getPointerOperand()->stripPointerCasts();
          } else {
            break;
          }
          if (auto hop_opt = ops_.eval(*astate, cur_v, CB, pred)) {
            AbstractValue hop_c = astate->getCanonical(hop_opt->addr);
            if (astate->isInPre(hop_c)) {
              astate->addNeedDynamicTypeSpecialization(hop_c);
            }
          }
        }
      }
    }
  }
  if (!F) {
    astate->addSkippedCall("<indirect>");
    astate->declareUnknownValues();
    modelUnknownCallEffects(ops_, *astate, CB, pred, factory_);
    return singleState(std::move(exec_state));
  }

  std::string func_name = F->getName().str();
  if (!transitive_access_policies_.empty()) {
    astate->getTransitiveInfo().recordCallResolution(
        CB, CB->getFunction(), TransitiveInfo::CallResolution::Resolved, {F});
  }

  // No-return termination (`exit`, `_exit`, `abort`, `__assert_fail`, or
  // LLVM `noreturn` attribute): stop path cleanly without emitting spurious
  // post-exit leaks or UAFs.
  if (F->doesNotReturn() || func_name == "exit" || func_name == "_exit" ||
      func_name == "_Exit" || func_name == "abort" ||
      func_name == "__assert_fail" || func_name == "__assert_rtn" ||
      func_name == "_ZSt9terminatev") {
    return {ExecutionDomain::abortProgram(
        std::make_unique<AbductiveDomain>(astate->clone()),
        OperationResult::Success, Trace())};
  }

  const TaintConfig &taint_cfg = TaintOperations::getConfig();

  // Taint sources & source blocks (driven by unified TaintConfig ProcedureUnit
  // targets, including optional field_index for field-sensitive tainting).
  auto source_units = taint_cfg.matchSources(F);
  auto block_units = taint_cfg.matchSourceBlocks(F);
  source_units.insert(source_units.end(), block_units.begin(),
                      block_units.end());
  if (!source_units.empty()) {
    bool tainted_return = false;
    bool tainted_any_arg = false;
    for (const ProcedureUnit *unit : source_units) {
      std::vector<TaintKind> kinds =
          unit->kinds.empty() ? std::vector<TaintKind>{TaintKind::UserInput()}
                              : unit->kinds;
      if (unit->appliesToReturn()) {
        AbstractValue ret_val = factory_.createFresh(CB);
        if (unit->field_index) {
          TaintOperations::taintField(*astate, ret_val, *unit->field_index,
                                      kinds, CB, factory_);
        } else {
          TaintOperations::taint(*astate, ret_val, kinds, CB);
        }
        astate->getPostStack().add(CB, Address(ret_val));
        tainted_return = true;
      } else {
        for (unsigned i = 0; i < CB->arg_size(); ++i) {
          if (!unit->appliesToArgument(i))
            continue;
          auto arg_opt = ops_.eval(*astate, CB->getArgOperand(i), CB, pred);
          if (arg_opt) {
            AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
            astate->getPostAttrs().remove(canon_arg, Attribute::Uninitialized);
            if (unit->field_index) {
              TaintOperations::taintField(*astate, canon_arg,
                                          *unit->field_index, kinds, CB,
                                          factory_);
            } else {
              TaintOperations::taint(*astate, canon_arg, kinds, CB);
            }
            tainted_any_arg = true;
          }
        }
      }
    }
    if ((tainted_return || tainted_any_arg) && F->isDeclaration() &&
        !models_->hasModel(F)) {
      if (!tainted_return && !CB->getType()->isVoidTy()) {
        AbstractValue ret_val = factory_.createFresh(CB);
        Address ret_addr(ret_val);
        ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CB,
                                  CB->getFunction());
        astate->getPostStack().add(CB, ret_addr);
        if (CB->getType()->isIntegerTy()) {
          astate->getPathFormula().addIntegerConstraint(ret_val);
        }
      }
      return singleState(std::move(exec_state));
    }
  }

  // Allocation sources matching constructor / factory names.
  std::vector<TaintKind> alloc_src_kinds =
      taint_cfg.matchAllocationSourceForName(func_name);
  if (!alloc_src_kinds.empty() && CB->getType()->isVoidTy() &&
      CB->arg_size() >= 1) {
    if (auto this_opt = ops_.eval(*astate, CB->getArgOperand(0), CB, pred)) {
      TaintOperations::taint(*astate, astate->getCanonical(this_opt->addr),
                             alloc_src_kinds, CB);
    }
  }

  // Taint sinks (both intraprocedural check AND interprocedural
  // MustNotBeTainted abduction, supporting field-sensitive `field_index`!)
  auto sink_units = taint_cfg.matchSinks(F);
  if (!sink_units.empty()) {
    std::set<std::pair<unsigned, std::optional<unsigned>>> checked_args;
    for (const ProcedureUnit *unit : sink_units) {
      for (unsigned i = 0; i < CB->arg_size(); ++i) {
        if (!unit->appliesToArgument(i) ||
            !checked_args.insert({i, unit->field_index}).second)
          continue;
        auto arg_opt = ops_.eval(*astate, CB->getArgOperand(i), CB, pred);
        if (arg_opt) {
          AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
          bool reported =
              unit->field_index
                  ? TaintOperations::checkSinkField(
                        *astate, canon_arg, *unit->field_index, func_name, CB)
                  : TaintOperations::checkSink(*astate, canon_arg, func_name,
                                               CB);
          if (!reported && !unit->field_index && astate->isInPre(canon_arg)) {
            Trace sink_trace;
            sink_trace.addEvent(CB, "Taint sink: " + func_name);
            std::vector<TaintKind> kinds =
                unit->kinds.empty()
                    ? std::vector<TaintKind>{TaintKind::UserInput()}
                    : unit->kinds;
            astate->abduceMustNotBeTainted(
                canon_arg,
                MustNotBeTaintedSink(func_name, std::move(kinds), CB,
                                     std::move(sink_trace),
                                     astate->nextTimestamp()));
          }
        }
      }
    }
  }

  // Taint sanitizers (both local sanitization AND interprocedural post record,
  // supporting field-sensitive `field_index`).
  auto sanitizer_units = taint_cfg.matchSanitizers(F);
  if (!sanitizer_units.empty()) {
    for (const ProcedureUnit *unit : sanitizer_units) {
      TaintKind sanitizer_kind =
          unit->kinds.empty() ? TaintKind::Unknown() : unit->kinds.front();
      for (unsigned i = 0; i < CB->arg_size(); ++i) {
        if (!unit->appliesToArgument(i))
          continue;
        auto arg_opt = ops_.eval(*astate, CB->getArgOperand(i), CB, pred);
        if (arg_opt) {
          AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
          if (unit->field_index) {
            TaintOperations::sanitizeField(*astate, canon_arg,
                                           *unit->field_index, sanitizer_kind,
                                           CB);
          } else {
            TaintOperations::sanitize(*astate, canon_arg, sanitizer_kind, CB);
            astate->addTaintSanitized(
                canon_arg,
                SanitizerRecord{sanitizer_kind, CB, astate->nextTimestamp()});
          }
        }
      }
    }
  }

  // Taint propagators (for unmodeled declaration propagators)
  auto prop_units = taint_cfg.matchPropagators(F);
  if (!prop_units.empty() && F->isDeclaration() && !models_->hasModel(F)) {
    std::vector<AbstractValue> src_avs;
    for (unsigned i = 0; i < CB->arg_size(); ++i) {
      auto arg_opt = ops_.eval(*astate, CB->getArgOperand(i), CB, pred);
      if (arg_opt) {
        src_avs.push_back(astate->getCanonical(arg_opt->addr));
      }
    }
    if (!CB->getType()->isVoidTy()) {
      AbstractValue ret_val = factory_.createFresh(CB);
      Address ret_addr(ret_val);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CB,
                                CB->getFunction());
      astate->getPostStack().add(CB, ret_addr);
      for (AbstractValue src : src_avs) {
        TaintOperations::propagate(*astate, src, ret_val, CB);
        if (astate->isInPre(src)) {
          astate->addPropagateTaintFrom(
              ret_val, PropagateTaintFromInfo{{src}, func_name});
        }
      }
      return singleState(std::move(exec_state));
    }
  }

  // Try library models first (forwarding resolved indirect callee if any).
  // Fast O(1) lookup avoids cloning exec_state on every user-function call.
  if (models_->hasModel(F)) {
    auto model_res = models_->dispatch(CB, exec_state, pred, F);
    if (model_res.handled) {
      auto &lib_result = model_res.states;
      if (!lib_result.empty()) {
        // Check if an allocator result (malloc/operator new) is cast to a
        // struct type registered in `allocation_sources_` (or factory returns
        // one).
        std::vector<TaintKind> ret_alloc_kinds = alloc_src_kinds;
        if (ret_alloc_kinds.empty() && CB->getType()->isPointerTy()) {
          for (const llvm::User *U : CB->users()) {
            if (const auto *BC = llvm::dyn_cast<llvm::BitCastInst>(U)) {
              if (auto *PT = llvm::dyn_cast<llvm::PointerType>(BC->getType())) {
                if (auto *ST = llvm::dyn_cast<llvm::StructType>(
                        PT->getPointerElementType())) {
                  if (ST->hasName()) {
                    ret_alloc_kinds = taint_cfg.matchAllocationSourceForName(
                        ST->getName().str());
                    if (!ret_alloc_kinds.empty())
                      break;
                  }
                }
              }
            }
          }
        }
        if (!ret_alloc_kinds.empty() && !CB->getType()->isVoidTy()) {
          for (auto &st : lib_result) {
            if (auto *a = st.getAstate()) {
              if (const Address *ret_addr = a->getPostStack().find(CB)) {
                TaintOperations::taint(*a, a->getCanonical(ret_addr->addr),
                                       ret_alloc_kinds, CB);
              }
            }
          }
        }
      }
      return lib_result;
    }
  }

  // Fallback for malloc/calloc/realloc/free if invoked via InvokeInst before
  // Step 5 upgrades PulseModels to CallBase.
  if (F->getName() == "malloc" || F->getName() == "calloc" ||
      F->getName() == "realloc") {
    AbstractValue av = factory_.createFresh(CB);
    AllocatorKind ak = (F->getName() == "realloc") ? AllocatorKind::CRealloc
                                                   : AllocatorKind::CMalloc;
    ops_.allocate(*astate, av, CB, ak);
    astate->addNonNull(av);
    if (F->getName() == "malloc" && CB->arg_size() >= 1) {
      if (auto *CI0 = llvm::dyn_cast<llvm::ConstantInt>(CB->getArgOperand(0))) {
        if (!CI0->isNegative() && CI0->getBitWidth() <= 64) {
          astate->setAllocationSize(av, CI0->getZExtValue());
        }
      }
    } else if (F->getName() == "calloc" && CB->arg_size() >= 2) {
      auto *n = llvm::dyn_cast<llvm::ConstantInt>(CB->getArgOperand(0));
      auto *s = llvm::dyn_cast<llvm::ConstantInt>(CB->getArgOperand(1));
      if (n && s && !n->isNegative() && !s->isNegative() &&
          n->getBitWidth() <= 64 && s->getBitWidth() <= 64) {
        __int128 prod = static_cast<__int128>(n->getZExtValue()) *
                        static_cast<__int128>(s->getZExtValue());
        if (prod >= 0 && prod <= std::numeric_limits<uint64_t>::max()) {
          astate->setAllocationSize(av, static_cast<uint64_t>(prod));
        }
      }
    } else if (F->getName() == "realloc" && CB->arg_size() >= 2) {
      if (auto *CI1 = llvm::dyn_cast<llvm::ConstantInt>(CB->getArgOperand(1))) {
        if (!CI1->isNegative() && CI1->getBitWidth() <= 64) {
          astate->setAllocationSize(av, CI1->getZExtValue());
        }
      }
    }
    astate->getPostStack().add(CB, Address(av));
    return singleState(std::move(exec_state));
  }
  if (F->getName() == "free") {
    if (CB->arg_size() > 0) {
      auto ptr_opt = ops_.eval(*astate, CB->getArgOperand(0), CB, pred);
      if (ptr_opt) {
        ops_.invalidate(*astate, *ptr_opt, CB, InvalidationKind::CFree);
      }
    }
    return singleState(std::move(exec_state));
  }

  // Helper to check whether a recursive call re-enters `caller_fn` with
  // unchanged entry formal arguments on a manifest path (Infer
  // PulseMutualRecursion).
  auto argsMatchEntryFormals = [](const AbductiveDomain &state,
                                  const llvm::Function *caller_fn,
                                  const std::vector<AbstractValue> &args) {
    if (!caller_fn || args.size() != caller_fn->arg_size()) {
      return false;
    }
    unsigned idx = 0;
    for (const auto &Arg : caller_fn->args()) {
      const Address *pre_addr = state.getPreStack().find(&Arg);
      if (!pre_addr) {
        return false;
      }
      AbstractValue formal_c = state.getCanonical(pre_addr->addr);
      AbstractValue actual_c = state.getCanonical(args[idx]);
      if (!state.getPathFormula().areEqual(formal_c, actual_c)) {
        return false;
      }
      ++idx;
    }
    return true;
  };

  // Mutual recursion cycle handling: record structured edge & diagnose
  // manifest direct infinite recursion with unchanged arguments.
  if (current_scc_.count(F) > 0) {
    std::vector<AbstractValue> actual_avs;
    actual_avs.reserve(CB->arg_size());
    for (unsigned i = 0; i < CB->arg_size(); ++i) {
      if (auto arg_opt = ops_.eval(*astate, CB->getArgOperand(i), CB, pred)) {
        actual_avs.push_back(astate->getCanonical(arg_opt->addr));
      } else {
        actual_avs.push_back(factory_.createFresh(CB));
      }
    }
    const llvm::Function *caller_fn = CB->getFunction();
    astate->addRecursiveCall(F->getName().str());
    astate->addRecursiveCallEdge(
        RecursiveCallEdge{F, caller_fn, CB, actual_avs});

    auto is_allocated = [astate](AbstractValue v) {
      return astate->isAllocated(v);
    };
    if (F == caller_fn && astate->getPathFormula().isManifest(is_allocated) &&
        argsMatchEntryFormals(*astate, caller_fn, actual_avs)) {
      Trace cycle_trace;
      cycle_trace.addEvent(
          CB, "Direct recursive call to " + F->getName().str() +
                  " with identical arguments");
      auto diag = std::make_unique<MutualRecursionCycle>(
          CB, F->getName().str() + " -> " + F->getName().str(),
          std::move(cycle_trace));
      DiagnosticManager::getInstance().report(std::move(diag));
    }

    astate->addSkippedCall(F->getName().str());
    astate->declareUnknownValues();
    modelUnknownCallEffects(ops_, *astate, CB, pred, factory_);
    return singleState(std::move(exec_state));
  }

  if (F->isDeclaration()) {
    astate->addSkippedCall(F->getName().str());
    astate->declareUnknownValues();
    modelUnknownCallEffects(ops_, *astate, CB, pred, factory_);
    if (!alloc_src_kinds.empty() && !CB->getType()->isVoidTy()) {
      if (const Address *ret_addr = astate->getPostStack().find(CB)) {
        TaintOperations::taint(*astate, astate->getCanonical(ret_addr->addr),
                               alloc_src_kinds, CB);
      }
    }
    return singleState(std::move(exec_state));
  }

  // On-demand analysis for indirectly resolved functions not visited earlier
  // in static direct-call-graph order.
  if (!summary_manager_.hasSummary(F) && active_functions_.count(F) == 0) {
    analyzeFunction(F);
  }

  if (summary_manager_.hasSummary(F)) {
    PulseLogger::incrementCounter("summaries.applied");
    auto summary_results = applySummaryImproved(F, exec_state, CB, pred);
    if (!summary_results.empty()) {
      // Check for manifest mutual recursion cycles (caller -> F -> ... ->
      // caller with unchanged entry arguments).
      const llvm::Function *caller_fn = CB->getFunction();
      for (const auto &res_state : summary_results) {
        const AbductiveDomain *res_a = res_state.getAstate();
        if (!res_a || res_a->getRecursiveCallEdges().empty())
          continue;
        auto is_allocated = [res_a](AbstractValue v) {
          return res_a->isAllocated(v);
        };
        if (!res_a->getPathFormula().isManifest(is_allocated))
          continue;
        for (const auto &edge : res_a->getRecursiveCallEdges()) {
          if (edge.callee == caller_fn &&
              argsMatchEntryFormals(*res_a, caller_fn, edge.args)) {
            Trace cycle_trace;
            cycle_trace.addEvent(CB, "Calls " + F->getName().str());
            if (edge.call_site) {
              cycle_trace.addEvent(
                  edge.call_site,
                  "Recursively calls back into " + caller_fn->getName().str() +
                      " with identical arguments");
            }
            std::string desc = caller_fn->getName().str() + " -> " +
                               F->getName().str() + " -> " +
                               caller_fn->getName().str();
            auto diag = std::make_unique<MutualRecursionCycle>(
                CB, desc, std::move(cycle_trace));
            DiagnosticManager::getInstance().report(std::move(diag));
            break;
          }
        }
      }
      return summary_results;
    }
  }

  astate->addSkippedCall(F->getName().str());
  astate->declareUnknownValues();
  modelUnknownCallEffects(ops_, *astate, CB, pred, factory_);
  return singleState(std::move(exec_state));
}

ExecutionDomain PulseChecker::handleAlloca(const llvm::AllocaInst *AI,
                                           ExecutionDomain exec_state) {
  auto *astate = exec_state.getAstate();
  AbstractValue av = factory_.getOrCreate(AI);
  ops_.allocate(*astate, av, AI, AllocatorKind::Stack);
  astate->addNonNull(av);
  if (AI->hasName()) {
    astate->setDecompilerName(av, AI->getName().str());
  }
  if (AI->getModule()) {
    const llvm::DataLayout &DL = AI->getModule()->getDataLayout();
    uint64_t elem_size = DL.getTypeAllocSize(AI->getAllocatedType());
    uint64_t total_size = elem_size;
    if (AI->isArrayAllocation()) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(AI->getArraySize())) {
        uint64_t count = CI->getZExtValue();
        total_size = elem_size * count;
      } else {
        total_size = 0;
        if (auto sz_opt = ops_.eval(*astate, AI->getArraySize(), AI, nullptr)) {
          astate->setSymbolicAllocationSize(
              av, astate->getCanonical(sz_opt->addr));
        }
      }
    }
    if (total_size > 0) {
      astate->setAllocationSize(av, total_size);
    }
  }
  astate->getPostAttrs().add(av, Attribute::Stack);
  astate->getPostAttrs().add(av, Attribute::Uninitialized);
  astate->getPostStack().add(AI, Address(av));
  if (auto *ST = llvm::dyn_cast<llvm::StructType>(AI->getAllocatedType())) {
    if (ST->hasName()) {
      auto alloc_kinds =
          TaintOperations::getConfig().matchAllocationSourceForName(
              ST->getName().str());
      if (!alloc_kinds.empty()) {
        TaintOperations::taint(*astate, av, alloc_kinds, AI);
      }
    }
  }
  return exec_state;
}

ExecutionDomain PulseChecker::handleReturn(const llvm::ReturnInst *RI,
                                           ExecutionDomain exec_state) {
  auto *astate = exec_state.getAstate();
  if (astate && !topl_program_.empty()) {
    const auto *function = RI->getFunction();
    bool hasCaller = false;
    std::vector<const llvm::User *> uses(function->user_begin(),
                                         function->user_end());
    std::set<const llvm::User *> visited;
    while (!uses.empty() && !hasCaller) {
      const auto *user = uses.back();
      uses.pop_back();
      if (!visited.insert(user).second)
        continue;
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user)) {
        hasCaller = call->getCalledOperand()->stripPointerCasts() == function &&
                    !current_scc_.count(call->getFunction()) &&
                    call->getFunction() != function;
      } else {
        uses.insert(uses.end(), user->user_begin(), user->user_end());
      }
    }
    if (!hasCaller || function->hasAddressTaken())
      reportTopl(*astate);
  }

  std::optional<AbstractValue> returned_value = std::nullopt;
  if (RI && astate && RI->getNumOperands() > 0) {
    const llvm::Value *ret_v = RI->getReturnValue();
    if (ret_v) {
      auto ret_opt = ops_.eval(*astate, ret_v, RI, nullptr);
      if (ret_opt) {
        AbstractValue canon_ret = astate->getCanonical(ret_opt->addr);
        returned_value = canon_ret;
        if (ret_v->getType()->isPointerTy() &&
            astate->getPostAttrs().has(canon_ret, Attribute::Stack)) {
          Trace trace = Trace::fromValueHistory(ret_opt->history);
          trace.addEvent(RI, "Returning address derived from stack allocation");
          auto diag = std::make_unique<StackVariableAddressEscape>(
              RI, canon_ret, "Stack address escapes via return",
              "Do not return addresses of local variables; allocate on heap or "
              "return by value.",
              std::move(trace));
          DiagnosticManager::getInstance().report(std::move(diag));
        }
      }
    }
  }

  if (exec_state.isContinueProgram() && exec_state.getAstate()) {
    return ExecutionDomain::exitProgram(
        std::make_unique<AbductiveDomain>(exec_state.getAstate()->clone()),
        returned_value);
  }
  return exec_state;
}

ExecutionDomain
PulseChecker::handleComparison(const llvm::Instruction *I,
                               ExecutionDomain exec_state,
                               const llvm::BasicBlock *pred_bb) {
  (void)I;
  (void)pred_bb;
  return exec_state;
}

std::optional<ExecutionDomain> PulseChecker::applyBranchCondition(
    ExecutionDomain state, const llvm::BranchInst *BI, unsigned successor_index,
    const llvm::BasicBlock *pred_bb) {
  if (!BI->isConditional() || successor_index > 1)
    return std::optional<ExecutionDomain>(std::move(state));
  return assumeCondition(std::move(state), BI->getCondition(),
                         successor_index == 0, BI, pred_bb);
}

std::optional<ExecutionDomain> PulseChecker::assumeCondition(
    ExecutionDomain state, const llvm::Value *condition, bool assumed_true,
    const llvm::Instruction *location, const llvm::BasicBlock *pred_bb) {
  if (!condition || !condition->getType()->isIntegerTy(1))
    return std::nullopt;

  if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
    return constant->isOne() == assumed_true
               ? std::optional<ExecutionDomain>(std::move(state))
               : std::nullopt;
  }

  ExecutionDomain forked = std::move(state);
  auto *astate = forked.getAstate();
  if (!astate)
    return std::nullopt;

  auto finish = [&]() -> std::optional<ExecutionDomain> {
    IncorporateResult inc = astate->incorporatePendingNewEqs();
    if (inc.isUnsat()) {
      return std::nullopt;
    }
    if (inc.isPotentialInvalidAccess()) {
      return ExecutionDomain::latentInvalidAccess(
          std::make_unique<AbductiveDomain>(astate->clone()), inc.address,
          std::make_pair(inc.must_be_valid.trace.clone(),
                         inc.must_be_valid.reason),
          {});
    }
    const PulseFormula &formula = astate->getPathFormula();
    if (formula.isUnsat())
      return std::nullopt;
    return std::optional<ExecutionDomain>(std::move(forked));
  };

  if (auto *FCmp = llvm::dyn_cast<llvm::FCmpInst>(condition)) {
    auto a0_opt = ops_.eval(*astate, FCmp->getOperand(0), FCmp, pred_bb);
    auto a1_opt = ops_.eval(*astate, FCmp->getOperand(1), FCmp, pred_bb);
    if (!a0_opt || !a1_opt)
      return std::nullopt;
    AbstractValue av0 = astate->getCanonical(a0_opt->addr);
    AbstractValue av1 = astate->getCanonical(a1_opt->addr);
    const bool both_const =
        astate->getPathFormula().asConstantFloat(av0).has_value() &&
        astate->getPathFormula().asConstantFloat(av1).has_value();
    if (!detail::applyFloatCmpConstraint(astate->getPathFormula(),
                                         FCmp->getPredicate(), av0, av1,
                                         assumed_true)) {
      return std::nullopt;
    }
    PathConditionAtom atom = PathConditionAtom::makeRel(
        PathConditionAtom::Kind::Other, av0, av1);
    atom.is_ground = both_const;
    astate->getPathFormula().recordCondition(atom, 0);
    return finish();
  }

  auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(condition);
  if (!ICmp) {
    auto condition_opt = ops_.eval(*astate, condition, location, pred_bb);
    auto *bool_constant = llvm::ConstantInt::get(
        llvm::cast<llvm::IntegerType>(condition->getType()), assumed_true);
    auto constant_opt =
        ops_.eval(*astate, bool_constant, location, pred_bb);
    if (!condition_opt || !constant_opt)
      return std::nullopt;
    AbstractValue cond_c = astate->getCanonical(condition_opt->addr);
    AbstractValue const_c = astate->getCanonical(constant_opt->addr);
    const bool already_const =
        astate->getPathFormula().asConstantInt(cond_c).has_value();
    if (!astate->getPathFormula().addEquality(cond_c, const_c)) {
      return std::nullopt;
    }
    PathConditionAtom atom = assumed_true ? PathConditionAtom::neZero(cond_c)
                                          : PathConditionAtom::eqZero(cond_c);
    atom.is_ground = already_const;
    astate->getPathFormula().recordCondition(atom, 0);
    return finish();
  }

  llvm::Value *op0 = ICmp->getOperand(0);
  llvm::Value *op1 = ICmp->getOperand(1);
  llvm::ICmpInst::Predicate cmp_pred = ICmp->getPredicate();
  bool op0_null = detail::isNullPointerConstantValue(op0);
  bool op1_null = detail::isNullPointerConstantValue(op1);
  bool is_then = assumed_true;

  if (op0_null || op1_null) {
    if (cmp_pred != llvm::ICmpInst::ICMP_EQ &&
        cmp_pred != llvm::ICmpInst::ICMP_NE) {
      return std::nullopt;
    }
    llvm::Value *ptr = op0_null ? op1 : op0;
    auto ptr_opt = ops_.eval(*astate, ptr, ICmp, pred_bb);
    if (!ptr_opt)
      return std::nullopt;
    AbstractValue canon_ptr_av = astate->getCanonical(ptr_opt->addr);
    const bool taking_null =
        (cmp_pred == llvm::ICmpInst::ICMP_EQ) ? is_then : !is_then;

    if (taking_null) {
      if (astate->getPostAttrs().has(canon_ptr_av, Attribute::Allocated) ||
          astate->getPostAttrs().has(canon_ptr_av, Attribute::Invalid) ||
          astate->getPostAttrs().has(canon_ptr_av, Attribute::Stack) ||
          astate->getPostAttrs().has(canon_ptr_av, Attribute::Global) ||
          astate->getPathFormula().isNonNull(canon_ptr_av)) {
        return std::nullopt;
      }
      const bool already_concrete_null =
          astate->getPostAttrs().has(canon_ptr_av, Attribute::Null) ||
          astate->getPathFormula().isNull(canon_ptr_av);
      if (!astate->getPathFormula().addNull(canon_ptr_av)) {
        return std::nullopt;
      }
      PathConditionAtom atom = PathConditionAtom::eqZero(canon_ptr_av);
      atom.is_ground = already_concrete_null;
      astate->getPathFormula().recordCondition(atom, 0);
    } else {
      if (astate->getPostAttrs().has(canon_ptr_av, Attribute::Null) ||
          astate->getPathFormula().isNull(canon_ptr_av)) {
        return std::nullopt;
      }
      const bool already_non_null =
          astate->isAllocated(canon_ptr_av) ||
          astate->getPathFormula().isNonNull(canon_ptr_av);
      if (!astate->getPathFormula().addNonNull(canon_ptr_av)) {
        return std::nullopt;
      }
      PathConditionAtom atom = PathConditionAtom::neZero(canon_ptr_av);
      atom.is_ground = already_non_null;
      astate->getPathFormula().recordCondition(atom, 0);
    }
    return finish();
  }

  if (op0->getType()->isIntegerTy() && op1->getType()->isIntegerTy()) {
    auto a0_opt = ops_.eval(*astate, op0, ICmp, pred_bb);
    auto a1_opt = ops_.eval(*astate, op1, ICmp, pred_bb);
    if (!a0_opt || !a1_opt)
      return std::nullopt;
    AbstractValue av0 = astate->getCanonical(a0_opt->addr);
    AbstractValue av1 = astate->getCanonical(a1_opt->addr);

    const bool both_const =
        astate->getPathFormula().asConstantInt(av0).has_value() &&
        astate->getPathFormula().asConstantInt(av1).has_value();

    llvm::ICmpInst::Predicate eff_pred =
        is_then ? cmp_pred : detail::invertIcmpPred(cmp_pred);
    if (!detail::applyIntegerIcmpConstraint(astate->getPathFormula(), eff_pred,
                                            av0, av1)) {
      return std::nullopt;
    }
    PathConditionAtom atom =
        PathConditionAtom::makeRel(atomKindFromIcmpPred(eff_pred), av0, av1);
    atom.is_ground = both_const;
    astate->getPathFormula().recordCondition(atom, 0);
    return finish();
  }

  if (cmp_pred != llvm::ICmpInst::ICMP_EQ &&
      cmp_pred != llvm::ICmpInst::ICMP_NE)
    return std::nullopt;

  if (op0->getType()->isPointerTy() && op1->getType()->isPointerTy()) {
    auto av0_opt = ops_.eval(*astate, op0, ICmp, pred_bb);
    auto av1_opt = ops_.eval(*astate, op1, ICmp, pred_bb);
    if (!av0_opt || !av1_opt)
      return std::nullopt;
    AbstractValue av0 = astate->getCanonical(av0_opt->addr);
    AbstractValue av1 = astate->getCanonical(av1_opt->addr);

    bool should_be_equal =
        (cmp_pred == llvm::ICmpInst::ICMP_EQ) ? is_then : !is_then;
    if (should_be_equal) {
      if (!astate->getPathFormula().addEquality(av0, av1))
        return std::nullopt;
      PathConditionAtom atom = PathConditionAtom::makeRel(
          PathConditionAtom::Kind::Equal, av0, av1);
      atom.is_ground = (av0 == av1);
      astate->getPathFormula().recordCondition(atom, 0);
    } else {
      if (!astate->getPathFormula().addDisequality(av0, av1))
        return std::nullopt;
      PathConditionAtom atom = PathConditionAtom::makeRel(
          PathConditionAtom::Kind::NotEqual, av0, av1);
      atom.is_ground = astate->isAllocated(av0) && astate->isAllocated(av1);
      astate->getPathFormula().recordCondition(atom, 0);
    }
    return finish();
  }

  return std::nullopt;
}

} // namespace pulse
