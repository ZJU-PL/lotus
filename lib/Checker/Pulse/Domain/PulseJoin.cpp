#include "Checker/Pulse/Domain/PulseJoin.h"

#include "Checker/Pulse/Core/PulsePathContext.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Report/PulseLogger.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <optional>

namespace pulse {

//===----------------------------------------------------------------------===//
// PulseJoin
//
// Joins two `AbductiveDomain`s at the same program point.
//
// Sound incorrectness note:
// - Joining must not conjoin path conditions (that would discard feasible
//   witness paths). The formula join used here is a disjunction approximation.
// - Heap/stack joins introduce fresh join values when the two sides disagree.
//   This intentionally forgets some relational structure, trading recall for
//   scalability while preserving witnessability.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// JoinState Helper Methods
//===----------------------------------------------------------------------===//

static ValueHistory joinHistories(const ValueHistory &hist1,
                                  const ValueHistory &hist2) {
  if (hist1.sharesHead(hist2)) {
    return hist1;
  }
  // If histories are equal, return one of them
  if (hist1.isEmpty() && hist2.isEmpty()) {
    return hist1;
  }
  if (hist1.isEmpty()) {
    return hist2;
  }
  if (hist2.isEmpty()) {
    return hist1;
  }

  // Check if histories are equal by comparing events
  const auto &events1 = hist1.getEvents();
  const auto &events2 = hist2.getEvents();

  if (events1.size() == events2.size()) {
    bool equal = true;
    for (size_t i = 0; i < events1.size(); ++i) {
      if (events1[i].kind != events2[i].kind ||
          events1[i].location != events2[i].location ||
          events1[i].function != events2[i].function) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return hist1; // Histories are equal
    }
  }

  // Histories differ: find common prefix and merge
  // Find the longest common prefix
  size_t common_prefix = 0;
  size_t min_size = std::min(events1.size(), events2.size());
  for (size_t i = 0; i < min_size; ++i) {
    if (events1[i].kind == events2[i].kind &&
        events1[i].location == events2[i].location &&
        events1[i].function == events2[i].function) {
      common_prefix++;
    } else {
      break;
    }
  }

  // If there's a common prefix, return history with that prefix
  // Otherwise, return epoch (empty history)
  if (common_prefix > 0) {
    ValueHistory result;
    // Rebuild history with common prefix events
    for (size_t i = 0; i < common_prefix; ++i) {
      result.addEvent(events1[i].kind, events1[i].location, events1[i].function,
                      events1[i].description);
    }
    return result;
  }

  // No common prefix: return epoch (conservative)
  return ValueHistory();
}

static ValueHistory joinHistoriesOpts(std::optional<ValueHistory> hist1_opt,
                                      std::optional<ValueHistory> hist2_opt) {
  if (hist1_opt && hist2_opt) {
    return joinHistories(*hist1_opt, *hist2_opt);
  }
  // If only one side has history, return epoch
  return ValueHistory();
}

//===----------------------------------------------------------------------===//
// Value Joining
//===----------------------------------------------------------------------===//

std::pair<AbstractValue, ValueHistory>
PulseJoin::joinValuesHists(JoinState &state, const AbstractValue &lhs_val,
                           const ValueHistory &lhs_hist,
                           const AbstractValue &rhs_val,
                           const ValueHistory &rhs_hist) {
  using OptPair =
      std::pair<std::optional<AbstractValue>, std::optional<AbstractValue>>;

  if (lhs_val == rhs_val) {
    // Same value: x↦v ⊔ x↦v = x↦v
    state.rev_subst[lhs_val] = OptPair{lhs_val, rhs_val};
    return {lhs_val, joinHistories(lhs_hist, rhs_hist)};
  }

  // Different values: x↦v ⊔ x↦v' = x↦v''
  const uint64_t packed = JoinState::packOptPair(lhs_val, rhs_val);
  auto it = state.subst.find(packed);
  if (it != state.subst.end()) {
    // Already joined this pair
    return {it->second, joinHistories(lhs_hist, rhs_hist)};
  }

  // Create fresh joined value
  AbstractValue v_join = state.factory->createFresh(nullptr);
  state.subst[packed] = v_join;
  state.rev_subst[v_join] = OptPair{lhs_val, rhs_val};

  return {v_join, joinHistories(lhs_hist, rhs_hist)};
}

std::pair<AbstractValue, ValueHistory> PulseJoin::joinValuesHistsOpts(
    JoinState &state, const AbductiveDomain &lhs_astate,
    const std::optional<std::pair<AbstractValue, ValueHistory>> &lhs_opt,
    const AbductiveDomain &rhs_astate,
    const std::optional<std::pair<AbstractValue, ValueHistory>> &rhs_opt) {
  (void)lhs_astate;
  (void)rhs_astate;
  using OptPair =
      std::pair<std::optional<AbstractValue>, std::optional<AbstractValue>>;

  if (!lhs_opt && !rhs_opt) {
    // Both empty - shouldn't happen
    assert(false && "Both sides empty in join");
    AbstractValue v_join = state.factory->createFresh();
    return {v_join, ValueHistory()};
  }

  if (!lhs_opt || !rhs_opt) {
    // One-sided: x↦v ⊔ emp = x↦v' (memoized to prevent fresh-AV blowup)
    std::optional<AbstractValue> l_av =
        lhs_opt ? std::optional<AbstractValue>(lhs_opt->first) : std::nullopt;
    std::optional<AbstractValue> r_av =
        rhs_opt ? std::optional<AbstractValue>(rhs_opt->first) : std::nullopt;
    const uint64_t packed = JoinState::packOptPair(l_av, r_av);
    auto it = state.subst.find(packed);
    if (it != state.subst.end()) {
      return {it->second, ValueHistory()};
    }
    AbstractValue v_join = state.factory->createFresh(nullptr);
    state.subst[packed] = v_join;
    state.rev_subst[v_join] = OptPair{l_av, r_av};
    ValueHistory hist_join = joinHistoriesOpts(
        lhs_opt ? std::optional<ValueHistory>(lhs_opt->second) : std::nullopt,
        rhs_opt ? std::optional<ValueHistory>(rhs_opt->second) : std::nullopt);
    return {v_join, hist_join};
  }

  // Both sides have values
  if (lhs_opt->first == rhs_opt->first) {
    // Same value: x↦v ⊔ x↦v = x↦v
    state.rev_subst[lhs_opt->first] = OptPair{lhs_opt->first, rhs_opt->first};
    return {lhs_opt->first, joinHistories(lhs_opt->second, rhs_opt->second)};
  }

  // Different values: use cached join
  const uint64_t packed =
      JoinState::packOptPair(lhs_opt->first, rhs_opt->first);
  auto it = state.subst.find(packed);
  if (it != state.subst.end()) {
    return {it->second, joinHistories(lhs_opt->second, rhs_opt->second)};
  }

  return joinValuesHists(state, lhs_opt->first, lhs_opt->second, rhs_opt->first,
                         rhs_opt->second);
}

//===----------------------------------------------------------------------===//
// Heap Joining (iterative, in-place, bounded)
//===----------------------------------------------------------------------===//

void PulseJoin::joinHeaps(
    JoinState &state, Heap &heap_join, const AbductiveDomain &lhs_astate,
    const std::optional<std::pair<AbstractValue, ValueHistory>> &root_lhs_opt,
    const AbductiveDomain &rhs_astate,
    const std::optional<std::pair<AbstractValue, ValueHistory>> &root_rhs_opt,
    bool use_pre_heap) {
  using OptVH = std::optional<std::pair<AbstractValue, ValueHistory>>;

  if (!root_lhs_opt && !root_rhs_opt) {
    return;
  }

  constexpr size_t kMaxJoinVisitedPairs = 512;
  constexpr unsigned kMaxJoinDepth = 24;

  const Heap &lhs_heap =
      use_pre_heap ? lhs_astate.getPreHeap() : lhs_astate.getPostHeap();
  const Heap &rhs_heap =
      use_pre_heap ? rhs_astate.getPreHeap() : rhs_astate.getPostHeap();
  const auto &lhs_edges_map = lhs_heap.getEdges();
  const auto &rhs_edges_map = rhs_heap.getEdges();

  struct WorkItem {
    OptVH lhs_opt;
    OptVH rhs_opt;
    unsigned depth;
  };
  std::deque<WorkItem> worklist;
  worklist.push_back(WorkItem{root_lhs_opt, root_rhs_opt, 0});

  while (!worklist.empty()) {
    WorkItem item = std::move(worklist.front());
    worklist.pop_front();

    if (!item.lhs_opt && !item.rhs_opt) {
      continue;
    }

    const uint64_t visited_key = JoinState::packOptPair(
        item.lhs_opt ? std::optional<AbstractValue>(item.lhs_opt->first)
                     : std::nullopt,
        item.rhs_opt ? std::optional<AbstractValue>(item.rhs_opt->first)
                     : std::nullopt);

    if (state.visited.count(visited_key) > 0) {
      continue;
    }
    if (state.visited.size() >= kMaxJoinVisitedPairs) {
      break;
    }
    state.visited.insert(visited_key);

    auto v_hist_join = joinValuesHistsOpts(state, lhs_astate, item.lhs_opt,
                                           rhs_astate, item.rhs_opt);
    if (use_pre_heap) {
      heap_join.registerAddress(v_hist_join.first);
    }

    if (item.depth >= kMaxJoinDepth) {
      continue;
    }

    // Collect edges from both sides
    const std::map<Access, Address> *lhs_edges = nullptr;
    const std::map<Access, Address> *rhs_edges = nullptr;
    if (item.lhs_opt) {
      auto lhs_it = lhs_edges_map.find(item.lhs_opt->first);
      if (lhs_it != lhs_edges_map.end() && !lhs_it->second.empty()) {
        lhs_edges = &lhs_it->second;
      }
    }
    if (item.rhs_opt) {
      auto rhs_it = rhs_edges_map.find(item.rhs_opt->first);
      if (rhs_it != rhs_edges_map.end() && !rhs_it->second.empty()) {
        rhs_edges = &rhs_it->second;
      }
    }
    if (!lhs_edges && !rhs_edges) {
      continue;
    }

    auto processEdge = [&](const Access &access, const Address *lhs_tgt,
                           const Address *rhs_tgt) {
      OptVH lhs_target_opt;
      OptVH rhs_target_opt;
      if (lhs_tgt) {
        lhs_target_opt = {{lhs_tgt->addr, lhs_tgt->history}};
      }
      if (rhs_tgt) {
        rhs_target_opt = {{rhs_tgt->addr, rhs_tgt->history}};
      }

      auto target_v_hist = joinValuesHistsOpts(
          state, lhs_astate, lhs_target_opt, rhs_astate, rhs_target_opt);

      Address target_addr(target_v_hist.first);
      target_addr.history = target_v_hist.second;
      heap_join.addEdge(v_hist_join.first, access, target_addr);
      if (use_pre_heap) {
        heap_join.registerAddress(target_v_hist.first);
      }

      if (state.visited.size() < kMaxJoinVisitedPairs) {
        worklist.push_back(WorkItem{std::move(lhs_target_opt),
                                    std::move(rhs_target_opt), item.depth + 1});
      }
    };

    // Zero-allocation sorted merge over lhs_edges and rhs_edges.
    if (lhs_edges && rhs_edges) {
      auto lit = lhs_edges->begin();
      auto rit = rhs_edges->begin();
      while (lit != lhs_edges->end() && rit != rhs_edges->end()) {
        if (lit->first < rit->first) {
          processEdge(lit->first, &lit->second, nullptr);
          ++lit;
        } else if (rit->first < lit->first) {
          processEdge(rit->first, nullptr, &rit->second);
          ++rit;
        } else {
          processEdge(lit->first, &lit->second, &rit->second);
          ++lit;
          ++rit;
        }
      }
      for (; lit != lhs_edges->end(); ++lit) {
        processEdge(lit->first, &lit->second, nullptr);
      }
      for (; rit != rhs_edges->end(); ++rit) {
        processEdge(rit->first, nullptr, &rit->second);
      }
    } else if (lhs_edges) {
      for (const auto &kv : *lhs_edges) {
        processEdge(kv.first, &kv.second, nullptr);
      }
    } else if (rhs_edges) {
      for (const auto &kv : *rhs_edges) {
        processEdge(kv.first, nullptr, &kv.second);
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Stack Joining
//===----------------------------------------------------------------------===//

std::pair<Stack, Heap>
PulseJoin::joinStacks(JoinState &state, const AbductiveDomain &lhs_astate,
                      const AbductiveDomain &rhs_astate) {
  Stack stack_pre_join;
  Heap heap_pre_join;

  auto processVar = [&](const llvm::Value *var, const Address *lhs_addr,
                        const Address *rhs_addr) {
    std::optional<std::pair<AbstractValue, ValueHistory>> lhs_opt;
    std::optional<std::pair<AbstractValue, ValueHistory>> rhs_opt;
    if (lhs_addr) {
      lhs_opt = {{lhs_addr->addr, lhs_addr->history}};
    }
    if (rhs_addr) {
      rhs_opt = {{rhs_addr->addr, rhs_addr->history}};
    }
    auto v_hist_join =
        joinValuesHistsOpts(state, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
    joinHeaps(state, heap_pre_join, lhs_astate, lhs_opt, rhs_astate, rhs_opt,
              /*use_pre_heap=*/true);
    Address joined_addr(v_hist_join.first);
    joined_addr.history = v_hist_join.second;
    stack_pre_join.add(var, joined_addr);
  };

  const auto &lhs_map = lhs_astate.getPreStack().getMap();
  const auto &rhs_map = rhs_astate.getPreStack().getMap();
  auto lit = lhs_map.begin();
  auto rit = rhs_map.begin();
  while (lit != lhs_map.end() && rit != rhs_map.end()) {
    if (lit->first < rit->first) {
      processVar(lit->first, &lit->second, nullptr);
      ++lit;
    } else if (rit->first < lit->first) {
      processVar(rit->first, nullptr, &rit->second);
      ++rit;
    } else {
      processVar(lit->first, &lit->second, &rit->second);
      ++lit;
      ++rit;
    }
  }
  for (; lit != lhs_map.end(); ++lit) {
    processVar(lit->first, &lit->second, nullptr);
  }
  for (; rit != rhs_map.end(); ++rit) {
    processVar(rit->first, nullptr, &rit->second);
  }

  return {std::move(stack_pre_join), std::move(heap_pre_join)};
}

//===----------------------------------------------------------------------===//
// Attribute Joining
//===----------------------------------------------------------------------===//

std::optional<Attribute> PulseJoin::joinOneSidedAttribute(Attribute attr) {
  // One-sided attributes: keep if they're "weak" (true in some branches),
  // drop if they're "strong" (must be true in all branches)
  switch (attr) {
  case Attribute::Allocated:
    // Keep: if allocated in one branch, it might be allocated
    return attr;

  case Attribute::Stack:
  case Attribute::Global:
    // Base-kind facts must be stable; drop on one-sided to avoid guessing.
    return std::nullopt;

  case Attribute::Invalid:
  case Attribute::Null:
  case Attribute::Uninitialized:
    // Drop: these are "strong" - if only in one branch, we can't assume them
    return std::nullopt;

  case Attribute::Tainted:
    // Keep: if tainted in one branch, conservatively assume tainted
    // (matches Infer's approach of weakening one-sided attributes)
    return attr;

  case Attribute::FileHandle:
  case Attribute::Lock:
  case Attribute::AsyncResource:
    // Drop: resource attributes are too strong for one-sided join
    return std::nullopt;

  default:
    return std::nullopt;
  }
}

std::optional<Attribute>
PulseJoin::joinTwoSidedAttribute(JoinState &state, Attribute attr1,
                                 Attribute attr2, AbstractValue lhs_val,
                                 AbstractValue rhs_val) {
  if (attr1 == attr2) {
    return attr1; // Same attribute
  }

  // Handle specific attribute types
  switch (attr1) {
  case Attribute::Allocated:
    if (attr2 == Attribute::Allocated) {
      return Attribute::Allocated; // Both allocated
    }
    // Allocated vs Invalid/Null - incompatible
    return std::nullopt;

  case Attribute::Stack:
    if (attr2 == Attribute::Stack) {
      return Attribute::Stack;
    }
    return std::nullopt;

  case Attribute::Global:
    if (attr2 == Attribute::Global) {
      return Attribute::Global;
    }
    return std::nullopt;

  case Attribute::Invalid:
    if (attr2 == Attribute::Invalid) {
      return Attribute::Invalid; // Both invalid
    }
    // Invalid vs Allocated - incompatible
    return std::nullopt;

  case Attribute::Null:
    if (attr2 == Attribute::Null) {
      return Attribute::Null; // Both null
    }
    // Null vs NonNull - incompatible
    return std::nullopt;

  case Attribute::Uninitialized:
    if (attr2 == Attribute::Uninitialized) {
      return Attribute::Uninitialized; // Both uninitialized
    }
    // Uninitialized vs Initialized - incompatible
    return std::nullopt;

  case Attribute::Tainted:
    if (attr2 == Attribute::Tainted) {
      return Attribute::Tainted; // Both tainted (union of taint sources)
    }
    // Tainted vs NotTainted - keep tainted (conservative)
    return Attribute::Tainted;

  case Attribute::FileHandle:
  case Attribute::Lock:
  case Attribute::AsyncResource:
    // Resource attributes: if both have same resource, keep it
    if (attr1 == attr2) {
      return attr1;
    }
    // Different resources - incompatible
    return std::nullopt;

  default:
    // Unknown attribute type - incompatible
    return std::nullopt;
  }
}

AttributeSet
PulseJoin::joinAttributes(JoinState &state, const AbductiveDomain &lhs_astate,
                          const AbductiveDomain &rhs_astate, bool use_pre_attrs,
                          AbstractValue joined_addr,
                          std::optional<AbstractValue> lhs_addr_opt,
                          std::optional<AbstractValue> rhs_addr_opt) {
  AttributeSet result;

  AttributeSet lhs_attrs;
  AttributeSet rhs_attrs;

  if (lhs_addr_opt) {
    lhs_attrs = use_pre_attrs ? lhs_astate.getPreAttrs().get(*lhs_addr_opt)
                              : lhs_astate.getPostAttrs().get(*lhs_addr_opt);
  }
  if (rhs_addr_opt) {
    rhs_attrs = use_pre_attrs ? rhs_astate.getPreAttrs().get(*rhs_addr_opt)
                              : rhs_astate.getPostAttrs().get(*rhs_addr_opt);
  }

  // Collect all attributes
  std::set<Attribute> all_attrs;
  for (Attribute attr : lhs_attrs) {
    all_attrs.insert(attr);
  }
  for (Attribute attr : rhs_attrs) {
    all_attrs.insert(attr);
  }

  // Join each attribute
  for (Attribute attr : all_attrs) {
    bool in_lhs = lhs_attrs.count(attr) > 0;
    bool in_rhs = rhs_attrs.count(attr) > 0;

    std::optional<Attribute> joined_attr;
    if (in_lhs && in_rhs) {
      // Two-sided
      if (lhs_addr_opt && rhs_addr_opt) {
        joined_attr = joinTwoSidedAttribute(state, attr, attr, *lhs_addr_opt,
                                            *rhs_addr_opt);
      } else {
        joined_attr = attr; // Same attribute on both sides
      }
    } else {
      // One-sided
      joined_attr = joinOneSidedAttribute(attr);
    }

    if (joined_attr) {
      result.insert(*joined_attr);
    }
  }

  return result;
}

//===----------------------------------------------------------------------===//
// Formula Joining
//===----------------------------------------------------------------------===//

PulseFormula PulseJoin::joinFormulas(const AbductiveDomain &lhs,
                                     const AbductiveDomain &rhs) {
  const PulseFormula &lhs_formula = lhs.getPathFormula();
  const PulseFormula &rhs_formula = rhs.getPathFormula();

  // Join formulas (disjunction): represent "lhs path condition OR rhs path
  // condition". Disjunctive equalities from rev_subst are attached in
  // joinAbductive once the join substitution map has been built.
  return PulseFormula::join(lhs_formula, rhs_formula);
}

//===----------------------------------------------------------------------===//
// Main Join Operations
//===----------------------------------------------------------------------===//

std::optional<AbductiveDomain>
PulseJoin::joinAbductive(const AbductiveDomain &lhs,
                         const AbductiveDomain &rhs) {
  // Value-renaming joins would also need to rename temporal registers.
  // Keep these temporal paths as separate disjuncts instead.
  if (!lhs.getToplHistory().events.empty() ||
      !rhs.getToplHistory().events.empty())
    return std::nullopt;
  PulseLogger::trace("Joining abductive domains");
  PulseLogger::incrementCounter("joins.performed");

  // Check formula consistency first (preliminary check)
  PulseFormula preliminary_formula = joinFormulas(lhs, rhs);
  if (!preliminary_formula.isConsistent() || preliminary_formula.isUnsat()) {
    PulseLogger::debug("Join failed: formula contradiction");
    PulseLogger::incrementCounter("joins.failed");
    return std::nullopt; // Contradiction
  }

  // Create join state with factory
  // We need a factory - create a temporary one
  AbstractValueFactory factory;
  JoinState join_state(&factory);

  // Join pre stacks (this also joins pre heaps iteratively in place)
  auto pre_result = joinStacks(join_state, lhs, rhs);
  Stack stack_pre_join = std::move(pre_result.first);
  Heap heap_pre_join = std::move(pre_result.second);

  // Join post stacks and heaps separately
  Stack stack_post_join;
  Heap heap_post_join;

  // Reset visited set for post join (as in Infer)
  join_state.visited.clear();

  // Join post stack via zero-allocation sorted merge
  {
    auto processPostVar = [&](const llvm::Value *var, const Address *lhs_addr,
                              const Address *rhs_addr) {
      std::optional<std::pair<AbstractValue, ValueHistory>> lhs_opt;
      std::optional<std::pair<AbstractValue, ValueHistory>> rhs_opt;
      if (lhs_addr) {
        lhs_opt = {{lhs_addr->addr, lhs_addr->history}};
      }
      if (rhs_addr) {
        rhs_opt = {{rhs_addr->addr, rhs_addr->history}};
      }
      auto v_hist_join =
          joinValuesHistsOpts(join_state, lhs, lhs_opt, rhs, rhs_opt);
      joinHeaps(join_state, heap_post_join, lhs, lhs_opt, rhs, rhs_opt);
      Address joined_addr(v_hist_join.first);
      joined_addr.history = v_hist_join.second;
      stack_post_join.add(var, joined_addr);
    };
    const auto &lhs_map = lhs.getPostStack().getMap();
    const auto &rhs_map = rhs.getPostStack().getMap();
    auto lit = lhs_map.begin();
    auto rit = rhs_map.begin();
    while (lit != lhs_map.end() && rit != rhs_map.end()) {
      if (lit->first < rit->first) {
        processPostVar(lit->first, &lit->second, nullptr);
        ++lit;
      } else if (rit->first < lit->first) {
        processPostVar(rit->first, nullptr, &rit->second);
        ++rit;
      } else {
        processPostVar(lit->first, &lit->second, &rit->second);
        ++lit;
        ++rit;
      }
    }
    for (; lit != lhs_map.end(); ++lit) {
      processPostVar(lit->first, &lit->second, nullptr);
    }
    for (; rit != rhs_map.end(); ++rit) {
      processPostVar(rit->first, nullptr, &rit->second);
    }
  }

  // Join attributes using rev_subst
  AddressAttributes attrs_pre_join, attrs_post_join;

  // For each joined value in rev_subst, join its attributes
  // This follows Infer's pattern: iterate through rev_subst to join attributes
  for (const auto &kv : join_state.rev_subst) {
    AbstractValue v_join = kv.first;
    const auto &pair_opt = kv.second;
    std::optional<AbstractValue> lhs_addr_opt = pair_opt.first;
    std::optional<AbstractValue> rhs_addr_opt = pair_opt.second;

    // Join post attributes
    AttributeSet joined_post_attrs =
        joinAttributes(join_state, lhs, rhs, /*use_pre_attrs=*/false, v_join,
                       lhs_addr_opt, rhs_addr_opt);
    for (Attribute attr : joined_post_attrs) {
      attrs_post_join.add(v_join, attr);
    }

    // Join pre attributes
    AttributeSet joined_pre_attrs =
        joinAttributes(join_state, lhs, rhs, /*use_pre_attrs=*/true, v_join,
                       lhs_addr_opt, rhs_addr_opt);
    for (Attribute attr : joined_pre_attrs) {
      attrs_pre_join.add(v_join, attr);
    }
  }

  // Reuse already-computed formula join instead of joining a second time.
  PulseFormula joined_formula = std::move(preliminary_formula);

  // Add disjunctive equalities from rev_subst: for each v_join mapping to
  // (v_lhs, v_rhs), record `v_join = v_lhs ∨ v_join = v_rhs` (and exact
  // constant disjunction / interval hull / non-nullness) so relational facts
  // survive joins (Infer PulseJoin + PulseFormula.join).
  const PulseFormula &lhs_f = lhs.getPathFormula();
  const PulseFormula &rhs_f = rhs.getPathFormula();
  llvm::DenseSet<AbstractValue> lhs_constrained;
  llvm::DenseSet<AbstractValue> rhs_constrained;
  lhs_f.collectConstrainedValues(lhs_constrained);
  rhs_f.collectConstrainedValues(rhs_constrained);
  for (const auto &kv : join_state.rev_subst) {
    AbstractValue v_join = kv.first;
    const auto &pair_opt = kv.second;
    if (pair_opt.first && pair_opt.second) {
      AbstractValue l_av = *pair_opt.first;
      AbstractValue r_av = *pair_opt.second;
      if (v_join == l_av && v_join == r_av) {
        continue;
      }
      auto c1 = lhs_f.asConstantInt(l_av);
      auto c2 = rhs_f.asConstantInt(r_av);
      if (c1 && c2) {
        (void)joined_formula.addDisjunctiveEquality(v_join, l_av, r_av, c1, c2);
      } else {
        auto lo1 = lhs_f.getLowerBound(l_av);
        auto lo2 = rhs_f.getLowerBound(r_av);
        auto hi1 = lhs_f.getUpperBound(l_av);
        auto hi2 = rhs_f.getUpperBound(r_av);
        if (lo1 && lo2 && hi1 && hi2) {
          (void)joined_formula.addBounds(v_join, std::min(*lo1, *lo2),
                                         std::max(*hi1, *hi2));
        } else if (lo1 && lo2) {
          (void)joined_formula.addBounds(v_join, std::min(*lo1, *lo2),
                                         std::numeric_limits<int64_t>::max());
        } else if (hi1 && hi2) {
          (void)joined_formula.addBounds(v_join,
                                         std::numeric_limits<int64_t>::min(),
                                         std::max(*hi1, *hi2));
        }
        if (lhs_f.isNonNull(l_av) && rhs_f.isNonNull(r_av)) {
          (void)joined_formula.addNonNull(v_join);
        } else if (lhs_f.isNull(l_av) && rhs_f.isNull(r_av)) {
          (void)joined_formula.addNull(v_join);
        }
        if (lhs_f.isInteger(l_av) && rhs_f.isInteger(r_av)) {
          joined_formula.addIntegerConstraint(v_join);
        }
        auto t1 = lhs_f.getExactType(l_av);
        auto t2 = rhs_f.getExactType(r_av);
        if (t1 && t2 && *t1 == *t2) {
          (void)joined_formula.addExactType(v_join, *t1);
        }
        if (lhs_constrained.count(lhs_f.getCanonicalReadOnly(l_av)) > 0 ||
            rhs_constrained.count(rhs_f.getCanonicalReadOnly(r_av)) > 0) {
          (void)joined_formula.addDisjunctiveEquality(v_join, l_av, r_av);
        }
      }
    } else if (pair_opt.first || pair_opt.second) {
      // Transfer unary facts from the present side directly onto `v_join`
      // without adding `v_join = v_side` into `equalities_`, which would
      // accumulate unbounded dead equality chains across repeated joins.
      const PulseFormula &src_f = pair_opt.first ? lhs_f : rhs_f;
      AbstractValue src_av = pair_opt.first ? *pair_opt.first : *pair_opt.second;
      auto lo = src_f.getLowerBound(src_av);
      auto hi = src_f.getUpperBound(src_av);
      if (lo && hi) {
        (void)joined_formula.addBounds(v_join, *lo, *hi);
      } else if (lo) {
        (void)joined_formula.addBounds(v_join, *lo,
                                       std::numeric_limits<int64_t>::max());
      } else if (hi) {
        (void)joined_formula.addBounds(v_join,
                                       std::numeric_limits<int64_t>::min(), *hi);
      }
      if (src_f.isNonNull(src_av)) {
        (void)joined_formula.addNonNull(v_join);
      } else if (src_f.isNull(src_av)) {
        (void)joined_formula.addNull(v_join);
      }
      if (src_f.isInteger(src_av)) {
        joined_formula.addIntegerConstraint(v_join);
      }
      if (auto t = src_f.getExactType(src_av)) {
        (void)joined_formula.addExactType(v_join, *t);
      }
    }
  }

  // Create joined domain
  AbductiveDomain joined;
  joined.getPostStack() = std::move(stack_post_join);
  joined.getPreStack() = std::move(stack_pre_join);
  joined.getPostHeap() = std::move(heap_post_join);
  joined.getPreHeap() = std::move(heap_pre_join);
  joined.getPostAttrs() = std::move(attrs_post_join);
  joined.getPreAttrs() = std::move(attrs_pre_join);
  joined.setPathFormula(
      std::make_unique<PulseFormula>(std::move(joined_formula)));

  // Join additional fields (from AbductiveDomain::merge logic)
  // Merge transitive info
  joined.setTransitiveInfo(
      TransitiveInfo::merge(lhs.getTransitiveInfo(), rhs.getTransitiveInfo()));

  // Merge unknown values flag (OR operation)
  joined.setUnknownValues(lhs.hasUnknownValues() || rhs.hasUnknownValues());

  // Merge skipped calls (union)
  for (const std::string &call : lhs.getSkippedCalls()) {
    joined.addSkippedCall(call);
  }
  for (const std::string &call : rhs.getSkippedCalls()) {
    joined.addSkippedCall(call);
  }

  // Merge need_dynamic_type_specialization (union)
  for (AbstractValue av : lhs.getNeedDynamicTypeSpecialization()) {
    joined.addNeedDynamicTypeSpecialization(av);
  }
  for (AbstractValue av : rhs.getNeedDynamicTypeSpecialization()) {
    joined.addNeedDynamicTypeSpecialization(av);
  }

  // Merge recursive calls (union)
  for (const std::string &call : lhs.getRecursiveCalls()) {
    joined.addRecursiveCall(call);
  }
  for (const std::string &call : rhs.getRecursiveCalls()) {
    joined.addRecursiveCall(call);
  }

  // Preserve loop header info and path stamps from lhs (as in Infer).
  for (const auto &kv : lhs.getLoopHeaderInfo()) {
    joined.pushLoopHeaderInfo(kv.first, kv.second);
  }
  joined.getLoopPathStamps() = lhs.getLoopPathStamps();
  joined.setTimestampCounter(
      std::max(lhs.getTimestampCounter(), rhs.getTimestampCounter()));

  // Preserve parameterized attributes, invalidation metadata, allocation
  // sizes/roots/info, dynamic types, and taint along rev_subst.
  for (const auto &kv : join_state.rev_subst) {
    AbstractValue v_join = kv.first;
    const auto &l_opt = kv.second.first;
    const auto &r_opt = kv.second.second;

    // Invalidation info (when both sides invalidated or attribute preserved)
    if (joined.getPostAttrs().has(v_join, Attribute::Invalid)) {
      if (l_opt) {
        if (auto inv = lhs.getInvalidationInfo(*l_opt)) {
          joined.setInvalidationInfo(v_join, inv->first, inv->second);
        }
      } else if (r_opt) {
        if (auto inv = rhs.getInvalidationInfo(*r_opt)) {
          joined.setInvalidationInfo(v_join, inv->first, inv->second);
        }
      }
    }

    // Allocation sizes: keep tighter bound when both sides know size.
    std::optional<uint64_t> l_sz =
        l_opt ? lhs.getAllocationSize(*l_opt) : std::nullopt;
    std::optional<uint64_t> r_sz =
        r_opt ? rhs.getAllocationSize(*r_opt) : std::nullopt;
    if (l_sz && r_sz) {
      joined.setAllocationSize(v_join, std::min(*l_sz, *r_sz));
    } else if (l_sz && !r_opt) {
      joined.setAllocationSize(v_join, *l_sz);
    } else if (r_sz && !l_opt) {
      joined.setAllocationSize(v_join, *r_sz);
    }

    // Allocation info (keep when present on both or one-sided weak).
    if (l_opt) {
      if (auto ai = lhs.getAllocationInfo(*l_opt)) {
        joined.setAllocationInfo(v_join, *ai);
      }
    } else if (r_opt) {
      if (auto ai = rhs.getAllocationInfo(*r_opt)) {
        joined.setAllocationInfo(v_join, *ai);
      }
    }

    // WrittenTo: keep if written on either side (weak / conservative for
    // unmodified-copy checking).
    if (l_opt) {
      if (auto wt = lhs.getWrittenTo(*l_opt)) {
        joined.markWrittenTo(v_join, wt->location, wt->trace.clone());
      }
    }
    if (r_opt) {
      if (auto wt = rhs.getWrittenTo(*r_opt)) {
        joined.markWrittenTo(v_join, wt->location, wt->trace.clone());
      }
    }

    // Pre MustBeValid & MustBeInitialized (keep when required on both sides).
    if (l_opt && r_opt) {
      auto l_mbv = lhs.getPreMustBeValid().find(lhs.getCanonical(*l_opt));
      auto r_mbv = rhs.getPreMustBeValid().find(rhs.getCanonical(*r_opt));
      if (l_mbv != lhs.getPreMustBeValid().end() &&
          r_mbv != rhs.getPreMustBeValid().end()) {
        joined.getPreMustBeValid().emplace(v_join, l_mbv->second);
      }
      auto l_mbi = lhs.getPreMustBeInitialized().find(lhs.getCanonical(*l_opt));
      auto r_mbi = rhs.getPreMustBeInitialized().find(rhs.getCanonical(*r_opt));
      if (l_mbi != lhs.getPreMustBeInitialized().end() &&
          r_mbi != rhs.getPreMustBeInitialized().end()) {
        joined.getPreMustBeInitialized().emplace(v_join, l_mbi->second);
      }
      auto l_dt = lhs.getDynamicType(*l_opt);
      auto r_dt = rhs.getDynamicType(*r_opt);
      if (l_dt && r_dt && *l_dt == *r_dt) {
        joined.setDynamicType(v_join, *l_dt);
      }
    }

    // Taint: union of taint items from both sides.
    if (l_opt && lhs.getTaintDomain().has(*l_opt)) {
      for (const auto &item : lhs.getTaintDomain().get(*l_opt)) {
        joined.getTaintDomain().add(v_join, item);
      }
    }
    if (r_opt && rhs.getTaintDomain().has(*r_opt)) {
      for (const auto &item : rhs.getTaintDomain().get(*r_opt)) {
        joined.getTaintDomain().add(v_join, item);
      }
    }
  }

  joined.canonicalize();
  return joined;
}

std::optional<std::pair<AbductiveDomain, PathContext>>
PulseJoin::join(const AbductiveDomain &lhs, const PathContext &path_lhs,
                const AbductiveDomain &rhs, const PathContext &path_rhs) {
  auto joined_domain_opt = joinAbductive(lhs, rhs);
  if (!joined_domain_opt) {
    return std::nullopt;
  }

  PathContext joined_path = PathContext::join(path_lhs, path_rhs);
  // Use std::make_pair to avoid copy constructor issues
  return std::optional<std::pair<AbductiveDomain, PathContext>>(
      std::make_pair(std::move(*joined_domain_opt), joined_path));
}

AbductiveDomain PulseJoin::joinSummaries(const AbductiveDomain &lhs,
                                         const AbductiveDomain &rhs) {
  auto joined_opt = joinAbductive(lhs, rhs);
  if (!joined_opt) {
    // If join fails, return empty domain (or could return lhs as fallback)
    return AbductiveDomain();
  }
  // Move the result to avoid copy constructor
  return std::move(*joined_opt);
}

} // namespace pulse
