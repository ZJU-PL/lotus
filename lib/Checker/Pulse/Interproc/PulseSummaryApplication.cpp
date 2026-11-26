#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Core/PulseCallState.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseContradiction.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <algorithm>
#include <optional>
#include <queue>
#include <set>
#include <vector>

#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

namespace pulse {

namespace {

static bool calleeAddressIsTouched(const AbductiveDomain *callee_pre,
                                   const AbductiveDomain *callee_post,
                                   AbstractValue callee_av) {
  AbstractValue pre_c = callee_pre->getCanonical(callee_av);
  AbstractValue post_c = callee_post->getCanonical(callee_av);

  auto pre_it = callee_pre->getPreHeap().getEdges().find(pre_c);
  if (pre_it != callee_pre->getPreHeap().getEdges().end() &&
      !pre_it->second.empty()) {
    return true;
  }
  if (callee_post->hasWrittenTo(post_c)) {
    return true;
  }
  if (callee_post->getPostAttrs().has(post_c, Attribute::Invalid)) {
    return true;
  }
  auto post_it = callee_post->getPostHeap().getEdges().find(post_c);
  if (post_it != callee_post->getPostHeap().getEdges().end() &&
      !post_it->second.empty()) {
    return true;
  }
  return false;
}

static bool calleeAddressPairConflicts(const AbductiveDomain *callee_pre,
                                       const AbductiveDomain *callee_post,
                                       const PulseFormula &callee_pre_formula,
                                       AbstractValue a, AbstractValue b) {
  AbstractValue ac = callee_pre->getCanonical(a);
  AbstractValue bc = callee_pre->getCanonical(b);
  if (ac == bc || callee_pre_formula.areEqual(ac, bc)) {
    return false;
  }
  if (callee_pre_formula.areDisequal(ac, bc)) {
    return true;
  }
  // If both distinct callee addresses are mutated or accessed in the heap,
  // aliasing them requires alias-specialized re-analysis (Infer PulseInterproc).
  return calleeAddressIsTouched(callee_pre, callee_post, ac) &&
         calleeAddressIsTouched(callee_pre, callee_post, bc);
}

/**
 * Infer's PulseInterproc.visit: register a (callee_addr -> caller_addr) mapping
 * in CallState and detect aliasing conflicts against rev_subst_.
 */
static bool visitMapping(CallState &call_state,
                         const AbductiveDomain *callee_pre,
                         const AbductiveDomain *callee_post,
                         const PulseFormula &callee_pre_formula,
                         AbstractValue callee_addr,
                         const ValueHistory &callee_hist,
                         AbstractValue caller_addr,
                         const ValueHistory &caller_hist,
                         const CallState::LazyHeapPath &path) {
  AbductiveDomain &caller_astate = call_state.getAstate();
  AbstractValue c_callee = callee_pre->getCanonical(callee_addr);
  AbstractValue c_caller = caller_astate.getCanonical(caller_addr);

  // Check if callee_addr was already mapped to a different caller address.
  if (auto prev_caller = call_state.getSubst().substitute(c_callee)) {
    AbstractValue prev_c = caller_astate.getCanonical(*prev_caller);
    if (!(prev_c == c_caller)) {
      if (!caller_astate.getPathFormula().addEquality(prev_c, c_caller)) {
        return false;
      }
      auto inc = caller_astate.incorporatePendingNewEqs();
      if (!inc.isOk()) {
        return false;
      }
      c_caller = caller_astate.getCanonical(c_caller);
    }
    return true;
  }

  // Check reverse substitution for aliasing of two distinct callee addresses.
  if (auto existing = call_state.toCalleeAddr(c_caller)) {
    AbstractValue existing_callee = callee_pre->getCanonical(existing->first);
    if (calleeAddressPairConflicts(callee_pre, callee_post, callee_pre_formula,
                                   existing_callee, c_callee)) {
      call_state.recordAliasConflict(c_caller, existing_callee,
                                     existing->second, c_callee, path);
    }
  } else {
    call_state.addRevSubst(c_caller, c_callee, path);
  }

  call_state.getSubst().add(c_callee, c_caller);
  ValueHistory merged_hist = caller_hist;
  if (merged_hist.isEmpty()) {
    merged_hist = callee_hist;
  }
  call_state.getHistMap()[c_callee] = merged_hist;
  return true;
}

/**
 * Infer-style recursive precondition materialization operating directly on
 * CallState.
 * - Delays ArrayIndex edges until after `andCalleeFormula`.
 * - If `caller_addr` is Invalid/Null, records `first_error_` and stops
 *   traversing edges from it without immediately failing the entry, so
 *   `checkAllValid` reports the interprocedural UAF/NPD!
 */
static bool materializePreFromAddress(
    AbstractValueFactory &factory, const AbductiveDomain *callee_pre,
    const AbductiveDomain *callee_post, const PulseFormula &callee_pre_formula,
    AbstractValue callee_addr, AbstractValue caller_addr,
    const ValueHistory &caller_hist, const CallState::LazyHeapPath &path,
    CallState &call_state) {
  AbstractValue c_callee = callee_pre->getCanonical(callee_addr);
  AbductiveDomain &caller_astate = call_state.getAstate();
  AbstractValue c_caller = caller_astate.getCanonical(caller_addr);

  if (call_state.getVisited().count(c_callee) > 0) {
    return true;
  }
  call_state.getVisited().insert(c_callee);

  // If caller address is already invalid or null, stop materializing its
  // outgoing edges and record first_error_; checkAllValid will diagnose it!
  if (caller_astate.getPostAttrs().has(c_caller, Attribute::Invalid) ||
      caller_astate.getPostAttrs().has(c_caller, Attribute::Null) ||
      caller_astate.getPathFormula().isNull(c_caller)) {
    call_state.setFirstError(c_caller);
    return true;
  }

  const auto &pre_edges = callee_pre->getPreHeap().getEdges();
  auto pre_it = pre_edges.find(c_callee);
  if (pre_it == pre_edges.end() || pre_it->second.empty()) {
    return true;
  }

  for (const auto &edge_kv : pre_it->second) {
    const Access &access = edge_kv.first;
    const Address &callee_target = edge_kv.second;
    AbstractValue callee_target_canon =
        callee_pre->getCanonical(callee_target.addr);

    // Delay array index edges until after formula conjunction (Infer design).
    if (access.kind == AccessKind::ArrayIndex) {
      call_state.getArrayIndicesToVisit().emplace_back(
          callee_target_canon, callee_target.history, access,
          std::make_pair(c_caller, caller_hist), path);
      continue;
    }

    CallState::LazyHeapPath next_path = path.push(access);
    const Address *caller_target =
        caller_astate.getPostHeap().findEdge(c_caller, access);
    if (!caller_target) {
      AbstractValue fresh_target = factory.createFresh();
      Address target_addr(fresh_target);
      target_addr.history = caller_hist;

      caller_astate.getPostHeap().addEdge(c_caller, access, target_addr);
      caller_astate.abduceToPre(c_caller, access, target_addr);
      caller_astate.abduceAttrToPre(c_caller, Attribute::Allocated);

      if (!visitMapping(call_state, callee_pre, callee_post, callee_pre_formula,
                        callee_target_canon, callee_target.history,
                        fresh_target, target_addr.history, next_path)) {
        return false;
      }
      if (!materializePreFromAddress(factory, callee_pre, callee_post,
                                     callee_pre_formula, callee_target_canon,
                                     fresh_target, target_addr.history,
                                     next_path, call_state)) {
        return false;
      }
    } else {
      AbstractValue caller_target_canon =
          caller_astate.getCanonical(caller_target->addr);
      if (!visitMapping(call_state, callee_pre, callee_post, callee_pre_formula,
                        callee_target_canon, callee_target.history,
                        caller_target_canon, caller_target->history,
                        next_path)) {
        return false;
      }
      if (!materializePreFromAddress(factory, callee_pre, callee_post,
                                     callee_pre_formula, callee_target_canon,
                                     caller_target_canon,
                                     caller_target->history, next_path,
                                     call_state)) {
        return false;
      }
    }
  }

  return true;
}

static Access substituteAccess(const Access &acc, const Substitution &subst,
                               const AbductiveDomain &caller_astate,
                               AbstractValueFactory &factory,
                               Substitution &mut_subst) {
  if (acc.kind != AccessKind::ArrayIndex) {
    return acc;
  }
  Access out = acc;
  if (auto mapped = subst.substitute(acc.index)) {
    out.index = caller_astate.getCanonical(*mapped);
  } else {
    AbstractValue fresh = factory.createFresh();
    mut_subst.add(acc.index, fresh);
    out.index = fresh;
  }
  return out;
}

static std::set<AbstractValue>
collectReachableCallerValues(const AbductiveDomain &astate, AbstractValue root) {
  std::set<AbstractValue> visited;
  std::queue<AbstractValue> q;
  AbstractValue c_root = astate.getCanonical(root);
  visited.insert(c_root);
  q.push(c_root);
  while (!q.empty()) {
    AbstractValue cur = q.front();
    q.pop();
    auto it = astate.getPostHeap().getEdges().find(cur);
    if (it == astate.getPostHeap().getEdges().end()) {
      continue;
    }
    for (const auto &edge_kv : it->second) {
      AbstractValue nxt = astate.getCanonical(edge_kv.second.addr);
      if (visited.insert(nxt).second) {
        q.push(nxt);
      }
    }
  }
  return visited;
}

static Trace buildViaCallTrace(const Trace &callee_trace,
                               const llvm::Function *callee,
                               const llvm::CallBase *CI) {
  Trace combined;
  if (CI) {
    combined.addEvent(CI, "Call to " + (callee ? callee->getName().str()
                                               : std::string("<callee>")));
  }
  for (const auto &ev : callee_trace.getEvents()) {
    combined.addEvent(ev.location, ev.function, ev.description);
  }
  return combined;
}

static void applyPostCondition(
    const AbductiveDomain *callee_post, const AbductiveDomain *callee_pre,
    CallState &call_state, AbstractValueFactory &factory, PulseOperations &ops,
    const llvm::Function *callee, const llvm::CallBase *CI) {
  AbductiveDomain &caller_astate = call_state.getAstate();
  Substitution &subst = call_state.getSubst();

  auto getOrCreateCaller = [&](AbstractValue callee_raw) -> AbstractValue {
    AbstractValue c = callee_post->getCanonical(callee_raw);
    if (auto mapped = subst.substitute(c)) {
      return caller_astate.getCanonical(*mapped);
    }
    AbstractValue fresh = factory.createFresh(CI);
    subst.add(c, fresh);
    return caller_astate.getCanonical(fresh);
  };

  // 1. Delete callee pre-heap edges ONLY on cells that were written to in
  //    callee_post (Infer's read-only cell preservation optimization).
  for (const auto &kv : callee_pre->getPreHeap().getEdges()) {
    AbstractValue formal_from = callee_pre->getCanonical(kv.first);
    if (!callee_post->hasWrittenTo(formal_from)) {
      continue;
    }
    auto caller_from_opt = subst.substitute(formal_from);
    if (!caller_from_opt) {
      continue;
    }
    AbstractValue caller_from = caller_astate.getCanonical(*caller_from_opt);
    for (const auto &edge_kv : kv.second) {
      Access caller_acc =
          substituteAccess(edge_kv.first, subst, caller_astate, factory, subst);
      caller_astate.getPostHeap().removeEdge(caller_from, caller_acc);
    }
  }

  // 2. Apply post-heap edges (skipping untouched read-only pre-edges that
  //    already exist identically in the caller).
  for (const auto &kv : callee_post->getPostHeap().getEdges()) {
    AbstractValue formal_from = callee_post->getCanonical(kv.first);
    AbstractValue caller_from = getOrCreateCaller(formal_from);
    const bool written = callee_post->hasWrittenTo(formal_from);

    for (const auto &edge_kv : kv.second) {
      const Access &callee_acc = edge_kv.first;
      const Address &formal_target = edge_kv.second;
      Access caller_acc =
          substituteAccess(callee_acc, subst, caller_astate, factory, subst);

      AbstractValue formal_target_canon =
          callee_post->getCanonical(formal_target.addr);
      const Address *pre_edge =
          callee_pre->getPreHeap().findEdge(formal_from, callee_acc);
      const bool unchanged_readonly =
          !written && pre_edge &&
          callee_pre->getCanonical(pre_edge->addr) == formal_target_canon &&
          caller_astate.getPostHeap().findEdge(caller_from, caller_acc) !=
              nullptr;
      if (unchanged_readonly) {
        continue;
      }

      AbstractValue caller_target = getOrCreateCaller(formal_target_canon);
      Address caller_target_addr(caller_target);
      caller_target_addr.history = formal_target.history;
      if (CI) {
        caller_target_addr.history.addEvent(ValueHistory::EventKind::FunctionCall,
                                            CI, CI->getFunction());
      }
      caller_astate.getPostHeap().addEdge(caller_from, caller_acc,
                                          caller_target_addr);

      if (written || !pre_edge) {
        caller_astate.getPostAttrs().remove(caller_from,
                                            Attribute::Uninitialized);
        caller_astate.markWrittenTo(caller_from, CI);
      }
    }
  }

  // 3. Apply post WrittenTo markers (even when no new edge was added).
  for (const auto &kv : callee_post->getPostWrittenTo()) {
    auto caller_opt = subst.substitute(callee_post->getCanonical(kv.first));
    if (!caller_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_opt);
    caller_astate.markWrittenTo(caller_av, CI,
                                buildViaCallTrace(kv.second.trace, callee, CI));
    caller_astate.getPostAttrs().remove(caller_av, Attribute::Uninitialized);
  }

  // 4. Apply post AllocationInfo (enables interprocedural leak tracking for
  //    factory functions returning newly allocated memory/resources).
  for (const auto &kv : callee_post->getPostAllocationInfo()) {
    AbstractValue formal_av = callee_post->getCanonical(kv.first);
    // Only transfer allocations newly created by the callee (not caller pre).
    if (callee_pre->isInPre(formal_av)) {
      continue;
    }
    auto caller_opt = subst.substitute(formal_av);
    if (!caller_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_opt);
    AllocationInfo info = kv.second;
    info.trace = buildViaCallTrace(info.trace, callee, CI);
    if (!info.location) {
      info.location = CI;
    }
    caller_astate.setAllocationInfo(caller_av, std::move(info));
    caller_astate.getPostAttrs().add(caller_av, Attribute::Allocated);
  }

  // 5. Apply post UnknownEffect markers.
  for (const auto &kv : callee_post->getPostUnknownEffect()) {
    auto caller_opt = subst.substitute(callee_post->getCanonical(kv.first));
    if (!caller_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_opt);
    caller_astate.setUnknownEffect(caller_av, kv.second);
  }

  // 6. Apply post boolean attributes (preserving exact InvalidationKind!).
  for (const auto &kv : callee_post->getPostAttrs().getAttrs()) {
    AbstractValue formal_av = callee_post->getCanonical(kv.first);
    auto caller_av_opt = subst.substitute(formal_av);
    if (!caller_av_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_av_opt);

    for (Attribute attr : kv.second) {
      if (attr == Attribute::Invalid) {
        InvalidationKind inv_kind = InvalidationKind::Other;
        const llvm::Instruction *inv_loc = CI;
        if (auto info = callee_post->getInvalidationInfo(formal_av)) {
          inv_kind = info->first;
          if (info->second) {
            inv_loc = info->second;
          }
        }
        ops.invalidate(caller_astate, Address(caller_av), inv_loc, inv_kind);
      } else if (attr == Attribute::Stack) {
        // Do not taint caller addresses with callee-local Stack unless already
        // marked Invalid (GoneOutOfScope handled above).
        continue;
      } else {
        caller_astate.getPostAttrs().add(caller_av, attr);
      }
    }
  }

  // 7. Apply allocation sizes, roots, dynamic types.
  for (const auto &kv : callee_post->getAllocationSizes()) {
    if (auto caller_opt = subst.substitute(callee_post->getCanonical(kv.first))) {
      caller_astate.setAllocationSize(caller_astate.getCanonical(*caller_opt),
                                      kv.second);
    }
  }
  for (const auto &kv : callee_post->getAllocationRoots()) {
    auto c_addr = subst.substitute(callee_post->getCanonical(kv.first));
    auto c_root = subst.substitute(callee_post->getCanonical(kv.second));
    if (c_addr && c_root) {
      caller_astate.setAllocationRoot(caller_astate.getCanonical(*c_addr),
                                      caller_astate.getCanonical(*c_root));
    }
  }
  for (const auto &kv : callee_post->getDynamicTypes()) {
    if (auto caller_opt = subst.substitute(callee_post->getCanonical(kv.first))) {
      caller_astate.setDynamicType(caller_astate.getCanonical(*caller_opt),
                                   kv.second);
    }
  }

  // 8. Apply interprocedural taint transfers:
  //    (a) Taints introduced by callee (e.g. wrapper around getenv/read).
  for (const auto &kv : callee_post->getTaintDomain().getMap()) {
    auto caller_opt = subst.substitute(callee_post->getCanonical(kv.first));
    if (!caller_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_opt);
    for (TaintItem item : kv.second) {
      if (item.intra_procedural_only) {
        continue;
      }
      if (CI) {
        item.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                              CI->getFunction());
      }
      caller_astate.getTaintDomain().add(caller_av, std::move(item));
      caller_astate.getPostAttrs().add(caller_av, Attribute::Tainted);
    }
  }

  //    (b) PropagateTaintFrom edges (source args -> dest/return value).
  for (const auto &kv : callee_post->getPostPropagateTaintFrom()) {
    auto dest_opt = subst.substitute(callee_post->getCanonical(kv.first));
    if (!dest_opt) {
      continue;
    }
    AbstractValue caller_dest = caller_astate.getCanonical(*dest_opt);
    for (const auto &prop : kv.second) {
      for (AbstractValue formal_src : prop.sources) {
        auto src_opt = subst.substitute(callee_post->getCanonical(formal_src));
        if (!src_opt) {
          continue;
        }
        AbstractValue caller_src = caller_astate.getCanonical(*src_opt);
        TaintOperations::propagate(caller_astate, caller_src, caller_dest, CI);
      }
    }
  }

  //    (c) Sanitizers applied inside callee.
  for (const auto &kv : callee_post->getPostTaintSanitized()) {
    auto caller_opt = subst.substitute(callee_post->getCanonical(kv.first));
    if (!caller_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_opt);
    for (const auto &san : kv.second) {
      TaintOperations::sanitize(caller_astate, caller_av, san.kind,
                                san.location ? san.location : CI);
    }
  }

  // 9. Propagate skipped calls, recursive calls/edges, unknown values, and
  //    TransitiveInfo.
  for (const auto &sc : callee_post->getSkippedCalls()) {
    caller_astate.addSkippedCall(sc);
  }
  caller_astate.addRecursiveCalls(callee_post->getRecursiveCalls());
  for (const auto &edge : callee_post->getRecursiveCallEdges()) {
    RecursiveCallEdge mapped = edge;
    for (AbstractValue &arg : mapped.args) {
      AbstractValue c = callee_post->getCanonical(arg);
      if (auto caller_arg = subst.substitute(c)) {
        arg = caller_astate.getCanonical(*caller_arg);
      } else {
        arg = getOrCreateCaller(c);
      }
    }
    caller_astate.addRecursiveCallEdge(std::move(mapped));
  }
  if (callee_post->hasUnknownValues()) {
    caller_astate.declareUnknownValues();
  }
  caller_astate.setTransitiveInfo(TransitiveInfo::merge(
      caller_astate.getTransitiveInfo(), callee_post->getTransitiveInfo()));
}

} // namespace

std::vector<ExecutionDomain> PulseChecker::applySummaryImproved(
    const llvm::Function *callee, const ExecutionDomain &caller_state,
    const llvm::CallBase *CI, const llvm::BasicBlock *pred,
    const PulseSummary *summary_override) {
  const PulseSummary *summary_ptr = summary_override
                                        ? summary_override
                                        : summary_manager_.getSummary(callee);
  if (!summary_ptr || !summary_ptr->isValid()) {
    return {};
  }

  const AbductiveDomain *caller_astate = caller_state.getAstate();
  if (!caller_astate) {
    return {caller_state};
  }

  const unsigned formal_arg_count = callee->arg_size();
  const unsigned actual_arg_count = CI->arg_size();
  if (!callee->isVarArg() && actual_arg_count != formal_arg_count) {
    return {};
  }

  // Infer PulseCallOperations.iter_call retry loop for on-demand specialization
  // (up to 3 retries on AliasingWithAllAliases or DynamicTypeNeeded).
  constexpr unsigned kMaxSpecializationRetries = 3u;
  SpecializationKey accumulated_spec;
  std::set<SpecializationKey> tried_specs;

  for (unsigned attempt = 0; attempt <= kMaxSpecializationRetries; ++attempt) {
    const PulseSummary &summary = *summary_ptr;
    std::vector<ExecutionDomain> results;
    results.reserve(
        std::min<unsigned>(kMaxDisjuncts, summary.getPrePostList().size()));
    std::optional<Contradiction> merged_contradiction;

    for (const auto &entry : summary.getPrePostList()) {
      const AbductiveDomain *pre = entry.getPre();
      const AbductiveDomain *post = entry.getPost();
      if (!pre || !post) {
        continue;
      }

      CallState call_state(
          std::make_unique<AbductiveDomain>(caller_astate->clone()));
      AbductiveDomain &work_astate = call_state.getAstate();
      const PulseFormula &callee_pre_formula = entry.getPreFormula();

      bool entry_failed = false;

      // 1. Evaluate and materialize formal parameters directly on CallState.
      unsigned arg_idx = 0;
      for (const auto &Arg : callee->args()) {
        if (arg_idx >= actual_arg_count) {
          break;
        }
        auto actual_opt =
            ops_.eval(work_astate, CI->getArgOperand(arg_idx), CI, pred);
        if (!actual_opt) {
          arg_idx++;
          continue;
        }

        AbstractValue actual_addr = work_astate.getCanonical(actual_opt->addr);
        if (auto formal_av_opt = summary.getFormalAV(&Arg)) {
          AbstractValue formal_av = pre->getCanonical(*formal_av_opt);
          CallState::LazyHeapPath root_path =
              CallState::LazyHeapPath::fromFormal(arg_idx, &Arg);

          if (!visitMapping(call_state, pre, post, callee_pre_formula,
                            formal_av, ValueHistory(), actual_addr,
                            actual_opt->history, root_path)) {
            entry_failed = true;
            break;
          }

          // Determine the formal's address in pre_stack (or fall back to
          // formal_av directly).
          AbstractValue formal_pre_addr = formal_av;
          if (const Address *pre_slot = pre->getPreStack().find(&Arg)) {
            formal_pre_addr = pre->getCanonical(pre_slot->addr);
            if (!(formal_pre_addr == formal_av)) {
              call_state.getSubst().add(formal_pre_addr, actual_addr);
            }
          }

          if (!materializePreFromAddress(factory_, pre, post,
                                         callee_pre_formula, formal_pre_addr,
                                         actual_addr, actual_opt->history,
                                         root_path, call_state)) {
            entry_failed = true;
            break;
          }
        }
        arg_idx++;
      }

      if (entry_failed) {
        continue;
      }

      // 2. Materialize global variables present in callee's pre-heap / post.
      for (const auto &kv : pre->getPreHeap().getEdges()) {
        AbstractValue pre_av = pre->getCanonical(kv.first);
        if (call_state.getSubst().substitute(pre_av).has_value()) {
          continue;
        }
        const llvm::Value *orig = pre_av.getValue();
        if (orig && llvm::isa<llvm::GlobalValue>(orig)) {
          auto gv_opt = ops_.eval(work_astate, orig, CI, pred);
          if (gv_opt) {
            AbstractValue caller_gv = work_astate.getCanonical(gv_opt->addr);
            CallState::LazyHeapPath gv_path =
                CallState::LazyHeapPath::fromGlobal(orig);
            if (visitMapping(call_state, pre, post, callee_pre_formula, pre_av,
                             ValueHistory(), caller_gv, gv_opt->history,
                             gv_path)) {
              (void)materializePreFromAddress(factory_, pre, post,
                                              callee_pre_formula, pre_av,
                                              caller_gv, gv_opt->history,
                                              gv_path, call_state);
            }
          }
        }
      }

      // 3. Check for aliasing contradiction discovered during visit().
      if (call_state.hasAliases()) {
        auto classes = call_state.extractAliasClasses();
        if (!classes.empty()) {
          merged_contradiction = mergeContradictions(
              merged_contradiction,
              Contradiction::makeAliasingWithAllAliases(std::move(classes)));
        }
        continue;
      }

      // 4. Preserve TOPL object identities (including values pruned from heap).
      for (const auto &event : post->getToplHistory().events) {
        for (const auto &argument : event.arguments) {
          if (!argument.value)
            continue;
          AbstractValue formal = pre->getCanonical(*argument.value);
          if (!call_state.getSubst().substitute(formal)) {
            const auto *origin = formal.getValue();
            AbstractValue actual =
                origin && llvm::isa<llvm::GlobalValue>(origin)
                    ? factory_.getOrCreate(origin)
                    : factory_.createFresh(CI);
            call_state.getSubst().add(formal, actual);
          }
        }
      }

      // 5. Conjoin callee formula via Infer's andCalleeFormula (bumps condition
      //    depths by +1, discharges conditions entailed by caller facts, and
      //    returns discovered caller NewEqs).
      auto merged_pre_opt = PulseFormula::andCalleeFormula(
          work_astate.getPathFormula(), callee_pre_formula,
          call_state.getSubst(), factory_);
      if (!merged_pre_opt) {
        merged_contradiction = mergeContradictions(
            merged_contradiction,
            Contradiction::makePathCondition(
                "Caller formula contradicts callee precondition"));
        continue;
      }

      work_astate.setPathFormula(
          std::make_unique<PulseFormula>(std::move(merged_pre_opt->first)));
      IncorporateResult inc_res =
          call_state.incorporateNewEqs(merged_pre_opt->second);
      if (inc_res.isUnsat()) {
        merged_contradiction = mergeContradictions(
            merged_contradiction,
            Contradiction::makePathCondition(
                "Incorporating callee equalities led to UNSAT"));
        continue;
      }
      if (inc_res.isPotentialInvalidAccess()) {
        Trace via_trace =
            buildViaCallTrace(inc_res.must_be_valid.trace, callee, CI);
        if (LatentIssue::isManifest(OperationResult::NullDereference,
                                    work_astate, inc_res.address)) {
          reportBug(OperationResult::NullDereference, CI, inc_res.address,
                    via_trace, &work_astate);
          results.push_back(ExecutionDomain::abortProgram(
              call_state.takeAstate(), OperationResult::NullDereference,
              std::move(via_trace)));
        } else {
          std::vector<
              std::pair<const llvm::Function *, const llvm::Instruction *>>
              cc;
          cc.emplace_back(callee, CI);
          results.push_back(ExecutionDomain::latentInvalidAccess(
              call_state.takeAstate(), inc_res.address,
              std::make_pair(std::move(via_trace),
                             inc_res.must_be_valid.reason),
              std::move(cc)));
        }
        if (results.size() >= kMaxDisjuncts)
          break;
        continue;
      }

      // 6. Materialize delayed ArrayIndex edges (including nested array/struct
      //    levels) now that symbolic index equalities have been resolved.
      bool delayed_ok = true;
      for (unsigned wave = 0;
           wave < 8 && delayed_ok && !call_state.getArrayIndicesToVisit().empty();
           ++wave) {
        auto delayed = std::move(call_state.getArrayIndicesToVisit());
        call_state.getArrayIndicesToVisit().clear();
        for (const auto &item : delayed) {
          Access caller_acc =
              substituteAccess(item.access_callee, call_state.getSubst(),
                               work_astate, factory_, call_state.getSubst());
          AbstractValue base_caller =
              work_astate.getCanonical(item.addr_hist_caller.first);
          const Address *existing =
              work_astate.getPostHeap().findEdge(base_caller, caller_acc);
          if (!existing && caller_acc.kind == AccessKind::ArrayIndex) {
            auto want_const =
                work_astate.getPathFormula().asConstantInt(caller_acc.index);
            if (!want_const) {
              want_const =
                  callee_pre_formula.asConstantInt(item.access_callee.index);
            }
            auto edge_it =
                work_astate.getPostHeap().getEdges().find(base_caller);
            if (edge_it != work_astate.getPostHeap().getEdges().end()) {
              for (const auto &ekv : edge_it->second) {
                if (ekv.first.kind != AccessKind::ArrayIndex ||
                    ekv.first.stride_bytes != caller_acc.stride_bytes) {
                  continue;
                }
                bool idx_matches = work_astate.getPathFormula().areEqual(
                    ekv.first.index, caller_acc.index);
                if (!idx_matches && want_const) {
                  auto have_const = work_astate.getPathFormula().asConstantInt(
                      ekv.first.index);
                  if (have_const && *have_const == *want_const) {
                    idx_matches = true;
                  }
                }
                if (idx_matches) {
                  existing = &ekv.second;
                  caller_acc = ekv.first;
                  call_state.getSubst().add(item.access_callee.index,
                                            ekv.first.index);
                  break;
                }
              }
            }
          }
          AbstractValue target_caller;
          ValueHistory target_hist = item.addr_hist_caller.second;
          if (existing) {
            target_caller = work_astate.getCanonical(existing->addr);
            target_hist = existing->history;
          } else {
            target_caller = factory_.createFresh();
            Address t_addr(target_caller);
            t_addr.history = target_hist;
            work_astate.getPostHeap().addEdge(base_caller, caller_acc, t_addr);
            work_astate.abduceToPre(base_caller, caller_acc, t_addr);
            work_astate.abduceAttrToPre(base_caller, Attribute::Allocated);
          }
          CallState::LazyHeapPath next_path =
              item.base_path.push(item.access_callee);
          if (!visitMapping(call_state, pre, post, callee_pre_formula,
                            item.addr_pre_dest, item.pre_hist, target_caller,
                            target_hist, next_path) ||
              !materializePreFromAddress(factory_, pre, post, callee_pre_formula,
                                         item.addr_pre_dest, target_caller,
                                         target_hist, next_path, call_state)) {
            delayed_ok = false;
            break;
          }
        }
      }
      if (!delayed_ok) {
        continue;
      }

      // 7. Check for dynamic type specialization needed.
      if (!post->getNeedDynamicTypeSpecialization().empty()) {
        std::map<HeapPath, AbstractValue> dt_paths;
        std::map<HeapPath, std::string> dt_names;
        for (AbstractValue need_av : post->getNeedDynamicTypeSpecialization()) {
          if (auto caller_av_opt =
                  call_state.getSubst().substitute(pre->getCanonical(need_av))) {
            AbstractValue c_av = work_astate.getCanonical(*caller_av_opt);
            if (auto dyn_ty = work_astate.getDynamicType(c_av)) {
              if (auto rev = call_state.toCalleeAddr(c_av)) {
                if (auto hp = rev->second.force()) {
                  dt_paths[*hp] = c_av;
                  dt_names[*hp] = *dyn_ty;
                }
              }
            }
          }
        }
        if (auto dt_c = checkDynamicTypeNeeded(dt_paths)) {
          dt_c->dynamic_type_names = std::move(dt_names);
          merged_contradiction = mergeContradictions(merged_contradiction, dt_c);
          continue;
        }
      }

      // 8. Timestamp-ordered checkAllValid (Infer PulseInterproc.check_all_valid):
      //    Checks callee precondition MustBeValid and MustBeInitialized against
      //    caller post-state, reporting interprocedural NPD/UAF/OOB/Uninit or
      //    abducing requirements into caller's pre.
      struct CheckItem {
        uint64_t timestamp;
        bool is_valid_check; // true = MustBeValid, false = MustBeInitialized
        AbstractValue callee_av;
        MustBeValidInfo mbv;
        MustBeInitializedInfo mbi;
      };
      std::vector<CheckItem> checks;
      for (const auto &kv : pre->getPreMustBeValid()) {
        CheckItem ci;
        ci.timestamp = kv.second.timestamp;
        ci.is_valid_check = true;
        ci.callee_av = pre->getCanonical(kv.first);
        ci.mbv = kv.second;
        checks.push_back(std::move(ci));
      }
      for (const auto &kv : pre->getPreMustBeInitialized()) {
        CheckItem ci;
        ci.timestamp = kv.second.timestamp;
        ci.is_valid_check = false;
        ci.callee_av = pre->getCanonical(kv.first);
        ci.mbi = kv.second;
        checks.push_back(std::move(ci));
      }
      std::sort(checks.begin(), checks.end(),
                [](const CheckItem &a, const CheckItem &b) {
                  return a.timestamp < b.timestamp;
                });

      bool stopped_on_error = false;
      for (const auto &chk : checks) {
        auto caller_opt = call_state.getSubst().substitute(chk.callee_av);
        if (!caller_opt) {
          continue;
        }
        AbstractValue caller_av = work_astate.getCanonical(*caller_opt);

        if (chk.is_valid_check) {
          std::optional<OperationResult> violation;
          if (work_astate.getPostAttrs().has(caller_av, Attribute::Invalid)) {
            violation = OperationResult::UseAfterFree;
          } else if (work_astate.getPostAttrs().has(caller_av,
                                                    Attribute::OutOfBounds)) {
            violation = OperationResult::OutOfBounds;
          } else if (work_astate.getPathFormula().isNull(caller_av) ||
                     (work_astate.getPostAttrs().has(caller_av,
                                                     Attribute::Null) &&
                      !work_astate.getPathFormula().isNonNull(caller_av))) {
            violation = OperationResult::NullDereference;
          }

          if (violation) {
            Trace via_trace = buildViaCallTrace(chk.mbv.trace, callee, CI);
            if (LatentIssue::isManifest(*violation, work_astate, caller_av)) {
              reportBug(*violation, CI, caller_av, via_trace, &work_astate);
              results.push_back(ExecutionDomain::abortProgram(
                  call_state.takeAstate(), *violation, std::move(via_trace)));
            } else {
              latent_issues_.emplace_back(
                  *violation, LatentIssue::issueKindFromResult(*violation),
                  caller_av, CI, via_trace.clone());
              latent_issues_.back().addCallingContext(callee, CI);
              results.push_back(ExecutionDomain::latentAbortProgram(
                  call_state.takeAstate(), &latent_issues_.back(), *violation,
                  std::move(via_trace)));
            }
            stopped_on_error = true;
            break;
          } else if (work_astate.isInPre(caller_av) &&
                     !work_astate.isAllocated(caller_av)) {
            work_astate.abduceMustBeValid(
                caller_av, buildViaCallTrace(chk.mbv.trace, callee, CI),
                chk.mbv.reason);
          }
        } else {
          // MustBeInitialized check.
          if (work_astate.getPostAttrs().has(caller_av,
                                             Attribute::Uninitialized) &&
              !work_astate.hasWrittenTo(caller_av)) {
            Trace via_trace = buildViaCallTrace(chk.mbi.trace, callee, CI);
            if (LatentIssue::isManifest(OperationResult::UninitializedRead,
                                        work_astate, caller_av)) {
              reportBug(OperationResult::UninitializedRead, CI, caller_av,
                        via_trace, &work_astate);
              results.push_back(ExecutionDomain::abortProgram(
                  call_state.takeAstate(), OperationResult::UninitializedRead,
                  std::move(via_trace)));
            } else {
              latent_issues_.emplace_back(
                  OperationResult::UninitializedRead,
                  LatentIssue::issueKindFromResult(
                      OperationResult::UninitializedRead),
                  caller_av, CI, via_trace.clone());
              latent_issues_.back().addCallingContext(callee, CI);
              results.push_back(ExecutionDomain::latentAbortProgram(
                  call_state.takeAstate(), &latent_issues_.back(),
                  OperationResult::UninitializedRead, std::move(via_trace)));
            }
            stopped_on_error = true;
            break;
          } else if (work_astate.isInPre(caller_av) &&
                     !work_astate.hasWrittenTo(caller_av) &&
                     !work_astate.isAllocated(caller_av)) {
            work_astate.abduceMustBeInitialized(
                caller_av, buildViaCallTrace(chk.mbi.trace, callee, CI));
          }
        }
      }

      if (stopped_on_error) {
        if (results.size() >= kMaxDisjuncts)
          break;
        continue;
      }

      // Compatibility check for hand-constructed summaries that only specify
      // pre_attrs_.Allocated without pre_must_be_valid_.
      if (pre->getPreMustBeValid().empty()) {
        bool legacy_attr_fail = false;
        for (const auto &kv : pre->getPreAttrs().getAttrs()) {
          if (kv.second.count(Attribute::Allocated) == 0)
            continue;
          auto caller_opt =
              call_state.getSubst().substitute(pre->getCanonical(kv.first));
          if (!caller_opt)
            continue;
          AbstractValue caller_av = work_astate.getCanonical(*caller_opt);
          if (work_astate.getPostAttrs().has(caller_av, Attribute::Invalid) ||
              work_astate.getPostAttrs().has(caller_av, Attribute::Null) ||
              work_astate.getPathFormula().isNull(caller_av)) {
            legacy_attr_fail = true;
            break;
          }
        }
        if (legacy_attr_fail) {
          continue;
        }
      }

      // 9. Determine caller return abstract value before applying post.
      std::optional<AbstractValue> caller_ret = std::nullopt;
      if (entry.getReturnValue()) {
        AbstractValue formal_ret = post->getCanonical(*entry.getReturnValue());
        auto caller_ret_opt = call_state.getSubst().substitute(formal_ret);
        if (caller_ret_opt) {
          caller_ret = work_astate.getCanonical(*caller_ret_opt);
        } else {
          AbstractValue fresh_ret = factory_.createFresh(CI);
          call_state.getSubst().add(formal_ret, fresh_ret);
          caller_ret = work_astate.getCanonical(fresh_ret);
        }
      }

      // 10. Apply post-condition (edges, attributes, invalidations,
      //     allocations, taint transfers, sanitizers, transitive info).
      applyPostCondition(post, pre, call_state, factory_, ops_, callee, CI);

      // Conjoin post formula if it adds facts beyond pre formula.
      auto merged_post_opt = PulseFormula::andCalleeFormula(
          work_astate.getPathFormula(), entry.getPostFormula(),
          call_state.getSubst(), factory_);
      if (!merged_post_opt) {
        continue;
      }
      work_astate.setPathFormula(
          std::make_unique<PulseFormula>(std::move(merged_post_opt->first)));
      auto post_inc = call_state.incorporateNewEqs(merged_post_opt->second);
      if (post_inc.isUnsat()) {
        continue;
      }

      // 11. Interprocedural taint sink check (MustNotBeTainted in callee pre).
      for (const auto &kv : pre->getPreMustNotBeTainted()) {
        auto caller_opt =
            call_state.getSubst().substitute(pre->getCanonical(kv.first));
        if (!caller_opt) {
          continue;
        }
        AbstractValue caller_av = work_astate.getCanonical(*caller_opt);
        std::set<AbstractValue> reachable =
            collectReachableCallerValues(work_astate, caller_av);

        for (const auto &sink : kv.second) {
          bool reported = false;
          for (AbstractValue rv : reachable) {
            if (TaintOperations::checkSink(work_astate, rv, sink.sink_name,
                                           CI)) {
              reported = true;
              break;
            }
          }
          if (!reported && work_astate.isInPre(caller_av)) {
            MustNotBeTaintedSink prop_sink = sink;
            prop_sink.trace = buildViaCallTrace(sink.trace, callee, CI);
            prop_sink.location = CI;
            work_astate.abduceMustNotBeTainted(caller_av, std::move(prop_sink));
          }
        }
      }

      // 12. Append TOPL events from callee summary.
      for (auto event : post->getToplHistory().events) {
        for (auto &argument : event.arguments) {
          if (argument.value) {
            argument.value = call_state.getSubst().substituteOrIdentity(
                pre->getCanonical(*argument.value));
          }
        }
        if (event.callingContext.size() < 8) {
          event.callingContext.insert(event.callingContext.begin(), CI);
        }
        work_astate.getToplHistory().append(std::move(event));
      }
      work_astate.getToplHistory().truncated |= post->getToplHistory().truncated;

      // 13. Handle latent issues (LatentAbortProgram / LatentInvalidAccess).
      if (entry.getLatentIssue()) {
        const auto &latent = *entry.getLatentIssue();
        AbstractValue formal_addr = post->getCanonical(latent.address);
        auto caller_addr_opt = call_state.getSubst().substitute(formal_addr);
        AbstractValue caller_addr =
            caller_addr_opt ? *caller_addr_opt : factory_.createFresh(CI);
        if (!caller_addr_opt) {
          call_state.getSubst().add(formal_addr, caller_addr);
        }
        caller_addr = work_astate.getCanonical(caller_addr);

        Trace issue_trace = buildViaCallTrace(latent.trace, callee, CI);

        if (LatentIssue::isManifest(latent.diagnostic, work_astate,
                                    caller_addr)) {
          reportBug(latent.diagnostic, CI, caller_addr, issue_trace,
                    &work_astate);
          results.push_back(ExecutionDomain::abortProgram(
              call_state.takeAstate(), latent.diagnostic,
              std::move(issue_trace)));
        } else if (latent.is_latent_invalid_access) {
          auto cc = latent.calling_context;
          cc.emplace_back(callee, CI);
          results.push_back(ExecutionDomain::latentInvalidAccess(
              call_state.takeAstate(), caller_addr,
              std::make_pair(std::move(issue_trace),
                             latent.must_be_valid_reason),
              std::move(cc)));
        } else {
          latent_issues_.emplace_back(
              latent.diagnostic,
              LatentIssue::issueKindFromResult(latent.diagnostic), caller_addr,
              CI, issue_trace.clone());
          for (const auto &cc : latent.calling_context) {
            latent_issues_.back().addCallingContext(cc.first, cc.second);
          }
          latent_issues_.back().addCallingContext(callee, CI);
          results.push_back(ExecutionDomain::latentAbortProgram(
              call_state.takeAstate(), &latent_issues_.back(),
              latent.diagnostic, std::move(issue_trace)));
        }
        if (results.size() >= kMaxDisjuncts) {
          break;
        }
        continue;
      }

      // 14. Bind return value onto caller's stack (or raise exception).
      if (entry.isExceptionRaised()) {
        std::optional<AbstractValue> caller_exc;
        if (entry.getExceptionValue()) {
          AbstractValue fe = post->getCanonical(*entry.getExceptionValue());
          if (auto ce = call_state.getSubst().substitute(fe)) {
            caller_exc = work_astate.getCanonical(*ce);
          } else {
            AbstractValue fresh_exc = factory_.createFresh(CI);
            call_state.getSubst().add(fe, fresh_exc);
            caller_exc = work_astate.getCanonical(fresh_exc);
          }
          if (!entry.getExceptionType().empty()) {
            work_astate.setDynamicType(*caller_exc, entry.getExceptionType());
          }
        }
        results.push_back(ExecutionDomain::exceptionRaised(
            call_state.takeAstate(), caller_exc, entry.getExceptionType()));
        if (results.size() >= kMaxDisjuncts) {
          break;
        }
        continue;
      }

      if (caller_ret) {
        Address ret_addr(work_astate.getCanonical(*caller_ret));
        ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                  CI->getFunction());
        work_astate.getPostStack().add(CI, ret_addr);
      }

      results.push_back(ExecutionDomain::continueProgram(call_state.takeAstate()));
      if (results.size() >= kMaxDisjuncts) {
        break;
      }
    }

    if (!results.empty()) {
      // Propagate non-disjunctive callee parameter modifications into the
      // caller's non-disjunctive domain (suppresses false UnnecessaryCopy /
      // ConstRefableParam warnings when callees mutate passed aggregates).
      if (!summary.getNonDisj().getModifiedParamIndices().empty()) {
        AbductiveDomain eval_copy = caller_astate->clone();
        for (unsigned arg_idx :
             summary.getNonDisj().getModifiedParamIndices()) {
          if (arg_idx < actual_arg_count) {
            if (auto actual_opt = ops_.eval(
                    eval_copy, CI->getArgOperand(arg_idx), CI, pred)) {
              analysis_non_disj_.markAddressModified(
                  eval_copy.getCanonical(actual_opt->addr), eval_copy);
            }
          }
        }
      }
      return results;
    }

    // On-demand specialization retry (only when not using a manual override).
    if (summary_override || !merged_contradiction) {
      break;
    }

    SpecializationKey next_spec;
    if (merged_contradiction->kind ==
        ContradictionKind::AliasingWithAllAliases) {
      next_spec = SpecializationKey::fromAliases(
          merged_contradiction->alias_classes_with_paths);
    } else if (merged_contradiction->kind ==
               ContradictionKind::DynamicTypeNeeded) {
      for (const auto &kv : merged_contradiction->dynamic_type_names) {
        next_spec.dynamic_types_by_path[kv.first] = kv.second;
      }
      for (const auto &kv : merged_contradiction->dynamic_type_paths) {
        if (next_spec.dynamic_types_by_path.count(kv.first) == 0) {
          if (auto dyn_ty = caller_astate->getDynamicType(kv.second)) {
            next_spec.dynamic_types_by_path[kv.first] = *dyn_ty;
          }
        }
      }
      next_spec.normalize();
    } else {
      break;
    }

    accumulated_spec.merge(next_spec);
    if (accumulated_spec.isBottom() || tried_specs.count(accumulated_spec) > 0) {
      break;
    }
    tried_specs.insert(accumulated_spec);

    const PulseSummary *specialized =
        analyzeFunctionSpecialized(callee, accumulated_spec);
    if (!specialized || !specialized->isValid()) {
      break;
    }
    summary_ptr = specialized;
  }

  return {};
}

} // namespace pulse
