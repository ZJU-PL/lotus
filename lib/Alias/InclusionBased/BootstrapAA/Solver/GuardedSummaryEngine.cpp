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
      std::size_t d = m_hierarchy.depth[comp] + 1;
      for (Id succ : m_hierarchy.successors[comp])
        d = std::max(d, m_hierarchy.depth[succ]);
      return d;
    }
    return m_hierarchy.cyclic[comp] ? m_hierarchy.depth[comp]
                                    : m_hierarchy.depth[comp] + 1;
  }
  if (key.kind == SummaryKind::ExitReturn) {
    std::size_t d = 0;
    for (Id ret_site : m_index.return_sites[key.function]) {
      const Instruction &ri = *m_index.by_site[ret_site].inst;
      if (!ri.operands.empty() && ri.operands[0] != INVALID)
        d = std::max(
            d, m_hierarchy.depth[m_hierarchy.value_components[ri.operands[0]]]);
    }
    return d;
  }
  if (key.target.kind == ExprKind::Val && key.target.id != INVALID)
    return m_hierarchy.depth[m_hierarchy.value_components[key.target.id]];
  if (key.target.kind == ExprKind::Deref && key.target.id != INVALID) {
    const Id comp = m_hierarchy.value_components[key.target.id];
    return m_hierarchy.cyclic[comp] ? m_hierarchy.depth[comp]
                                    : m_hierarchy.depth[comp] + 1;
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
      // Entry-block SSA dominance: a single-def, non-PHI value defined in the
      // entry block (block 0) dominates every other block in the function.
      if (latest_def == INVALID && !m_options.path_sensitive &&
          defs.size() == 1 && m_index.phi_defs[root_key.target.id].empty()) {
        const InstLocation &dloc = m_index.by_site[defs[0]];
        if (dloc.function == qloc.function && dloc.block == 0 &&
            qloc.block != 0 && m_reachability.reachable_site[defs[0]])
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
    // Strip function-scoped formal-argument conditions when exporting
    // ExitReturn/ExitMem summaries across the function's entry boundary.
    // LocalPoint summaries keep the function's own argument conditions intact
    // so upward caller splicing can correlate them at call sites before
    // crossing the caller's entry boundary.
    if (owner.kind != SummaryKind::LocalPoint) {
      entry.path = quantifyFuncEntryConditions(owner.function, entry.path);
      if (m_options.path_sensitive_interprocedural &&
          owner.function < m_program.functions.size())
        addKilledConds(
            entry.killed,
            m_program.functions[owner.function].entry_kills_conditions);
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
        BddManager::NodeId combined_p =
            m_options.path_sensitive_interprocedural
                ? m_bdd.makeAnd(caller_before_p, entry.path)
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
        BddManager::NodeId combined_p =
            m_options.path_sensitive_interprocedural
                ? m_bdd.makeAnd(caller_before_p, ce.path)
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
        BddManager::NodeId combined_p =
            m_options.path_sensitive_interprocedural
                ? m_bdd.makeAnd(caller_before_p, ce.path)
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

      if (targets.opaque) {
        const BddManager::NodeId opaque_path =
            inst.writes_memory ? quantifyGlobalConditions(call_path)
                               : call_path;
        KilledConds opaque_k = call_k;
        if (inst.writes_memory)
          addKilledConds(opaque_k, m_program.global_conditions);
        if ((cur_q.kind == ExprKind::Val || cur_q.kind == ExprKind::Deref) &&
            cur_q.id == inst.result) {
          enqueue(item.owner, {inst.id, Point::Before}, Expr::makeTop(), cur_g,
                  opaque_path, std::move(opaque_k));
        } else if (inst.writes_memory &&
                   (cur_q.kind == ExprKind::Mem ||
                    cur_q.kind == ExprKind::Deref)) {
          enqueue(item.owner, {inst.id, Point::Before}, Expr::makeTop(), cur_g,
                  opaque_path, std::move(opaque_k));
        } else {
          enqueue(item.owner, {inst.id, Point::Before}, cur_q, cur_g,
                  opaque_path, std::move(opaque_k));
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
            m_bdd.literal(edge.cond_var, edge.cond_polarity);
        next_path = m_bdd.makeAnd(next_path, lit);
        if (m_bdd.isFalse(next_path)) {
          ++m_stats.path_infeasible_prunes;
          continue;
        }
      }

      const Id pred_site =
          pb.instructions.empty() ? INVALID : pb.instructions.back().id;
      std::vector<SummaryEntry> after_phi;
      applyPhiBackward(edge, cur_q, cur_g, next_path, target_depth,
                       pred_site != INVALID ? pred_site : block_insts[0].id,
                       after_phi);

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
                      m_bdd.literal(pedge.cond_var, pedge.cond_polarity));
                  if (m_bdd.isFalse(ppath)) {
                    ++m_stats.path_infeasible_prunes;
                    continue;
                  }
                }
                std::vector<SummaryEntry> pphi;
                applyPhiBackward(pedge, cq, cg, ppath, target_depth,
                                 ppblock.instructions.empty()
                                     ? item.pos.site
                                     : ppblock.instructions.back().id,
                                 pphi);
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

  for (const SummaryKey &k : seeded_in_scc) {
    m_finalized.insert(k);
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
    // Crossing `fn`'s entry upward into callers: existentially quantify
    // conditions scoped to a single activation of `fn` (e.g. `fn`'s own
    // formal arguments) AFTER intra-`fn` path correlation has completed.
    p = quantifyFuncEntryConditions(fn, p);
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

  SummaryKey local_key{start_fn, SummaryKind::LocalPoint, Expr::makeVal(val),
                       site, point};
  const std::size_t prunes_before = m_stats.path_infeasible_prunes;
  const auto local_entries = computeSummary(local_key);
  if (m_stats.path_infeasible_prunes > prunes_before)
    pruned_any_path = true;
  for (const SummaryEntry &se : local_entries) {
    if (!isGuardSatisfiable(se.guard, nullptr))
      continue;
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

      if (q.isTerminal()) {
        // Only reached when `track_inter_path && !m_bdd.isTrue(cur_path)`:
        // quantify conditions killed along caller `g`'s path before conjoining
        // with `g`'s path condition to `call_site`.
        SummaryKey pk{g, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                      Point::Before};
        const auto path_entries = computeSummary(pk);
        for (const SummaryEntry &se : path_entries) {
          if (!isGuardSatisfiable(se.guard, nullptr))
            continue;
          const BddManager::NodeId lifted =
              applyKilledToPath(cur_path, se.killed);
          const BddManager::NodeId combined_p = m_bdd.makeAnd(lifted, se.path);
          pushFrontier(g, q, combined_p);
        }
        continue;
      }

      std::vector<SummaryEntry> caller_exprs;
      substituteCalleeExpr(h, ci, q, 0, true, 0, GUARD_TRUE, cur_path,
                           caller_exprs);
      for (const SummaryEntry &ce : caller_exprs) {
        if (ce.source.isTerminal()) {
          if (track_inter_path && !m_bdd.isTrue(cur_path)) {
            SummaryKey pk{g, SummaryKind::LocalPoint, Expr::makeNull(),
                          call_site, Point::Before};
            const auto path_entries = computeSummary(pk);
            for (const SummaryEntry &se : path_entries) {
              if (!isGuardSatisfiable(se.guard, nullptr))
                continue;
              const BddManager::NodeId lifted =
                  applyKilledToPath(cur_path, se.killed);
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
            (!track_inter_path || m_bdd.isTrue(cur_path))) {
          ++m_stats.skipped_functions;
          pushFrontier(g, ce.source, cur_path);
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
                  ? m_bdd.makeAnd(applyKilledToPath(cur_path, se.killed),
                                  se.path)
                  : BddManager::TRUE_NODE;
          pushFrontier(g, se.source, combined_p);
        }
      }
    }
  }

  m_active_fsci.erase(fkey);
  if (path_pruned_out)
    *path_pruned_out = result.empty() && pruned_any_path &&
                       m_options.path_sensitive &&
                       !isPathReachable(site, point, nullptr);
  return result;
}

bool GuardedSummarySolver::validateContext(Id site, const Context &ctx) {
  if (!m_reachability.reachable_site[site])
    return false;
  Id current_fn = m_program.entry;
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
      const PointsToSet tpts = evalGuardPointer(ci.indirect_target, call_site,
                                                &prefix, false, exact);
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
                                           const Context *ctx) {
  if (!m_reachability.reachable_site[site])
    return false;
  const Id leaf_fn = m_index.by_site[site].function;
  if (ctx != nullptr) {
    if (!validateContext(site, *ctx))
      return false;
    SummaryKey pk{leaf_fn, SummaryKind::LocalPoint, Expr::makeNull(), site,
                  point};
    const auto entries = computeSummary(pk);
    std::set<BddManager::NodeId> frontier;
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, ctx) || m_bdd.isFalse(se.path))
        continue;
      if (!m_options.path_sensitive_interprocedural)
        return true;
      frontier.insert(quantifyFuncEntryConditions(leaf_fn, se.path));
    }
    if (frontier.empty())
      return false;
    Context prefix = *ctx;
    for (std::size_t step = ctx->size(); step > 0 && !frontier.empty();
         --step) {
      tick();
      const Id call_site = (*ctx)[step - 1];
      prefix.pop_back();
      const Id caller_fn = m_index.by_site[call_site].function;
      SummaryKey cpk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                     call_site, Point::Before};
      const auto call_entries = computeSummary(cpk);
      std::set<BddManager::NodeId> next_frontier;
      for (BddManager::NodeId cur_path : frontier) {
        for (const SummaryEntry &se : call_entries) {
          if (!isGuardSatisfiable(se.guard, &prefix))
            continue;
          const BddManager::NodeId lifted =
              applyKilledToPath(cur_path, se.killed);
          const BddManager::NodeId combined = m_bdd.makeAnd(lifted, se.path);
          if (!m_bdd.isFalse(combined))
            next_frontier.insert(
                quantifyFuncEntryConditions(caller_fn, combined));
        }
      }
      frontier = std::move(next_frontier);
    }
    return !frontier.empty();
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
    p = quantifyFuncEntryConditions(fn, p);
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
      SummaryKey cpk{g, SummaryKind::LocalPoint, Expr::makeNull(), call_site,
                     Point::Before};
      const auto call_entries = computeSummary(cpk);
      for (const SummaryEntry &se : call_entries) {
        if (!isGuardSatisfiable(se.guard, nullptr))
          continue;
        const BddManager::NodeId lifted =
            applyKilledToPath(cur_path, se.killed);
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
QueryResult GuardedSummarySolver::computeFscs(Id val, Id site,
                                              const Context &ctx, Point point,
                                              std::set<Expr> *entry_anchors) {
  FscsKey key{val, site, point, ctx};
  if (entry_anchors == nullptr) {
    auto cached = m_fscs_cache.find(key);
    if (cached != m_fscs_cache.end()) {
      ++m_stats.context_cache_hits;
      return cached->second;
    }
  }

  if (!validateContext(site, ctx))
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
  const bool track_inter_path = m_options.path_sensitive_interprocedural;
  bool pruned_any_path = false;
  PointsToSet result;
  std::set<std::pair<Expr, BddManager::NodeId>> frontier;

  auto insertFrontier = [&](std::set<std::pair<Expr, BddManager::NodeId>> &dst,
                            Id fn, const Expr &e, BddManager::NodeId p,
                            bool at_entry) {
    if (m_bdd.isFalse(p)) {
      pruned_any_path = true;
      ++m_stats.path_infeasible_prunes;
      return;
    }
    if (!track_inter_path)
      p = BddManager::TRUE_NODE;
    p = quantifyFuncEntryConditions(fn, p);
    if (e.isTerminal() && (at_entry || m_bdd.isTrue(p))) {
      if (entry_anchors)
        entry_anchors->insert(e);
      evaluateAtEntry(e, result);
      return;
    }
    dst.emplace(e, p);
  };

  const Id leaf_fn = m_index.by_site[site].function;
  SummaryKey local_key{leaf_fn, SummaryKind::LocalPoint, Expr::makeVal(val),
                       site, point};
  const std::size_t prunes_before = m_stats.path_infeasible_prunes;
  const auto local_entries = computeSummary(local_key);
  if (m_stats.path_infeasible_prunes > prunes_before)
    pruned_any_path = true;
  for (const SummaryEntry &se : local_entries) {
    if (!isGuardSatisfiable(se.guard, &ctx))
      continue;
    insertFrontier(frontier, leaf_fn, se.source, se.path, ctx.empty());
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
    const bool next_at_entry = (step == 1);

    std::set<std::pair<Expr, BddManager::NodeId>> next_frontier;
    for (const auto &[q, cur_path] : frontier) {
      if (q.isTerminal()) {
        SummaryKey pk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                      call_site, Point::Before};
        const auto path_entries = computeSummary(pk);
        for (const SummaryEntry &se : path_entries) {
          if (!isGuardSatisfiable(se.guard, &prefix))
            continue;
          const BddManager::NodeId lifted =
              applyKilledToPath(cur_path, se.killed);
          insertFrontier(next_frontier, caller_fn, q,
                         m_bdd.makeAnd(lifted, se.path), next_at_entry);
        }
        continue;
      }

      std::vector<SummaryEntry> caller_exprs;
      substituteCalleeExpr(callee_fn, ci, q, 0, true, 0, GUARD_TRUE, cur_path,
                           caller_exprs);
      for (const SummaryEntry &ce : caller_exprs) {
        if (ce.source.isTerminal()) {
          if (track_inter_path && !m_bdd.isTrue(cur_path)) {
            SummaryKey pk{caller_fn, SummaryKind::LocalPoint, Expr::makeNull(),
                          call_site, Point::Before};
            const auto path_entries = computeSummary(pk);
            for (const SummaryEntry &se : path_entries) {
              if (!isGuardSatisfiable(se.guard, &prefix))
                continue;
              const BddManager::NodeId lifted =
                  applyKilledToPath(cur_path, se.killed);
              insertFrontier(next_frontier, caller_fn, ce.source,
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
            (!track_inter_path || m_bdd.isTrue(cur_path))) {
          ++m_stats.skipped_functions;
          next_frontier.emplace(ce.source, cur_path);
          continue;
        }
        SummaryKey ck{caller_fn, SummaryKind::LocalPoint, ce.source, call_site,
                      Point::Before};
        const auto caller_entries = computeSummary(ck);
        for (const SummaryEntry &se : caller_entries) {
          if (!isGuardSatisfiable(se.guard, &prefix))
            continue;
          const BddManager::NodeId combined_p =
              track_inter_path
                  ? m_bdd.makeAnd(applyKilledToPath(cur_path, se.killed),
                                  se.path)
                  : BddManager::TRUE_NODE;
          insertFrontier(next_frontier, caller_fn, se.source, combined_p,
                         next_at_entry);
        }
      }
    }
    frontier = std::move(next_frontier);
  }

  for (const auto &[q, cur_path] : frontier) {
    if (m_bdd.isFalse(cur_path))
      continue;
    if (entry_anchors)
      entry_anchors->insert(q);
    evaluateAtEntry(q, result);
  }

  m_active_fscs.erase(key);

  if (result.empty()) {
    if (m_options.path_sensitive && pruned_any_path &&
        !isPathReachable(site, point, &ctx)) {
      QueryResult qr{{}, QueryStatus::Unreachable};
      m_fscs_cache.emplace(std::move(key), qr);
      return qr;
    }
    result = PointsToSet::top();
  }
  QueryResult qr{std::move(result), QueryStatus::Complete};
  m_fscs_cache.emplace(std::move(key), qr);
  return qr;
}

// Algorithm 3 Step 2 (p. 256 col. 2): Forward summary propagation from entry
// anchor set `A` at `m_program.entry` across reachable call chains down to
// `(target_fn, site, point)` to compute the alias set `Q` directly.
void GuardedSummarySolver::propagateForwardAnchorsFsci(
    const std::set<Expr> &anchors, Id target_fn, Id site, Point point,
    const std::vector<Id> &candidates, std::set<Id> &out_aliases) {
  if (anchors.empty())
    return;

  const bool track_inter_path = m_options.path_sensitive_interprocedural;

  // Resolve concrete objects represented by entry anchors so constant / field
  // expressions match forward along summaries.
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

  auto matchSummaryForward =
      [&](const SummaryEntry &se, const FrontierMap &frontier,
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
            anchor_objs.contains(se.source.id)) {
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
        } else if (se.source.kind == ExprKind::Null &&
                   anchor_objs.contains(NULL_OBJECT)) {
          if (!track_inter_path) {
            out_paths.push_back(BddManager::TRUE_NODE);
          } else {
            for (const auto &[fe, fps] : frontier) {
              PointsToSet fo;
              evaluateAtEntry(fe, fo);
              if (fo.contains(NULL_OBJECT))
                for (BddManager::NodeId fp : fps)
                  consider(fp);
            }
          }
        }
      };

  std::map<Id, FrontierMap> fwd_processed;
  std::deque<std::pair<Id, FrontierMap>> fwd_work;
  FrontierMap entry_map;
  for (const Expr &a : anchors)
    addToFrontier(entry_map, a, BddManager::TRUE_NODE);
  fwd_processed[m_program.entry] = entry_map;
  fwd_work.emplace_back(m_program.entry, std::move(entry_map));

  // Build reachable call sites per function restricted to FS-reachable funcs.
  std::map<Id, std::vector<Id>> outgoing_calls;
  for (Id call_site : m_index.call_sites) {
    if (!m_reachability.reachable_site[call_site])
      continue;
    const Id caller = m_index.by_site[call_site].function;
    if (m_fs_reachable_function[caller])
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
        if (m_program.functions[callee].external ||
            !m_fs_reachable_function[callee])
          continue;

        FrontierMap callee_in;
        // 1. Terminal anchors always propagate into reachable callees.
        for (const auto &[e, fps] : cur_map) {
          if (!e.isTerminal())
            continue;
          if (!track_inter_path) {
            addToFrontier(callee_in, e, BddManager::TRUE_NODE);
          } else {
            for (const SummaryEntry &se : getCallPathEntries()) {
              if (!isGuardSatisfiable(se.guard, nullptr))
                continue;
              for (BddManager::NodeId fp : fps) {
                const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                if (!m_bdd.isFalse(j))
                  addToFrontier(callee_in, e, applyKilledToPath(j, se.killed));
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
          if (actual == INVALID)
            continue;
          if (m_program.values[actual].constant) {
            if (m_program.values[actual].initial.intersects(anchor_objs)) {
              if (!track_inter_path) {
                addToFrontier(callee_in, Expr::makeVal(param),
                              BddManager::TRUE_NODE);
              } else {
                for (const SummaryEntry &se : getCallPathEntries()) {
                  if (!isGuardSatisfiable(se.guard, nullptr))
                    continue;
                  for (const auto &[fe, fps] : cur_map) {
                    PointsToSet fo;
                    evaluateAtEntry(fe, fo);
                    if (!fo.intersects(m_program.values[actual].initial))
                      continue;
                    for (BddManager::NodeId fp : fps) {
                      const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                      if (!m_bdd.isFalse(j))
                        addToFrontier(callee_in, Expr::makeVal(param),
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
          for (const SummaryEntry &se : entries) {
            if (!isGuardSatisfiable(se.guard, nullptr))
              continue;
            std::vector<BddManager::NodeId> matched;
            matchSummaryForward(se, cur_map, matched);
            for (BddManager::NodeId mp : matched)
              addToFrontier(callee_in, Expr::makeVal(param), mp);
          }
        }

        // 3. Forward Retain / Propagate for relevant memory objects.
        for (const auto &[e, fps] : cur_map) {
          if (e.kind == ExprKind::Mem && !canFunctionModifyObj(h, e.id)) {
            ++m_stats.skipped_functions;
            if (!track_inter_path) {
              addToFrontier(callee_in, e, BddManager::TRUE_NODE);
            } else {
              for (const SummaryEntry &se : getCallPathEntries()) {
                if (!isGuardSatisfiable(se.guard, nullptr))
                  continue;
                for (BddManager::NodeId fp : fps) {
                  const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                  if (!m_bdd.isFalse(j))
                    addToFrontier(callee_in, e,
                                  applyKilledToPath(j, se.killed));
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
              matchSummaryForward(se, cur_map, matched);
              for (BddManager::NodeId mp : matched)
                addToFrontier(callee_in, Expr::makeMem(obj), mp);
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
  for (Id cand : candidates) {
    if (m_program.values[cand].constant) {
      if (m_program.values[cand].initial.intersects(anchor_objs))
        out_aliases.insert(cand);
      continue;
    }
    SummaryKey ck{target_fn, SummaryKind::LocalPoint, Expr::makeVal(cand), site,
                  point};
    const auto entries = computeSummary(ck);
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, nullptr))
        continue;
      std::vector<BddManager::NodeId> matched;
      matchSummaryForward(se, target_frontier, matched);
      if (!matched.empty()) {
        out_aliases.insert(cand);
        break;
      }
    }
  }
}

// Section 3 (p. 258): Context-sensitive forward propagation of entry anchor
// set `A` along exact call sequence `ctx = [c_1, ..., c_k]` to compute `Q`.
void GuardedSummarySolver::propagateForwardAnchorsFscs(
    const std::set<Expr> &anchors, const Context &ctx, Id site, Point point,
    const std::vector<Id> &candidates, std::set<Id> &out_aliases) {
  if (anchors.empty())
    return;

  const bool track_inter_path = m_options.path_sensitive_interprocedural;

  PointsToSet anchor_objs;
  for (const Expr &a : anchors)
    evaluateAtEntry(a, anchor_objs);

  using FrontierMap = std::map<Expr, std::vector<BddManager::NodeId>>;
  auto addToFrontier = [&](FrontierMap &map, const Expr &e,
                           BddManager::NodeId p) {
    if (m_bdd.isFalse(p))
      return;
    if (!track_inter_path)
      p = BddManager::TRUE_NODE;
    auto &vec = map[e];
    for (BddManager::NodeId sp : vec)
      if (m_bdd.isTrue(sp) || sp == p)
        return;
    if (vec.size() >= m_options.max_guards_per_state) {
      p = BddManager::TRUE_NODE;
      for (BddManager::NodeId sp : vec)
        if (m_bdd.isTrue(sp))
          return;
    }
    if (m_bdd.isTrue(p))
      vec.clear();
    vec.push_back(p);
  };

  auto matchSummaryForward =
      [&](const SummaryEntry &se, const FrontierMap &frontier,
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
            anchor_objs.contains(se.source.id)) {
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
        } else if (se.source.kind == ExprKind::Null &&
                   anchor_objs.contains(NULL_OBJECT)) {
          if (!track_inter_path) {
            out_paths.push_back(BddManager::TRUE_NODE);
          } else {
            for (const auto &[fe, fps] : frontier) {
              PointsToSet fo;
              evaluateAtEntry(fe, fo);
              if (fo.contains(NULL_OBJECT))
                for (BddManager::NodeId fp : fps)
                  consider(fp);
            }
          }
        }
      };

  FrontierMap frontier;
  for (const Expr &a : anchors)
    addToFrontier(frontier, a, BddManager::TRUE_NODE);
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
    for (const auto &[e, fps] : frontier) {
      if (!e.isTerminal())
        continue;
      if (!track_inter_path) {
        addToFrontier(next_frontier, e, BddManager::TRUE_NODE);
      } else {
        for (const SummaryEntry &se : getCallPathEntries()) {
          if (!isGuardSatisfiable(se.guard, &prefix))
            continue;
          for (BddManager::NodeId fp : fps) {
            const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
            if (!m_bdd.isFalse(j))
              addToFrontier(next_frontier, e, applyKilledToPath(j, se.killed));
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
      if (actual == INVALID)
        continue;
      if (m_program.values[actual].constant) {
        if (m_program.values[actual].initial.intersects(anchor_objs)) {
          if (!track_inter_path) {
            addToFrontier(next_frontier, Expr::makeVal(param),
                          BddManager::TRUE_NODE);
          } else {
            for (const SummaryEntry &se : getCallPathEntries()) {
              if (!isGuardSatisfiable(se.guard, &prefix))
                continue;
              for (const auto &[fe, fps] : frontier) {
                PointsToSet fo;
                evaluateAtEntry(fe, fo);
                if (!fo.intersects(m_program.values[actual].initial))
                  continue;
                for (BddManager::NodeId fp : fps) {
                  const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
                  if (!m_bdd.isFalse(j))
                    addToFrontier(next_frontier, Expr::makeVal(param),
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
      for (const SummaryEntry &se : entries) {
        if (!isGuardSatisfiable(se.guard, &prefix))
          continue;
        std::vector<BddManager::NodeId> matched;
        matchSummaryForward(se, frontier, matched);
        for (BddManager::NodeId mp : matched)
          addToFrontier(next_frontier, Expr::makeVal(param), mp);
      }
    }

    for (const auto &[e, fps] : frontier) {
      if (e.kind == ExprKind::Mem && !canFunctionModifyObj(caller_fn, e.id)) {
        ++m_stats.skipped_functions;
        if (!track_inter_path) {
          addToFrontier(next_frontier, e, BddManager::TRUE_NODE);
        } else {
          for (const SummaryEntry &se : getCallPathEntries()) {
            if (!isGuardSatisfiable(se.guard, &prefix))
              continue;
            for (BddManager::NodeId fp : fps) {
              const BddManager::NodeId j = m_bdd.makeAnd(fp, se.path);
              if (!m_bdd.isFalse(j))
                addToFrontier(next_frontier, e,
                              applyKilledToPath(j, se.killed));
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
          if (!isGuardSatisfiable(se.guard, &prefix))
            continue;
          std::vector<BddManager::NodeId> matched;
          matchSummaryForward(se, frontier, matched);
          for (BddManager::NodeId mp : matched)
            addToFrontier(next_frontier, Expr::makeMem(obj), mp);
        }
      }
    }

    frontier = std::move(next_frontier);
    prefix.push_back(call_site);
  }

  for (Id cand : candidates) {
    if (m_program.values[cand].constant) {
      if (m_program.values[cand].initial.intersects(anchor_objs))
        out_aliases.insert(cand);
      continue;
    }
    SummaryKey ck{leaf_fn, SummaryKind::LocalPoint, Expr::makeVal(cand), site,
                  point};
    const auto entries = computeSummary(ck);
    for (const SummaryEntry &se : entries) {
      if (!isGuardSatisfiable(se.guard, &ctx))
        continue;
      std::vector<BddManager::NodeId> matched;
      matchSummaryForward(se, frontier, matched);
      if (!matched.empty()) {
        out_aliases.insert(cand);
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
// computation and FSCI points-to computation in increasing Steensgaard depth.
void GuardedSummarySolver::runDovetailing() {
  std::map<std::size_t, std::vector<FsciKey>> derefs_by_depth;
  std::set<std::size_t> all_depths;
  for (Id v : m_slice.value_list)
    all_depths.insert(m_hierarchy.depth[m_hierarchy.value_components[v]]);

  auto recordFsciSite = [&](Id val, Id site, Point pt) {
    if (val == INVALID || m_program.values[val].constant ||
        !m_reachability.reachable_site[site])
      return;
    const Id comp = m_hierarchy.value_components[val];
    const std::size_t d = m_hierarchy.depth[comp];
    all_depths.insert(d);
    derefs_by_depth[d].emplace_back(val, site, pt);
  };

  for (Id site : m_slice.inst_list) {
    const Instruction &i = *m_index.by_site[site].inst;
    switch (i.opcode) {
    case Opcode::Load:
      recordFsciSite(i.operands[0], site, Point::Before);
      if (i.result != INVALID &&
          m_hierarchy.cyclic[m_hierarchy.value_components[i.result]])
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
          m_hierarchy.cyclic[m_hierarchy.value_components[i.result]])
        recordFsciSite(i.result, site, Point::After);
      break;
    default:
      break;
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
      if (m_hierarchy.cyclic[m_hierarchy.value_components[val]])
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
          if (depthOfSummaryKey(it->first) >= d) {
            m_finalized.erase(it->first);
            m_path_pruned_summaries.erase(it->first);
            it = m_summaries.erase(it);
          } else {
            ++it;
          }
        }
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
      bool any_tightened = false;
      for (auto &[k, v] : tightened) {
        if (m_fsci_cache[k] != v)
          any_tightened = true;
        m_fsci_cache[k] = std::move(v);
      }
      if (any_tightened)
        clearDepthSummaries();
      if (m_options.eager_dovetail_summaries)
        precomputeDepthSummaries(d);
    }

    m_completed_depth = d;
    m_has_completed_depth = true;
  }

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
                                        Point point) {
  if (m_failed)
    return {PointsToSet::top(), QueryStatus::ResourceLimit};
  try {
    if (context_path != nullptr)
      return computeFscs(value, site, *context_path, point);

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

// Algorithm 3 (Steps 1 & 2) and Section 3 (p. 258): compute entry anchors A
// backwards, then propagate forward to Q along summaries, combined soundly
// with concrete points-to intersection for offset/field/top aliases.
AliasResult GuardedSummarySolver::queryAliases(Id value, Id site,
                                               const Context *context_path,
                                               Point point) {
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
    if (context_path != nullptr) {
      base = computeFscs(value, site, *context_path, point, &entry_anchors);
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

    out.status = base.status;
    if (!base.reachable())
      return out;
    if (base.status == QueryStatus::ResourceLimit) {
      out.may_alias_all = true;
      return out;
    }

    std::vector<Id> in_scope;
    for (Id cand : m_cluster_values) {
      if (cand >= m_program.values.size())
        continue;
      const Value &vinfo = m_program.values[cand];
      if (!vinfo.constant && vinfo.function != INVALID && vinfo.function != fn)
        continue;
      if (cand != value && !base.points_to.isTop() &&
          !valueOf(m_program, m_upper, cand).intersects(base.points_to))
        continue;
      in_scope.push_back(cand);
    }

    if (base.points_to.isTop()) {
      out.may_alias_all = true;
      out.aliases = std::move(in_scope);
      return out;
    }

    std::set<Id> alias_set;
    alias_set.insert(value);

    // Algorithm 3 Step 2: forward summary propagation from entry anchors A -> Q.
    if (context_path != nullptr) {
      propagateForwardAnchorsFscs(entry_anchors, *context_path, site, point,
                                  in_scope, alias_set);
    } else {
      propagateForwardAnchorsFsci(entry_anchors, fn, site, point, in_scope,
                                  alias_set);
    }

    // Complete soundly for any candidate reaching concrete objects in
    // `base.points_to` via field offsets or symbolic dereferences not matched
    // syntactically by `entry_anchors`.
    for (Id cand : in_scope) {
      if (alias_set.count(cand))
        continue;
      QueryResult cq = query(cand, site, context_path, point);
      if (cq.status == QueryStatus::ResourceLimit) {
        out.status = QueryStatus::ResourceLimit;
        out.may_alias_all = true;
        return out;
      }
      if (cq.reachable() && cq.points_to.intersects(base.points_to))
        alias_set.insert(cand);
    }

    out.aliases.assign(alias_set.begin(), alias_set.end());
    return out;
  } catch (const LimitReached &) {
    m_failed = true;
    out.status = QueryStatus::ResourceLimit;
    out.may_alias_all = true;
    return out;
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
