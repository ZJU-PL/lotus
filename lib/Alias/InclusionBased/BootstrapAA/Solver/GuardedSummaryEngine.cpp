#include "Alias/InclusionBased/BootstrapAA/Solver/GuardedSummarySolver.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace lotus {
namespace bootstrap {
namespace detail {

std::size_t
GuardedSummarySolver::depthOfSummaryKey(const SummaryKey &key) const {
  if (key.kind == SummaryKind::ExitMem || key.target.kind == ExprKind::Mem) {
    const Id comp = m_hierarchy.object_components[key.target.id];
    if (!m_hierarchy.successors[comp].empty()) {
      std::size_t d = 0;
      for (Id succ : m_hierarchy.successors[comp])
        d = std::max(d, dovetailDepthOfComp(succ));
      return d;
    }
    return dovetailCyclicOfComp(comp) ? dovetailDepthOfComp(comp)
                                      : dovetailDepthOfComp(comp) + 1;
  }
  if (key.kind == SummaryKind::ExitReturn) {
    std::size_t d = 0;
    for (Id ret_site : m_index.return_sites[key.function]) {
      const Instruction &ri = *m_index.by_site[ret_site].inst;
      if (!ri.operands.empty() && ri.operands[0] != INVALID)
        d = std::max(
            d, dovetailDepthOfComp(m_hierarchy.value_components[ri.operands[0]]));
    }
    return d;
  }
  if (key.target.kind == ExprKind::Val && key.target.id != INVALID)
    return dovetailDepthOfComp(m_hierarchy.value_components[key.target.id]);
  if (key.target.kind == ExprKind::Deref && key.target.id != INVALID) {
    const Id comp = m_hierarchy.value_components[key.target.id];
    return dovetailCyclicOfComp(comp) ? dovetailDepthOfComp(comp)
                                      : dovetailDepthOfComp(comp) + 1;
  }
  return 0;
}

// Algorithm 5 (Kahlon PLDI '08, p. 257): Interprocedural backward summary
// computation with reverse-topological SCC handling, demand-driven recursion
// fixpoint, straight-line intra-block fast backward walk, and complete skipping
// of functions that cannot modify tracked locations.
const std::vector<SummaryEntry> &
GuardedSummarySolver::computeSummary(const SummaryKey &input_key) {
  SummaryKey root_key = input_key;
  // Exact intra-block SSA canonicalization: if `Val(v)` is queried at a
  // reachable point in the same basic block after its defining instruction
  // `def_site` (with no intervening redefinition of `v`), its backward summary
  // is identical to `(Val(v), def_site, Point::After)`. This avoids redundant
  // per-site summaries in long straight-line blocks.
  if (root_key.kind == SummaryKind::LocalPoint &&
      !m_options.path_sensitive_interprocedural &&
      root_key.target.kind == ExprKind::Val &&
      root_key.target.id != INVALID &&
      root_key.site < m_reachability.reachable_site.size() &&
      m_reachability.reachable_site[root_key.site]) {
    const auto &defs = m_index.defining_sites[root_key.target.id];
    if (!defs.empty()) {
      const InstLocation &qloc = m_index.by_site[root_key.site];
      Id latest_def = INVALID;
      std::size_t latest_idx = 0;
      for (Id dsite : defs) {
        const InstLocation &dloc = m_index.by_site[dsite];
        if (dloc.function == qloc.function && dloc.block == qloc.block &&
            (dloc.index < qloc.index ||
             (dloc.index == qloc.index && root_key.point == Point::After))) {
          if (latest_def == INVALID || dloc.index > latest_idx) {
            latest_def = dsite;
            latest_idx = dloc.index;
          }
        }
      }
      // Dominator-based SSA canonicalization: a single-def, non-PHI local SSA
      // value defined in a block that strictly dominates `qloc.block` has the
      // identical backward summary at `qloc` as at `(defs[0], Point::After)`.
      if (latest_def == INVALID && !m_options.path_sensitive &&
          defs.size() == 1 && m_index.phi_defs[root_key.target.id].empty()) {
        const InstLocation &dloc = m_index.by_site[defs[0]];
        if (dloc.function == qloc.function && dloc.block != qloc.block &&
            m_reachability.reachable_site[defs[0]] &&
            m_index.blockDominates(qloc.function, dloc.block, qloc.block))
          latest_def = defs[0];
      }
      if (latest_def != INVALID) {
        root_key.site = latest_def;
        root_key.point = Point::After;
      }
    }
  }
  if (m_finalized.count(root_key)) {
    if (m_path_pruned_summaries.count(root_key))
      ++m_stats.path_infeasible_prunes;
    return m_summaries[root_key];
  }

  const std::size_t sum_prunes_before = m_stats.path_infeasible_prunes;
  const Id root_scc = m_call_graph.function_scc[root_key.function];
  std::deque<WorkItem> work;
  using ProcKey = std::tuple<SummaryKey, CfgPos, Expr>;
  std::map<ProcKey,
           std::vector<std::tuple<GuardId, BddManager::NodeId, KilledConds>>>
      processed;
  std::set<SummaryKey> seeded_in_scc;
  std::set<SummaryKey> top_saturated;
  std::map<SummaryKey, std::set<Waiter>> waiters;

  auto recordState = [&](const SummaryKey &owner, CfgPos pos, const Expr &q,
                         GuardId &g, BddManager::NodeId &path,
                         KilledConds &killed) -> bool {
    if (g == GUARD_FALSE || m_bdd.isFalse(path) || top_saturated.count(owner))
      return false;
    auto &seen = processed[{owner, pos, q}];
    for (const auto &[sg, sp, sk] : seen) {
      if (guardSubsumes(sg, g) &&
          (!m_options.path_sensitive || m_bdd.isTrue(sp) || sp == path) &&
          (!m_options.path_sensitive_interprocedural ||
           killedSubsumes(sk, killed)))
        return false;
    }
    if (seen.size() >= m_options.max_guards_per_state) {
      g = GUARD_TRUE;
      path = BddManager::TRUE_NODE;
      if (m_options.path_sensitive_interprocedural) {
        killed.all = true;
        killed.vars.clear();
      }
      ++m_stats.guards_widened;
      for (const auto &[sg, sp, sk] : seen)
        if (sg == GUARD_TRUE && m_bdd.isTrue(sp) &&
            (!m_options.path_sensitive_interprocedural || sk.all))
          return false;
    }
    seen.erase(
        std::remove_if(
            seen.begin(), seen.end(),
            [&](const auto &prev) {
              return guardSubsumes(g, std::get<0>(prev)) &&
                     (!m_options.path_sensitive || m_bdd.isTrue(path) ||
                      path == std::get<1>(prev)) &&
                     (!m_options.path_sensitive_interprocedural ||
                      killedSubsumes(killed, std::get<2>(prev)));
            }),
        seen.end());
    seen.emplace_back(g, path, killed);
    return true;
  };

  auto enqueue = [&](const SummaryKey &owner, CfgPos pos, const Expr &q,
                     GuardId g, BddManager::NodeId path,
                     KilledConds killed = {}) {
    if (!recordState(owner, pos, q, g, path, killed))
      return;
    work.push_back({owner, pos, q, g, path, std::move(killed)});
  };

  auto seedKey = [&](const SummaryKey &key) {
    if (m_finalized.count(key) || !seeded_in_scc.insert(key).second)
      return;
    countContext(key);
    m_summaries[key];
    if (key.kind == SummaryKind::LocalPoint) {
      if (!m_reachability.reachable_site[key.site])
        return;
      enqueue(key, {key.site, key.point}, key.target, GUARD_TRUE,
              BddManager::TRUE_NODE);
      return;
    }
    if (!m_reachability.may_return[key.function])
      return;
    for (Id ret_site : m_index.return_sites[key.function]) {
      if (!m_reachability.reachable_site[ret_site])
        continue;
      const Instruction &ret = *m_index.by_site[ret_site].inst;
      if (key.kind == SummaryKind::ExitReturn) {
        if (ret.operands.empty())
          continue;
        const Id rv = ret.operands[0];
        if (rv == INVALID)
          enqueue(key, {ret_site, Point::Before}, Expr::makeTop(), GUARD_TRUE,
                  BddManager::TRUE_NODE);
        else if (m_program.values[rv].constant) {
          std::vector<SummaryEntry> srcs;
          emitPointsToSources(m_program.values[rv].initial, 0, true, 0,
                              GUARD_TRUE, BddManager::TRUE_NODE, srcs);
          for (const auto &s : srcs)
            enqueue(key, {ret_site, Point::Before}, s.source, s.guard, s.path);
        } else {
          enqueue(key, {ret_site, Point::Before}, Expr::makeVal(rv), GUARD_TRUE,
                  BddManager::TRUE_NODE);
        }
      } else {
        enqueue(key, {ret_site, Point::Before}, key.target, GUARD_TRUE,
                BddManager::TRUE_NODE);
      }
    }
  };

  auto addSummaryEntry = [&](const SummaryKey &owner, SummaryEntry entry) {
    if (top_saturated.count(owner))
      return;
    // Record function-scoped formal-argument conditions in `entry.killed` so
    // caller paths carrying colliding recursive vars are quantified. Under
    // `path_sensitive_interprocedural`, keep those conditions in `entry.path`
    // so `applyCallCondSubst()` substitutes (or quantifies) them at each call
    // site with the caller's actual arguments!
    if (owner.kind != SummaryKind::LocalPoint) {
      if (m_options.path_sensitive_interprocedural) {
        if (owner.function < m_program.functions.size())
          addKilledConds(
              entry.killed,
              m_program.functions[owner.function].entry_kills_conditions);
      } else {
        entry.path = quantifyFuncEntryConditions(owner.function, entry.path);
      }
    }
    if (!m_options.path_sensitive_interprocedural) {
      entry.path = BddManager::TRUE_NODE;
      entry.killed = {};
    }
    auto &list = m_summaries[owner];
    for (const SummaryEntry &existing : list) {
      if (existing.source == entry.source &&
          guardSubsumes(existing.guard, entry.guard) &&
          (!m_options.path_sensitive_interprocedural ||
           ((m_bdd.isTrue(existing.path) || existing.path == entry.path) &&
            killedSubsumes(existing.killed, entry.killed))))
        return;
    }
    std::size_t same_src = 0;
    for (const SummaryEntry &existing : list)
      if (existing.source == entry.source)
        ++same_src;
    if (same_src >= m_options.max_guards_per_state) {
      entry.guard = GUARD_TRUE;
      entry.path = BddManager::TRUE_NODE;
      if (m_options.path_sensitive_interprocedural) {
        entry.killed.all = true;
        entry.killed.vars.clear();
      }
      ++m_stats.guards_widened;
      for (const SummaryEntry &existing : list)
        if (existing.source == entry.source && existing.guard == GUARD_TRUE &&
            m_bdd.isTrue(existing.path) &&
            (!m_options.path_sensitive_interprocedural || existing.killed.all))
          return;
    }
    if (entry.source.kind == ExprKind::Top && entry.guard == GUARD_TRUE &&
        m_bdd.isTrue(entry.path) &&
        (!m_options.path_sensitive_interprocedural || entry.killed.all)) {
      list.clear();
      top_saturated.insert(owner);
    } else {
      list.erase(
          std::remove_if(
              list.begin(), list.end(),
              [&](const SummaryEntry &prev) {
                return prev.source == entry.source &&
                       guardSubsumes(entry.guard, prev.guard) &&
                       (!m_options.path_sensitive_interprocedural ||
                        ((m_bdd.isTrue(entry.path) ||
                          entry.path == prev.path) &&
                         killedSubsumes(entry.killed, prev.killed)));
              }),
          list.end());
    }
    list.push_back(entry);
    ++m_stats.summary_tuples;
    ++m_stats.summary_updates;

    auto wit = waiters.find(owner);
    if (wit != waiters.end()) {
      for (const Waiter &w : wit->second) {
        if (top_saturated.count(w.owner))
          continue;
        ++m_stats.scc_reschedules;
        const Instruction &ci = *m_index.by_site[w.call_site].inst;
        GuardId shifted = prependCallSite(entry.guard, w.call_site);
        GuardId combined_g = conjoinGuards(w.call_guard, shifted);
        const BddManager::NodeId caller_before_p =
            m_options.path_sensitive_interprocedural
                ? applyKilledToPath(w.call_path, entry.killed)
                : quantifyFuncConditions(w.callee, w.call_path);
        const BddManager::NodeId callee_sub_p =
            m_options.path_sensitive_interprocedural
                ? applyCallCondSubst(w.callee, ci, entry.path)
                : BddManager::TRUE_NODE;
        BddManager::NodeId combined_p =
            m_options.path_sensitive_interprocedural
                ? m_bdd.makeAnd(caller_before_p, callee_sub_p)
                : caller_before_p;
        if (combined_g == GUARD_FALSE || m_bdd.isFalse(combined_p)) {
          if (m_bdd.isFalse(combined_p))
            ++m_stats.path_infeasible_prunes;
          continue;
        }
        KilledConds combined_k = w.call_killed;
        mergeKilledConds(combined_k, entry.killed);
        std::vector<SummaryEntry> subst;
        substituteCalleeExpr(w.callee, ci, entry.source, w.caller_off,
                             w.caller_known, w.caller_stride, combined_g,
                             combined_p, subst);
        for (const SummaryEntry &se : subst)
          enqueue(w.owner, {w.call_site, Point::Before}, se.source, se.guard,
                  se.path, combined_k);
      }
    }
  };

  auto spliceCalleeSummary = [&](const SummaryKey &owner, Id call_site,
                                 Id callee, const SummaryKey &callee_key,
                                 std::int64_t caller_off, bool caller_known,
                                 std::uint64_t caller_stride,
                                 GuardId call_guard,
                                 BddManager::NodeId call_path,
                                 const KilledConds &call_killed) {
    const Instruction &ci = *m_index.by_site[call_site].inst;
    const Id callee_scc = m_call_graph.function_scc[callee];
    if (callee_scc != root_scc) {
      const auto &callee_entries = computeSummary(callee_key);
      for (const SummaryEntry &ce : callee_entries) {
        GuardId shifted = prependCallSite(ce.guard, call_site);
        GuardId combined_g = conjoinGuards(call_guard, shifted);
        const BddManager::NodeId caller_before_p =
            m_options.path_sensitive_interprocedural
                ? applyKilledToPath(call_path, ce.killed)
                : quantifyFuncConditions(callee, call_path);
        const BddManager::NodeId callee_sub_p =
            m_options.path_sensitive_interprocedural
                ? applyCallCondSubst(callee, ci, ce.path)
                : BddManager::TRUE_NODE;
        BddManager::NodeId combined_p =
            m_options.path_sensitive_interprocedural
                ? m_bdd.makeAnd(caller_before_p, callee_sub_p)
                : caller_before_p;
        if (combined_g == GUARD_FALSE || m_bdd.isFalse(combined_p)) {
          if (m_bdd.isFalse(combined_p))
            ++m_stats.path_infeasible_prunes;
          continue;
        }
        KilledConds combined_k = call_killed;
        mergeKilledConds(combined_k, ce.killed);
        std::vector<SummaryEntry> subst;
        substituteCalleeExpr(callee, ci, ce.source, caller_off, caller_known,
                             caller_stride, combined_g, combined_p, subst);
        for (const SummaryEntry &se : subst)
          enqueue(owner, {call_site, Point::Before}, se.source, se.guard,
                  se.path, combined_k);
      }
    } else {
      Waiter w{owner,        call_site,     callee,     caller_off,
               caller_known, caller_stride, call_guard, call_path,
               call_killed};
      const bool newly_registered = waiters[callee_key].insert(w).second;
      seedKey(callee_key);
      if (!newly_registered)
        return;
      const auto &existing = m_summaries[callee_key];
      for (const SummaryEntry &ce : existing) {
        ++m_stats.scc_reschedules;
        GuardId shifted = prependCallSite(ce.guard, call_site);
        GuardId combined_g = conjoinGuards(call_guard, shifted);
        const BddManager::NodeId caller_before_p =
            m_options.path_sensitive_interprocedural
                ? applyKilledToPath(call_path, ce.killed)
                : quantifyFuncConditions(callee, call_path);
        const BddManager::NodeId callee_sub_p =
            m_options.path_sensitive_interprocedural
                ? applyCallCondSubst(callee, ci, ce.path)
                : BddManager::TRUE_NODE;
        BddManager::NodeId combined_p =
            m_options.path_sensitive_interprocedural
                ? m_bdd.makeAnd(caller_before_p, callee_sub_p)
                : caller_before_p;
        if (combined_g == GUARD_FALSE || m_bdd.isFalse(combined_p)) {
          if (m_bdd.isFalse(combined_p))
            ++m_stats.path_infeasible_prunes;
          continue;
        }
        KilledConds combined_k = call_killed;
        mergeKilledConds(combined_k, ce.killed);
        std::vector<SummaryEntry> subst;
        substituteCalleeExpr(callee, ci, ce.source, caller_off, caller_known,
                             caller_stride, combined_g, combined_p, subst);
        for (const SummaryEntry &se : subst)
          enqueue(owner, {call_site, Point::Before}, se.source, se.guard,
                  se.path, combined_k);
      }
    }
  };

  seedKey(root_key);

  while (!work.empty()) {
    WorkItem item = std::move(work.front());
    work.pop_front();
    if (top_saturated.count(item.owner))
      continue;
    tick();

    const InstLocation &loc = m_index.by_site[item.pos.site];
    const Id f = loc.function;
    const auto &block_insts = m_program.functions[f].blocks[loc.block].instructions;
    const std::size_t target_depth = depthOfSummaryKey(item.owner);

    // Fast straight-line intra-block backward walk: skip Nop/Return and
    // irrelevant statements (not in St_P) until a Call site, branching
    // transfer, or block entry.
    std::size_t cur_idx = loc.index;
    bool at_after = (item.pos.point == Point::After);
    Expr cur_q = item.q;
    GuardId cur_g = item.guard;
    BddManager::NodeId cur_p = item.path;
    KilledConds cur_k = std::move(item.killed);
    bool hit_call = false;
    bool stopped = false;

    while (true) {
      if (at_after) {
        const Instruction &ci = block_insts[cur_idx];
        if (ci.opcode == Opcode::Call) {
          if (cur_idx != loc.index || item.pos.point != Point::After) {
            enqueue(item.owner, {ci.id, Point::After}, cur_q, cur_g, cur_p,
                    cur_k);
            stopped = true;
          } else {
            hit_call = true;
          }
          break;
        }
        if (!m_options.path_sensitive ||
            (ci.defines_condition == INVALID && ci.kills_conditions.empty())) {
          if (ci.opcode == Opcode::Nop || ci.opcode == Opcode::Return ||
              cur_q.isTerminal() || !m_slice.instructions[ci.id]) {
            at_after = false;
            continue;
          }
        }
        if (m_options.path_sensitive_interprocedural) {
          addKilledCond(cur_k, ci.defines_condition);
          addKilledConds(cur_k, ci.kills_conditions);
        }
        std::vector<SummaryEntry> next;
        transferBackward(ci, cur_q, cur_g, cur_p, target_depth, next);
        if (next.empty()) {
          stopped = true;
          break;
        }
        for (std::size_t k = 1; k < next.size(); ++k)
          enqueue(item.owner, {ci.id, Point::Before}, next[k].source,
                  next[k].guard, next[k].path, cur_k);
        cur_q = next[0].source;
        cur_g = next[0].guard;
        cur_p = next[0].path;
        if (cur_g == GUARD_FALSE || m_bdd.isFalse(cur_p)) {
          stopped = true;
          break;
        }
        at_after = false;
        continue;
      }
      if (cur_idx == 0)
        break;
      --cur_idx;
      at_after = true;
    }

    if (stopped)
      continue;

    // Step 1: Call return site handling (Algorithm 5 lines 9-18).
    if (hit_call) {
      const Instruction &inst = block_insts[cur_idx];
      BddManager::NodeId call_path = quantifyInstConditions(inst, cur_p);
      KilledConds call_k = cur_k;
      if (m_options.path_sensitive_interprocedural) {
        addKilledCond(call_k, inst.defines_condition);
        addKilledConds(call_k, inst.kills_conditions);
      }

      // Fast skip (Algorithm 4 line 2 / Algorithm 5 line 17): a caller-local
      // SSA Val (not defined by this Call) or terminal Expr cannot be modified
      // by any callee, so we only check whether this Call may return without
      // resolving unsliced indirect targets!
      if (cur_q.isTerminal() ||
          (cur_q.kind == ExprKind::Val && cur_q.id != inst.result)) {
        const Targets coarse_targets = callees(
            m_program, inst,
            valueOf(m_program, m_upper, inst.indirect_target));
        bool returns = coarse_targets.opaque;
        BddManager::NodeId skip_path = call_path;
        KilledConds skip_k = call_k;
        if (coarse_targets.opaque && inst.writes_memory) {
          skip_path = quantifyGlobalConditions(skip_path);
          addKilledConds(skip_k, m_program.global_conditions);
        }
        for (Id c : coarse_targets.functions) {
          returns |= m_reachability.may_return[c];
          if (m_program.functions[c].external) {
            if (inst.writes_memory && m_program.functions[c].writes_memory) {
              skip_path = quantifyGlobalConditions(skip_path);
              addKilledConds(skip_k, m_program.global_conditions);
            }
          } else {
            skip_path = quantifyFuncConditions(c, skip_path);
            if (c < m_func_kills_conditions.size())
              addKilledConds(skip_k, m_func_kills_conditions[c]);
          }
        }
        if (returns) {
          ++m_stats.skipped_functions;
          enqueue(item.owner, {inst.id, Point::Before}, cur_q, cur_g, skip_path,
                  std::move(skip_k));
        }
        continue;
      }

      const bool is_indirect = (inst.callee == INVALID);
      const PointsToSet target_pts =
          is_indirect
              ? ((inst.indirect_target != INVALID &&
                  m_slice.values[inst.indirect_target])
                     ? fsciForSummary(inst.indirect_target, inst.id,
                                      target_depth)
                     : valueOf(m_program, m_upper, inst.indirect_target))
              : PointsToSet{};
      const Targets targets = callees(m_program, inst, target_pts);

      const bool noop_havoc = (m_options.havoc_policy == HavocPolicy::Noop);
      if (targets.opaque) {
        if (noop_havoc) {
          enqueue(item.owner, {inst.id, Point::Before}, cur_q, cur_g,
                  call_path, call_k);
        } else {
          const BddManager::NodeId opaque_path =
              inst.writes_memory ? quantifyGlobalConditions(call_path)
                                 : call_path;
          KilledConds opaque_k = call_k;
          if (inst.writes_memory)
            addKilledConds(opaque_k, m_program.global_conditions);
          if ((cur_q.kind == ExprKind::Val || cur_q.kind == ExprKind::Deref) &&
              cur_q.id == inst.result) {
            enqueue(item.owner, {inst.id, Point::Before}, Expr::makeTop(),
                    cur_g, opaque_path, std::move(opaque_k));
          } else if (inst.writes_memory &&
                     (cur_q.kind == ExprKind::Mem ||
                      cur_q.kind == ExprKind::Deref)) {
            enqueue(item.owner, {inst.id, Point::Before}, Expr::makeTop(),
                    cur_g, opaque_path, std::move(opaque_k));
          } else {
            enqueue(item.owner, {inst.id, Point::Before}, cur_q, cur_g,
                    opaque_path, std::move(opaque_k));
          }
        }
      }

      auto findFuncObject = [&](Id fn) -> Id {
        for (Id o = 2; o < m_program.objects.size(); ++o)
          if (m_program.objects[o].kind == ObjectKind::Function &&
              m_program.objects[o].function == fn)
            return o;
        return INVALID;
      };

      for (Id callee : targets.functions) {
        if (!m_reachability.may_return[callee])
          continue;

        GuardId cg = cur_g;
        if (is_indirect && inst.indirect_target != INVALID &&
            !target_pts.isTop() && target_pts.size() > 1 &&
            !m_program.values[inst.indirect_target].constant) {
          const Id fobj = findFuncObject(callee);
          if (fobj != INVALID) {
            GuardAtom atom;
            atom.site = inst.id;
            atom.kind = GuardKind::PointsTo;
            atom.lhs = inst.indirect_target;
            atom.rhs = fobj;
            cg = conjoinAtom(cur_g, atom);
            if (cg == GUARD_FALSE)
              continue;
          }
        }

        if (m_program.functions[callee].external) {
          if (noop_havoc) {
            enqueue(item.owner, {inst.id, Point::Before}, cur_q, cg,
                    call_path, call_k);
            continue;
          }
          const BddManager::NodeId ext_path =
              (inst.writes_memory && m_program.functions[callee].writes_memory)
                  ? quantifyGlobalConditions(call_path)
                  : call_path;
          KilledConds ext_k = call_k;
          if (inst.writes_memory && m_program.functions[callee].writes_memory)
            addKilledConds(ext_k, m_program.global_conditions);
          if ((cur_q.kind == ExprKind::Val || cur_q.kind == ExprKind::Deref) &&
              cur_q.id == inst.result) {
            enqueue(item.owner, {inst.id, Point::Before}, Expr::makeTop(), cg,
                    ext_path, std::move(ext_k));
          } else if (inst.writes_memory &&
                     m_program.functions[callee].writes_memory &&
                     (cur_q.kind == ExprKind::Mem ||
                      cur_q.kind == ExprKind::Deref)) {
            enqueue(item.owner, {inst.id, Point::Before}, Expr::makeTop(), cg,
                    ext_path, std::move(ext_k));
          } else {
            enqueue(item.owner, {inst.id, Point::Before}, cur_q, cg, ext_path,
                    std::move(ext_k));
          }
          continue;
        }

        auto calleeSkipK = [&]() {
          KilledConds k = call_k;
          if (callee < m_func_kills_conditions.size())
            addKilledConds(k, m_func_kills_conditions[callee]);
          return k;
        };

        if (cur_q.isTerminal()) {
          enqueue(item.owner, {inst.id, Point::Before}, cur_q, cg,
                  quantifyFuncConditions(callee, call_path), calleeSkipK());
        } else if (cur_q.kind == ExprKind::Val && cur_q.id == inst.result) {
          SummaryKey ck{callee, SummaryKind::ExitReturn, Expr::makeTop(),
                        INVALID, Point::Before};
          spliceCalleeSummary(item.owner, inst.id, callee, ck, cur_q.val_offset,
                              cur_q.val_offset_known, cur_q.val_stride, cg,
                              call_path, call_k);
        } else if (cur_q.kind == ExprKind::Deref && cur_q.id == inst.result) {
          std::vector<SummaryEntry> mat;
          materializeDeref(cur_q, inst.id, target_depth, cg, call_path, mat,
                           Point::After);
          for (const SummaryEntry &me : mat) {
            if (me.source.kind == ExprKind::Mem &&
                canFunctionModifyObj(callee, me.source.id)) {
              SummaryKey ck{callee, SummaryKind::ExitMem,
                            Expr::makeMem(me.source.id), INVALID,
                            Point::Before};
              spliceCalleeSummary(item.owner, inst.id, callee, ck,
                                  me.source.val_offset,
                                  me.source.val_offset_known,
                                  me.source.val_stride, me.guard, me.path,
                                  call_k);
            } else {
              if (me.source.kind == ExprKind::Mem)
                ++m_stats.skipped_functions;
              enqueue(item.owner, {inst.id, Point::Before}, me.source, me.guard,
                      quantifyFuncConditions(callee, me.path), calleeSkipK());
            }
          }
        } else if (cur_q.kind == ExprKind::Val) {
          ++m_stats.skipped_functions;
          enqueue(item.owner, {inst.id, Point::Before}, cur_q, cg,
                  quantifyFuncConditions(callee, call_path), calleeSkipK());
        } else if (cur_q.kind == ExprKind::Mem) {
          if (!canFunctionModifyObj(callee, cur_q.id)) {
            ++m_stats.skipped_functions;
            enqueue(item.owner, {inst.id, Point::Before}, cur_q, cg,
                    quantifyFuncConditions(callee, call_path), calleeSkipK());
          } else {
            SummaryKey ck{callee, SummaryKind::ExitMem, Expr::makeMem(cur_q.id),
                          INVALID, Point::Before};
            spliceCalleeSummary(item.owner, inst.id, callee, ck,
                                cur_q.val_offset, cur_q.val_offset_known,
                                cur_q.val_stride, cg, call_path, call_k);
          }
        } else if (cur_q.kind == ExprKind::Deref) {
          const PointsToSet pt_s = applyOffset(
              fsciForSummary(cur_q.id, inst.id, target_depth),
              cur_q.deref_offset, cur_q.deref_offset_known, cur_q.deref_stride);
          if (!canFunctionModifyAny(callee, pt_s)) {
            ++m_stats.skipped_functions;
            enqueue(item.owner, {inst.id, Point::Before}, cur_q, cg,
                    quantifyFuncConditions(callee, call_path), calleeSkipK());
          } else {
            std::vector<SummaryEntry> mat;
            materializeDeref(cur_q, inst.id, target_depth, cg, call_path, mat);
            for (const SummaryEntry &me : mat) {
              if (me.source.kind == ExprKind::Mem &&
                  canFunctionModifyObj(callee, me.source.id)) {
                SummaryKey ck{callee, SummaryKind::ExitMem,
                              Expr::makeMem(me.source.id), INVALID,
                              Point::Before};
                spliceCalleeSummary(item.owner, inst.id, callee, ck,
                                    me.source.val_offset,
                                    me.source.val_offset_known,
                                    me.source.val_stride, me.guard, me.path,
                                    call_k);
              } else {
                enqueue(item.owner, {inst.id, Point::Before}, me.source,
                        me.guard, quantifyFuncConditions(callee, me.path),
                        calleeSkipK());
              }
            }
          }
        }
      }
      continue;
    }

    // Step 2: Reached entry (`cur_idx == 0`, `Point::Before`) of `loc.block`.
    if (loc.index != 0 || item.pos.point != Point::Before) {
      if (!recordState(item.owner, {block_insts[0].id, Point::Before}, cur_q,
                       cur_g, cur_p, cur_k))
        continue;
    }

    if (loc.block == 0) {
      addSummaryEntry(item.owner, {cur_q, cur_g, cur_p, cur_k});
      continue;
    }

    auto predBlockFallsThrough = [&](const Block &pb) -> bool {
      if (pb.instructions.empty())
        return true;
      const Instruction &last_inst = pb.instructions.back();
      if (!m_reachability.reachable_site[last_inst.id])
        return false;
      if (last_inst.opcode == Opcode::Return)
        return false;
      if (last_inst.opcode == Opcode::Call) {
        const Targets t = callees(
            m_program, last_inst,
            valueOf(m_program, m_upper, last_inst.indirect_target));
        bool returns = t.opaque;
        for (Id c : t.functions)
          returns |= m_reachability.may_return[c];
        if (!returns)
          return false;
      }
      return true;
    };

    for (const auto &[pred_block, edge_idx] : m_index.pred_edges[f][loc.block]) {
      const Block &pb = m_program.functions[f].blocks[pred_block];
      const Edge &edge = pb.successors[edge_idx];

      if (!predBlockFallsThrough(pb))
        continue;

      BddManager::NodeId next_path = cur_p;
      if (m_options.path_sensitive && edge.cond_var != INVALID) {
        const BddManager::NodeId lit =
            evalCondNode(edge.cond_var, edge.cond_polarity);
        next_path = m_bdd.makeAnd(next_path, lit);
        if (m_bdd.isFalse(next_path)) {
          ++m_stats.path_infeasible_prunes;
          continue;
        }
      }

      const Id pred_site =
          pb.instructions.empty() ? INVALID : pb.instructions.back().id;
      const Id guard_site =
          pred_site != INVALID ? pred_site : block_insts[0].id;
      const Point guard_point =
          pred_site != INVALID ? Point::After : Point::Before;
      const GuardId next_guard = applyEdgePtrGuards(
          edge, guard_site, guard_point, cur_g, target_depth);
      if (next_guard == GUARD_FALSE)
        continue;
      std::vector<SummaryEntry> after_phi;
      applyPhiBackward(edge, cur_q, next_guard, next_path, target_depth,
                       guard_site, after_phi);

      for (const SummaryEntry &pe : after_phi) {
        if (pb.instructions.empty()) {
          if (pred_block == 0) {
            addSummaryEntry(item.owner, {pe.source, pe.guard, pe.path, cur_k});
          } else {
            std::deque<
                std::tuple<Id, Expr, GuardId, BddManager::NodeId, KilledConds>>
                ebq;
            std::map<std::pair<Id, Expr>,
                     std::vector<
                         std::tuple<GuardId, BddManager::NodeId, KilledConds>>>
                seen_eb;
            auto pushEmptyBlock = [&](Id cb, const Expr &cq, GuardId cg,
                                      BddManager::NodeId cp, KilledConds ck) {
              if (cg == GUARD_FALSE || m_bdd.isFalse(cp))
                return;
              auto &seen = seen_eb[{cb, cq}];
              for (const auto &[sg, sp, sk] : seen) {
                if (guardSubsumes(sg, cg) &&
                    (!m_options.path_sensitive || m_bdd.isTrue(sp) ||
                     sp == cp) &&
                    (!m_options.path_sensitive_interprocedural ||
                     killedSubsumes(sk, ck)))
                  return;
              }
              if (seen.size() >= m_options.max_guards_per_state) {
                cg = GUARD_TRUE;
                cp = BddManager::TRUE_NODE;
                if (m_options.path_sensitive_interprocedural) {
                  ck.all = true;
                  ck.vars.clear();
                }
                ++m_stats.guards_widened;
                for (const auto &[sg, sp, sk] : seen)
                  if (sg == GUARD_TRUE && m_bdd.isTrue(sp) &&
                      (!m_options.path_sensitive_interprocedural || sk.all))
                    return;
              }
              seen.erase(
                  std::remove_if(
                      seen.begin(), seen.end(),
                      [&](const auto &prev) {
                        return guardSubsumes(cg, std::get<0>(prev)) &&
                               (!m_options.path_sensitive || m_bdd.isTrue(cp) ||
                                cp == std::get<1>(prev)) &&
                               (!m_options.path_sensitive_interprocedural ||
                                killedSubsumes(ck, std::get<2>(prev)));
                      }),
                  seen.end());
              seen.emplace_back(cg, cp, ck);
              ebq.emplace_back(cb, cq, cg, cp, std::move(ck));
            };
            pushEmptyBlock(pred_block, pe.source, pe.guard, pe.path, cur_k);
            while (!ebq.empty()) {
              auto [cb, cq, cg, cp, ck] = std::move(ebq.front());
              ebq.pop_front();
              for (const auto &[ppb, peidx] : m_index.pred_edges[f][cb]) {
                const Block &ppblock = m_program.functions[f].blocks[ppb];
                if (!predBlockFallsThrough(ppblock))
                  continue;
                const Edge &pedge = ppblock.successors[peidx];
                BddManager::NodeId ppath = cp;
                if (m_options.path_sensitive && pedge.cond_var != INVALID) {
                  ppath = m_bdd.makeAnd(
                      ppath,
                      evalCondNode(pedge.cond_var, pedge.cond_polarity));
                  if (m_bdd.isFalse(ppath)) {
                    ++m_stats.path_infeasible_prunes;
                    continue;
                  }
                }
                const bool p_empty = ppblock.instructions.empty();
                const Id pguard_site = p_empty
                                           ? item.pos.site
                                           : ppblock.instructions.back().id;
                const Point pguard_point =
                    p_empty ? Point::Before : Point::After;
                const GuardId pguard = applyEdgePtrGuards(
                    pedge, pguard_site, pguard_point, cg, target_depth);
                if (pguard == GUARD_FALSE)
                  continue;
                std::vector<SummaryEntry> pphi;
                applyPhiBackward(pedge, cq, pguard, ppath, target_depth,
                                 pguard_site, pphi);
                for (const SummaryEntry &ppe : pphi) {
                  if (!ppblock.instructions.empty()) {
                    enqueue(item.owner,
                            {ppblock.instructions.back().id, Point::After},
                            ppe.source, ppe.guard, ppe.path, ck);
                  } else if (ppb == 0) {
                    addSummaryEntry(item.owner,
                                    {ppe.source, ppe.guard, ppe.path, ck});
                  } else {
                    pushEmptyBlock(ppb, ppe.source, ppe.guard, ppe.path, ck);
                  }
                }
              }
            }
          }
        } else {
          enqueue(item.owner, {pb.instructions.back().id, Point::After},
                  pe.source, pe.guard, pe.path, cur_k);
        }
      }
    }
  }

  const bool in_cyclic_pass =
      m_cyclic_pass1_depth != std::numeric_limits<std::size_t>::max() ||
      m_in_cyclic_pass2;
  for (const SummaryKey &k : seeded_in_scc) {
    m_finalized.insert(k);
    if (in_cyclic_pass)
      m_pass_created_summaries.insert(k);
    if (m_summaries[k].empty() &&
        m_stats.path_infeasible_prunes > sum_prunes_before)
      m_path_pruned_summaries.insert(k);
  }
  m_stats.bdd_nodes = std::max(m_stats.bdd_nodes, m_bdd.nodeCount());
  return m_summaries[root_key];
}

// Algorithm 3 Step 1 (Kahlon PLDI '08, p. 256): Flow-Sensitive
// Context-Insensitive backward summary splicing from `(val, site, point)` back
// to `entry` (computing both resolved PointsToSet and entry-level anchor set A,
// carrying ROBDD path constraints upward when `path_sensitive_interprocedural`
// is enabled).
PointsToSet GuardedSummarySolver::computeFsciExpr(
    const Expr &target, Id site, Point point, std::set<Expr> *entry_anchors,
    bool *path_pruned_out) {
  if (path_pruned_out)
    *path_pruned_out = false;
  if (!m_reachability.reachable_site[site])
    return PointsToSet{};
  const Id start_fn = m_index.by_site[site].function;
  if (!m_fs_reachable_function[start_fn])
    return PointsToSet{};

  ++m_stats.fsci_queries;

  const bool track_inter_path = m_options.path_sensitive_interprocedural;
  bool pruned_any_path = false;
  PointsToSet result;
  std::deque<std::tuple<Id, Expr, BddManager::NodeId>> processing;
  std::map<std::pair<Id, Expr>, std::vector<BddManager::NodeId>> processed;

  auto pushFrontier = [&](Id fn, const Expr &e, BddManager::NodeId p) {
    if (m_bdd.isFalse(p)) {
      pruned_any_path = true;
      ++m_stats.path_infeasible_prunes;
      return;
    }
    if (!track_inter_path)
      p = BddManager::TRUE_NODE;
    if (fn == m_program.entry) {
      if (entry_anchors)
        entry_anchors->insert(e);
      evaluateAtEntry(e, result);
      if (e.isTerminal())
        return;
    } else if (e.isTerminal() && m_bdd.isTrue(p)) {
      if (entry_anchors)
        entry_anchors->insert(e);
      evaluateAtEntry(e, result);
      return;
    }
    auto &seen = processed[{fn, e}];
    for (BddManager::NodeId sp : seen)
      if (m_bdd.isTrue(sp) || sp == p)
        return;
    if (seen.size() >= m_options.max_guards_per_state) {
      p = BddManager::TRUE_NODE;
      ++m_stats.guards_widened;
      for (BddManager::NodeId sp : seen)
        if (m_bdd.isTrue(sp))
          return;
    }
    if (m_bdd.isTrue(p))
      seen.clear();
    seen.push_back(p);
    processing.emplace_back(fn, e, p);
  };

  SummaryKey local_key{start_fn, SummaryKind::LocalPoint, target, site, point};
  const std::size_t prunes_before = m_stats.path_infeasible_prunes;
  const auto local_entries = computeSummary(local_key);
  if (m_stats.path_infeasible_prunes > prunes_before)
    pruned_any_path = true;
  for (const SummaryEntry &se : local_entries) {
    if (!isGuardSatisfiable(se.guard, nullptr)) {
      if (m_options.path_sensitive)
        pruned_any_path = true;
      continue;
    }
    pushFrontier(start_fn, se.source, se.path);
  }

  while (!processing.empty()) {
    auto [h, q, cur_path] = std::move(processing.front());
    processing.pop_front();
    tick();

    for (Id call_site : m_index.incoming_calls[h]) {
      if (!m_reachability.reachable_site[call_site])
        continue;
      const InstLocation &cloc = m_index.by_site[call_site];
      const Id g = cloc.function;
      if (!m_fs_reachable_function[g])
        continue;
      const Instruction &ci = *cloc.inst;

      if (ci.callee == INVALID && ci.indirect_target != INVALID) {
        bool exact = true;
        const PointsToSet tpts = evalGuardPointer(ci.indirect_target, call_site,
                                                  nullptr, false, exact);
        const Targets ct = callees(m_program, ci, tpts);
        if (!ct.functions.count(h))
          continue;
      }

      const BddManager::NodeId sub_path =
          track_inter_path ? applyCallCondSubst(h, ci, cur_path)
                           : BddManager::TRUE_NODE;
      if (m_bdd.isFalse(sub_path)) {
        pruned_any_path = true;
        ++m_stats.path_infeasible_prunes;
        continue;
      }

      if (q.isTerminal()) {
        if (!track_inter_path || m_bdd.isTrue(sub_path)) {
          if (entry_anchors)
            entry_anchors->insert(q);
          evaluateAtEntry(q, result);
          continue;
        }
        SummaryKey pk{g, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                      Point::Before};
        const auto path_entries = computeSummary(pk);
        for (const SummaryEntry &se : path_entries) {
          if (!isGuardSatisfiable(se.guard, nullptr))
            continue;
          const BddManager::NodeId lifted =
              applyKilledToPath(sub_path, se.killed);
          const BddManager::NodeId combined_p = m_bdd.makeAnd(lifted, se.path);
          pushFrontier(g, q, combined_p);
        }
        continue;
      }

      std::vector<SummaryEntry> caller_exprs;
      substituteCalleeExpr(h, ci, q, 0, true, 0, GUARD_TRUE, sub_path,
                           caller_exprs);
      for (const SummaryEntry &ce : caller_exprs) {
        if (ce.source.isTerminal()) {
          if (track_inter_path && !m_bdd.isTrue(sub_path)) {
            SummaryKey pk{g, SummaryKind::LocalPoint, Expr::makeNull(),
                          call_site, Point::Before};
            const auto path_entries = computeSummary(pk);
            for (const SummaryEntry &se : path_entries) {
              if (!isGuardSatisfiable(se.guard, nullptr))
                continue;
              const BddManager::NodeId lifted =
                  applyKilledToPath(sub_path, se.killed);
              pushFrontier(g, ce.source, m_bdd.makeAnd(lifted, se.path));
            }
          } else {
            if (entry_anchors)
              entry_anchors->insert(ce.source);
            evaluateAtEntry(ce.source, result);
          }
          continue;
        }
        // Algorithm 3 line 11 (`Retain`): if `g` cannot modify `Mem(obj)` and
        // no interprocedural path condition needs checking inside `g`, skip!
        if (ce.source.kind == ExprKind::Mem &&
            !canFunctionModifyObj(g, ce.source.id) &&
            (!track_inter_path || m_bdd.isTrue(sub_path))) {
          ++m_stats.skipped_functions;
          pushFrontier(g, ce.source, sub_path);
          continue;
        }
        SummaryKey ck{g, SummaryKind::LocalPoint, ce.source, call_site,
                      Point::Before};
        const auto caller_entries = computeSummary(ck);
        for (const SummaryEntry &se : caller_entries) {
          if (!isGuardSatisfiable(se.guard, nullptr))
            continue;
          const BddManager::NodeId combined_p =
              track_inter_path
                  ? m_bdd.makeAnd(applyKilledToPath(sub_path, se.killed),
                                  se.path)
                  : BddManager::TRUE_NODE;
          pushFrontier(g, se.source, combined_p);
        }
      }
    }
  }

  if (path_pruned_out)
    *path_pruned_out = result.empty() && pruned_any_path &&
                       m_options.path_sensitive &&
                       !isPathReachable(site, point, nullptr);
  return result;
}

PointsToSet GuardedSummarySolver::computeFsci(Id val, Id site, Point point,
                                              std::set<Expr> *entry_anchors,
                                              bool *path_pruned_out) {
  if (path_pruned_out)
    *path_pruned_out = false;
  if (val == INVALID) {
    if (entry_anchors)
      entry_anchors->insert(Expr::makeTop());
    return PointsToSet::top();
  }
  if (m_program.values[val].constant) {
    const PointsToSet &init = m_program.values[val].initial;
    if (entry_anchors) {
      if (init.isTop())
        entry_anchors->insert(Expr::makeTop());
      else
        for (Id o : init.objects())
          entry_anchors->insert(o == NULL_OBJECT ? Expr::makeNull()
                                                 : Expr::makeAddr(o));
    }
    return init;
  }
  if (!m_reachability.reachable_site[site])
    return PointsToSet{};
  const Id start_fn = m_index.by_site[site].function;
  if (!m_fs_reachable_function[start_fn])
    return PointsToSet{};

  FsciKey fkey{val, site, point};
  if (m_active_fsci.count(fkey)) {
    auto it = m_fsci_cache.find(fkey);
    return it != m_fsci_cache.end() ? it->second
                                    : valueOf(m_program, m_upper, val);
  }
  m_active_fsci.insert(fkey);
  PointsToSet result = computeFsciExpr(Expr::makeVal(val), site, point,
                                       entry_anchors, path_pruned_out);
  m_active_fsci.erase(fkey);
  return result;
}

bool GuardedSummarySolver::validateContext(Id site, const Context &ctx,
                                           bool is_suffix) {
  if (!m_reachability.reachable_site[site])
    return false;
  if (is_suffix && ctx.empty())
    return m_fs_reachable_function[m_index.by_site[site].function];
  Id current_fn =
      is_suffix ? m_index.by_site[ctx[0]].function : m_program.entry;
  if (is_suffix && !m_fs_reachable_function[current_fn])
    return false;
  Context prefix;
  prefix.reserve(ctx.size());
  for (std::size_t i = 0; i < ctx.size(); ++i) {
    const Id call_site = ctx[i];
    const InstLocation &cloc = m_index.by_site[call_site];
    if (cloc.function != current_fn ||
        !m_reachability.reachable_site[call_site])
      return false;
    const Instruction &ci = *cloc.inst;
    const Id next_fn = (i + 1 < ctx.size())
                           ? m_index.by_site[ctx[i + 1]].function
                           : m_index.by_site[site].function;
    if (ci.callee != INVALID) {
      if (ci.callee != next_fn)
        return false;
    } else if (ci.indirect_target != INVALID) {
      bool exact = true;
      const PointsToSet tpts = evalGuardPointer(
          ci.indirect_target, call_site, is_suffix ? nullptr : &prefix, false,
          exact);
      const Targets ct = callees(m_program, ci, tpts);
      if (!ct.functions.count(next_fn))
        return false;
    }
    current_fn = next_fn;
    prefix.push_back(call_site);
  }
  return current_fn == m_index.by_site[site].function;
}

bool GuardedSummarySolver::isPathReachable(Id site, Point point,
                                           const Context *ctx,
                                           bool is_suffix) {
  if (!m_reachability.reachable_site[site])
    return false;
  if (ctx != nullptr && is_suffix && ctx->empty())
    return isPathReachable(site, point, nullptr, false);
  const Id leaf_fn = m_index.by_site[site].function;
  if (ctx != nullptr) {
    if (!validateContext(site, *ctx, is_suffix))
      return false;
    SummaryKey pk{leaf_fn, SummaryKind::LocalPoint, Expr::makeNull(), site,
                  point};
    const auto entries = computeSummary(pk);
    std::set<BddManager::NodeId> frontier;
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, is_suffix ? nullptr : ctx) ||
          m_bdd.isFalse(se.path))
        continue;
      if (!m_options.path_sensitive_interprocedural)
        return true;
      frontier.insert(se.path);
    }
    if (frontier.empty())
      return false;
    Context prefix = *ctx;
    for (std::size_t step = ctx->size(); step > 0 && !frontier.empty();
         --step) {
      tick();
      const Id call_site = (*ctx)[step - 1];
      prefix.pop_back();
      const InstLocation &cloc = m_index.by_site[call_site];
      const Id caller_fn = cloc.function;
      const Id callee_fn = (step == ctx->size())
                               ? leaf_fn
                               : m_index.by_site[(*ctx)[step]].function;
      const Instruction &ci = *cloc.inst;
      SummaryKey cpk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                     call_site, Point::Before};
      const auto call_entries = computeSummary(cpk);
      std::set<BddManager::NodeId> next_frontier;
      for (BddManager::NodeId cur_path : frontier) {
        const BddManager::NodeId sub_p =
            applyCallCondSubst(callee_fn, ci, cur_path);
        if (m_bdd.isFalse(sub_p))
          continue;
        for (const SummaryEntry &se : call_entries) {
          if (!isGuardSatisfiable(se.guard, is_suffix ? nullptr : &prefix))
            continue;
          const BddManager::NodeId lifted = applyKilledToPath(sub_p, se.killed);
          const BddManager::NodeId combined = m_bdd.makeAnd(lifted, se.path);
          if (!m_bdd.isFalse(combined))
            next_frontier.insert(combined);
        }
      }
      frontier = std::move(next_frontier);
    }
    if (frontier.empty())
      return false;
    if (!is_suffix)
      return true;
    const Id root_fn = m_index.by_site[(*ctx)[0]].function;
    if (root_fn == m_program.entry)
      return true;
    std::deque<std::pair<Id, BddManager::NodeId>> processing;
    std::map<Id, std::vector<BddManager::NodeId>> processed;
    auto pushUp = [&](Id fn, BddManager::NodeId p) -> bool {
      if (m_bdd.isFalse(p))
        return false;
      if (fn == m_program.entry)
        return true;
      auto &seen = processed[fn];
      for (BddManager::NodeId sp : seen)
        if (m_bdd.isTrue(sp) || sp == p)
          return false;
      if (seen.size() >= m_options.max_guards_per_state) {
        p = BddManager::TRUE_NODE;
        for (BddManager::NodeId sp : seen)
          if (m_bdd.isTrue(sp))
            return false;
      }
      if (m_bdd.isTrue(p))
        seen.clear();
      seen.push_back(p);
      processing.emplace_back(fn, p);
      return false;
    };
    for (BddManager::NodeId p : frontier)
      if (pushUp(root_fn, p))
        return true;
    while (!processing.empty()) {
      auto [h, cur_path] = processing.front();
      processing.pop_front();
      tick();
      for (Id call_site : m_index.incoming_calls[h]) {
        if (!m_reachability.reachable_site[call_site])
          continue;
        const InstLocation &cloc = m_index.by_site[call_site];
        const Id g = cloc.function;
        if (!m_fs_reachable_function[g])
          continue;
        const Instruction &ci = *cloc.inst;
        if (ci.callee == INVALID && ci.indirect_target != INVALID) {
          bool exact = true;
          const PointsToSet tpts = evalGuardPointer(
              ci.indirect_target, call_site, nullptr, false, exact);
          const Targets ct = callees(m_program, ci, tpts);
          if (!ct.functions.count(h))
            continue;
        }
        const BddManager::NodeId sub_p = applyCallCondSubst(h, ci, cur_path);
        if (m_bdd.isFalse(sub_p))
          continue;
        SummaryKey cpk{g, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                       Point::Before};
        const auto call_entries = computeSummary(cpk);
        for (const SummaryEntry &se : call_entries) {
          if (!isGuardSatisfiable(se.guard, nullptr))
            continue;
          const BddManager::NodeId lifted = applyKilledToPath(sub_p, se.killed);
          if (pushUp(g, m_bdd.makeAnd(lifted, se.path)))
            return true;
        }
      }
    }
    return false;
  }

  if (!m_fs_reachable_function[leaf_fn])
    return false;
  SummaryKey pk{leaf_fn, SummaryKind::LocalPoint, Expr::makeNull(), site,
                point};
  const auto entries = computeSummary(pk);
  std::deque<std::pair<Id, BddManager::NodeId>> processing;
  std::map<Id, std::vector<BddManager::NodeId>> processed;
  auto pushNode = [&](Id fn, BddManager::NodeId p) -> bool {
    if (m_bdd.isFalse(p))
      return false;
    if (!m_options.path_sensitive_interprocedural)
      return true;
    if (fn == m_program.entry)
      return true;
    auto &seen = processed[fn];
    for (BddManager::NodeId sp : seen)
      if (m_bdd.isTrue(sp) || sp == p)
        return false;
    if (seen.size() >= m_options.max_guards_per_state) {
      p = BddManager::TRUE_NODE;
      for (BddManager::NodeId sp : seen)
        if (m_bdd.isTrue(sp))
          return false;
    }
    if (m_bdd.isTrue(p))
      seen.clear();
    seen.push_back(p);
    processing.emplace_back(fn, p);
    return false;
  };
  for (const SummaryEntry &se : entries) {
    if (!isGuardSatisfiable(se.guard, nullptr) || m_bdd.isFalse(se.path))
      continue;
    if (pushNode(leaf_fn, se.path))
      return true;
  }
  while (!processing.empty()) {
    auto [h, cur_path] = processing.front();
    processing.pop_front();
    tick();
    for (Id call_site : m_index.incoming_calls[h]) {
      if (!m_reachability.reachable_site[call_site])
        continue;
      const InstLocation &cloc = m_index.by_site[call_site];
      const Id g = cloc.function;
      if (!m_fs_reachable_function[g])
        continue;
      const Instruction &ci = *cloc.inst;
      if (ci.callee == INVALID && ci.indirect_target != INVALID) {
        bool exact = true;
        const PointsToSet tpts = evalGuardPointer(ci.indirect_target, call_site,
                                                  nullptr, false, exact);
        const Targets ct = callees(m_program, ci, tpts);
        if (!ct.functions.count(h))
          continue;
      }
      const BddManager::NodeId sub_p = applyCallCondSubst(h, ci, cur_path);
      if (m_bdd.isFalse(sub_p))
        continue;
      SummaryKey cpk{g, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                     Point::Before};
      const auto call_entries = computeSummary(cpk);
      for (const SummaryEntry &se : call_entries) {
        if (!isGuardSatisfiable(se.guard, nullptr))
          continue;
        const BddManager::NodeId lifted = applyKilledToPath(sub_p, se.killed);
        if (pushNode(g, m_bdd.makeAnd(lifted, se.path)))
          return true;
      }
    }
  }
  return false;
}

// Section 3 (p. 258): Computing Flow- and Context-Sensitive points-to & entry
// anchors A by splicing summaries along exact context `ctx = [c_1, ..., c_k]`,
// carrying ROBDD path formulas `con_b` upward when interprocedural path
// sensitivity is enabled.
QueryResult GuardedSummarySolver::computeFscsExpr(
    const Expr &target, Id site, const Context &ctx, Point point,
    std::set<Expr> *entry_anchors, bool widen_empty_to_top, bool is_suffix) {
  if (is_suffix && ctx.empty()) {
    const Id fn = m_index.by_site[site].function;
    if (!m_reachability.reachable_site[site] || !m_fs_reachable_function[fn])
      return {{}, QueryStatus::Unreachable};
    bool path_pruned = false;
    PointsToSet pts =
        computeFsciExpr(target, site, point, entry_anchors, &path_pruned);
    if (pts.empty()) {
      if (m_options.path_sensitive && path_pruned)
        return {{}, QueryStatus::Unreachable};
      if (widen_empty_to_top)
        pts = PointsToSet::top();
    }
    return {std::move(pts), QueryStatus::Complete};
  }
  if (!validateContext(site, ctx, is_suffix))
    return {{}, QueryStatus::Unreachable};

  const bool track_inter_path = m_options.path_sensitive_interprocedural;
  bool pruned_any_path = false;
  PointsToSet result;
  std::set<std::pair<Expr, BddManager::NodeId>> frontier;

  auto insertFrontier = [&](std::set<std::pair<Expr, BddManager::NodeId>> &dst,
                            const Expr &e, BddManager::NodeId p,
                            bool at_entry) {
    if (m_bdd.isFalse(p)) {
      pruned_any_path = true;
      ++m_stats.path_infeasible_prunes;
      return;
    }
    if (!track_inter_path)
      p = BddManager::TRUE_NODE;
    if (e.isTerminal() && (at_entry || m_bdd.isTrue(p))) {
      if (entry_anchors)
        entry_anchors->insert(e);
      evaluateAtEntry(e, result);
      return;
    }
    dst.emplace(e, p);
  };

  const Id leaf_fn = m_index.by_site[site].function;
  SummaryKey local_key{leaf_fn, SummaryKind::LocalPoint, target, site, point};
  const std::size_t prunes_before = m_stats.path_infeasible_prunes;
  const auto local_entries = computeSummary(local_key);
  if (m_stats.path_infeasible_prunes > prunes_before)
    pruned_any_path = true;
  for (const SummaryEntry &se : local_entries) {
    if (!isGuardSatisfiable(se.guard, is_suffix ? nullptr : &ctx)) {
      if (m_options.path_sensitive)
        pruned_any_path = true;
      continue;
    }
    insertFrontier(frontier, se.source, se.path, !is_suffix && ctx.empty());
  }

  Context prefix = ctx;
  for (std::size_t step = ctx.size(); step > 0 && !frontier.empty(); --step) {
    tick();
    const Id call_site = ctx[step - 1];
    prefix.pop_back();
    const InstLocation &cloc = m_index.by_site[call_site];
    const Id caller_fn = cloc.function;
    const Id callee_fn = (step == ctx.size())
                             ? leaf_fn
                             : m_index.by_site[ctx[step]].function;
    const Instruction &ci = *cloc.inst;
    const bool next_at_entry = (!is_suffix && step == 1);
    const Context *guard_ctx = is_suffix ? nullptr : &prefix;

    std::set<std::pair<Expr, BddManager::NodeId>> next_frontier;
    for (const auto &[q, cur_path] : frontier) {
      const BddManager::NodeId sub_path =
          track_inter_path ? applyCallCondSubst(callee_fn, ci, cur_path)
                           : BddManager::TRUE_NODE;
      if (m_bdd.isFalse(sub_path)) {
        pruned_any_path = true;
        ++m_stats.path_infeasible_prunes;
        continue;
      }

      if (q.isTerminal()) {
        if (!track_inter_path || m_bdd.isTrue(sub_path)) {
          if (entry_anchors)
            entry_anchors->insert(q);
          evaluateAtEntry(q, result);
          continue;
        }
        SummaryKey pk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                      call_site, Point::Before};
        const auto path_entries = computeSummary(pk);
        for (const SummaryEntry &se : path_entries) {
          if (!isGuardSatisfiable(se.guard, guard_ctx))
            continue;
          const BddManager::NodeId lifted =
              applyKilledToPath(sub_path, se.killed);
          insertFrontier(next_frontier, q, m_bdd.makeAnd(lifted, se.path),
                         next_at_entry);
        }
        continue;
      }

      std::vector<SummaryEntry> caller_exprs;
      substituteCalleeExpr(callee_fn, ci, q, 0, true, 0, GUARD_TRUE, sub_path,
                           caller_exprs);
      for (const SummaryEntry &ce : caller_exprs) {
        if (ce.source.isTerminal()) {
          if (track_inter_path && !m_bdd.isTrue(sub_path)) {
            SummaryKey pk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                          call_site, Point::Before};
            const auto path_entries = computeSummary(pk);
            for (const SummaryEntry &se : path_entries) {
              if (!isGuardSatisfiable(se.guard, guard_ctx))
                continue;
              const BddManager::NodeId lifted =
                  applyKilledToPath(sub_path, se.killed);
              insertFrontier(next_frontier, ce.source,
                             m_bdd.makeAnd(lifted, se.path), next_at_entry);
            }
          } else {
            if (entry_anchors)
              entry_anchors->insert(ce.source);
            evaluateAtEntry(ce.source, result);
          }
          continue;
        }
        if (ce.source.kind == ExprKind::Mem &&
            !canFunctionModifyObj(caller_fn, ce.source.id) &&
            (!track_inter_path || m_bdd.isTrue(sub_path))) {
          ++m_stats.skipped_functions;
          next_frontier.emplace(ce.source, sub_path);
          continue;
        }
        SummaryKey ck{caller_fn, SummaryKind::LocalPoint, ce.source, call_site,
                      Point::Before};
        const auto caller_entries = computeSummary(ck);
        for (const SummaryEntry &se : caller_entries) {
          if (!isGuardSatisfiable(se.guard, guard_ctx))
            continue;
          const BddManager::NodeId combined_p =
              track_inter_path
                  ? m_bdd.makeAnd(applyKilledToPath(sub_path, se.killed),
                                  se.path)
                  : BddManager::TRUE_NODE;
          insertFrontier(next_frontier, se.source, combined_p, next_at_entry);
        }
      }
    }
    frontier = std::move(next_frontier);
  }

  if (!is_suffix) {
    for (const auto &[q, cur_path] : frontier) {
      if (m_bdd.isFalse(cur_path))
        continue;
      if (entry_anchors)
        entry_anchors->insert(q);
      evaluateAtEntry(q, result);
    }
  } else if (!frontier.empty()) {
    // Suffix mode: continue FSCI upward summary splicing from `root_fn`
    // (the caller of `ctx[0]`) over all reachable call stacks up to `entry`.
    const Id root_fn = m_index.by_site[ctx[0]].function;
    std::deque<std::tuple<Id, Expr, BddManager::NodeId>> processing;
    std::map<std::pair<Id, Expr>, std::vector<BddManager::NodeId>> processed;
    auto pushUp = [&](Id fn, const Expr &e, BddManager::NodeId p) {
      if (m_bdd.isFalse(p)) {
        pruned_any_path = true;
        ++m_stats.path_infeasible_prunes;
        return;
      }
      if (!track_inter_path)
        p = BddManager::TRUE_NODE;
      if (fn == m_program.entry) {
        if (entry_anchors)
          entry_anchors->insert(e);
        evaluateAtEntry(e, result);
        if (e.isTerminal())
          return;
      } else if (e.isTerminal() && m_bdd.isTrue(p)) {
        if (entry_anchors)
          entry_anchors->insert(e);
        evaluateAtEntry(e, result);
        return;
      }
      auto &seen = processed[{fn, e}];
      for (BddManager::NodeId sp : seen)
        if (m_bdd.isTrue(sp) || sp == p)
          return;
      if (seen.size() >= m_options.max_guards_per_state) {
        p = BddManager::TRUE_NODE;
        ++m_stats.guards_widened;
        for (BddManager::NodeId sp : seen)
          if (m_bdd.isTrue(sp))
            return;
      }
      if (m_bdd.isTrue(p))
        seen.clear();
      seen.push_back(p);
      processing.emplace_back(fn, e, p);
    };
    for (const auto &[q, cur_path] : frontier)
      pushUp(root_fn, q, cur_path);
    while (!processing.empty()) {
      auto [h, q, cur_path] = std::move(processing.front());
      processing.pop_front();
      tick();
      for (Id call_site : m_index.incoming_calls[h]) {
        if (!m_reachability.reachable_site[call_site])
          continue;
        const InstLocation &cloc = m_index.by_site[call_site];
        const Id g = cloc.function;
        if (!m_fs_reachable_function[g])
          continue;
        const Instruction &ci = *cloc.inst;
        if (ci.callee == INVALID && ci.indirect_target != INVALID) {
          bool exact = true;
          const PointsToSet tpts = evalGuardPointer(
              ci.indirect_target, call_site, nullptr, false, exact);
          const Targets ct = callees(m_program, ci, tpts);
          if (!ct.functions.count(h))
            continue;
        }
        const BddManager::NodeId sub_path =
            track_inter_path ? applyCallCondSubst(h, ci, cur_path)
                             : BddManager::TRUE_NODE;
        if (m_bdd.isFalse(sub_path)) {
          pruned_any_path = true;
          ++m_stats.path_infeasible_prunes;
          continue;
        }
        if (q.isTerminal()) {
          if (!track_inter_path || m_bdd.isTrue(sub_path)) {
            if (entry_anchors)
              entry_anchors->insert(q);
            evaluateAtEntry(q, result);
            continue;
          }
          SummaryKey pk{g, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                        Point::Before};
          const auto path_entries = computeSummary(pk);
          for (const SummaryEntry &se : path_entries) {
            if (!isGuardSatisfiable(se.guard, nullptr))
              continue;
            const BddManager::NodeId lifted =
                applyKilledToPath(sub_path, se.killed);
            pushUp(g, q, m_bdd.makeAnd(lifted, se.path));
          }
          continue;
        }
        std::vector<SummaryEntry> caller_exprs;
        substituteCalleeExpr(h, ci, q, 0, true, 0, GUARD_TRUE, sub_path,
                             caller_exprs);
        for (const SummaryEntry &ce : caller_exprs) {
          if (ce.source.isTerminal()) {
            if (track_inter_path && !m_bdd.isTrue(sub_path)) {
              SummaryKey pk{g, SummaryKind::LocalPoint, Expr::makeNull(),
                            call_site, Point::Before};
              const auto path_entries = computeSummary(pk);
              for (const SummaryEntry &se : path_entries) {
                if (!isGuardSatisfiable(se.guard, nullptr))
                  continue;
                const BddManager::NodeId lifted =
                    applyKilledToPath(sub_path, se.killed);
                pushUp(g, ce.source, m_bdd.makeAnd(lifted, se.path));
              }
            } else {
              if (entry_anchors)
                entry_anchors->insert(ce.source);
              evaluateAtEntry(ce.source, result);
            }
            continue;
          }
          if (ce.source.kind == ExprKind::Mem &&
              !canFunctionModifyObj(g, ce.source.id) &&
              (!track_inter_path || m_bdd.isTrue(sub_path))) {
            ++m_stats.skipped_functions;
            pushUp(g, ce.source, sub_path);
            continue;
          }
          SummaryKey ck{g, SummaryKind::LocalPoint, ce.source, call_site,
                        Point::Before};
          const auto caller_entries = computeSummary(ck);
          for (const SummaryEntry &se : caller_entries) {
            if (!isGuardSatisfiable(se.guard, nullptr))
              continue;
            const BddManager::NodeId combined_p =
                track_inter_path
                    ? m_bdd.makeAnd(applyKilledToPath(sub_path, se.killed),
                                    se.path)
                    : BddManager::TRUE_NODE;
            pushUp(g, se.source, combined_p);
          }
        }
      }
    }
  }

  if (result.empty()) {
    if (m_options.path_sensitive && pruned_any_path &&
        !isPathReachable(site, point, &ctx, is_suffix))
      return {{}, QueryStatus::Unreachable};
    if (widen_empty_to_top)
      result = PointsToSet::top();
  }
  return {std::move(result), QueryStatus::Complete};
}

QueryResult GuardedSummarySolver::computeFscs(Id val, Id site,
                                              const Context &ctx, Point point,
                                              std::set<Expr> *entry_anchors,
                                              bool is_suffix) {
  FscsKey key{val, site, point, ctx, is_suffix};
  if (entry_anchors == nullptr) {
    auto cached = m_fscs_cache.find(key);
    if (cached != m_fscs_cache.end()) {
      ++m_stats.context_cache_hits;
      return cached->second;
    }
  }

  if (!validateContext(site, ctx, is_suffix))
    return {{}, QueryStatus::Unreachable};

  if (m_program.values[val].constant) {
    const PointsToSet &init = m_program.values[val].initial;
    if (entry_anchors) {
      if (init.isTop())
        entry_anchors->insert(Expr::makeTop());
      else
        for (Id o : init.objects())
          entry_anchors->insert(o == NULL_OBJECT ? Expr::makeNull()
                                                 : Expr::makeAddr(o));
    }
    QueryResult qr{init, QueryStatus::Complete};
    m_fscs_cache.emplace(std::move(key), qr);
    return qr;
  }

  m_active_fscs.insert(key);
  QueryResult qr = computeFscsExpr(Expr::makeVal(val), site, ctx, point,
                                   entry_anchors, /*widen_empty_to_top=*/true,
                                   is_suffix);
  m_active_fscs.erase(key);
  m_fscs_cache.emplace(std::move(key), qr);
  return qr;
}

// Algorithm 3 Step 2 (p. 256 col. 2): Forward summary propagation from entry
// anchor set `A` at `m_program.entry` across reachable call chains down to
// `(target_fn, site, point)` to compute the alias set `Q` directly.
void GuardedSummarySolver::propagateForwardAnchorsFsci(
    const std::set<Expr> &anchors, Id target_fn, Id site, Point point,
    const std::vector<Id> &candidates, const std::vector<Id> &obj_candidates,
    std::set<Id> &out_aliases, std::set<Id> &out_obj_aliases,
    bool &exact_complete) {
  exact_complete = true;
  if (anchors.empty())
    return;

  const bool track_inter_path = m_options.path_sensitive_interprocedural;

  // Resolve concrete objects represented by entry anchors so constant / field
  // expressions match forward along summaries.
  PointsToSet anchor_objs;
  for (const Expr &a : anchors)
    evaluateAtEntry(a, anchor_objs);

  // Restrict forward call-graph traversal strictly to functions that can reach
  // `target_fn` (avoids exploring unrelated branches of the call graph!).
  std::vector<bool> can_reach_target(m_program.functions.size(), false);
  {
    std::deque<Id> bq{target_fn};
    can_reach_target[target_fn] = true;
    while (!bq.empty()) {
      const Id cur = bq.front();
      bq.pop_front();
      for (Id cs : m_index.incoming_calls[cur]) {
        if (!m_reachability.reachable_site[cs])
          continue;
        const Id caller = m_index.by_site[cs].function;
        if (m_fs_reachable_function[caller] && !can_reach_target[caller]) {
          can_reach_target[caller] = true;
          bq.push_back(caller);
        }
      }
    }
  }
  if (!can_reach_target[m_program.entry])
    return;

  using FrontierMap = std::map<Expr, std::vector<BddManager::NodeId>>;
  auto addToFrontier = [&](FrontierMap &map, const Expr &e,
                           BddManager::NodeId p) -> bool {
    if (m_bdd.isFalse(p))
      return false;
    if (!track_inter_path)
      p = BddManager::TRUE_NODE;
    auto &vec = map[e];
    for (BddManager::NodeId sp : vec)
      if (m_bdd.isTrue(sp) || sp == p)
        return false;
    if (vec.size() >= m_options.max_guards_per_state) {
      p = BddManager::TRUE_NODE;
      for (BddManager::NodeId sp : vec)
        if (m_bdd.isTrue(sp))
          return false;
    }
    if (m_bdd.isTrue(p))
      vec.clear();
    vec.push_back(p);
    return true;
  };

  auto hasSymbolicOffsetOrDeref = [](const Expr &e) -> bool {
    return e.kind == ExprKind::Deref || e.val_offset != 0 ||
           !e.val_offset_known || e.val_stride != 0;
  };

  auto matchSummaryForward =
      [&](const SummaryEntry &se, const FrontierMap &frontier, bool at_entry,
          std::vector<BddManager::NodeId> &out_paths) {
        auto consider = [&](BddManager::NodeId fp) {
          const BddManager::NodeId joined =
              track_inter_path ? m_bdd.makeAnd(fp, se.path)
                               : BddManager::TRUE_NODE;
          if (!m_bdd.isFalse(joined))
            out_paths.push_back(
                track_inter_path ? applyKilledToPath(joined, se.killed)
                                 : BddManager::TRUE_NODE);
        };
        auto it = frontier.find(se.source);
        if (it != frontier.end()) {
          for (BddManager::NodeId fp : it->second)
            consider(fp);
        }
        if (se.source.kind == ExprKind::Top) {
          if (!track_inter_path) {
            out_paths.push_back(BddManager::TRUE_NODE);
          } else {
            for (const auto &[fe, fps] : frontier)
              for (BddManager::NodeId fp : fps)
                consider(fp);
          }
          return;
        }
        if (se.source.kind == ExprKind::Addr &&
            se.source.id != NULL_OBJECT && anchor_objs.contains(se.source.id)) {
          if (!track_inter_path) {
            out_paths.push_back(BddManager::TRUE_NODE);
          } else {
            for (const auto &[fe, fps] : frontier) {
              PointsToSet fo;
              evaluateAtEntry(fe, fo);
              if (fo.contains(se.source.id))
                for (BddManager::NodeId fp : fps)
                  consider(fp);
            }
          }
        } else if (at_entry && !se.source.isTerminal()) {
          PointsToSet src_pts;
          evaluateAtEntry(se.source, src_pts);
          if (memoryIntersects(src_pts, anchor_objs)) {
            if (!track_inter_path) {
              out_paths.push_back(BddManager::TRUE_NODE);
            } else {
              for (const auto &[fe, fps] : frontier) {
                PointsToSet fo;
                evaluateAtEntry(fe, fo);
                if (memoryIntersects(src_pts, fo))
                  for (BddManager::NodeId fp : fps)
                    consider(fp);
              }
            }
          }
        }
        if (out_paths.empty() &&
            (hasSymbolicOffsetOrDeref(se.source) ||
             (se.source.kind == ExprKind::Mem &&
              m_program.objects[se.source.id].kind != ObjectKind::Global)))
          exact_complete = false;
      };

  std::map<Id, FrontierMap> fwd_processed;
  std::deque<std::pair<Id, FrontierMap>> fwd_work;
  FrontierMap entry_map;
  for (const Expr &a : anchors)
    addToFrontier(entry_map, a, BddManager::TRUE_NODE);
  if (!anchor_objs.isTop()) {
    for (Id o : anchor_objs.objects())
      if (o != NULL_OBJECT)
        addToFrontier(entry_map, Expr::makeAddr(o), BddManager::TRUE_NODE);
  }
  // Seed global memory cells and entry parameters whose initial values
  // intersect `anchor_objs` so forward propagation catches aliases originating
  // from unmodified globals or entry arguments without backward fallback.
  for (Id o : m_slice.object_list) {
    if (m_program.objects[o].kind == ObjectKind::Global &&
        memoryIntersects(m_program.objects[o].initial, anchor_objs))
      addToFrontier(entry_map, Expr::makeMem(o), BddManager::TRUE_NODE);
  }
  for (Id ep : m_program.functions[m_program.entry].parameters) {
    if (ep != INVALID && m_slice.values[ep])
      addToFrontier(entry_map, Expr::makeVal(ep), BddManager::TRUE_NODE);
  }
  fwd_processed[m_program.entry] = entry_map;
  fwd_work.emplace_back(m_program.entry, std::move(entry_map));

  // Build reachable call sites per function restricted to `can_reach_target`.
  std::map<Id, std::vector<Id>> outgoing_calls;
  for (Id call_site : m_index.call_sites) {
    if (!m_reachability.reachable_site[call_site])
      continue;
    const Id caller = m_index.by_site[call_site].function;
    if (can_reach_target[caller])
      outgoing_calls[caller].push_back(call_site);
  }

  while (!fwd_work.empty()) {
    const Id h = fwd_work.front().first;
    FrontierMap cur_map = std::move(fwd_work.front().second);
    fwd_work.pop_front();
    tick();

    auto oit = outgoing_calls.find(h);
    if (oit == outgoing_calls.end())
      continue;
    const bool h_at_entry = (h == m_program.entry);
    for (Id call_site : oit->second) {
      const Instruction &ci = *m_index.by_site[call_site].inst;
      Targets targets;
      if (ci.callee != INVALID) {
        targets.functions.insert(ci.callee);
      } else if (ci.indirect_target != INVALID) {
        bool exact = true;
        const PointsToSet tpts = evalGuardPointer(ci.indirect_target, call_site,
                                                  nullptr, false, exact);
        targets = callees(m_program, ci, tpts);
      }

      std::vector<SummaryEntry> call_path_entries;
      bool computed_call_path = false;
      auto getCallPathEntries = [&]() -> const std::vector<SummaryEntry> & {
        if (!computed_call_path) {
          computed_call_path = true;
          SummaryKey pk{h, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                        Point::Before};
          call_path_entries = computeSummary(pk);
        }
        return call_path_entries;
      };

      for (Id callee : targets.functions) {
        if (m_program.functions[callee].external || !can_reach_target[callee])
          continue;

        FrontierMap callee_in;
        auto pushCallee = [&](const Expr &e, BddManager::NodeId p) {
          if (track_inter_path) {
            p = quantifyFuncEntryConditions(callee, p);
            if (m_call_graph.scc_recursive[m_call_graph.function_scc[callee]])
              p = quantifyFuncConditions(callee, p);
          }
          addToFrontier(callee_in, e, p);
        };
        // 1. Terminal anchors always propagate into reachable callees.
        for (const auto &[e, fps] : cur_map) {
          if (!e.isTerminal())
            continue;
          if (!track_inter_path) {
            pushCallee(e, BddManager::TRUE_NODE);
          } else {
            for (const SummaryEntry &se : getCallPathEntries()) {
              if (!isGuardSatisfiable(se.guard, nullptr))
                continue;
              for (BddManager::NodeId fp : fps) {
                const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                if (!m_bdd.isFalse(j))
                  pushCallee(e, applyKilledToPath(j, se.killed));
              }
            }
          }
        }

        // 2. Formal parameters whose matching actual has a satisfiable summary
        //    from `cur_map` to `(call_site, Before)`.
        const auto &params = m_program.functions[callee].parameters;
        for (std::size_t a = 0; a < params.size(); ++a) {
          const Id param = params[a];
          if (param == INVALID || !m_slice.values[param])
            continue;
          const Id actual = a < ci.operands.size() ? ci.operands[a] : INVALID;
          if (actual == INVALID) {
            // Omitted/non-pointer actual widens to Top inside callee
            // (matching substituteCalleeExpr), so param may alias any non-null
            // anchor.
            exact_complete = false;
            if (!track_inter_path)
              pushCallee(Expr::makeVal(param), BddManager::TRUE_NODE);
            continue;
          }
          if (m_program.values[actual].constant) {
            if (m_program.values[actual].initial.empty()) {
              exact_complete = false;
            } else if (memoryIntersects(m_program.values[actual].initial,
                                        anchor_objs)) {
              if (!track_inter_path) {
                pushCallee(Expr::makeVal(param), BddManager::TRUE_NODE);
              } else {
                for (const SummaryEntry &se : getCallPathEntries()) {
                  if (!isGuardSatisfiable(se.guard, nullptr))
                    continue;
                  for (const auto &[fe, fps] : cur_map) {
                    PointsToSet fo;
                    evaluateAtEntry(fe, fo);
                    if (!memoryIntersects(fo, m_program.values[actual].initial))
                      continue;
                    for (BddManager::NodeId fp : fps) {
                      const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                      if (!m_bdd.isFalse(j))
                        pushCallee(Expr::makeVal(param),
                                   applyKilledToPath(j, se.killed));
                    }
                  }
                }
              }
            }
            continue;
          }
          SummaryKey ak{h, SummaryKind::LocalPoint, Expr::makeVal(actual),
                        call_site, Point::Before};
          const auto entries = computeSummary(ak);
          if (entries.empty())
            exact_complete = false;
          for (const SummaryEntry &se : entries) {
            if (!isGuardSatisfiable(se.guard, nullptr))
              continue;
            std::vector<BddManager::NodeId> matched;
            matchSummaryForward(se, cur_map, h_at_entry, matched);
            for (BddManager::NodeId mp : matched)
              pushCallee(Expr::makeVal(param), mp);
          }
        }

        // 3. Forward Retain / Propagate for relevant memory objects.
        for (const auto &[e, fps] : cur_map) {
          if (e.kind == ExprKind::Mem && !canFunctionModifyObj(h, e.id)) {
            ++m_stats.skipped_functions;
            if (!track_inter_path) {
              pushCallee(e, BddManager::TRUE_NODE);
            } else {
              for (const SummaryEntry &se : getCallPathEntries()) {
                if (!isGuardSatisfiable(se.guard, nullptr))
                  continue;
                for (BddManager::NodeId fp : fps) {
                  const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                  if (!m_bdd.isFalse(j))
                    pushCallee(e, applyKilledToPath(j, se.killed));
                }
              }
            }
          }
        }
        if (m_func_modifies_any[h]) {
          for (Id obj : m_slice.object_list) {
            if (!canFunctionModifyObj(h, obj))
              continue;
            SummaryKey mk{h, SummaryKind::LocalPoint, Expr::makeMem(obj),
                          call_site, Point::Before};
            const auto entries = computeSummary(mk);
            for (const SummaryEntry &se : entries) {
              if (!isGuardSatisfiable(se.guard, nullptr))
                continue;
              std::vector<BddManager::NodeId> matched;
              matchSummaryForward(se, cur_map, h_at_entry, matched);
              for (BddManager::NodeId mp : matched)
                pushCallee(Expr::makeMem(obj), mp);
            }
          }
        }

        FrontierMap not_processed;
        auto &seen = fwd_processed[callee];
        for (const auto &[e, fps] : callee_in) {
          for (BddManager::NodeId fp : fps)
            if (addToFrontier(seen, e, fp))
              addToFrontier(not_processed, e, fp);
        }
        if (!not_processed.empty())
          fwd_work.emplace_back(callee, std::move(not_processed));
      }
    }
  }

  const auto fit = fwd_processed.find(target_fn);
  if (fit == fwd_processed.end())
    return;
  const FrontierMap &target_frontier = fit->second;
  const bool target_at_entry = (target_fn == m_program.entry);
  for (Id cand : candidates) {
    if (m_program.values[cand].constant) {
      if (memoryIntersects(m_program.values[cand].initial, anchor_objs))
        out_aliases.insert(cand);
      continue;
    }
    SummaryKey ck{target_fn, SummaryKind::LocalPoint, Expr::makeVal(cand), site,
                  point};
    const auto entries = computeSummary(ck);
    if (entries.empty())
      exact_complete = false;
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, nullptr))
        continue;
      std::vector<BddManager::NodeId> matched;
      matchSummaryForward(se, target_frontier, target_at_entry, matched);
      if (!matched.empty()) {
        out_aliases.insert(cand);
        break;
      }
    }
  }
  for (Id obj : obj_candidates) {
    if (!canFunctionModifyObj(target_fn, obj)) {
      if (target_frontier.count(Expr::makeMem(obj)) ||
          (target_at_entry &&
           m_program.objects[obj].kind == ObjectKind::Global &&
           memoryIntersects(m_program.objects[obj].initial, anchor_objs))) {
        out_obj_aliases.insert(obj);
      }
      continue;
    }
    SummaryKey mk{target_fn, SummaryKind::LocalPoint, Expr::makeMem(obj), site,
                  point};
    const auto entries = computeSummary(mk);
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, nullptr))
        continue;
      std::vector<BddManager::NodeId> matched;
      matchSummaryForward(se, target_frontier, target_at_entry, matched);
      if (!matched.empty()) {
        out_obj_aliases.insert(obj);
        break;
      }
    }
  }
}

// Section 3 (p. 258): Context-sensitive forward propagation of entry anchor
// set `A` along exact call sequence `ctx = [c_1, ..., c_k]` to compute `Q`.
void GuardedSummarySolver::propagateForwardAnchorsFscs(
    const std::set<Expr> &anchors, const Context &ctx, Id site, Point point,
    const std::vector<Id> &candidates, const std::vector<Id> &obj_candidates,
    std::set<Id> &out_aliases, std::set<Id> &out_obj_aliases,
    bool &exact_complete, bool is_suffix) {
  exact_complete = true;
  if (anchors.empty())
    return;
  if (is_suffix && ctx.empty()) {
    propagateForwardAnchorsFsci(anchors, m_index.by_site[site].function, site,
                                point, candidates, obj_candidates, out_aliases,
                                out_obj_aliases, exact_complete);
    return;
  }

  const bool track_inter_path = m_options.path_sensitive_interprocedural;

  PointsToSet anchor_objs;
  for (const Expr &a : anchors)
    evaluateAtEntry(a, anchor_objs);

  using FrontierMap = std::map<Expr, std::vector<BddManager::NodeId>>;
  auto addToFrontier = [&](FrontierMap &map, const Expr &e,
                           BddManager::NodeId p) -> bool {
    if (m_bdd.isFalse(p))
      return false;
    if (!track_inter_path)
      p = BddManager::TRUE_NODE;
    auto &vec = map[e];
    for (BddManager::NodeId sp : vec)
      if (m_bdd.isTrue(sp) || sp == p)
        return false;
    if (vec.size() >= m_options.max_guards_per_state) {
      p = BddManager::TRUE_NODE;
      for (BddManager::NodeId sp : vec)
        if (m_bdd.isTrue(sp))
          return false;
    }
    if (m_bdd.isTrue(p))
      vec.clear();
    vec.push_back(p);
    return true;
  };

  auto hasSymbolicOffsetOrDeref = [](const Expr &e) -> bool {
    return e.kind == ExprKind::Deref || e.val_offset != 0 ||
           !e.val_offset_known || e.val_stride != 0;
  };

  auto matchSummaryForward =
      [&](const SummaryEntry &se, const FrontierMap &frontier, bool at_entry,
          std::vector<BddManager::NodeId> &out_paths) {
        auto consider = [&](BddManager::NodeId fp) {
          const BddManager::NodeId joined =
              track_inter_path ? m_bdd.makeAnd(fp, se.path)
                               : BddManager::TRUE_NODE;
          if (!m_bdd.isFalse(joined))
            out_paths.push_back(
                track_inter_path ? applyKilledToPath(joined, se.killed)
                                 : BddManager::TRUE_NODE);
        };
        auto it = frontier.find(se.source);
        if (it != frontier.end()) {
          for (BddManager::NodeId fp : it->second)
            consider(fp);
        }
        if (se.source.kind == ExprKind::Top) {
          if (!track_inter_path) {
            out_paths.push_back(BddManager::TRUE_NODE);
          } else {
            for (const auto &[fe, fps] : frontier)
              for (BddManager::NodeId fp : fps)
                consider(fp);
          }
          return;
        }
        if (se.source.kind == ExprKind::Addr &&
            se.source.id != NULL_OBJECT && anchor_objs.contains(se.source.id)) {
          if (!track_inter_path) {
            out_paths.push_back(BddManager::TRUE_NODE);
          } else {
            for (const auto &[fe, fps] : frontier) {
              PointsToSet fo;
              evaluateAtEntry(fe, fo);
              if (fo.contains(se.source.id))
                for (BddManager::NodeId fp : fps)
                  consider(fp);
            }
          }
        } else if (at_entry && !se.source.isTerminal()) {
          PointsToSet src_pts;
          evaluateAtEntry(se.source, src_pts);
          if (memoryIntersects(src_pts, anchor_objs)) {
            if (!track_inter_path) {
              out_paths.push_back(BddManager::TRUE_NODE);
            } else {
              for (const auto &[fe, fps] : frontier) {
                PointsToSet fo;
                evaluateAtEntry(fe, fo);
                if (memoryIntersects(src_pts, fo))
                  for (BddManager::NodeId fp : fps)
                    consider(fp);
              }
            }
          }
        }
        if (out_paths.empty() &&
            (hasSymbolicOffsetOrDeref(se.source) ||
             (se.source.kind == ExprKind::Mem &&
              m_program.objects[se.source.id].kind != ObjectKind::Global)))
          exact_complete = false;
      };

  FrontierMap entry_frontier;
  for (const Expr &a : anchors)
    addToFrontier(entry_frontier, a, BddManager::TRUE_NODE);
  if (!anchor_objs.isTop()) {
    for (Id o : anchor_objs.objects())
      if (o != NULL_OBJECT)
        addToFrontier(entry_frontier, Expr::makeAddr(o), BddManager::TRUE_NODE);
  }
  for (Id o : m_slice.object_list) {
    if (m_program.objects[o].kind == ObjectKind::Global &&
        memoryIntersects(m_program.objects[o].initial, anchor_objs))
      addToFrontier(entry_frontier, Expr::makeMem(o), BddManager::TRUE_NODE);
  }
  for (Id ep : m_program.functions[m_program.entry].parameters) {
    if (ep != INVALID && m_slice.values[ep])
      addToFrontier(entry_frontier, Expr::makeVal(ep), BddManager::TRUE_NODE);
  }

  FrontierMap frontier;
  const Id root_fn =
      (is_suffix && !ctx.empty()) ? m_index.by_site[ctx[0]].function
                                  : m_program.entry;
  if (!is_suffix || root_fn == m_program.entry) {
    frontier = std::move(entry_frontier);
  } else {
    // Suffix mode: propagate FSCI forward from `entry` to `root_fn`, then
    // follow the exact suffix `ctx` below.
    std::vector<bool> can_reach_root(m_program.functions.size(), false);
    {
      std::deque<Id> bq{root_fn};
      can_reach_root[root_fn] = true;
      while (!bq.empty()) {
        const Id cur = bq.front();
        bq.pop_front();
        for (Id cs : m_index.incoming_calls[cur]) {
          if (!m_reachability.reachable_site[cs])
            continue;
          const Id caller = m_index.by_site[cs].function;
          if (m_fs_reachable_function[caller] && !can_reach_root[caller]) {
            can_reach_root[caller] = true;
            bq.push_back(caller);
          }
        }
      }
    }
    if (!can_reach_root[m_program.entry])
      return;
    std::map<Id, std::vector<Id>> outgoing_calls;
    for (Id cs : m_index.call_sites) {
      if (!m_reachability.reachable_site[cs])
        continue;
      const Id caller = m_index.by_site[cs].function;
      if (can_reach_root[caller])
        outgoing_calls[caller].push_back(cs);
    }
    std::map<Id, FrontierMap> fwd_processed;
    std::deque<std::pair<Id, FrontierMap>> fwd_work;
    fwd_processed[m_program.entry] = entry_frontier;
    fwd_work.emplace_back(m_program.entry, std::move(entry_frontier));
    while (!fwd_work.empty()) {
      const Id h = fwd_work.front().first;
      FrontierMap cur_map = std::move(fwd_work.front().second);
      fwd_work.pop_front();
      tick();
      auto oit = outgoing_calls.find(h);
      if (oit == outgoing_calls.end())
        continue;
      const bool h_at_entry = (h == m_program.entry);
      for (Id cs : oit->second) {
        const Instruction &ci = *m_index.by_site[cs].inst;
        Targets targets;
        if (ci.callee != INVALID) {
          targets.functions.insert(ci.callee);
        } else if (ci.indirect_target != INVALID) {
          bool exact = true;
          const PointsToSet tpts = evalGuardPointer(ci.indirect_target, cs,
                                                    nullptr, false, exact);
          targets = callees(m_program, ci, tpts);
        }
        std::vector<SummaryEntry> call_path_entries;
        bool computed_call_path = false;
        auto getCallPath = [&]() -> const std::vector<SummaryEntry> & {
          if (!computed_call_path) {
            computed_call_path = true;
            SummaryKey pk{h, SummaryKind::LocalPoint, Expr::makeNull(), cs,
                          Point::Before};
            call_path_entries = computeSummary(pk);
          }
          return call_path_entries;
        };
        for (Id callee : targets.functions) {
          if (m_program.functions[callee].external || !can_reach_root[callee])
            continue;
          FrontierMap callee_in;
          auto pushCallee = [&](const Expr &e, BddManager::NodeId p) {
            if (track_inter_path) {
              p = quantifyFuncEntryConditions(callee, p);
              if (m_call_graph.scc_recursive[m_call_graph.function_scc[callee]])
                p = quantifyFuncConditions(callee, p);
            }
            addToFrontier(callee_in, e, p);
          };
          for (const auto &[e, fps] : cur_map) {
            if (!e.isTerminal())
              continue;
            if (!track_inter_path) {
              pushCallee(e, BddManager::TRUE_NODE);
            } else {
              for (const SummaryEntry &se : getCallPath()) {
                if (!isGuardSatisfiable(se.guard, nullptr))
                  continue;
                for (BddManager::NodeId fp : fps) {
                  const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                  if (!m_bdd.isFalse(j))
                    pushCallee(e, applyKilledToPath(j, se.killed));
                }
              }
            }
          }
          const auto &params = m_program.functions[callee].parameters;
          for (std::size_t a = 0; a < params.size(); ++a) {
            const Id param = params[a];
            if (param == INVALID || !m_slice.values[param])
              continue;
            const Id actual = a < ci.operands.size() ? ci.operands[a] : INVALID;
            if (actual == INVALID) {
              exact_complete = false;
              if (!track_inter_path)
                pushCallee(Expr::makeVal(param), BddManager::TRUE_NODE);
              continue;
            }
            if (m_program.values[actual].constant) {
              if (m_program.values[actual].initial.empty()) {
                exact_complete = false;
              } else if (memoryIntersects(m_program.values[actual].initial,
                                          anchor_objs)) {
                pushCallee(Expr::makeVal(param), BddManager::TRUE_NODE);
              }
              continue;
            }
            SummaryKey ak{h, SummaryKind::LocalPoint, Expr::makeVal(actual), cs,
                          Point::Before};
            const auto entries = computeSummary(ak);
            if (entries.empty())
              exact_complete = false;
            for (const SummaryEntry &se : entries) {
              if (!isGuardSatisfiable(se.guard, nullptr))
                continue;
              std::vector<BddManager::NodeId> matched;
              matchSummaryForward(se, cur_map, h_at_entry, matched);
              for (BddManager::NodeId mp : matched)
                pushCallee(Expr::makeVal(param), mp);
            }
          }
          for (const auto &[e, fps] : cur_map) {
            if (e.kind == ExprKind::Mem && !canFunctionModifyObj(h, e.id)) {
              ++m_stats.skipped_functions;
              pushCallee(e, BddManager::TRUE_NODE);
            }
          }
          if (m_func_modifies_any[h]) {
            for (Id obj : m_slice.object_list) {
              if (!canFunctionModifyObj(h, obj))
                continue;
              SummaryKey mk{h, SummaryKind::LocalPoint, Expr::makeMem(obj), cs,
                            Point::Before};
              const auto entries = computeSummary(mk);
              for (const SummaryEntry &se : entries) {
                if (!isGuardSatisfiable(se.guard, nullptr))
                  continue;
                std::vector<BddManager::NodeId> matched;
                matchSummaryForward(se, cur_map, h_at_entry, matched);
                for (BddManager::NodeId mp : matched)
                  pushCallee(Expr::makeMem(obj), mp);
              }
            }
          }
          FrontierMap delta;
          FrontierMap &dest = fwd_processed[callee];
          for (const auto &[e, fps] : callee_in)
            for (BddManager::NodeId p : fps)
              if (addToFrontier(dest, e, p))
                addToFrontier(delta, e, p);
          if (!delta.empty())
            fwd_work.emplace_back(callee, std::move(delta));
        }
      }
    }
    frontier = std::move(fwd_processed[root_fn]);
  }

  Context prefix;
  prefix.reserve(ctx.size());
  const Id leaf_fn = m_index.by_site[site].function;

  for (std::size_t step = 0; step < ctx.size(); ++step) {
    tick();
    const Id call_site = ctx[step];
    const InstLocation &cloc = m_index.by_site[call_site];
    const Id caller_fn = cloc.function;
    const Id callee_fn = (step + 1 < ctx.size())
                             ? m_index.by_site[ctx[step + 1]].function
                             : leaf_fn;
    const Instruction &ci = *cloc.inst;
    const bool step_at_entry = (step == 0 && root_fn == m_program.entry);
    const Context *guard_ctx = is_suffix ? nullptr : &prefix;

    std::vector<SummaryEntry> call_path_entries;
    bool computed_call_path = false;
    auto getCallPathEntries = [&]() -> const std::vector<SummaryEntry> & {
      if (!computed_call_path) {
        computed_call_path = true;
        SummaryKey pk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                      call_site, Point::Before};
        call_path_entries = computeSummary(pk);
      }
      return call_path_entries;
    };

    FrontierMap next_frontier;
    auto pushNext = [&](const Expr &e, BddManager::NodeId p) {
      if (track_inter_path) {
        p = quantifyFuncEntryConditions(callee_fn, p);
        if (m_call_graph.scc_recursive[m_call_graph.function_scc[callee_fn]])
          p = quantifyFuncConditions(callee_fn, p);
      }
      addToFrontier(next_frontier, e, p);
    };
    for (const auto &[e, fps] : frontier) {
      if (!e.isTerminal())
        continue;
      if (!track_inter_path) {
        pushNext(e, BddManager::TRUE_NODE);
      } else {
        for (const SummaryEntry &se : getCallPathEntries()) {
          if (!isGuardSatisfiable(se.guard, guard_ctx))
            continue;
          for (BddManager::NodeId fp : fps) {
            const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
            if (!m_bdd.isFalse(j))
              pushNext(e, applyKilledToPath(j, se.killed));
          }
        }
      }
    }

    const auto &params = m_program.functions[callee_fn].parameters;
    for (std::size_t a = 0; a < params.size(); ++a) {
      const Id param = params[a];
      if (param == INVALID || !m_slice.values[param])
        continue;
      const Id actual = a < ci.operands.size() ? ci.operands[a] : INVALID;
      if (actual == INVALID) {
        // Omitted/non-pointer actual widens to Top inside callee
        // (matching substituteCalleeExpr), so param may alias any non-null
        // anchor.
        exact_complete = false;
        if (!track_inter_path)
          pushNext(Expr::makeVal(param), BddManager::TRUE_NODE);
        continue;
      }
      if (m_program.values[actual].constant) {
        if (m_program.values[actual].initial.empty()) {
          exact_complete = false;
        } else if (memoryIntersects(m_program.values[actual].initial,
                                    anchor_objs)) {
          if (!track_inter_path) {
            pushNext(Expr::makeVal(param), BddManager::TRUE_NODE);
          } else {
            for (const SummaryEntry &se : getCallPathEntries()) {
              if (!isGuardSatisfiable(se.guard, guard_ctx))
                continue;
              for (const auto &[fe, fps] : frontier) {
                PointsToSet fo;
                evaluateAtEntry(fe, fo);
                if (!memoryIntersects(fo, m_program.values[actual].initial))
                  continue;
                for (BddManager::NodeId fp : fps) {
                  const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                  if (!m_bdd.isFalse(j))
                    pushNext(Expr::makeVal(param),
                             applyKilledToPath(j, se.killed));
                }
              }
            }
          }
        }
        continue;
      }
      SummaryKey ak{caller_fn, SummaryKind::LocalPoint, Expr::makeVal(actual),
                    call_site, Point::Before};
      const auto entries = computeSummary(ak);
      if (entries.empty())
        exact_complete = false;
      for (const SummaryEntry &se : entries) {
        if (!isGuardSatisfiable(se.guard, guard_ctx))
          continue;
        std::vector<BddManager::NodeId> matched;
        matchSummaryForward(se, frontier, step_at_entry, matched);
        for (BddManager::NodeId mp : matched)
          pushNext(Expr::makeVal(param), mp);
      }
    }

    for (const auto &[e, fps] : frontier) {
      if (e.kind == ExprKind::Mem && !canFunctionModifyObj(caller_fn, e.id)) {
        ++m_stats.skipped_functions;
        if (!track_inter_path) {
          pushNext(e, BddManager::TRUE_NODE);
        } else {
          for (const SummaryEntry &se : getCallPathEntries()) {
            if (!isGuardSatisfiable(se.guard, guard_ctx))
              continue;
            for (BddManager::NodeId fp : fps) {
              const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
              if (!m_bdd.isFalse(j))
                pushNext(e, applyKilledToPath(j, se.killed));
            }
          }
        }
      }
    }
    if (m_func_modifies_any[caller_fn]) {
      for (Id obj : m_slice.object_list) {
        if (!canFunctionModifyObj(caller_fn, obj))
          continue;
        SummaryKey mk{caller_fn, SummaryKind::LocalPoint, Expr::makeMem(obj),
                      call_site, Point::Before};
        const auto entries = computeSummary(mk);
        for (const SummaryEntry &se : entries) {
          if (!isGuardSatisfiable(se.guard, guard_ctx))
            continue;
          std::vector<BddManager::NodeId> matched;
          matchSummaryForward(se, frontier, step_at_entry, matched);
          for (BddManager::NodeId mp : matched)
            pushNext(Expr::makeMem(obj), mp);
        }
      }
    }

    frontier = std::move(next_frontier);
    prefix.push_back(call_site);
  }

  const bool leaf_at_entry = (!is_suffix && ctx.empty());
  const Context *leaf_guard_ctx = is_suffix ? nullptr : &ctx;
  for (Id cand : candidates) {
    if (m_program.values[cand].constant) {
      if (memoryIntersects(m_program.values[cand].initial, anchor_objs))
        out_aliases.insert(cand);
      continue;
    }
    SummaryKey ck{leaf_fn, SummaryKind::LocalPoint, Expr::makeVal(cand), site,
                  point};
    const auto entries = computeSummary(ck);
    if (entries.empty())
      exact_complete = false;
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, leaf_guard_ctx))
        continue;
      std::vector<BddManager::NodeId> matched;
      matchSummaryForward(se, frontier, leaf_at_entry, matched);
      if (!matched.empty()) {
        out_aliases.insert(cand);
        break;
      }
    }
  }
  for (Id obj : obj_candidates) {
    if (!canFunctionModifyObj(leaf_fn, obj)) {
      if (frontier.count(Expr::makeMem(obj)) ||
          (leaf_at_entry && m_program.objects[obj].kind == ObjectKind::Global &&
           memoryIntersects(m_program.objects[obj].initial, anchor_objs))) {
        out_obj_aliases.insert(obj);
      }
      continue;
    }
    SummaryKey mk{leaf_fn, SummaryKind::LocalPoint, Expr::makeMem(obj), site,
                  point};
    const auto entries = computeSummary(mk);
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, leaf_guard_ctx))
        continue;
      std::vector<BddManager::NodeId> matched;
      matchSummaryForward(se, frontier, leaf_at_entry, matched);
      if (!matched.empty()) {
        out_obj_aliases.insert(obj);
        break;
      }
    }
  }
}

// Literal Algorithm 2 lines 2 & 8 (p. 255): eagerly precompute function
// summaries (ExitReturn and ExitMem) in reverse topological SCC order for
// pointers/objects at Steensgaard depth `depth`.
void GuardedSummarySolver::precomputeDepthSummaries(std::size_t depth) {
  for (Id scc : m_call_graph.reverse_topo_sccs) {
    for (Id f : m_call_graph.scc_members[scc]) {
      if (m_program.functions[f].external || !m_slice.relevant_functions[f] ||
          !m_reachability.may_return[f] || !m_reachability.reachable_function[f])
        continue;
      SummaryKey ret_key{f, SummaryKind::ExitReturn, Expr::makeTop(), INVALID,
                         Point::Before};
      if (!m_index.return_sites[f].empty() &&
          depthOfSummaryKey(ret_key) == depth) {
        bool has_ptr_ret = false;
        for (Id rs : m_index.return_sites[f])
          if (!m_index.by_site[rs].inst->operands.empty())
            has_ptr_ret = true;
        if (has_ptr_ret)
          computeSummary(ret_key);
      }
      if (m_func_modifies_any[f]) {
        for (Id obj : m_slice.object_list) {
          if (!canFunctionModifyObj(f, obj))
            continue;
          SummaryKey mem_key{f, SummaryKind::ExitMem, Expr::makeMem(obj),
                             INVALID, Point::Before};
          if (depthOfSummaryKey(mem_key) == depth)
            computeSummary(mem_key);
        }
      }
    }
  }
}

// Tighten function-level FSCI reachability from `m_program.entry` using
// converged flow-sensitive indirect-call targets in `m_fsci_cache` (falling
// back soundly to `m_upper` for any unsliced indirect call).
void GuardedSummarySolver::computeFlowSensitiveReachability() {
  const std::size_t nf = m_program.functions.size();
  m_fs_reachable_function.assign(nf, false);
  if (m_program.entry >= nf)
    return;
  std::deque<Id> q{m_program.entry};
  m_fs_reachable_function[m_program.entry] = true;
  while (!q.empty()) {
    const Id f = q.front();
    q.pop_front();
    const Function &func = m_program.functions[f];
    if (func.external)
      continue;
    for (const Block &block : func.blocks) {
      for (const Instruction &inst : block.instructions) {
        if (inst.opcode != Opcode::Call ||
            !m_reachability.reachable_site[inst.id])
          continue;
        PointsToSet target_pts;
        if (inst.callee == INVALID && inst.indirect_target != INVALID) {
          auto it = m_fsci_cache.find(
              {inst.indirect_target, inst.id, Point::Before});
          if (it != m_fsci_cache.end())
            target_pts = it->second;
          else
            target_pts = valueOf(m_program, m_upper, inst.indirect_target);
        }
        const Targets targets = callees(m_program, inst, target_pts);
        for (Id callee : targets.functions) {
          if (!m_fs_reachable_function[callee]) {
            m_fs_reachable_function[callee] = true;
            q.push_back(callee);
          }
        }
      }
    }
  }
}

// Algorithm 2 (Kahlon PLDI '08, p. 255): Top-down dovetailing of summary
// computation and FSCI points-to computation in increasing Steensgaard + Field
// dependency depth.
void GuardedSummarySolver::runDovetailing() {
  std::map<std::size_t, std::vector<FsciKey>> derefs_by_depth;
  std::set<std::size_t> all_depths;
  for (Id v : m_slice.value_list)
    all_depths.insert(dovetailDepthOfComp(m_hierarchy.value_components[v]));
  if (m_options.eager_dovetail_summaries) {
    for (Id obj : m_slice.object_list)
      all_depths.insert(depthOfSummaryKey(
          {0, SummaryKind::ExitMem, Expr::makeMem(obj), INVALID, Point::Before}));
  }

  auto recordFsciSite = [&](Id val, Id site, Point pt) {
    if (val == INVALID || m_program.values[val].constant ||
        !m_reachability.reachable_site[site])
      return;
    const Id comp = m_hierarchy.value_components[val];
    const std::size_t d = dovetailDepthOfComp(comp);
    all_depths.insert(d);
    derefs_by_depth[d].emplace_back(val, site, pt);
  };

  for (Id site : m_slice.inst_list) {
    const Instruction &i = *m_index.by_site[site].inst;
    switch (i.opcode) {
    case Opcode::Load:
      recordFsciSite(i.operands[0], site, Point::Before);
      if (i.result != INVALID &&
          dovetailCyclicOfComp(m_hierarchy.value_components[i.result]))
        recordFsciSite(i.result, site, Point::After);
      break;
    case Opcode::Store:
      recordFsciSite(i.operands[0], site, Point::Before);
      break;
    case Opcode::MemCopy:
      recordFsciSite(i.operands[0], site, Point::Before);
      recordFsciSite(i.operands[1], site, Point::Before);
      break;
    case Opcode::Havoc:
      if (!i.operands.empty())
        recordFsciSite(i.operands[0], site, Point::Before);
      break;
    case Opcode::Call:
      if (i.callee == INVALID && i.indirect_target != INVALID)
        recordFsciSite(i.indirect_target, site, Point::Before);
      if (i.result != INVALID &&
          dovetailCyclicOfComp(m_hierarchy.value_components[i.result]))
        recordFsciSite(i.result, site, Point::After);
      break;
    default:
      break;
    }
  }

  // Under path sensitivity, also record in-slice edge pointer-guard operands
  // so their FSCI sets are precomputed in top-down dovetail order.
  if (m_options.path_sensitive) {
    for (Id f = 0; f < m_program.functions.size(); ++f) {
      const Function &func = m_program.functions[f];
      if (func.external || !m_reachability.reachable_function[f])
        continue;
      for (const Block &block : func.blocks) {
        const Id fallback =
            block.instructions.empty() ? INVALID : block.instructions.back().id;
        for (const Edge &edge : block.successors) {
          for (const EdgePtrGuard &eg : edge.ptr_guards) {
            const Id gsite = eg.site != INVALID ? eg.site : fallback;
            if (gsite == INVALID)
              continue;
            if (eg.lhs < m_slice.values.size() && m_slice.values[eg.lhs])
              recordFsciSite(eg.lhs, gsite, Point::After);
            if ((eg.kind == EdgeGuardKind::Alias ||
                 eg.kind == EdgeGuardKind::NotAlias) &&
                eg.rhs < m_slice.values.size() && m_slice.values[eg.rhs])
              recordFsciSite(eg.rhs, gsite, Point::After);
          }
        }
      }
    }
  }

  m_stats.dovetail_levels = all_depths.size();

  for (std::size_t d : all_depths) {
    auto dit = derefs_by_depth.find(d);
    if (dit == derefs_by_depth.end()) {
      if (m_options.eager_dovetail_summaries)
        precomputeDepthSummaries(d);
      m_completed_depth = d;
      m_has_completed_depth = true;
      continue;
    }
    auto &keys = dit->second;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    bool has_cyclic = false;
    for (const auto &[val, site, pt] : keys)
      if (dovetailCyclicOfComp(m_hierarchy.value_components[val]))
        has_cyclic = true;

    if (!has_cyclic) {
      if (m_options.eager_dovetail_summaries)
        precomputeDepthSummaries(d);
      for (const auto &[val, site, pt] : keys) {
        bool pruned = false;
        PointsToSet pts = computeFsci(val, site, pt, nullptr, &pruned);
        if (pts.empty() && pruned)
          m_path_pruned_fsci.insert({val, site, pt});
        m_fsci_cache[{val, site, pt}] = std::move(pts);
      }
    } else {
      // Cyclic depth d:
      // Pass 1: Monotone least fixpoint (same-depth negative guards treated
      // as always satisfiable -> guaranteed sound convergence).
      m_cyclic_pass1_depth = d;
      m_pass1_keys.clear();
      m_pass1_pending_keys.clear();
      m_pass1_extra_added = 0;
      for (const auto &k : keys) {
        m_pass1_keys.insert(k);
        m_fsci_cache[k];
      }
      auto clearDepthSummaries = [&]() {
        for (auto it = m_summaries.begin(); it != m_summaries.end();) {
          if (depthOfSummaryKey(it->first) >= d ||
              m_pass_created_summaries.count(it->first)) {
            m_finalized.erase(it->first);
            m_path_pruned_summaries.erase(it->first);
            it = m_summaries.erase(it);
          } else {
            ++it;
          }
        }
        m_pass_created_summaries.clear();
      };
      bool changed = true;
      while (changed) {
        changed = false;
        clearDepthSummaries();
        for (std::size_t i = 0; i < keys.size(); ++i) {
          const auto &[val, site, pt] = keys[i];
          PointsToSet next = computeFsci(val, site, pt);
          PointsToSet &cur = m_fsci_cache[keys[i]];
          if (cur.join(next))
            changed = true;
        }
        if (!m_pass1_pending_keys.empty()) {
          keys.insert(keys.end(), m_pass1_pending_keys.begin(),
                      m_pass1_pending_keys.end());
          m_pass1_pending_keys.clear();
          changed = true;
        }
      }
      for (const auto &k : keys)
        m_cyclic_pass1_fsci[k] = m_fsci_cache[k];
      m_pass1_keys.clear();
      m_pass1_pending_keys.clear();

      // Pass 2: Precision tightening against converged Pass-1 sound sets.
      m_cyclic_pass1_depth = std::numeric_limits<std::size_t>::max();
      m_in_cyclic_pass2 = true;
      m_completed_depth = d;
      m_has_completed_depth = true;
      clearDepthSummaries();
      std::map<FsciKey, PointsToSet> tightened;
      for (const auto &k : keys) {
        const auto &[val, site, pt] = k;
        bool pruned = false;
        PointsToSet pts = computeFsci(val, site, pt, nullptr, &pruned);
        if (pts.empty() && pruned)
          m_path_pruned_fsci.insert(k);
        else
          m_path_pruned_fsci.erase(k);
        tightened[k] = std::move(pts);
      }
      m_in_cyclic_pass2 = false;
      bool any_tightened = false;
      for (auto &[k, v] : tightened) {
        if (m_fsci_cache[k] != v)
          any_tightened = true;
        m_fsci_cache[k] = std::move(v);
      }
      if (any_tightened)
        clearDepthSummaries();
      else
        m_pass_created_summaries.clear();
      if (m_options.eager_dovetail_summaries)
        precomputeDepthSummaries(d);
    }

    m_completed_depth = d;
    m_has_completed_depth = true;
  }

  m_dovetailing_done = true;
  computeFlowSensitiveReachability();
}

GuardedSummarySolver::GuardedSummarySolver(
    const Program &program, const ProgramIndex &index, const Sets &upper,
    const SteensgaardHierarchy &hierarchy, const CallGraphInfo &call_graph,
    const ReachabilityInfo &reachability, std::vector<Id> cluster_values,
    Slice slice, const Options &options)
    : m_program(program), m_index(index), m_upper(upper),
      m_hierarchy(hierarchy), m_call_graph(call_graph),
      m_reachability(reachability), m_cluster_values(std::move(cluster_values)),
      m_slice(std::move(slice)), m_options(options),
      m_bdd(options.max_bdd_nodes),
      m_fs_reachable_function(reachability.reachable_function) {
  m_guards.emplace_back();
  m_guard_ids.emplace(std::vector<AtomId>{}, GUARD_TRUE);

  m_stats.call_graph_sccs = m_call_graph.scc_count;
  m_stats.recursive_call_graph_sccs = m_call_graph.recursive_scc_count;
  m_stats.slice_values = m_slice.value_list.size();
  m_stats.slice_objects = m_slice.object_list.size();
  m_stats.slice_statements = m_slice.inst_list.size();

  for (const Object &obj : m_program.objects) {
    if (obj.field_sensitive && obj.size_bytes > 0)
      m_max_offset_span = std::max(m_max_offset_span,
                                   static_cast<std::int64_t>(obj.size_bytes));
    if (obj.offset > 0)
      m_max_offset_span = std::max(
          m_max_offset_span, static_cast<std::int64_t>(obj.offset) + 64);
  }

  computeFunctionModifications();
  computeFunctionConditionKills();
  computeInvariantSSAVals();
  computeDovetailHierarchy();

  try {
    if (m_options.max_contexts == 0 || m_options.max_steps == 0)
      throw LimitReached{};
    runDovetailing();
  } catch (const LimitReached &) {
    m_failed = true;
  }
}

QueryResult GuardedSummarySolver::query(Id value, Id site,
                                        const Context *context_path,
                                        Point point, ContextMatch match) {
  if (m_failed)
    return {PointsToSet::top(), QueryStatus::ResourceLimit};
  try {
    if (context_path != nullptr &&
        !(match == ContextMatch::Suffix && context_path->empty()))
      return computeFscs(value, site, *context_path, point, nullptr,
                         match == ContextMatch::Suffix);

    const Id fn = m_index.by_site[site].function;
    if (!m_reachability.reachable_site[site] || !m_fs_reachable_function[fn])
      return {{}, QueryStatus::Unreachable};
    FsciKey fkey{value, site, point};
    auto it = m_fsci_cache.find(fkey);
    PointsToSet pts;
    bool path_pruned = false;
    if (it != m_fsci_cache.end() && !m_options.path_sensitive_interprocedural) {
      ++m_stats.fsci_cache_hits;
      pts = it->second;
      path_pruned = (m_path_pruned_fsci.count(fkey) != 0);
    } else {
      pts = computeFsci(value, site, point, nullptr, &path_pruned);
      if (!path_pruned || !pts.empty())
        m_fsci_cache[fkey] = pts;
    }
    if (pts.empty()) {
      if (m_options.path_sensitive && path_pruned) {
        m_path_pruned_fsci.insert(fkey);
        return {{}, QueryStatus::Unreachable};
      }
      pts = PointsToSet::top();
    }
    return {std::move(pts), QueryStatus::Complete};
  } catch (const LimitReached &) {
    m_failed = true;
    return {PointsToSet::top(), QueryStatus::ResourceLimit};
  }
}

AliasResult GuardedSummarySolver::finishAliasQuery(
    const QueryResult &base, const std::set<Expr> &entry_anchors, Id self_val,
    Id self_obj, Id site, const Context *context_path, Point point,
    ContextMatch match) {
  AliasResult out;
  out.status = base.status;
  if (!base.reachable())
    return out;
  if (base.status == QueryStatus::ResourceLimit) {
    out.may_alias_all = true;
    return out;
  }

  const InstLocation &qloc = m_index.by_site[site];
  const Id fn = qloc.function;
  const Function &func = m_program.functions[fn];
  // Compute backward CFG reachability strictly before `qloc.block` so local
  // variables whose definitions can never reach `(site, point)` are skipped
  // instead of spawning dead SummaryKeys.
  std::vector<bool> can_reach_strict(func.blocks.size(), false);
  {
    std::deque<Id> bq;
    for (const auto &[pblock, eidx] : m_index.pred_edges[fn][qloc.block]) {
      (void)eidx;
      if (!can_reach_strict[pblock]) {
        can_reach_strict[pblock] = true;
        bq.push_back(pblock);
      }
    }
    while (!bq.empty()) {
      const Id cur = bq.front();
      bq.pop_front();
      for (const auto &[pblock, eidx] : m_index.pred_edges[fn][cur]) {
        (void)eidx;
        if (!can_reach_strict[pblock]) {
          can_reach_strict[pblock] = true;
          bq.push_back(pblock);
        }
      }
    }
  }
  auto isLocalDefReachable = [&](Id cand) -> bool {
    if (cand == self_val || m_program.values[cand].constant)
      return true;
    for (Id param : func.parameters)
      if (param == cand)
        return true;
    for (Id dsite : m_index.defining_sites[cand]) {
      const InstLocation &dloc = m_index.by_site[dsite];
      if (dloc.function != fn)
        continue;
      if (can_reach_strict[dloc.block])
        return true;
      if (dloc.block == qloc.block &&
          (dloc.index < qloc.index ||
           (dloc.index == qloc.index && point == Point::After)))
        return true;
    }
    for (const auto &[pf, pedge] : m_index.phi_defs[cand]) {
      if (pf != fn)
        continue;
      const Id target =
          func.blocks[pedge.first].successors[pedge.second].target;
      if (target == qloc.block || can_reach_strict[target])
        return true;
    }
    return false;
  };

  std::vector<Id> in_scope;
  for (Id cand : m_cluster_values) {
    if (cand >= m_program.values.size())
      continue;
    const Value &vinfo = m_program.values[cand];
    if (!vinfo.constant && vinfo.function != INVALID && vinfo.function != fn)
      continue;
    if (!isLocalDefReachable(cand))
      continue;
    const PointsToSet cand_upper = valueOf(m_program, m_upper, cand);
    if (cand != self_val && !cand_upper.empty() &&
        !memoryIntersects(cand_upper, base.points_to))
      continue;
    in_scope.push_back(cand);
  }

  const std::size_t ncomp = m_hierarchy.successors.size();
  std::vector<bool> target_comps(ncomp, false);
  if (base.points_to.isTop()) {
    for (Id o = 0; o < m_program.objects.size(); ++o) {
      if (o != NULL_OBJECT && o < m_hierarchy.object_components.size() &&
          m_hierarchy.object_components[o] < ncomp)
        target_comps[m_hierarchy.object_components[o]] = true;
    }
  } else {
    for (Id t : base.points_to.objects()) {
      if (t != NULL_OBJECT && t < m_hierarchy.object_components.size() &&
          m_hierarchy.object_components[t] < ncomp)
        target_comps[m_hierarchy.object_components[t]] = true;
    }
    if (m_hierarchy.top_component != INVALID &&
        m_hierarchy.top_component < ncomp)
      target_comps[m_hierarchy.top_component] = true;
  }

  auto objMayTargetBase = [&](Id obj) -> bool {
    if (obj >= m_hierarchy.object_components.size())
      return false;
    const Id ocomp = m_hierarchy.object_components[obj];
    if (ocomp >= ncomp)
      return false;
    if (m_hierarchy.cyclic[ocomp] && target_comps[ocomp])
      return true;
    for (Id succ : m_hierarchy.successors[ocomp])
      if (succ < ncomp && target_comps[succ])
        return true;
    return false;
  };

  std::vector<Id> in_scope_objs;
  for (Id obj : m_slice.object_list) {
    if (obj < 2 || obj >= m_program.objects.size())
      continue;
    const Object &oinfo = m_program.objects[obj];
    if (!isMemory(oinfo) || (oinfo.bytes == 0 && oinfo.size_bytes != 0))
      continue;
    if (obj == self_obj || objMayTargetBase(obj))
      in_scope_objs.push_back(obj);
  }

  const bool is_suffix = (match == ContextMatch::Suffix);
  const bool use_ctx =
      (context_path != nullptr && !(is_suffix && context_path->empty()));
  auto queryObjectRaw = [&](Id obj) -> PointsToSet {
    if (use_ctx) {
      QueryResult r = computeFscsExpr(Expr::makeMem(obj), site, *context_path,
                                      point, nullptr,
                                      /*widen_empty_to_top=*/false, is_suffix);
      return r.reachable() ? r.points_to : PointsToSet{};
    }
    return computeFsciExpr(Expr::makeMem(obj), site, point);
  };

  if (base.points_to.isTop()) {
    out.may_alias_all = true;
    out.aliases = std::move(in_scope);
    if (self_obj != INVALID &&
        std::find(in_scope_objs.begin(), in_scope_objs.end(), self_obj) ==
            in_scope_objs.end())
      in_scope_objs.push_back(self_obj);
    out.object_aliases = std::move(in_scope_objs);
    return out;
  }

  std::set<Id> alias_set;
  if (self_val != INVALID)
    alias_set.insert(self_val);
  std::set<Id> obj_alias_set;
  if (self_obj != INVALID)
    obj_alias_set.insert(self_obj);

  if (!memoryIntersects(base.points_to, base.points_to)) {
    // Strictly NULL_OBJECT points-to set: no dereferenceable memory aliases.
    out.aliases.assign(alias_set.begin(), alias_set.end());
    out.object_aliases.assign(obj_alias_set.begin(), obj_alias_set.end());
    return out;
  }

  // Algorithm 3 Step 2: forward summary propagation from entry anchors A -> Q.
  bool exact_complete = true;
  if (use_ctx) {
    propagateForwardAnchorsFscs(entry_anchors, *context_path, site, point,
                                in_scope, in_scope_objs, alias_set,
                                obj_alias_set, exact_complete, is_suffix);
  } else {
    propagateForwardAnchorsFsci(entry_anchors, fn, site, point, in_scope,
                                in_scope_objs, alias_set, obj_alias_set,
                                exact_complete);
  }

  // Only fall back to pairwise backward queries when forward propagation
  // encountered an unmatched symbolic offset or dereference expression.
  if (!exact_complete) {
    for (Id cand : in_scope) {
      if (alias_set.count(cand))
        continue;
      QueryResult cq = query(cand, site, context_path, point, match);
      if (cq.status == QueryStatus::ResourceLimit) {
        out.status = QueryStatus::ResourceLimit;
        out.may_alias_all = true;
        return out;
      }
      if (cq.reachable() && memoryIntersects(cq.points_to, base.points_to))
        alias_set.insert(cand);
    }
    for (Id obj : in_scope_objs) {
      if (obj_alias_set.count(obj))
        continue;
      const PointsToSet raw_pts = queryObjectRaw(obj);
      if (memoryIntersects(raw_pts, base.points_to))
        obj_alias_set.insert(obj);
    }
  }

  out.aliases.assign(alias_set.begin(), alias_set.end());
  out.object_aliases.assign(obj_alias_set.begin(), obj_alias_set.end());
  return out;
}

// Algorithm 3 (Steps 1 & 2) and Section 3 (p. 258): compute entry anchors A
// backwards, then propagate forward to Q along summaries.
AliasResult GuardedSummarySolver::queryAliases(Id value, Id site,
                                               const Context *context_path,
                                               Point point,
                                               ContextMatch match) {
  AliasResult out;
  if (m_failed) {
    out.status = QueryStatus::ResourceLimit;
    out.may_alias_all = true;
    return out;
  }

  try {
    const Id fn = m_index.by_site[site].function;
    std::set<Expr> entry_anchors;
    QueryResult base;
    const bool is_suffix = (match == ContextMatch::Suffix);
    if (context_path != nullptr && !(is_suffix && context_path->empty())) {
      base = computeFscs(value, site, *context_path, point, &entry_anchors,
                         is_suffix);
    } else {
      if (!m_reachability.reachable_site[site] || !m_fs_reachable_function[fn])
        return out;
      bool path_pruned = false;
      PointsToSet pts =
          computeFsci(value, site, point, &entry_anchors, &path_pruned);
      if (pts.empty()) {
        if (m_options.path_sensitive && path_pruned)
          return out;
        pts = PointsToSet::top();
      }
      base = {std::move(pts), QueryStatus::Complete};
    }
    return finishAliasQuery(base, entry_anchors, value, INVALID, site,
                            context_path, point, match);
  } catch (const LimitReached &) {
    m_failed = true;
    out.status = QueryStatus::ResourceLimit;
    out.may_alias_all = true;
    return out;
  }
}

QueryResult GuardedSummarySolver::queryObject(Id object, Id site,
                                              const Context *context_path,
                                              Point point,
                                              ContextMatch match) {
  if (m_failed)
    return {PointsToSet::top(), QueryStatus::ResourceLimit};
  try {
    const bool is_suffix = (match == ContextMatch::Suffix);
    if (context_path != nullptr && !(is_suffix && context_path->empty()))
      return computeFscsExpr(Expr::makeMem(object), site, *context_path, point,
                             nullptr, /*widen_empty_to_top=*/true, is_suffix);
    const Id fn = m_index.by_site[site].function;
    if (!m_reachability.reachable_site[site] || !m_fs_reachable_function[fn])
      return {{}, QueryStatus::Unreachable};
    bool path_pruned = false;
    PointsToSet pts = computeFsciExpr(Expr::makeMem(object), site, point,
                                      nullptr, &path_pruned);
    if (pts.empty()) {
      if (m_options.path_sensitive && path_pruned)
        return {{}, QueryStatus::Unreachable};
      pts = PointsToSet::top();
    }
    return {std::move(pts), QueryStatus::Complete};
  } catch (const LimitReached &) {
    m_failed = true;
    return {PointsToSet::top(), QueryStatus::ResourceLimit};
  }
}

AliasResult GuardedSummarySolver::queryObjectAliases(
    Id object, Id site, const Context *context_path, Point point,
    ContextMatch match) {
  AliasResult out;
  if (m_failed) {
    out.status = QueryStatus::ResourceLimit;
    out.may_alias_all = true;
    return out;
  }
  try {
    const Id fn = m_index.by_site[site].function;
    std::set<Expr> entry_anchors;
    QueryResult base;
    const bool is_suffix = (match == ContextMatch::Suffix);
    if (context_path != nullptr && !(is_suffix && context_path->empty())) {
      base = computeFscsExpr(Expr::makeMem(object), site, *context_path, point,
                             &entry_anchors, /*widen_empty_to_top=*/true,
                             is_suffix);
    } else {
      if (!m_reachability.reachable_site[site] || !m_fs_reachable_function[fn])
        return out;
      bool path_pruned = false;
      PointsToSet pts = computeFsciExpr(Expr::makeMem(object), site, point,
                                        &entry_anchors, &path_pruned);
      if (pts.empty()) {
        if (m_options.path_sensitive && path_pruned)
          return out;
        pts = PointsToSet::top();
      }
      base = {std::move(pts), QueryStatus::Complete};
    }
    return finishAliasQuery(base, entry_anchors, INVALID, object, site,
                            context_path, point, match);
  } catch (const LimitReached &) {
    m_failed = true;
    out.status = QueryStatus::ResourceLimit;
    out.may_alias_all = true;
    return out;
  }
}

bool GuardedSummarySolver::mustAlias(Id lhs, Id rhs, Id site,
                                     const Context *context_path,
                                     ContextMatch match) {
  if (m_failed)
    return false;
  try {
    QueryResult a = query(lhs, site, context_path, Point::Before, match);
    if (a.status != QueryStatus::Complete)
      return false;
    if (m_program.values[lhs].constant && m_program.values[lhs].initial.isTop())
      return false;
    if (lhs == rhs)
      return true;
    QueryResult b = query(rhs, site, context_path, Point::Before, match);
    if (b.status != QueryStatus::Complete)
      return false;
    if (m_program.values[rhs].constant && m_program.values[rhs].initial.isTop())
      return false;
    if (!memoryIntersects(a.points_to, b.points_to))
      return false;
    if (!a.points_to.isTop() && !b.points_to.isTop() &&
        a.points_to.size() == 1 && a.points_to == b.points_to) {
      const Id obj = *a.points_to.begin();
      if (obj != NULL_OBJECT && obj < m_index.exact_address_singleton.size() &&
          m_index.exact_address_singleton[obj])
        return true;
    }
    const bool is_suffix = (match == ContextMatch::Suffix);
    const Context *guard_ctx = is_suffix ? nullptr : context_path;
    // Symbolic must-alias: if both `lhs` and `rhs` reduce inside `fn` to a
    // single satisfiable unconditional summary pointing to the exact same
    // invariant SSA value + known offset (e.g., `q = p` on a non-singleton
    // heap pointer), they must hold identical runtime addresses at `site`.
    const Id fn = m_index.by_site[site].function;
    auto uniqueInvariantSource = [&](Id v, Expr &out_src) -> bool {
      if (v == INVALID)
        return false;
      if (m_program.values[v].constant)
        return false;
      SummaryKey sk{fn, SummaryKind::LocalPoint, Expr::makeVal(v), site,
                    Point::Before};
      const auto entries = computeSummary(sk);
      bool found = false;
      for (const SummaryEntry &se : entries) {
        if (!isGuardSatisfiable(se.guard, guard_ctx))
          continue;
        if (se.source.kind != ExprKind::Val || !se.source.val_offset_known ||
            se.source.val_stride != 0 ||
            !isInvariantValueInFunc(se.source.id, fn))
          return false;
        if (!found) {
          out_src = se.source;
          found = true;
        } else if (!(out_src == se.source)) {
          return false;
        }
      }
      return found;
    };
    Expr s_lhs, s_rhs;
    if (uniqueInvariantSource(lhs, s_lhs) && uniqueInvariantSource(rhs, s_rhs) &&
        s_lhs == s_rhs)
      return true;
    return false;
  } catch (const LimitReached &) {
    m_failed = true;
    return false;
  }
}

std::unique_ptr<ClusterSolver> createGuardedSummarySolver(
    const Program &program, const ProgramIndex &index, const Sets &upper,
    const SteensgaardHierarchy &hierarchy, const CallGraphInfo &call_graph,
    const ReachabilityInfo &reachability, std::vector<Id> cluster_values,
    Slice slice, const Options &options) {
  return std::make_unique<GuardedSummarySolver>(
      program, index, upper, hierarchy, call_graph, reachability,
      std::move(cluster_values), std::move(slice), options);
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
