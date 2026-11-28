#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>

namespace pulse {

namespace {

static std::string allocatorName(AllocatorKind kind) {
  switch (kind) {
  case AllocatorKind::CMalloc:
    return "malloc";
  case AllocatorKind::CRealloc:
    return "realloc";
  case AllocatorKind::CppNew:
    return "operator new";
  case AllocatorKind::CppNewArray:
    return "operator new[]";
  case AllocatorKind::FileHandle:
    return "File";
  case AllocatorKind::Socket:
    return "Socket";
  case AllocatorKind::CustomResource:
    return "Resource";
  case AllocatorKind::Stack:
    return "stack";
  case AllocatorKind::Global:
    return "global";
  }
  return "allocation";
}

static bool isResourceAllocator(AllocatorKind kind) {
  return kind == AllocatorKind::FileHandle || kind == AllocatorKind::Socket ||
         kind == AllocatorKind::CustomResource;
}

static bool isHeapAllocator(AllocatorKind kind) {
  return kind == AllocatorKind::CMalloc || kind == AllocatorKind::CRealloc ||
         kind == AllocatorKind::CppNew || kind == AllocatorKind::CppNewArray;
}

static std::set<AbstractValue>
computeReachable(const AbductiveDomain &astate, const Heap &heap,
                 const std::set<AbstractValue> &roots) {
  std::set<AbstractValue> reachable = roots;
  std::vector<AbstractValue> worklist(roots.begin(), roots.end());

  while (!worklist.empty()) {
    AbstractValue cur = worklist.back();
    worklist.pop_back();
    auto it = heap.getEdges().find(cur);
    if (it == heap.getEdges().end())
      continue;
    for (const auto &edge_kv : it->second) {
      if (edge_kv.first.kind == AccessKind::ArrayIndex &&
          !edge_kv.first.const_index.has_value()) {
        AbstractValue idx = astate.getCanonical(edge_kv.first.index);
        if (reachable.insert(idx).second) {
          worklist.push_back(idx);
        }
      }
      AbstractValue target = astate.getCanonical(edge_kv.second.addr);
      if (reachable.insert(target).second) {
        worklist.push_back(target);
      }
    }
  }

  return reachable;
}

static std::set<AbstractValue>
collectBoundaryRoots(const llvm::Function *F, const AbductiveDomain &astate,
                     std::optional<AbstractValue> ret_val) {
  std::set<AbstractValue> roots;
  for (const auto &Arg : F->args()) {
    if (const auto *addr = astate.getPostStack().find(&Arg)) {
      roots.insert(astate.getCanonical(addr->addr));
    }
    if (const auto *addr = astate.getPreStack().find(&Arg)) {
      roots.insert(astate.getCanonical(addr->addr));
    }
  }
  if (ret_val) {
    roots.insert(astate.getCanonical(*ret_val));
  }
  for (const auto &kv : astate.getPostAttrs().getAttrs()) {
    if (kv.second.count(Attribute::Global) > 0) {
      roots.insert(astate.getCanonical(kv.first));
    }
  }
  return roots;
}

static bool isManifestExitState(const AbductiveDomain &astate) {
  if (astate.hasUnknownValues() || astate.isOverApproximated() ||
      astate.preHeapHasAssumptions()) {
    return false;
  }
  if (!options::disableSMT() &&
      !astate.getPathFormula().isProvenSatisfiable()) {
    return false;
  }
  auto is_allocated = [&astate](AbstractValue v) {
    return astate.isAllocated(v);
  };
  return astate.getPathFormula().isManifest(is_allocated);
}

/**
 * Infer-style retain-cycle checker (PulseRetainCycleChecker.ml):
 * Detects unreachable ownership cycles among strong-refcounted (shared_ptr)
 * objects in post_heap_ whose cycles prevent deallocation at exit.
 */
static std::set<AbstractValue>
checkRetainCyclesOnExit(const llvm::Function *F, const AbductiveDomain &astate,
                        const std::set<AbstractValue> &live_post,
                        const llvm::Instruction *exit_loc) {
  std::set<AbstractValue> cycle_nodes;
  if (astate.getSharedPtrTargets().empty()) {
    return cycle_nodes;
  }

  if (!isManifestExitState(astate)) {
    return cycle_nodes;
  }

  std::set<AbstractValue> live_roots;
  for (AbstractValue lv : live_post) {
    live_roots.insert(astate.getCanonical(lv));
    live_roots.insert(astate.getAllocationRoot(lv));
  }

  const auto &edges = astate.getPostHeap().getEdges();
  for (AbstractValue raw_target : astate.getSharedPtrTargets()) {
    AbstractValue start = astate.getCanonical(raw_target);
    if (cycle_nodes.count(start) > 0 ||
        astate.getPostAttrs().has(start, Attribute::Invalid) ||
        live_post.count(start) > 0 || live_roots.count(start) > 0 ||
        live_roots.count(astate.getAllocationRoot(start)) > 0) {
      continue;
    }

    // Bounded BFS/DFS from `start` through post_heap_ to find a cycle back to
    // `start`.
    std::vector<AbstractValue> path;
    std::set<AbstractValue> visiting;
    std::function<bool(AbstractValue, unsigned)> dfs =
        [&](AbstractValue cur, unsigned depth) -> bool {
      if (depth > 12)
        return false;
      auto it = edges.find(cur);
      if (it == edges.end())
        return false;
      for (const auto &ekv : it->second) {
        // Weak references (std::weak_ptr) and control blocks do not
        // participate in strong ownership cycles.
        if (ekv.first.kind == AccessKind::Field &&
            (ekv.first.field_idx == 0x7FFFFFF0u ||
             ekv.first.field_idx == 0x7FFFFFF1u ||
             ekv.first.field_idx == 0x7FFFFFF2u)) {
          continue;
        }
        AbstractValue nxt = astate.getCanonical(ekv.second.addr);
        if (astate.getPostAttrs().has(nxt, Attribute::Invalid)) {
          continue;
        }
        if (nxt == start) {
          return true;
        }
        if (visiting.insert(nxt).second) {
          path.push_back(nxt);
          if (dfs(nxt, depth + 1)) {
            return true;
          }
          path.pop_back();
        }
      }
      return false;
    };

    if (dfs(start, 0)) {
      cycle_nodes.insert(start);
      for (AbstractValue n : path) {
        cycle_nodes.insert(n);
      }

      const llvm::Instruction *report_loc = exit_loc;
      Trace trace;
      if (auto info = astate.getAllocationInfo(start)) {
        if (info->location) {
          trace.addEvent(info->location, "Strong-refcounted object allocated");
          if (!report_loc)
            report_loc = info->location;
        }
      }
      if (!report_loc && !F->empty()) {
        report_loc = F->back().getTerminator();
      }
      if (report_loc) {
        trace.addEvent(report_loc,
                       "Strong reference cycle keeps refcount > 0 at exit");
        std::string name =
            astate.getDecompilerName(start).value_or("shared_ptr object");
        auto diag = std::make_unique<RetainCycle>(
            report_loc, name + " participates in a strong reference cycle",
            std::move(trace));
        DiagnosticManager::getInstance().report(std::move(diag));
      }
    }
  }

  return cycle_nodes;
}

/**
 * Infer-style leak detection at function exit (PulseAbductiveDomain.check_memory_leaks):
 * Reports MemoryLeak / ResourceLeak for any heap/resource allocation that is
 * unreachable from formals, return value, or globals (and not reachable via
 * derived pointer arithmetic / allocation root, not invalidated, and not passed
 * to an unknown function).
 */
static void checkLeaksOnExit(const llvm::Function *F,
                             const AbductiveDomain &astate,
                             const std::set<AbstractValue> &live_post,
                             const llvm::Instruction *exit_loc) {
  std::set<AbstractValue> retain_cycle_nodes =
      checkRetainCyclesOnExit(F, astate, live_post, exit_loc);

  if (!options::enableMemoryLeaks()) {
    return;
  }

  // Only report leaks on manifest paths (Infer checks manifestness before
  // reporting leaks so caller-conditioned exits don't emit spurious leaks).
  if (!isManifestExitState(astate)) {
    return;
  }

  // Compute live allocation roots (Infer's reaches_into for interior pointers).
  std::set<AbstractValue> live_roots;
  for (AbstractValue lv : live_post) {
    live_roots.insert(astate.getCanonical(lv));
    live_roots.insert(astate.getAllocationRoot(lv));
  }

  for (const auto &kv : astate.getPostAllocationInfo()) {
    AbstractValue addr = astate.getCanonical(kv.first);
    const AllocationInfo &info = kv.second;

    if (retain_cycle_nodes.count(addr) > 0) {
      continue; // Already diagnosed as a RetainCycle.
    }

    if (!isHeapAllocator(info.kind) && !isResourceAllocator(info.kind)) {
      continue;
    }
    if (astate.getPostAttrs().has(addr, Attribute::Invalid) ||
        astate.getPostAttrs().has(addr, Attribute::Stack) ||
        astate.getPostAttrs().has(addr, Attribute::Global)) {
      continue;
    }
    if (astate.getUnknownEffect(addr).has_value()) {
      continue;
    }
    // Check direct reachability or interior-pointer reachability.
    if (live_post.count(addr) > 0 || live_roots.count(addr) > 0 ||
        live_roots.count(astate.getAllocationRoot(addr)) > 0) {
      continue;
    }

    const llvm::Instruction *alloc_site = info.location;
    const llvm::Instruction *report_loc =
        info.unreachable_at ? info.unreachable_at
                            : (exit_loc ? exit_loc : alloc_site);
    if (!report_loc && !F->empty()) {
      report_loc = F->back().getTerminator();
    }
    if (!report_loc) {
      continue;
    }

    Trace leak_trace = info.trace.clone();
    if (alloc_site) {
      leak_trace.addEvent(alloc_site,
                          "Allocated via " + allocatorName(info.kind));
    }
    leak_trace.addEvent(report_loc, "Allocation becomes unreachable here");

    if (isResourceAllocator(info.kind)) {
      auto diag = std::make_unique<ResourceLeak>(
          report_loc, alloc_site, allocatorName(info.kind),
          std::move(leak_trace));
      DiagnosticManager::getInstance().report(std::move(diag));
    } else {
      auto diag = std::make_unique<MemoryLeak>(
          report_loc, alloc_site, allocatorName(info.kind),
          std::move(leak_trace));
      DiagnosticManager::getInstance().report(std::move(diag));
    }
  }
}

static AbductiveDomain
pruneForSummary(const llvm::Function *F, const AbductiveDomain &astate,
                std::optional<AbstractValue> ret_val, bool check_leaks,
                std::optional<AbstractValue> extra_keep_var = std::nullopt,
                bool keep_non_ground_conditions = false) {
  AbductiveDomain pruned = astate.clone();
  pruned.canonicalize();

  const std::set<AbstractValue> roots = collectBoundaryRoots(F, pruned, ret_val);

  std::set<AbstractValue> reachable_post =
      computeReachable(pruned, pruned.getPostHeap(), roots);
  std::set<AbstractValue> reachable_pre =
      computeReachable(pruned, pruned.getPreHeap(), roots);

  // Run leak detection on unreachable heap/resource allocations BEFORE pruning.
  if (check_leaks) {
    const llvm::Instruction *exit_loc = nullptr;
    for (const auto &BB : *F) {
      if (const auto *RI = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
        exit_loc = RI;
        break;
      }
    }
    checkLeaksOnExit(F, pruned, reachable_post, exit_loc);
  }

  // Keep variables referenced in TOPL history, taint, or MustNotBeTainted alive.
  std::set<AbstractValue> keep_vars;
  keep_vars.insert(reachable_post.begin(), reachable_post.end());
  keep_vars.insert(reachable_pre.begin(), reachable_pre.end());
  if (extra_keep_var) {
    AbstractValue ek = pruned.getCanonical(*extra_keep_var);
    keep_vars.insert(ek);
    reachable_post.insert(ek);
  }
  for (const auto &event : pruned.getToplHistory().events) {
    for (const auto &arg : event.arguments) {
      if (arg.value) {
        keep_vars.insert(pruned.getCanonical(*arg.value));
      }
    }
  }

  // Simplify formula and retain variables connected via live arithmetic.
  // Preserve non-ground branch hypotheses over dead local values (e.g.
  // `e = lookup(); if (!e) return NULL;`) so callers never misread conditional
  // exits as unconditional specs, while dropping tautological `p != 0` checks
  // on locally allocated pointers.
  auto is_local_alloc = [&pruned](AbstractValue v) {
    AbstractValue c = pruned.getCanonical(v);
    if (pruned.getPostAttrs().has(c, Attribute::Global)) {
      return true;
    }
    if (pruned.isInPre(c)) {
      return false;
    }
    return pruned.isAllocated(c) || pruned.getAllocationInfo(c).has_value() ||
           pruned.getInvalidationInfo(c).has_value() ||
           pruned.getPostAttrs().has(c, Attribute::Stack);
  };
  std::set<AbstractValue> live_arith =
      pruned.getPathFormula().simplifyForSummary(keep_vars, reachable_pre,
                                                 keep_non_ground_conditions,
                                                 is_local_alloc);
  reachable_post.insert(live_arith.begin(), live_arith.end());
  reachable_pre.insert(live_arith.begin(), live_arith.end());

  // Prune stacks: keep formals only.
  {
    Stack new_post;
    Stack new_pre;
    for (const auto &Arg : F->args()) {
      if (const auto *addr = pruned.getPostStack().find(&Arg)) {
        new_post.add(&Arg, *addr);
      }
      if (const auto *addr = pruned.getPreStack().find(&Arg)) {
        new_pre.add(&Arg, *addr);
      }
    }
    pruned.getPostStack() = std::move(new_post);
    pruned.getPreStack() = std::move(new_pre);
  }

  // Prune post heap.
  {
    Heap new_post;
    for (const auto &kv : pruned.getPostHeap().getEdges()) {
      AbstractValue from = pruned.getCanonical(kv.first);
      if (reachable_post.count(from) == 0)
        continue;
      for (const auto &edge_kv : kv.second) {
        AbstractValue to = pruned.getCanonical(edge_kv.second.addr);
        if (reachable_post.count(to) == 0)
          continue;
        new_post.addEdge(from, edge_kv.first, edge_kv.second);
      }
    }
    pruned.getPostHeap() = std::move(new_post);
  }

  // Prune pre heap (preserving registered pre addresses).
  {
    Heap new_pre;
    for (const auto &kv : pruned.getPreHeap().getEdges()) {
      AbstractValue from = pruned.getCanonical(kv.first);
      if (reachable_pre.count(from) == 0)
        continue;
      new_pre.registerAddress(from);
      for (const auto &edge_kv : kv.second) {
        AbstractValue to = pruned.getCanonical(edge_kv.second.addr);
        if (reachable_pre.count(to) == 0)
          continue;
        new_pre.addEdge(from, edge_kv.first, edge_kv.second);
        new_pre.registerAddress(to);
      }
    }
    pruned.getPreHeap() = std::move(new_pre);
  }

  // Prune post & pre boolean attributes.
  {
    AddressAttributes new_post_attrs;
    for (const auto &kv : pruned.getPostAttrs().getAttrs()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) == 0)
        continue;
      for (Attribute a : kv.second) {
        new_post_attrs.add(canon, a);
      }
      // Local stack allocations that escaped via return/out-param go out of
      // scope at function exit (Infer's invalidate_locals).
      if (kv.second.count(Attribute::Stack) > 0 &&
          kv.second.count(Attribute::Global) == 0) {
        new_post_attrs.add(canon, Attribute::Invalid);
        new_post_attrs.remove(canon, Attribute::Allocated);
        pruned.setInvalidationInfo(canon, InvalidationKind::GoneOutOfScope,
                                   nullptr);
      }
    }
    pruned.getPostAttrs() = std::move(new_post_attrs);

    AddressAttributes new_pre_attrs;
    for (const auto &kv : pruned.getPreAttrs().getAttrs()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_pre.count(canon) == 0)
        continue;
      for (Attribute a : kv.second) {
        new_pre_attrs.add(canon, a);
      }
    }
    pruned.getPreAttrs() = std::move(new_pre_attrs);
  }

  // Prune parameterized pre attributes.
  {
    std::map<AbstractValue, MustBeValidInfo> mbv;
    for (const auto &kv : pruned.getPreMustBeValid()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_pre.count(canon) > 0) {
        mbv.emplace(canon, kv.second);
      }
    }
    pruned.getPreMustBeValid() = std::move(mbv);

    std::map<AbstractValue, MustBeInitializedInfo> mbi;
    for (const auto &kv : pruned.getPreMustBeInitialized()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_pre.count(canon) > 0) {
        mbi.emplace(canon, kv.second);
      }
    }
    pruned.getPreMustBeInitialized() = std::move(mbi);

    std::map<AbstractValue, std::vector<MustNotBeTaintedSink>> mnt;
    for (const auto &kv : pruned.getPreMustNotBeTainted()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_pre.count(canon) > 0) {
        mnt[canon] = kv.second;
      }
    }
    pruned.getPreMustNotBeTainted() = std::move(mnt);
  }

  // Prune parameterized post attributes.
  {
    std::map<AbstractValue, WrittenToInfo> wt;
    for (const auto &kv : pruned.getPostWrittenTo()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0 || reachable_pre.count(canon) > 0) {
        wt.emplace(canon, kv.second);
      }
    }
    pruned.getPostWrittenTo() = std::move(wt);

    std::map<AbstractValue, AllocationInfo> ai;
    for (const auto &kv : pruned.getPostAllocationInfo()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0) {
        ai.emplace(canon, kv.second);
      }
    }
    pruned.getPostAllocationInfo() = std::move(ai);

    std::map<AbstractValue, std::vector<PropagateTaintFromInfo>> ptf;
    for (const auto &kv : pruned.getPostPropagateTaintFrom()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0) {
        ptf[canon] = kv.second;
      }
    }
    pruned.getPostPropagateTaintFrom() = std::move(ptf);

    std::map<AbstractValue, UnknownEffectInfo> ue;
    for (const auto &kv : pruned.getPostUnknownEffect()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0 || reachable_pre.count(canon) > 0) {
        ue.emplace(canon, kv.second);
      }
    }
    pruned.getPostUnknownEffect() = std::move(ue);

    std::map<AbstractValue, std::vector<SanitizerRecord>> san;
    for (const auto &kv : pruned.getPostTaintSanitized()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0 || reachable_pre.count(canon) > 0) {
        san[canon] = kv.second;
      }
    }
    pruned.getPostTaintSanitized() = std::move(san);

    TaintDomain new_taint;
    for (const auto &kv : pruned.getTaintDomain().getMap()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0) {
        for (const auto &item : kv.second) {
          new_taint.add(canon, item);
        }
      }
    }
    pruned.getTaintDomain() = std::move(new_taint);
  }

  // Prune allocation sizes & roots.
  {
    std::map<AbstractValue, uint64_t> sizes;
    for (const auto &kv : pruned.getAllocationSizes()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0) {
        sizes.emplace(canon, kv.second);
      }
    }
    pruned.getAllocationSizes() = std::move(sizes);

    std::map<AbstractValue, AbstractValue> alloc_roots;
    for (const auto &kv : astate.getAllocationRoots()) {
      AbstractValue addr = pruned.getCanonical(kv.first);
      AbstractValue root = pruned.getCanonical(kv.second);
      if (reachable_post.count(addr) > 0 || reachable_pre.count(addr) > 0) {
        alloc_roots.emplace(addr, root);
      }
    }
    pruned.getAllocationRoots() = std::move(alloc_roots);

    std::map<AbstractValue, int64_t> alloc_offsets;
    for (const auto &kv : astate.getAllocationOffsets()) {
      AbstractValue addr = pruned.getCanonical(kv.first);
      if (reachable_post.count(addr) > 0 || reachable_pre.count(addr) > 0) {
        alloc_offsets.emplace(addr, kv.second);
      }
    }
    pruned.getAllocationOffsets() = std::move(alloc_offsets);

    std::set<AbstractValue> kept_dyn_needed;
    for (AbstractValue need_av : pruned.getNeedDynamicTypeSpecialization()) {
      AbstractValue canon = pruned.getCanonical(need_av);
      if (reachable_pre.count(canon) > 0 || reachable_post.count(canon) > 0) {
        kept_dyn_needed.insert(canon);
      }
    }
    pruned.clearNeedDynamicTypeSpecialization();
    for (AbstractValue canon : kept_dyn_needed) {
      pruned.addNeedDynamicTypeSpecialization(canon);
    }
  }

  pruned.canonicalize();
  return pruned;
}

} // namespace

void PulseChecker::createSummary(
    const llvm::Function *F, const std::vector<ExecutionDomain> &exit_states,
    const std::vector<ExecutionDomain> &latent_exit_states,
    const SpecializationKey *spec_key) {
  PulseLogger::debug("Creating summary for " + F->getName().str() + " (" +
                     std::to_string(exit_states.size()) + " exit states, " +
                     std::to_string(latent_exit_states.size()) +
                     " latent states)");
  PulseLogger::incrementCounter("summaries.created");
  if (exit_states.empty() && latent_exit_states.empty() && spec_key)
    return;

  auto summary_ptr = std::make_unique<PulseSummary>(F);
  PulseSummary &summary = *summary_ptr;
  TransitiveInfo merged_transitive;

  // Prioritize exact witness states (`!hasUnknownValues() && !isOverApproximated()`)
  // before imprecise ones so unknown/over-approximated paths never evict exact
  // witnesses from callee summaries.
  auto stateQualityTier = [](const AbductiveDomain *astate) -> unsigned {
    if (!astate)
      return 4u;
    const bool unk = astate->hasUnknownValues();
    const bool over = astate->isOverApproximated();
    if (!unk && !over)
      return 0u;
    if (!over)
      return 1u;
    if (!unk)
      return 2u;
    return 3u;
  };

  std::vector<size_t> ordered_latent_indices;
  ordered_latent_indices.reserve(latent_exit_states.size());
  for (size_t i = 0; i < latent_exit_states.size(); ++i) {
    const auto &ls = latent_exit_states[i];
    if (!ls.isStopped())
      continue;
    if (!ls.isLatentAbortProgram() && !ls.isLatentInvalidAccess())
      continue;
    if (!ls.getAstate())
      continue;
    ordered_latent_indices.push_back(i);
  }
  std::stable_sort(ordered_latent_indices.begin(), ordered_latent_indices.end(),
                   [&](size_t a, size_t b) {
                     return stateQualityTier(latent_exit_states[a].getAstate()) <
                            stateQualityTier(latent_exit_states[b].getAstate());
                   });

  std::vector<size_t> ordered_exit_indices;
  ordered_exit_indices.reserve(exit_states.size());
  for (size_t i = 0; i < exit_states.size(); ++i) {
    const auto &es = exit_states[i];
    if (es.isAbortProgram() || es.isLatentAbortProgram() ||
        es.isLatentInvalidAccess() || es.isLatentSpecializedTypeIssue()) {
      continue;
    }
    if (!es.getAstate())
      continue;
    ordered_exit_indices.push_back(i);
  }
  std::stable_sort(ordered_exit_indices.begin(), ordered_exit_indices.end(),
                   [&](size_t a, size_t b) {
                     return stateQualityTier(exit_states[a].getAstate()) <
                            stateQualityTier(exit_states[b].getAstate());
                   });

  // Never let latent entries completely starve normal return summaries when
  // normal exit states exist (otherwise callers calling with valid arguments
  // get empty summary application and fall back to declareUnknownValues()).
  const unsigned kMaxLatentEntries =
      ordered_exit_indices.empty() ? kMaxDisjuncts
                                   : std::min(kMaxDisjuncts, 6u);
  unsigned latent_added = 0;
  for (size_t idx : ordered_latent_indices) {
    const auto &latent_state = latent_exit_states[idx];
    if (latent_added >= kMaxLatentEntries ||
        summary.getPrePostList().size() >= kMaxDisjuncts)
      break;
    const AbductiveDomain *astate = latent_state.getAstate();

    const auto &stopped = latent_state.getStoppedExecution();
    SummaryEntry::LatentIssueSummary latent;

    if (latent_state.isLatentInvalidAccess()) {
      latent.is_latent_invalid_access = true;
      latent.diagnostic = OperationResult::NullDereference;
      latent.address = astate->getCanonical(stopped.address);
      latent.trace = stopped.must_be_valid.first.clone();
      latent.must_be_valid_reason = stopped.must_be_valid.second;
      latent.calling_context = stopped.calling_context;
    } else if (stopped.latent_issue) {
      auto *issue = stopped.latent_issue;
      latent.diagnostic = issue->getDiagnostic();
      latent.address = astate->getCanonical(issue->getAddress());
      constexpr size_t kMaxTraceEventsForSummary = 1024;
      constexpr size_t kMaxCallingContextForSummary = 1024;
      if (issue->getTrace().getEvents().size() <= kMaxTraceEventsForSummary &&
          issue->getCallingContext().size() <= kMaxCallingContextForSummary) {
        latent.trace = issue->getTrace().clone();
        latent.calling_context = issue->getCallingContext();
      }
    } else {
      latent.diagnostic = stopped.diagnostic;
      latent.address = astate->getCanonical(stopped.address);
      latent.trace = stopped.trace_to_issue.clone();
      latent.calling_context = stopped.calling_context;
    }

    AbductiveDomain pruned = pruneForSummary(
        F, *astate, std::nullopt, false, latent.address,
        /*keep_non_ground_conditions=*/true);

    bool dup_latent = false;
    for (const auto &existing : summary.getPrePostList()) {
      const auto &ex_lat = existing.getLatentIssue();
      if (!ex_lat.has_value())
        continue;
      if (ex_lat->diagnostic == latent.diagnostic &&
          ex_lat->is_latent_invalid_access == latent.is_latent_invalid_access &&
          existing.getPost() &&
          AbductiveDomain::leq(pruned, *existing.getPost())) {
        dup_latent = true;
        break;
      }
    }
    if (dup_latent) {
      continue;
    }

    PulseFormula formula = pruned.getPathFormula().clone();
    merged_transitive =
        TransitiveInfo::merge(merged_transitive, pruned.getTransitiveInfo());
    auto shared_dom =
        std::make_shared<const AbductiveDomain>(std::move(pruned));
    summary.addPrePost(SummaryEntry(
        std::move(shared_dom), std::move(formula), std::nullopt,
        std::optional<SummaryEntry::LatentIssueSummary>(std::move(latent))));
    latent_added++;
  }

  bool has_any_entry = latent_added > 0;
  const unsigned kMaxSummaryNormalEntries = std::min(kMaxDisjuncts, 10u);
  const unsigned max_normal_entries =
      kMaxSummaryNormalEntries > 0 ? kMaxSummaryNormalEntries : 0u;
  const bool run_leak_check = (spec_key == nullptr);

  for (size_t idx : ordered_exit_indices) {
    const auto &exit_state = exit_states[idx];
    const AbductiveDomain *astate = exit_state.getAstate();

    has_any_entry = true;
    const bool is_exception = exit_state.isExceptionRaised();
    std::optional<AbstractValue> ret_val = std::nullopt;
    std::optional<AbstractValue> exc_val = exit_state.getExceptionValue();
    std::string exc_type = exit_state.getExceptionType();
    if (exit_state.isStopped()) {
      ret_val = exit_state.getStoppedExecution().return_value;
    }

    const unsigned normal_entries =
        static_cast<unsigned>(summary.getPrePostList().size()) - latent_added;
    if (normal_entries >= max_normal_entries) {
      break;
    }

    std::optional<AbstractValue> root_val = is_exception ? exc_val : ret_val;
    AbductiveDomain pruned = pruneForSummary(
        F, *astate, root_val, run_leak_check && !is_exception, std::nullopt,
        /*keep_non_ground_conditions=*/true);

    // Deduplicate structurally isomorphic summary entries (Infer's leq check).
    bool duplicate = false;
    for (const auto &existing : summary.getPrePostList()) {
      if (existing.getLatentIssue().has_value() ||
          existing.isExceptionRaised() != is_exception ||
          existing.getExceptionType() != exc_type)
        continue;
      if (existing.getPost() && AbductiveDomain::leq(pruned, *existing.getPost())) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    if (exc_val) {
      exc_val = pruned.getCanonical(*exc_val);
    }

    PulseFormula formula = pruned.getPathFormula().clone();
    merged_transitive =
        TransitiveInfo::merge(merged_transitive, pruned.getTransitiveInfo());
    auto shared_dom =
        std::make_shared<const AbductiveDomain>(std::move(pruned));
    summary.addPrePost(SummaryEntry(std::move(shared_dom), std::move(formula),
                                    ret_val, std::nullopt, is_exception,
                                    exc_val, std::move(exc_type)));
  }

  if (!has_any_entry) {
    if (!spec_key) {
      // Cache an empty summary (preserving non-disjunctive and transitive info)
      // so callers do not re-analyze this procedure from scratch on every call.
      summary.setTransitiveInfo(std::move(merged_transitive));
      summary.setNonDisj(analysis_non_disj_.createSummary());
      summary_manager_.storeSummary(F, std::move(summary));
    }
    return;
  }

  for (const auto &Arg : F->args()) {
    AbstractValue formal_av = factory_.getOrCreate(&Arg);
    summary.setFormalAV(&Arg, formal_av);
  }
  summary.setTransitiveInfo(std::move(merged_transitive));
  if (!spec_key) {
    summary.setNonDisj(analysis_non_disj_.createSummary());
    summary_manager_.storeSummary(F, std::move(summary));
  } else {
    specialization_manager_.addSpecializedSummary(F, *spec_key,
                                                  std::move(summary_ptr));
  }
}

std::vector<ExecutionDomain> PulseChecker::applySummary(
    const llvm::Function *callee, const ExecutionDomain &caller_state,
    const llvm::CallInst *CI, const llvm::BasicBlock *pred) {
  return applySummaryImproved(callee, caller_state, CI, pred, nullptr);
}

} // namespace pulse
