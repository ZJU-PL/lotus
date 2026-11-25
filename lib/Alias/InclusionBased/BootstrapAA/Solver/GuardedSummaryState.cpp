#include "Alias/InclusionBased/BootstrapAA/Solver/GuardedSummarySolver.h"

#include <algorithm>
#include <limits>
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
  const std::uint64_t stride = b_stride ? b_stride : a_stride;
  if (!a_known && a_stride == 0)
    return {0, false, 0};
  if (!b_known && b_stride == 0)
    return {0, false, 0};
  const bool known = a_known && b_known;
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

bool GuardedSummarySolver::atomsContradict(const GuardAtom &a,
                                           const GuardAtom &b) const {
  if (a.site != b.site || a.call_suffix != b.call_suffix ||
      a.context_insensitive || b.context_insensitive)
    return false;
  // Only detect contradictions outside loops (a loop site may execute
  // multiple times with different pointer targets).
  const InstLocation &loc = m_index.by_site[a.site];
  if (m_index.in_loop_block[loc.function][loc.block])
    return false;
  if (a.lhs == b.lhs && a.rhs == b.rhs && a.lhs_offset == b.lhs_offset &&
      a.lhs_offset_known == b.lhs_offset_known &&
      a.lhs_stride == b.lhs_stride) {
    if ((a.kind == GuardKind::PointsTo && b.kind == GuardKind::NotPointsTo) ||
        (a.kind == GuardKind::NotPointsTo && b.kind == GuardKind::PointsTo) ||
        (a.kind == GuardKind::Alias && b.kind == GuardKind::NotAlias) ||
        (a.kind == GuardKind::NotAlias && b.kind == GuardKind::Alias))
      return true;
  }
  return false;
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
              if (isMemory(m_program.objects[o])) {
                m_func_modifies[f].insert(o);
                m_func_modifies_any[f] = true;
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
                if (inst.write_bytes != 0) {
                  for (Id fo : m_program.overlappingFields(o, inst.write_bytes))
                    if (isMemory(m_program.objects[fo])) {
                      m_func_modifies[f].insert(fo);
                      m_func_modifies_any[f] = true;
                    }
                } else {
                  m_func_modifies[f].insert(o);
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
      m_hierarchy.depth[m_hierarchy.value_components[val]];
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
  const std::size_t vdepth = m_hierarchy.depth[comp];
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
    if (m_cyclic_pass1_depth == std::numeric_limits<std::size_t>::max() &&
        !m_active_fsci.count(fkey) && m_active_fsci.size() < 8) {
      bool pruned = false;
      PointsToSet pts = computeFsci(val, site, Point::Before, nullptr, &pruned);
      if (pts.empty() && pruned)
        m_path_pruned_fsci.insert(fkey);
      m_fsci_cache.emplace(fkey, pts);
      return pts;
    }
  }
  return valueOf(m_program, m_upper, val);
}

bool GuardedSummarySolver::isSameOrHigherDepth(Id val,
                                               std::size_t target_depth) const {
  if (val == INVALID || m_program.values[val].constant)
    return false;
  const Id comp = m_hierarchy.value_components[val];
  return m_hierarchy.depth[comp] >= target_depth;
}

PointsToSet GuardedSummarySolver::evalGuardPointer(Id val, Id site,
                                                   const Context *ctx,
                                                   bool for_negative_guard,
                                                   bool &is_exact_upper) {
  is_exact_upper = true;
  if (val == INVALID)
    return PointsToSet::top();
  if (m_program.values[val].constant)
    return m_program.values[val].initial;

  const Id comp = m_hierarchy.value_components[val];
  const std::size_t vdepth = m_hierarchy.depth[comp];

  if (m_cyclic_pass1_depth != std::numeric_limits<std::size_t>::max() &&
      vdepth >= m_cyclic_pass1_depth) {
    if (for_negative_guard) {
      is_exact_upper = false;
      return PointsToSet::top();
    }
    return lookupPass1Fsci(val, site, Point::Before);
  }

  if (ctx != nullptr) {
    FscsKey key{val, site, Point::Before, *ctx};
    auto cached = m_fscs_cache.find(key);
    if (cached != m_fscs_cache.end())
      return cached->second.reachable() ? cached->second.points_to
                                        : PointsToSet{};
    if (!m_active_fscs.count(key) && m_active_fscs.size() < 16) {
      QueryResult r = computeFscs(val, site, *ctx, Point::Before);
      if (r.status == QueryStatus::ResourceLimit)
        throw LimitReached{};
      return r.reachable() ? r.points_to : PointsToSet{};
    }
  }

  FsciKey fkey{val, site, Point::Before};
  auto it = m_fsci_cache.find(fkey);
  if (it != m_fsci_cache.end()) {
    ++m_stats.fsci_cache_hits;
    return it->second;
  }
  if (m_active_fsci.count(fkey)) {
    auto p1 = m_cyclic_pass1_fsci.find(fkey);
    if (p1 != m_cyclic_pass1_fsci.end())
      return p1->second;
    return valueOf(m_program, m_upper, val);
  }
  return computeFsci(val, site, Point::Before);
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
  PointsToSet lhs_pts =
      evalGuardPointer(atom.lhs, atom.site, eval_ctx, is_neg, exact);
  lhs_pts = applyOffset(lhs_pts, atom.lhs_offset, atom.lhs_offset_known,
                        atom.lhs_stride);

  switch (atom.kind) {
  case GuardKind::PointsTo:
    return lhs_pts.contains(atom.rhs);
  case GuardKind::NotPointsTo: {
    if (!exact)
      return true;
    const bool must_be_obj = !lhs_pts.isTop() && lhs_pts.size() == 1 &&
                             lhs_pts.contains(atom.rhs) &&
                             atom.rhs < m_program.objects.size() &&
                             m_program.objects[atom.rhs].singleton;
    return !must_be_obj;
  }
  case GuardKind::Alias: {
    bool exact_r = true;
    const PointsToSet rhs_pts =
        evalGuardPointer(atom.rhs, atom.site, eval_ctx, false, exact_r);
    return memoryIntersects(lhs_pts, rhs_pts);
  }
  case GuardKind::NotAlias: {
    if (!exact)
      return true;
    bool exact_r = true;
    const PointsToSet rhs_pts =
        evalGuardPointer(atom.rhs, atom.site, eval_ctx, true, exact_r);
    if (!exact_r)
      return true;
    const bool must_alias_singleton =
        !lhs_pts.isTop() && !rhs_pts.isTop() && lhs_pts.size() == 1 &&
        lhs_pts == rhs_pts && *lhs_pts.begin() < m_program.objects.size() &&
        m_program.objects[*lhs_pts.begin()].singleton;
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
