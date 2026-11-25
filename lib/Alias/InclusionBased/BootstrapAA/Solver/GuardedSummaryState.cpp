#include "Alias/InclusionBased/BootstrapAA/Solver/GuardedSummarySolver.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <numeric>
#include <utility>

namespace lotus {
namespace bootstrap {
namespace detail {

void GuardedSummarySolver::tick() {
  ++m_stats.steps;
  if (++m_steps > m_options.max_steps)
    throw LimitReached{};
}

void GuardedSummarySolver::countContext(const SummaryKey &key) {
  if (!m_counted_keys.insert(key).second)
    return;
  if (m_context_budget_used >= m_options.max_contexts)
    throw LimitReached{};
  ++m_context_budget_used;
  ++m_stats.contexts;
  ++m_stats.context_cache_misses;
  m_stats.maximum_contexts_per_cluster =
      std::max(m_stats.maximum_contexts_per_cluster, m_context_budget_used);
}

// Clamp byte offset accumulation to guarantee finite Expr state space while
// preserving constant struct-field offsets inside strided array regions.
std::tuple<std::int64_t, bool, std::uint64_t>
GuardedSummarySolver::combineOffset(std::int64_t a, bool a_known,
                                    std::uint64_t a_stride, std::int64_t b,
                                    bool b_known,
                                    std::uint64_t b_stride) const {
  const std::uint64_t stride =
      (a_stride && b_stride) ? std::gcd(a_stride, b_stride)
                             : (b_stride ? b_stride : a_stride);
  if (!a_known && a_stride == 0)
    return {0, false, 0};
  if (!b_known && b_stride == 0)
    return {0, false, 0};
  const bool known = a_known && b_known;
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
    return {0, false, 0};
  const std::int64_t sum = a + b;
  if (sum == 0)
    return {0, known, stride};
  if (m_max_offset_span == 0 || sum < -m_max_offset_span ||
      sum > m_max_offset_span)
    return {0, false, 0};
  return {sum, known, stride};
}

PointsToSet GuardedSummarySolver::applyOffset(const PointsToSet &base,
                                              std::int64_t off, bool known,
                                              std::uint64_t stride) const {
  if (base.empty() || base.isTop())
    return base;
  if (known && off == 0 && stride == 0)
    return base;
  PointsToSet out;
  for (Id obj : base.objects())
    out.join(m_program.resolveField(obj, off, known, stride));
  return out;
}

AtomId GuardedSummarySolver::internAtom(GuardAtom atom) {
  auto it = m_atom_ids.find(atom);
  if (it != m_atom_ids.end())
    return it->second;
  const AtomId id = static_cast<AtomId>(m_atoms.size());
  m_atoms.push_back(atom);
  m_atom_ids.emplace(std::move(atom), id);
  ++m_stats.guard_atoms_created;
  return id;
}

GuardId GuardedSummarySolver::internGuard(std::vector<AtomId> atoms) {
  std::sort(atoms.begin(), atoms.end());
  atoms.erase(std::unique(atoms.begin(), atoms.end()), atoms.end());
  if (atoms.size() > m_options.max_guard_atoms) {
    ++m_stats.guards_widened;
    return GUARD_TRUE;
  }
  auto it = m_guard_ids.find(atoms);
  if (it != m_guard_ids.end())
    return it->second;
  const GuardId id = static_cast<GuardId>(m_guards.size());
  m_guards.push_back(atoms);
  m_guard_ids.emplace(std::move(atoms), id);
  return id;
}

bool GuardedSummarySolver::isInvariantValueInFunc(Id val, Id func) const {
  if (val == INVALID || val >= m_invariant_ssa.size() || !m_invariant_ssa[val])
    return false;
  const Value &vinfo = m_program.values[val];
  return vinfo.constant || vinfo.function == func;
}

bool GuardedSummarySolver::atomsContradict(const GuardAtom &a,
                                           const GuardAtom &b) const {
  if (a.call_suffix != b.call_suffix || a.context_insensitive ||
      b.context_insensitive)
    return false;
  // Only detect contradictions outside loops: a site inside a loop, or a
  // callee site invoked through any call site inside a loop, may execute
  // multiple times with different pointer targets under the same call suffix.
  const InstLocation &loc_a = m_index.by_site[a.site];
  const InstLocation &loc_b = m_index.by_site[b.site];
  if (loc_a.function != loc_b.function)
    return false;
  if (m_index.in_loop_block[loc_a.function][loc_a.block] ||
      m_index.in_loop_block[loc_b.function][loc_b.block])
    return false;
  for (Id call_site : a.call_suffix) {
    const InstLocation &cloc = m_index.by_site[call_site];
    if (m_index.in_loop_block[cloc.function][cloc.block])
      return false;
  }
  const bool same_site = (a.site == b.site && a.site_point == b.site_point);
  const bool cross_site_ok =
      !same_site && a.lhs_offset_known && b.lhs_offset_known &&
      isInvariantValueInFunc(a.lhs, loc_a.function);
  if (!same_site && !cross_site_ok)
    return false;

  if (a.lhs == b.lhs && a.lhs_offset == b.lhs_offset &&
      a.lhs_offset_known == b.lhs_offset_known &&
      a.lhs_stride == b.lhs_stride) {
    if (a.rhs == b.rhs) {
      if ((a.kind == GuardKind::PointsTo &&
           b.kind == GuardKind::NotPointsTo) ||
          (a.kind == GuardKind::NotPointsTo && b.kind == GuardKind::PointsTo)) {
        return a.rhs == NULL_OBJECT ||
               (a.rhs < m_index.exact_address_singleton.size() &&
                m_index.exact_address_singleton[a.rhs]);
      }
      if ((a.kind == GuardKind::Alias && b.kind == GuardKind::NotAlias) ||
          (a.kind == GuardKind::NotAlias && b.kind == GuardKind::Alias)) {
        const GuardAtom &alias_atom =
            (a.kind == GuardKind::Alias) ? a : b;
        const GuardAtom &not_alias_atom =
            (a.kind == GuardKind::NotAlias) ? a : b;
        // Abstract memory overlap (`for_ptr_cmp == false`) does not contradict
        // exact pointer inequality (`for_ptr_cmp == true`) when interior byte
        // pointers can overlap within the same abstract object.
        if (!alias_atom.for_ptr_cmp && not_alias_atom.for_ptr_cmp)
          return false;
        return same_site || isInvariantValueInFunc(a.rhs, loc_a.function);
      }
    } else if (a.kind == GuardKind::PointsTo && b.kind == GuardKind::PointsTo &&
               a.lhs_offset_known && a.lhs_stride == 0) {
      // One fixed runtime pointer value cannot simultaneously point to two
      // distinct abstract objects (e.g. NULL vs non-null, or two distinct
      // concrete allocations / fields).
      return true;
    }
  }
  // Symmetric Alias / NotAlias check (`s ~ u` vs `u !~ s`) when offsets are 0.
  if (a.lhs_offset == 0 && b.lhs_offset == 0 && a.lhs_offset_known &&
      b.lhs_offset_known && a.lhs_stride == 0 && b.lhs_stride == 0 &&
      a.lhs == b.rhs && a.rhs == b.lhs) {
    if ((a.kind == GuardKind::Alias && b.kind == GuardKind::NotAlias) ||
        (a.kind == GuardKind::NotAlias && b.kind == GuardKind::Alias)) {
      const GuardAtom &alias_atom = (a.kind == GuardKind::Alias) ? a : b;
      const GuardAtom &not_alias_atom = (a.kind == GuardKind::NotAlias) ? a : b;
      if (!alias_atom.for_ptr_cmp && not_alias_atom.for_ptr_cmp)
        return false;
      return same_site || (isInvariantValueInFunc(a.lhs, loc_a.function) &&
                           isInvariantValueInFunc(a.rhs, loc_a.function));
    }
  }
  return false;
}

GuardId GuardedSummarySolver::applyEdgePtrGuards(const Edge &edge,
                                                 Id fallback_site,
                                                 Point fallback_point,
                                                 GuardId base,
                                                 std::size_t target_depth) {
  if (!m_options.path_sensitive || edge.ptr_guards.empty() ||
      base == GUARD_FALSE)
    return base;
  GuardId res = base;
  const auto isExactSingletonOrNull = [&](Id obj) -> bool {
    return obj == NULL_OBJECT ||
           (obj < m_index.exact_address_singleton.size() &&
            m_index.exact_address_singleton[obj]);
  };
  auto bestFinalUpper = [&](Id val, Id site, Point pt) -> PointsToSet {
    if (m_program.values[val].constant)
      return m_program.values[val].initial;
    if (val < m_slice.values.size() && m_slice.values[val]) {
      const std::size_t vd =
          dovetailDepthOfComp(m_hierarchy.value_components[val]);
      const bool finalized_depth =
          m_dovetailing_done ||
          (m_has_completed_depth && vd <= m_completed_depth &&
           vd < m_cyclic_pass1_depth);
      if (finalized_depth) {
        auto it = m_fsci_cache.find({val, site, pt});
        if (it != m_fsci_cache.end() && !it->second.empty())
          return it->second;
      }
    }
    return valueOf(m_program, m_upper, val);
  };

  for (const EdgePtrGuard &eg : edge.ptr_guards) {
    if (eg.lhs == INVALID || eg.rhs == INVALID ||
        eg.lhs >= m_program.values.size())
      continue;
    const bool use_eg_site = (eg.site != INVALID);
    const Id atom_site = use_eg_site ? eg.site : fallback_site;
    const Point atom_point = use_eg_site ? Point::After : fallback_point;
    if (atom_site == INVALID)
      continue;

    const bool is_alias_kind =
        (eg.kind == EdgeGuardKind::Alias || eg.kind == EdgeGuardKind::NotAlias);
    if (is_alias_kind && eg.rhs >= m_program.values.size())
      continue;
    if (!is_alias_kind && eg.rhs >= m_program.objects.size())
      continue;

    const PointsToSet upper_lhs = bestFinalUpper(eg.lhs, atom_site, atom_point);

    // O(1) sound simplification against tightest finalized upper bound
    // (finalized FSCI cache when available, otherwise whole-program `m_upper`):
    // immediately refute provably-infeasible edges and drop provably-tautological
    // pointer checks (such as non-null `malloc()` or initialized globals).
    if (!is_alias_kind) {
      if (eg.kind == EdgeGuardKind::PointsTo) {
        if (!upper_lhs.empty() && !upper_lhs.contains(eg.rhs)) {
          ++m_stats.guards_pruned;
          ++m_stats.path_infeasible_prunes;
          return GUARD_FALSE;
        }
        if (!upper_lhs.isTop() && upper_lhs.size() == 1 &&
            upper_lhs.contains(eg.rhs) && isExactSingletonOrNull(eg.rhs))
          continue;
      } else { // NotPointsTo
        if (!upper_lhs.isTop() && upper_lhs.size() == 1 &&
            upper_lhs.contains(eg.rhs) && isExactSingletonOrNull(eg.rhs)) {
          ++m_stats.guards_pruned;
          ++m_stats.path_infeasible_prunes;
          return GUARD_FALSE;
        }
        if (!upper_lhs.empty() && !upper_lhs.contains(eg.rhs))
          continue;
      }
    } else {
      const PointsToSet upper_rhs =
          bestFinalUpper(eg.rhs, atom_site, atom_point);
      const bool disjoint = !upper_lhs.empty() && !upper_rhs.empty() &&
                            !upper_lhs.intersects(upper_rhs);
      const bool must_eq =
          !upper_lhs.isTop() && !upper_rhs.isTop() && upper_lhs.size() == 1 &&
          upper_lhs == upper_rhs && isExactSingletonOrNull(*upper_lhs.begin());
      if (eg.kind == EdgeGuardKind::Alias) {
        if (disjoint) {
          ++m_stats.guards_pruned;
          ++m_stats.path_infeasible_prunes;
          return GUARD_FALSE;
        }
        if (must_eq)
          continue;
      } else { // NotAlias
        if (must_eq) {
          ++m_stats.guards_pruned;
          ++m_stats.path_infeasible_prunes;
          return GUARD_FALSE;
        }
        if (disjoint)
          continue;
      }
    }

    // Never attach an atom referencing out-of-slice pointers: their defining
    // statements are not in `m_slice.instructions`, so flow-sensitive
    // re-evaluation inside this cluster would be unsound (and `bestFinalUpper`
    // above already extracted everything provable from whole-program bounds).
    const bool lhs_in_slice =
        m_program.values[eg.lhs].constant ||
        (eg.lhs < m_slice.values.size() && m_slice.values[eg.lhs]);
    const bool rhs_in_slice =
        !is_alias_kind || m_program.values[eg.rhs].constant ||
        (eg.rhs < m_slice.values.size() && m_slice.values[eg.rhs]);
    if (!lhs_in_slice || !rhs_in_slice)
      continue;

    // Preserve Algorithm 2 top-down dovetail stratification during dovetailing:
    // do not attach an optional edge pointer guard on a higher-depth pointer
    // while computing a lower-depth summary (which would invert top-down order).
    if (!m_dovetailing_done) {
      if (!m_program.values[eg.lhs].constant &&
          dovetailDepthOfComp(m_hierarchy.value_components[eg.lhs]) >
              target_depth)
        continue;
      if (is_alias_kind && !m_program.values[eg.rhs].constant &&
          dovetailDepthOfComp(m_hierarchy.value_components[eg.rhs]) >
              target_depth)
        continue;
    }

    // During Pass 1 of cyclic dovetailing, negative guards at the active cyclic
    // depth cannot be evaluated and only add churn.
    if (m_cyclic_pass1_depth != std::numeric_limits<std::size_t>::max() &&
        (eg.kind == EdgeGuardKind::NotPointsTo ||
         eg.kind == EdgeGuardKind::NotAlias) &&
        (isInActivePass1(eg.lhs) ||
         (is_alias_kind && isInActivePass1(eg.rhs))))
      continue;

    GuardAtom atom;
    atom.for_ptr_cmp = true;
    atom.site = atom_site;
    atom.site_point = atom_point;
    atom.lhs = eg.lhs;
    atom.rhs = eg.rhs;
    switch (eg.kind) {
    case EdgeGuardKind::PointsTo:
      atom.kind = GuardKind::PointsTo;
      break;
    case EdgeGuardKind::NotPointsTo:
      atom.kind = GuardKind::NotPointsTo;
      break;
    case EdgeGuardKind::Alias:
      atom.kind = GuardKind::Alias;
      break;
    case EdgeGuardKind::NotAlias:
      atom.kind = GuardKind::NotAlias;
      break;
    }

    // Always check syntactic contradiction with existing atoms in `res`, but
    // never let an optional edge pointer guard push `res` past `max_guard_atoms`
    // (which would widen and erase core Def. 8 Store/Load guards!).
    const auto &existing = m_guards[res];
    bool contradicted = false;
    for (AtomId eid : existing) {
      if (atomsContradict(m_atoms[eid], atom)) {
        ++m_stats.guards_pruned;
        contradicted = true;
        break;
      }
    }
    if (contradicted) {
      ++m_stats.path_infeasible_prunes;
      return GUARD_FALSE;
    }
    if (existing.size() >= m_options.max_guard_atoms)
      continue;
    res = conjoinAtom(res, atom);
    if (res == GUARD_FALSE) {
      ++m_stats.path_infeasible_prunes;
      return GUARD_FALSE;
    }
  }
  return res;
}

GuardId GuardedSummarySolver::conjoinAtom(GuardId base, const GuardAtom &atom) {
  if (base == GUARD_FALSE)
    return GUARD_FALSE;
  const auto &existing = m_guards[base];
  for (AtomId eid : existing) {
    if (atomsContradict(m_atoms[eid], atom)) {
      ++m_stats.guards_pruned;
      return GUARD_FALSE;
    }
  }
  const AtomId aid = internAtom(atom);
  std::vector<AtomId> next = existing;
  next.push_back(aid);
  return internGuard(std::move(next));
}

GuardId GuardedSummarySolver::conjoinGuards(GuardId a, GuardId b) {
  if (a == GUARD_FALSE || b == GUARD_FALSE)
    return GUARD_FALSE;
  if (a == GUARD_TRUE)
    return b;
  if (b == GUARD_TRUE)
    return a;
  GuardId res = a;
  for (AtomId aid : m_guards[b]) {
    res = conjoinAtom(res, m_atoms[aid]);
    if (res == GUARD_FALSE)
      return GUARD_FALSE;
  }
  return res;
}

GuardId GuardedSummarySolver::prependCallSite(GuardId g, Id call_site) {
  if (g == GUARD_TRUE || g == GUARD_FALSE)
    return g;
  std::vector<AtomId> shifted;
  shifted.reserve(m_guards[g].size());
  for (AtomId aid : m_guards[g]) {
    GuardAtom atom = m_atoms[aid];
    if (!atom.context_insensitive) {
      const bool cyclic_suffix =
          std::find(atom.call_suffix.begin(), atom.call_suffix.end(),
                    call_site) != atom.call_suffix.end();
      if (cyclic_suffix || atom.call_suffix.size() >= 8) {
        atom.context_insensitive = true;
        atom.call_suffix.clear();
      } else {
        atom.call_suffix.insert(atom.call_suffix.begin(), call_site);
      }
    }
    shifted.push_back(internAtom(std::move(atom)));
  }
  return internGuard(std::move(shifted));
}

bool GuardedSummarySolver::guardSubsumes(GuardId weaker,
                                         GuardId stronger) const {
  if (weaker == GUARD_TRUE)
    return true;
  if (stronger == GUARD_TRUE)
    return false;
  const auto &w = m_guards[weaker];
  const auto &s = m_guards[stronger];
  if (w.size() > s.size())
    return false;
  return std::includes(s.begin(), s.end(), w.begin(), w.end());
}

// Compute per-function transitive memory modification sets strictly over
// relevant statements `St_P` (Kahlon PLDI '08 §3 Fig. 5 & Algorithm 5 lines
// 14-17: functions containing no modifying statements of `St_P` are skipped).
void GuardedSummarySolver::computeFunctionModifications() {
  const std::size_t nf = m_program.functions.size();
  m_func_modifies.resize(nf);
  m_func_modifies_any.assign(nf, false);

  for (Id f = 0; f < nf; ++f) {
    const Function &func = m_program.functions[f];
    if (func.external) {
      if (func.writes_memory) {
        m_func_modifies[f] = PointsToSet::top();
        m_func_modifies_any[f] = true;
      }
      continue;
    }
    for (const Block &block : func.blocks) {
      for (const Instruction &inst : block.instructions) {
        if (!m_reachability.reachable_site[inst.id] ||
            !m_slice.instructions[inst.id])
          continue;
        switch (inst.opcode) {
        case Opcode::Store: {
          const PointsToSet addr =
              valueOf(m_program, m_upper, inst.operands[0]);
          if (addr.isTop()) {
            m_func_modifies[f] = PointsToSet::top();
            m_func_modifies_any[f] = true;
          } else {
            for (Id o : addr.objects()) {
              if (!isMemory(m_program.objects[o]))
                continue;
              m_func_modifies[f].insert(o);
              m_func_modifies_any[f] = true;
              if (inst.write_bytes != 0) {
                for (Id fo : m_program.overlappingFields(o, inst.write_bytes))
                  if (fo != o && isMemory(m_program.objects[fo]))
                    m_func_modifies[f].insert(fo);
              }
            }
          }
          break;
        }
        case Opcode::Allocate: {
          const Id base = m_program.baseOf(inst.object);
          if (base < m_program.fields_of_base.size() &&
              !m_program.fields_of_base[base].empty()) {
            for (auto [off, fo] : m_program.fields_of_base[base]) {
              if (isMemory(m_program.objects[fo])) {
                m_func_modifies[f].insert(fo);
                m_func_modifies_any[f] = true;
              }
            }
          } else if (isMemory(m_program.objects[inst.object])) {
            m_func_modifies[f].insert(inst.object);
            m_func_modifies_any[f] = true;
          }
          break;
        }
        case Opcode::MemCopy: {
          const PointsToSet dst = valueOf(m_program, m_upper, inst.operands[0]);
          if (dst.isTop()) {
            m_func_modifies[f] = PointsToSet::top();
            m_func_modifies_any[f] = true;
          } else {
            for (Id o : dst.objects())
              for (Id fo : m_program.overlappingFields(o, inst.write_bytes))
                if (isMemory(m_program.objects[fo])) {
                  m_func_modifies[f].insert(fo);
                  m_func_modifies_any[f] = true;
                }
          }
          break;
        }
        case Opcode::Havoc: {
          if (inst.operands.empty()) {
            m_func_modifies[f] = PointsToSet::top();
            m_func_modifies_any[f] = true;
          } else {
            const PointsToSet addr =
                valueOf(m_program, m_upper, inst.operands[0]);
            if (addr.isTop()) {
              m_func_modifies[f] = PointsToSet::top();
              m_func_modifies_any[f] = true;
            } else {
              for (Id o : addr.objects()) {
                if (!isMemory(m_program.objects[o]))
                  continue;
                for (Id fo : m_program.overlappingFields(o, inst.write_bytes))
                  if (isMemory(m_program.objects[fo])) {
                    m_func_modifies[f].insert(fo);
                    m_func_modifies_any[f] = true;
                  }
              }
            }
          }
          break;
        }
        case Opcode::Call: {
          const Targets t = callees(
              m_program, inst,
              valueOf(m_program, m_upper, inst.indirect_target));
          if (t.opaque && inst.writes_memory) {
            m_func_modifies[f] = PointsToSet::top();
            m_func_modifies_any[f] = true;
          }
          for (Id callee : t.functions) {
            if (m_program.functions[callee].external && inst.writes_memory &&
                m_program.functions[callee].writes_memory) {
              m_func_modifies[f] = PointsToSet::top();
              m_func_modifies_any[f] = true;
            }
          }
          break;
        }
        default:
          break;
        }
      }
    }
  }

  // Propagate transitively in reverse topological SCC order.
  for (Id scc : m_call_graph.reverse_topo_sccs) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (Id f : m_call_graph.scc_members[scc]) {
        for (Id callee : m_call_graph.successors[f]) {
          if (m_program.functions[callee].external)
            continue;
          if (m_func_modifies[f].join(m_func_modifies[callee])) {
            m_func_modifies_any[f] = true;
            changed = true;
          }
        }
      }
    }
  }
}

bool GuardedSummarySolver::canFunctionModifyObj(Id f, Id obj) const {
  return m_func_modifies_any[f] && m_func_modifies[f].contains(obj);
}

bool GuardedSummarySolver::canFunctionModifyAny(Id f,
                                                const PointsToSet &objs) const {
  return m_func_modifies_any[f] && m_func_modifies[f].intersects(objs);
}

void GuardedSummarySolver::computeFunctionConditionKills() {
  const std::size_t nf = m_program.functions.size();
  m_func_kills_conditions.assign(nf, {});
  if (!m_options.path_sensitive)
    return;

  std::vector<std::set<Id>> kills(nf);
  for (Id f = 0; f < nf; ++f) {
    const Function &func = m_program.functions[f];
    if (func.external)
      continue;
    for (Id cid : func.entry_kills_conditions)
      if (cid != INVALID)
        kills[f].insert(cid);
    for (const Block &block : func.blocks) {
      for (const Instruction &inst : block.instructions) {
        if (!m_reachability.reachable_site[inst.id])
          continue;
        if (inst.defines_condition != INVALID)
          kills[f].insert(inst.defines_condition);
        for (Id cid : inst.kills_conditions)
          if (cid != INVALID)
            kills[f].insert(cid);
        if (inst.opcode == Opcode::Call && inst.writes_memory &&
            !m_program.global_conditions.empty()) {
          const Targets t = callees(
              m_program, inst,
              valueOf(m_program, m_upper, inst.indirect_target));
          bool may_callback = t.opaque;
          for (Id callee : t.functions)
            if (m_program.functions[callee].external &&
                m_program.functions[callee].writes_memory)
              may_callback = true;
          if (may_callback)
            for (Id cid : m_program.global_conditions)
              if (cid != INVALID)
                kills[f].insert(cid);
        }
      }
    }
  }
  for (Id scc : m_call_graph.reverse_topo_sccs) {
    bool changed = true;
    while (changed) {
      changed = false;
      for (Id f : m_call_graph.scc_members[scc]) {
        for (Id callee : m_call_graph.successors[f]) {
          if (m_program.functions[callee].external)
            continue;
          for (Id cid : kills[callee])
            if (kills[f].insert(cid).second)
              changed = true;
        }
      }
    }
  }
  for (Id f = 0; f < nf; ++f)
    m_func_kills_conditions[f].assign(kills[f].begin(), kills[f].end());
}

void GuardedSummarySolver::computeInvariantSSAVals() {
  const std::size_t nv = m_program.values.size();
  m_invariant_ssa.assign(nv, false);
  for (Id v = 0; v < nv; ++v) {
    const Value &vinfo = m_program.values[v];
    if (vinfo.constant) {
      m_invariant_ssa[v] = true;
      continue;
    }
    if (vinfo.function == INVALID || !m_index.phi_defs[v].empty())
      continue;
    const auto &defs = m_index.defining_sites[v];
    if (defs.empty()) {
      const auto &params = m_program.functions[vinfo.function].parameters;
      if (std::find(params.begin(), params.end(), v) != params.end())
        m_invariant_ssa[v] = true;
    } else if (defs.size() == 1) {
      const InstLocation &dloc = m_index.by_site[defs[0]];
      if (dloc.function == vinfo.function &&
          !m_index.in_loop_block[dloc.function][dloc.block])
        m_invariant_ssa[v] = true;
    }
  }
}

// Compute slice-local dovetail dependency depths and cycles, combining
// Steensgaard's points-to hierarchy edges with `Opcode::Field` dependencies
// inside `m_slice` while keeping Steensgaard partitions strictly field-sensitive.
void GuardedSummarySolver::computeDovetailHierarchy() {
  const std::size_t nc = m_hierarchy.successors.size();
  std::vector<std::vector<Id>> succ = m_hierarchy.successors;
  std::vector<std::vector<Id>> pred = m_hierarchy.predecessors;
  std::vector<bool> self_loop = m_hierarchy.cyclic;

  for (Id site : m_slice.inst_list) {
    const Instruction &inst = *m_index.by_site[site].inst;
    if (inst.opcode != Opcode::Field || inst.result == INVALID ||
        inst.operands.empty() || inst.operands[0] == INVALID)
      continue;
    const Id from = m_hierarchy.value_components[inst.operands[0]];
    const Id to = m_hierarchy.value_components[inst.result];
    if (from >= nc || to >= nc)
      continue;
    if (from == to) {
      continue;
    }
    if (std::find(succ[from].begin(), succ[from].end(), to) ==
        succ[from].end()) {
      succ[from].push_back(to);
      pred[to].push_back(from);
    }
  }

  std::vector<Id> comp_scc;
  Id scc_count = 0;
  computeKosarajuSCCs(nc, succ, pred, comp_scc, scc_count);

  std::vector<std::size_t> scc_size(scc_count, 0);
  std::vector<bool> scc_cyclic(scc_count, false);
  std::vector<std::vector<Id>> scc_succ(scc_count);
  std::vector<std::vector<Id>> scc_pred(scc_count);
  for (Id c = 0; c < nc; ++c) {
    const Id from = comp_scc[c];
    ++scc_size[from];
    if (self_loop[c])
      scc_cyclic[from] = true;
    for (Id target : succ[c]) {
      const Id to = comp_scc[target];
      if (from == to) {
        scc_cyclic[from] = true;
      } else if (std::find(scc_succ[from].begin(), scc_succ[from].end(), to) ==
                 scc_succ[from].end()) {
        scc_succ[from].push_back(to);
        scc_pred[to].push_back(from);
      }
    }
  }
  for (Id s = 0; s < scc_count; ++s)
    if (scc_size[s] > 1)
      scc_cyclic[s] = true;

  std::vector<std::size_t> indegree(scc_count, 0);
  std::vector<std::size_t> scc_depth(scc_count, 0);
  std::deque<Id> work;
  for (Id s = 0; s < scc_count; ++s) {
    indegree[s] = scc_pred[s].size();
    if (indegree[s] == 0)
      work.push_back(s);
  }
  while (!work.empty()) {
    const Id s = work.front();
    work.pop_front();
    for (Id next : scc_succ[s]) {
      scc_depth[next] = std::max(scc_depth[next], scc_depth[s] + 1);
      if (--indegree[next] == 0)
        work.push_back(next);
    }
  }

  m_dovetail_depth.resize(nc, 0);
  m_dovetail_cyclic.resize(nc, false);
  for (Id c = 0; c < nc; ++c) {
    m_dovetail_depth[c] = scc_depth[comp_scc[c]];
    m_dovetail_cyclic[c] = scc_cyclic[comp_scc[c]];
  }
}

std::size_t GuardedSummarySolver::dovetailDepthOfComp(Id comp) const {
  return comp < m_dovetail_depth.size() ? m_dovetail_depth[comp] : 0;
}

bool GuardedSummarySolver::dovetailCyclicOfComp(Id comp) const {
  return comp < m_dovetail_cyclic.size() ? m_dovetail_cyclic[comp] : false;
}

bool GuardedSummarySolver::isInActivePass1(Id val) const {
  if (m_cyclic_pass1_depth == std::numeric_limits<std::size_t>::max() ||
      val == INVALID || m_program.values[val].constant)
    return false;
  return dovetailDepthOfComp(m_hierarchy.value_components[val]) >=
         m_cyclic_pass1_depth;
}

BddManager::NodeId GuardedSummarySolver::evalCondNode(Id cond_id,
                                                      bool polarity) {
  if (!m_options.path_sensitive || cond_id == INVALID)
    return BddManager::TRUE_NODE;
  if (cond_id >= m_program.cond_nodes.size())
    return m_bdd.literal(cond_id, polarity);
  if (m_cond_bdd_cache.size() < m_program.cond_nodes.size())
    m_cond_bdd_cache.assign(m_program.cond_nodes.size(),
                            std::numeric_limits<BddManager::NodeId>::max());
  BddManager::NodeId pos = m_cond_bdd_cache[cond_id];
  if (pos == std::numeric_limits<BddManager::NodeId>::max()) {
    const CondNode &node = m_program.cond_nodes[cond_id];
    switch (node.op) {
    case CondOp::Atom:
      pos = m_bdd.literal(node.lhs, true);
      break;
    case CondOp::Not:
      pos = m_bdd.makeNot(evalCondNode(node.lhs, true));
      break;
    case CondOp::And:
      pos = m_bdd.makeAnd(evalCondNode(node.lhs, true),
                          evalCondNode(node.rhs, true));
      break;
    case CondOp::Or:
      pos = m_bdd.makeOr(evalCondNode(node.lhs, true),
                         evalCondNode(node.rhs, true));
      break;
    }
    m_cond_bdd_cache[cond_id] = pos;
  }
  return polarity ? pos : m_bdd.makeNot(pos);
}

BddManager::NodeId
GuardedSummarySolver::applyCallCondSubst(Id callee,
                                         const Instruction &call_inst,
                                         BddManager::NodeId path) {
  if (!m_options.path_sensitive || m_bdd.isTrue(path) || m_bdd.isFalse(path))
    return path;
  if (!m_options.path_sensitive_interprocedural)
    return quantifyFuncEntryConditions(callee, path);

  const auto &entry_kills = m_program.functions[callee].entry_kills_conditions;
  if (call_inst.cond_subst.empty())
    return quantifyFuncEntryConditions(callee, path);

  const Id caller_fn = m_index.by_site[call_inst.id].function;
  const bool self_recursive = (caller_fn == callee);

  // Guard against variable collisions during sequential substitution if any
  // caller expression references a callee condition ID (e.g., swapping params).
  std::set<Id> callee_vars;
  for (const auto &sub : call_inst.cond_subst)
    if (sub.callee_cond != INVALID)
      callee_vars.insert(sub.callee_cond);
  auto condRefsAny = [&](Id cid, auto &&self_ref) -> bool {
    if (cid == INVALID)
      return false;
    if (callee_vars.count(cid))
      return true;
    if (cid >= m_program.cond_nodes.size())
      return false;
    const CondNode &n = m_program.cond_nodes[cid];
    if (n.op == CondOp::Atom)
      return callee_vars.count(n.lhs) != 0;
    if (n.op == CondOp::Not)
      return self_ref(n.lhs, self_ref);
    return self_ref(n.lhs, self_ref) || self_ref(n.rhs, self_ref);
  };
  bool any_collision = self_recursive;
  if (!any_collision) {
    for (const auto &sub : call_inst.cond_subst)
      if (sub.caller_cond != INVALID && condRefsAny(sub.caller_cond, condRefsAny)) {
        any_collision = true;
        break;
      }
  }

  std::vector<Id> substituted;
  substituted.reserve(call_inst.cond_subst.size());
  for (const auto &sub : call_inst.cond_subst) {
    if (sub.callee_cond == INVALID)
      continue;
    if (any_collision && sub.caller_cond != INVALID) {
      // Avoid sequential variable collision across recursive/swapping frames.
      path = m_bdd.exists(path, sub.callee_cond);
      substituted.push_back(sub.callee_cond);
      continue;
    }
    const BddManager::NodeId rep =
        (sub.caller_cond == INVALID)
            ? (sub.polarity ? BddManager::TRUE_NODE : BddManager::FALSE_NODE)
            : evalCondNode(sub.caller_cond, sub.polarity);
    path = m_bdd.substitute(path, sub.callee_cond, rep);
    substituted.push_back(sub.callee_cond);
    if (m_bdd.isFalse(path))
      return BddManager::FALSE_NODE;
  }
  for (Id cid : entry_kills) {
    if (std::find(substituted.begin(), substituted.end(), cid) ==
        substituted.end()) {
      path = m_bdd.exists(path, cid);
      if (m_bdd.isTrue(path) || m_bdd.isFalse(path))
        break;
    }
  }
  return path;
}

void GuardedSummarySolver::addKilledCond(KilledConds &dst, Id cid) const {
  if (!m_options.path_sensitive_interprocedural || dst.all || cid == INVALID)
    return;
  auto it = std::lower_bound(dst.vars.begin(), dst.vars.end(), cid);
  if (it != dst.vars.end() && *it == cid)
    return;
  dst.vars.insert(it, cid);
  if (dst.vars.size() > std::max<std::size_t>(m_options.max_guard_atoms, 16)) {
    dst.all = true;
    dst.vars.clear();
  }
}

void GuardedSummarySolver::addKilledConds(KilledConds &dst,
                                          const std::vector<Id> &cids) const {
  if (!m_options.path_sensitive_interprocedural || dst.all || cids.empty())
    return;
  for (Id cid : cids) {
    addKilledCond(dst, cid);
    if (dst.all)
      return;
  }
}

void GuardedSummarySolver::mergeKilledConds(KilledConds &dst,
                                            const KilledConds &src) const {
  if (!m_options.path_sensitive_interprocedural || dst.all || src.empty())
    return;
  if (src.all) {
    dst.all = true;
    dst.vars.clear();
    return;
  }
  addKilledConds(dst, src.vars);
}

bool GuardedSummarySolver::killedSubsumes(const KilledConds &weaker,
                                          const KilledConds &stronger) const {
  if (weaker.all)
    return true;
  if (stronger.all)
    return false;
  if (weaker.vars.size() < stronger.vars.size())
    return false;
  return std::includes(weaker.vars.begin(), weaker.vars.end(),
                       stronger.vars.begin(), stronger.vars.end());
}

BddManager::NodeId
GuardedSummarySolver::applyKilledToPath(BddManager::NodeId path,
                                        const KilledConds &killed) {
  if (!m_options.path_sensitive || m_bdd.isTrue(path) || m_bdd.isFalse(path) ||
      killed.empty())
    return path;
  if (killed.all)
    return BddManager::TRUE_NODE;
  return m_bdd.existsAll(path, killed.vars);
}

BddManager::NodeId
GuardedSummarySolver::quantifyInstConditions(const Instruction &inst,
                                             BddManager::NodeId path) {
  if (!m_options.path_sensitive || m_bdd.isTrue(path) || m_bdd.isFalse(path))
    return path;
  if (inst.defines_condition != INVALID)
    path = m_bdd.exists(path, inst.defines_condition);
  if (!inst.kills_conditions.empty())
    path = m_bdd.existsAll(path, inst.kills_conditions);
  return path;
}

BddManager::NodeId
GuardedSummarySolver::quantifyFuncConditions(Id func, BddManager::NodeId path) {
  if (!m_options.path_sensitive || m_bdd.isTrue(path) || m_bdd.isFalse(path))
    return path;
  if (func < m_func_kills_conditions.size() &&
      !m_func_kills_conditions[func].empty())
    path = m_bdd.existsAll(path, m_func_kills_conditions[func]);
  return path;
}

BddManager::NodeId
GuardedSummarySolver::quantifyFuncEntryConditions(Id func,
                                                  BddManager::NodeId path) {
  if (!m_options.path_sensitive || m_bdd.isTrue(path) || m_bdd.isFalse(path))
    return path;
  if (func < m_program.functions.size() &&
      !m_program.functions[func].entry_kills_conditions.empty())
    path =
        m_bdd.existsAll(path, m_program.functions[func].entry_kills_conditions);
  return path;
}

BddManager::NodeId
GuardedSummarySolver::quantifyGlobalConditions(BddManager::NodeId path) {
  if (!m_options.path_sensitive || m_bdd.isTrue(path) || m_bdd.isFalse(path) ||
      m_program.global_conditions.empty())
    return path;
  return m_bdd.existsAll(path, m_program.global_conditions);
}

PointsToSet GuardedSummarySolver::lookupPass1Fsci(Id val, Id site, Point pt) {
  if (val == INVALID)
    return PointsToSet::top();
  if (m_program.values[val].constant)
    return m_program.values[val].initial;
  const std::size_t vdepth =
      dovetailDepthOfComp(m_hierarchy.value_components[val]);
  if (vdepth > m_cyclic_pass1_depth)
    return valueOf(m_program, m_upper, val);
  FsciKey k{val, site, pt};
  if (m_pass1_keys.count(k))
    return m_fsci_cache[k];
  if (site < m_reachability.reachable_site.size() &&
      m_reachability.reachable_site[site] &&
      m_pass1_extra_added < MAX_PASS1_EXTRA_KEYS) {
    m_pass1_keys.insert(k);
    m_pass1_pending_keys.push_back(k);
    m_fsci_cache[k];
    ++m_pass1_extra_added;
    return PointsToSet{};
  }
  return valueOf(m_program, m_upper, val);
}

PointsToSet GuardedSummarySolver::fsciForSummary(Id val, Id site,
                                                 std::size_t target_depth) {
  if (val == INVALID)
    return PointsToSet::top();
  if (m_program.values[val].constant)
    return m_program.values[val].initial;
  const Id comp = m_hierarchy.value_components[val];
  const std::size_t vdepth = dovetailDepthOfComp(comp);
  if (m_cyclic_pass1_depth != std::numeric_limits<std::size_t>::max() &&
      vdepth >= m_cyclic_pass1_depth) {
    return lookupPass1Fsci(val, site, Point::Before);
  }
  if (m_has_completed_depth && vdepth <= m_completed_depth &&
      (vdepth < target_depth ||
       m_cyclic_pass1_depth == std::numeric_limits<std::size_t>::max())) {
    FsciKey fkey{val, site, Point::Before};
    auto it = m_fsci_cache.find(fkey);
    if (it != m_fsci_cache.end()) {
      if (it->second.empty() && m_path_pruned_fsci.count(fkey))
        ++m_stats.path_infeasible_prunes;
      return it->second;
    }
    if (m_cyclic_pass1_depth == std::numeric_limits<std::size_t>::max()) {
      if (!m_active_fsci.count(fkey) && m_active_fsci.size() < 8) {
        const bool prev_hit = m_hit_active_fsci_cycle;
        m_hit_active_fsci_cycle = false;
        bool pruned = false;
        PointsToSet pts =
            computeFsci(val, site, Point::Before, nullptr, &pruned);
        const bool this_hit = m_hit_active_fsci_cycle;
        m_hit_active_fsci_cycle = prev_hit || this_hit;
        if (!this_hit && !m_in_cyclic_pass2) {
          if (pts.empty() && pruned)
            m_path_pruned_fsci.insert(fkey);
          m_fsci_cache.emplace(fkey, pts);
        }
        return pts;
      }
      m_hit_active_fsci_cycle = true;
    }
  }
  return valueOf(m_program, m_upper, val);
}

bool GuardedSummarySolver::isSameOrHigherDepth(Id val,
                                               std::size_t target_depth) const {
  if (val == INVALID || m_program.values[val].constant)
    return false;
  const Id comp = m_hierarchy.value_components[val];
  return dovetailDepthOfComp(comp) >= target_depth;
}

PointsToSet GuardedSummarySolver::evalGuardPointer(Id val, Id site,
                                                   const Context *ctx,
                                                   bool for_negative_guard,
                                                   bool &is_exact_upper,
                                                   Point site_point) {
  is_exact_upper = true;
  if (val == INVALID)
    return PointsToSet::top();
  if (m_program.values[val].constant)
    return m_program.values[val].initial;

  if (val >= m_slice.values.size() || !m_slice.values[val])
    return valueOf(m_program, m_upper, val);

  const Id comp = m_hierarchy.value_components[val];
  const std::size_t vdepth = dovetailDepthOfComp(comp);

  if (m_cyclic_pass1_depth != std::numeric_limits<std::size_t>::max() &&
      vdepth >= m_cyclic_pass1_depth) {
    if (for_negative_guard) {
      is_exact_upper = false;
      return PointsToSet::top();
    }
    return lookupPass1Fsci(val, site, site_point);
  }

  if (ctx != nullptr) {
    FscsKey key{val, site, site_point, *ctx};
    auto cached = m_fscs_cache.find(key);
    if (cached != m_fscs_cache.end())
      return cached->second.reachable() ? cached->second.points_to
                                        : PointsToSet{};
    if (!m_active_fscs.count(key) && m_active_fscs.size() < 16) {
      QueryResult r = computeFscs(val, site, *ctx, site_point);
      if (r.status == QueryStatus::ResourceLimit)
        throw LimitReached{};
      return r.reachable() ? r.points_to : PointsToSet{};
    }
  }

  FsciKey fkey{val, site, site_point};
  auto it = m_fsci_cache.find(fkey);
  if (it != m_fsci_cache.end()) {
    ++m_stats.fsci_cache_hits;
    return it->second;
  }
  if (m_active_fsci.count(fkey) || m_active_fsci.size() >= 8) {
    m_hit_active_fsci_cycle = true;
    auto p1 = m_cyclic_pass1_fsci.find(fkey);
    if (p1 != m_cyclic_pass1_fsci.end())
      return p1->second;
    return valueOf(m_program, m_upper, val);
  }
  const bool prev_hit = m_hit_active_fsci_cycle;
  m_hit_active_fsci_cycle = false;
  bool pruned = false;
  PointsToSet pts = computeFsci(val, site, site_point, nullptr, &pruned);
  const bool this_hit = m_hit_active_fsci_cycle;
  m_hit_active_fsci_cycle = prev_hit || this_hit;
  const bool can_cache =
      !this_hit && !m_in_cyclic_pass2 &&
      (m_dovetailing_done ||
       (m_has_completed_depth && vdepth <= m_completed_depth &&
        vdepth < m_cyclic_pass1_depth));
  if (can_cache) {
    if (pts.empty() && pruned)
      m_path_pruned_fsci.insert(fkey);
    m_fsci_cache.emplace(fkey, pts);
  }
  return pts;
}

bool GuardedSummarySolver::memoryIntersects(const PointsToSet &a,
                                            const PointsToSet &b) const {
  if (a.empty() || b.empty())
    return false;
  if (a.isTop()) {
    if (b.isTop())
      return true;
    for (Id o : b.objects())
      if (o != NULL_OBJECT)
        return true;
    return false;
  }
  if (b.isTop()) {
    for (Id o : a.objects())
      if (o != NULL_OBJECT)
        return true;
    return false;
  }
  for (Id o : a.objects())
    if (o != NULL_OBJECT && b.contains(o))
      return true;
  return false;
}

bool GuardedSummarySolver::isAtomSatisfiable(const GuardAtom &atom,
                                             const Context *ctx) {
  Context full_ctx;
  const Context *eval_ctx = nullptr;
  if (ctx != nullptr && !atom.context_insensitive) {
    full_ctx = *ctx;
    full_ctx.insert(full_ctx.end(), atom.call_suffix.begin(),
                    atom.call_suffix.end());
    eval_ctx = &full_ctx;
  }

  const bool is_neg = (atom.kind == GuardKind::NotPointsTo ||
                       atom.kind == GuardKind::NotAlias);
  bool exact = true;
  PointsToSet lhs_pts = evalGuardPointer(atom.lhs, atom.site, eval_ctx, is_neg,
                                         exact, atom.site_point);
  lhs_pts = applyOffset(lhs_pts, atom.lhs_offset, atom.lhs_offset_known,
                        atom.lhs_stride);

  switch (atom.kind) {
  case GuardKind::PointsTo:
    return lhs_pts.contains(atom.rhs);
  case GuardKind::NotPointsTo: {
    if (!exact)
      return true;
    const bool singleton_ok =
        atom.rhs == NULL_OBJECT ||
        (atom.rhs < m_index.exact_address_singleton.size() &&
         m_index.exact_address_singleton[atom.rhs]);
    const bool must_be_obj = !lhs_pts.isTop() && lhs_pts.size() == 1 &&
                             lhs_pts.contains(atom.rhs) && singleton_ok;
    return !must_be_obj;
  }
  case GuardKind::Alias: {
    bool exact_r = true;
    const PointsToSet rhs_pts = evalGuardPointer(
        atom.rhs, atom.site, eval_ctx, false, exact_r, atom.site_point);
    return atom.for_ptr_cmp ? lhs_pts.intersects(rhs_pts)
                            : memoryIntersects(lhs_pts, rhs_pts);
  }
  case GuardKind::NotAlias: {
    if (!exact)
      return true;
    bool exact_r = true;
    const PointsToSet rhs_pts = evalGuardPointer(
        atom.rhs, atom.site, eval_ctx, true, exact_r, atom.site_point);
    if (!exact_r)
      return true;
    const bool must_alias_singleton =
        !lhs_pts.isTop() && !rhs_pts.isTop() && lhs_pts.size() == 1 &&
        lhs_pts == rhs_pts &&
        ((*lhs_pts.begin() == NULL_OBJECT && atom.for_ptr_cmp) ||
         (*lhs_pts.begin() < m_index.exact_address_singleton.size() &&
          m_index.exact_address_singleton[*lhs_pts.begin()]));
    return !must_alias_singleton;
  }
  }
  return true;
}

bool GuardedSummarySolver::isGuardSatisfiable(GuardId g, const Context *ctx) {
  if (g == GUARD_TRUE)
    return true;
  if (g == GUARD_FALSE)
    return false;
  for (AtomId aid : m_guards[g]) {
    if (!isAtomSatisfiable(m_atoms[aid], ctx)) {
      ++m_stats.guards_pruned;
      return false;
    }
  }
  return true;
}

void GuardedSummarySolver::emitPointsToSources(
    const PointsToSet &pts, std::int64_t val_off, bool val_known,
    std::uint64_t val_stride, GuardId guard, BddManager::NodeId path,
    std::vector<SummaryEntry> &out) const {
  const PointsToSet shifted = applyOffset(pts, val_off, val_known, val_stride);
  if (shifted.isTop()) {
    out.push_back({Expr::makeTop(), guard, path});
    return;
  }
  for (Id o : shifted.objects()) {
    if (o == UNKNOWN)
      out.push_back({Expr::makeTop(), guard, path});
    else if (o == NULL_OBJECT)
      out.push_back({Expr::makeNull(), guard, path});
    else
      out.push_back({Expr::makeAddr(o), guard, path});
  }
}

void GuardedSummarySolver::substituteCalleeExpr(
    Id callee, const Instruction &call_inst, const Expr &callee_expr,
    std::int64_t caller_off, bool caller_known, std::uint64_t caller_stride,
    GuardId guard, BddManager::NodeId path,
    std::vector<SummaryEntry> &out) const {
  const auto &params = m_program.functions[callee].parameters;
  auto findParam = [&](Id val) -> std::size_t {
    for (std::size_t a = 0; a < params.size(); ++a)
      if (params[a] == val)
        return a;
    return params.size();
  };

  switch (callee_expr.kind) {
  case ExprKind::Top:
    out.push_back({Expr::makeTop(), guard, path});
    return;
  case ExprKind::Null:
    out.push_back({Expr::makeNull(), guard, path});
    return;
  case ExprKind::Addr: {
    PointsToSet pts = m_program.resolveField(callee_expr.id, caller_off,
                                             caller_known, caller_stride);
    emitPointsToSources(pts, 0, true, 0, guard, path, out);
    return;
  }
  case ExprKind::Mem: {
    auto [off, kn, st] = combineOffset(
        callee_expr.val_offset, callee_expr.val_offset_known,
        callee_expr.val_stride, caller_off, caller_known, caller_stride);
    out.push_back({Expr::makeMem(callee_expr.id, off, kn, st), guard, path});
    return;
  }
  case ExprKind::Val: {
    auto [off, kn, st] = combineOffset(
        callee_expr.val_offset, callee_expr.val_offset_known,
        callee_expr.val_stride, caller_off, caller_known, caller_stride);
    if (m_program.values[callee_expr.id].constant) {
      emitPointsToSources(m_program.values[callee_expr.id].initial, off, kn, st,
                          guard, path, out);
      return;
    }
    const std::size_t idx = findParam(callee_expr.id);
    if (idx == params.size())
      return;
    const Id actual =
        idx < call_inst.operands.size() ? call_inst.operands[idx] : INVALID;
    if (actual == INVALID) {
      out.push_back({Expr::makeTop(), guard, path});
    } else if (m_program.values[actual].constant) {
      emitPointsToSources(m_program.values[actual].initial, off, kn, st, guard,
                          path, out);
    } else {
      out.push_back({Expr::makeVal(actual, off, kn, st), guard, path});
    }
    return;
  }
  case ExprKind::Deref: {
    auto [voff, vkn, vst] = combineOffset(
        callee_expr.val_offset, callee_expr.val_offset_known,
        callee_expr.val_stride, caller_off, caller_known, caller_stride);
    if (m_program.values[callee_expr.id].constant) {
      const PointsToSet addr = applyOffset(
          m_program.values[callee_expr.id].initial, callee_expr.deref_offset,
          callee_expr.deref_offset_known, callee_expr.deref_stride);
      if (addr.isTop()) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      for (Id obj : addr.objects()) {
        if (obj == NULL_OBJECT)
          continue;
        const Object &info = m_program.objects[obj];
        if (!isMemory(info) ||
            (callee_expr.read_bytes != 0 &&
             (info.bytes == 0 || info.bytes != callee_expr.read_bytes)))
          out.push_back({Expr::makeTop(), guard, path});
        else
          out.push_back({Expr::makeMem(obj, voff, vkn, vst), guard, path});
      }
      return;
    }
    const std::size_t idx = findParam(callee_expr.id);
    if (idx == params.size())
      return;
    const Id actual =
        idx < call_inst.operands.size() ? call_inst.operands[idx] : INVALID;
    if (actual == INVALID) {
      out.push_back({Expr::makeTop(), guard, path});
    } else if (m_program.values[actual].constant) {
      const PointsToSet addr = applyOffset(
          m_program.values[actual].initial, callee_expr.deref_offset,
          callee_expr.deref_offset_known, callee_expr.deref_stride);
      if (addr.isTop()) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      for (Id obj : addr.objects()) {
        if (obj == NULL_OBJECT)
          continue;
        const Object &info = m_program.objects[obj];
        if (!isMemory(info) ||
            (callee_expr.read_bytes != 0 &&
             (info.bytes == 0 || info.bytes != callee_expr.read_bytes)))
          out.push_back({Expr::makeTop(), guard, path});
        else
          out.push_back({Expr::makeMem(obj, voff, vkn, vst), guard, path});
      }
    } else {
      out.push_back({Expr::makeDeref(actual, callee_expr.deref_offset,
                                     callee_expr.deref_offset_known,
                                     callee_expr.deref_stride, voff, vkn, vst,
                                     callee_expr.read_bytes),
                     guard, path});
    }
    return;
  }
  }
}

void GuardedSummarySolver::evaluateAtEntry(const Expr &q,
                                           PointsToSet &out) const {
  switch (q.kind) {
  case ExprKind::Top:
    out = PointsToSet::top();
    return;
  case ExprKind::Null:
    out.insert(NULL_OBJECT);
    return;
  case ExprKind::Addr:
    out.insert(q.id);
    return;
  case ExprKind::Val: {
    if (m_program.values[q.id].constant) {
      out.join(applyOffset(m_program.values[q.id].initial, q.val_offset,
                           q.val_offset_known, q.val_stride));
      return;
    }
    const auto &params = m_program.functions[m_program.entry].parameters;
    for (Id param : params)
      if (param == q.id) {
        out = PointsToSet::top();
        return;
      }
    return;
  }
  case ExprKind::Mem: {
    const Object &info = m_program.objects[q.id];
    if (info.kind == ObjectKind::Global)
      out.join(applyOffset(info.initial, q.val_offset, q.val_offset_known,
                           q.val_stride));
    return;
  }
  case ExprKind::Deref: {
    PointsToSet base;
    evaluateAtEntry(Expr::makeVal(q.id, q.deref_offset, q.deref_offset_known,
                                  q.deref_stride),
                    base);
    if (base.isTop()) {
      out = PointsToSet::top();
      return;
    }
    for (Id obj : base.objects()) {
      if (obj == NULL_OBJECT)
        continue;
      const Object &info = m_program.objects[obj];
      if (!isMemory(info) ||
          (q.read_bytes != 0 && (info.bytes == 0 || info.bytes != q.read_bytes)))
        out = PointsToSet::top();
      else if (info.kind == ObjectKind::Global)
        out.join(applyOffset(info.initial, q.val_offset, q.val_offset_known,
                             q.val_stride));
    }
    return;
  }
  }
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
