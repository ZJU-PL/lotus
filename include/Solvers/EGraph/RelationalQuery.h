#pragma once

#include "Solvers/EGraph/EGraph.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace lotus::egraph {

// Flat contiguous buffer of match tuples: each match occupies `stride` Ids,
// where entry 0 is the root e-class Id and entries 1..stride-1 are the bound
// pattern variable values in canonical variable-slot order.
struct FlatMatches {
  uint32_t stride = 1;
  PodVec<Id> data;

  void reset(uint32_t new_stride) noexcept {
    stride = new_stride;
    data.clear();
  }

  size_t count() const noexcept {
    return stride == 0 ? 0 : data.size() / stride;
  }

  bool empty() const noexcept { return data.empty(); }
};

namespace detail {

template <typename L> struct QueryAtom {
  L node_template;
  typename L::Discriminant disc;
  bool exact_disc_match = false;
  bool is_ground = false;
  uint32_t out_reg = 0;
  llvm::SmallVector<uint32_t, 4> child_regs;

  QueryAtom(const L &node, uint32_t out,
            llvm::SmallVector<uint32_t, 4> children, bool ground)
      : node_template(node), disc(node.discriminant()),
        exact_disc_match(discriminantImpliesMatch(node)), is_ground(ground),
        out_reg(out), child_regs(std::move(children)) {}
};

// Dynamic execution step in a relational join plan.
struct JoinStep {
  enum class Kind : uint8_t {
    ScanTable,   // Scan OpTable[op_id] (full or delta tail)
    ProbeEClass, // Probe OpTable[op_id].by_eclass[regs[out_reg]]
    ProbeCol,    // Probe OpTable[op_id].by_col[probe_col][regs[child[probe_col]]]
    LookupExact, // All children bound -> single O(1) lookupCanonical()
    ScanClasses, // Bare variable -> scan active e-classes
  };

  Kind kind = Kind::ScanTable;
  uint32_t atom_idx = 0;
  uint32_t op_id = 0;
  uint32_t probe_col = 0;
  // Version filtering for semi-naive deduplication:
  // Only rows with min_version <= row.version <= max_version are accepted.
  uint32_t min_version = 0;
  uint32_t max_version = UINT32_MAX;
  // Bitmask / flags of which positions are CHECKED vs BOUND during this step:
  bool check_out = false;
  bool c0_bound_for_bsearch = false;
  uint32_t out_reg = 0;
  uint32_t arity = 0;
  llvm::SmallVector<uint32_t, 4> child_regs;
  llvm::SmallVector<uint8_t, 4> check_child; // 1 = check eq, 0 = bind
  bool needs_node_match = false;
};

template <typename L> class RelationalQuery {
public:
  RelationalQuery() = default;

  static RelationalQuery compileFromPattern(const PatternAst<L> &ast) {
    RelationalQuery q;
    if (ast.empty()) {
      return q;
    }
    q.root_reg_ = q.compileAst(ast, std::nullopt);
    q.finalize();
    return q;
  }

  static RelationalQuery compileFromMultiPattern(
      const std::vector<std::pair<Var, PatternAst<L>>> &clauses) {
    RelationalQuery q;
    if (clauses.empty()) {
      return q;
    }
    for (size_t i = 0; i < clauses.size(); ++i) {
      uint32_t r = q.compileAst(clauses[i].second, clauses[i].first);
      if (i == 0) {
        q.root_reg_ = r;
      }
    }
    q.finalize();
    return q;
  }

  bool empty() const noexcept { return num_regs_ == 0; }
  const std::vector<Var> &vars() const noexcept { return vars_; }
  uint32_t stride() const noexcept {
    return static_cast<uint32_t>(1 + vars_.size());
  }

  // Returns variable slot index in [0, vars_.size()), or UINT32_MAX if absent.
  uint32_t varSlot(const Var &v) const noexcept {
    const uint32_t sym = v.name().id();
    for (uint32_t i = 0; i < vars_.size(); ++i) {
      if (vars_[i].name().id() == sym) {
        return i;
      }
    }
    return UINT32_MAX;
  }

  // Run query across the entire e-graph into a flat match buffer.
  // If `since_version > 0`, runs exact semi-naive evaluation returning only
  // matches involving at least one atom updated at version > since_version.
  template <typename A>
  void runFlat(const EGraph<L, A> &egraph, FlatMatches &out,
               size_t limit = std::numeric_limits<size_t>::max(),
               uint32_t since_version = 0,
               const WorkControl *control = nullptr,
               bool sorted_classes = false) const {
    out.reset(stride());
    if (limit == 0 || num_regs_ == 0) {
      return;
    }

    llvm::SmallVector<Id, 16> regs(num_regs_, Id(0));
    llvm::SmallVector<uint8_t, 16> prebound(num_regs_, 0);

    // 1. Resolve all ground subterms once via O(1) hashcons lookup!
    bool any_ground_new = false;
    for (uint32_t gi : ground_atom_order_) {
      const auto &atom = atoms_[gi];
      L node = atom.node_template;
      auto &&ch = node.childrenMut();
      for (size_t k = 0; k < ch.size(); ++k) {
        ch[k] = regs[atom.child_regs[k]];
      }
      auto found = egraph.lookupCanonical(node);
      if (!found) {
        return; // Ground subterm missing -> 0 matches in O(1)!
      }
      Id canon = *found;
      if (prebound[atom.out_reg] && regs[atom.out_reg] != canon) {
        return;
      }
      regs[atom.out_reg] = canon;
      prebound[atom.out_reg] = 1;
      if (since_version > 0 && !any_ground_new) {
        // Check if this ground node itself is new/rebuilt/owner-changed.
        uint32_t op_id = egraph.findOpId(atom.disc);
        if (op_id != EGraph<L, A>::kInvalidOpId) {
          const auto &by_ec = egraph.opByEClass(op_id);
          OpCsrRange r = by_ec.lookup(canon);
          for (uint32_t j = r.begin; j < r.end; ++j) {
            const OpRow &row = by_ec.entries[j];
            if (row.version > since_version &&
                (atom.exact_disc_match ||
                 atom.node_template.matches(egraph.enodeAt(row.node_idx)))) {
              bool ch_eq = true;
              for (size_t k = 0; k < atom.child_regs.size(); ++k) {
                if (egraph.enodeChild(row, k) != regs[atom.child_regs[k]]) {
                  ch_eq = false;
                  break;
                }
              }
              if (ch_eq) {
                any_ground_new = true;
                break;
              }
            }
          }
        }
      }
    }

    // 2. Resolve OpIds for all non-ground atoms; abort in O(1) if any op absent!
    const size_t m = active_atoms_.size();
    llvm::SmallVector<uint32_t, 8> atom_op_ids(m, EGraph<L, A>::kInvalidOpId);
    for (size_t i = 0; i < m; ++i) {
      uint32_t op_id = egraph.findOpId(atoms_[active_atoms_[i]].disc);
      if (op_id == EGraph<L, A>::kInvalidOpId ||
          egraph.opTable(op_id).live_count == 0) {
        return;
      }
      atom_op_ids[i] = op_id;
    }

    size_t remaining = limit;

    // 3. Handle pure-ground / bare-variable queries (m == 0).
    if (m == 0) {
      if (bare_var_regs_.empty()) {
        if (since_version == 0 || any_ground_new) {
          emitMatch(regs, out, remaining);
        }
        return;
      }
      // Bare variable(s) only (e.g. `?x`)
      runBareVarsOnly(egraph, regs, prebound, 0, out, remaining, control,
                      sorted_classes);
      return;
    }

    // 4. Ultra-fast specialized path for 1-atom queries (no bare vars)!
    if (m == 1 && bare_var_regs_.empty()) {
      runSingleAtom(egraph, active_atoms_[0], atom_op_ids[0], regs, prebound,
                    (any_ground_new ? 0u : since_version), out, remaining,
                    control);
      return;
    }

    // 5. Ultra-fast specialized path for 2-atom nested parent->child queries!
    if (m == 2 && bare_var_regs_.empty() && control == nullptr &&
        is_simple_two_atom_chain_) {
      runSimpleTwoAtomChain(egraph, atom_op_ids[0], atom_op_ids[1], regs,
                            prebound, (any_ground_new ? 0u : since_version),
                            out, remaining);
      return;
    }

    // 5b. Ultra-fast specialized path for 3-atom sibling-shared-c0 patterns
    // like `(Add (Mul ?a ?b) (Mul ?a ?c))` and `(Mul (Pow ?a ?b) (Pow ?a ?c))`!
    if (m == 3 && bare_var_regs_.empty() && control == nullptr &&
        is_factor_sibling_join_) {
      runFactorSiblingJoin(egraph, atom_op_ids[factor_outer_slot_],
                           atom_op_ids[factor_left_slot_],
                           atom_op_ids[factor_right_slot_],
                           (any_ground_new ? 0u : since_version), out,
                           remaining);
      return;
    }

    // 6. General multi-atom relational join (with exact semi-naive delta split)!
    if (since_version == 0 || any_ground_new) {
      runBranch(egraph, atom_op_ids, regs, prebound, /*delta_idx=*/-1,
                /*since_version=*/0, out, remaining, control, sorted_classes);
    } else {
      for (size_t d = 0; d < m && remaining > 0; ++d) {
        const OpTable &dt = egraph.opTable(atom_op_ids[d]);
        if (dt.deltaStart(since_version) >= dt.rows.size()) {
          continue; // Zero delta rows for atom d -> skip branch in O(1)!
        }
        runBranch(egraph, atom_op_ids, regs, prebound, static_cast<int>(d),
                  since_version, out, remaining, control, sorted_classes);
      }
    }
  }

  // Run query anchored at a single specific e-class `eclass` (for searchEClass).
  template <typename A>
  void runSingleEClassFlat(const EGraph<L, A> &egraph, Id eclass,
                           FlatMatches &out,
                           size_t limit = std::numeric_limits<size_t>::max(),
                           const WorkControl *control = nullptr) const {
    out.reset(stride());
    if (limit == 0 || num_regs_ == 0) {
      return;
    }
    Id canon_root = egraph.find(eclass);
    llvm::SmallVector<Id, 16> regs(num_regs_, Id(0));
    llvm::SmallVector<uint8_t, 16> prebound(num_regs_, 0);
    regs[root_reg_] = canon_root;
    prebound[root_reg_] = 1;

    // Resolve ground subterms
    for (uint32_t gi : ground_atom_order_) {
      const auto &atom = atoms_[gi];
      L node = atom.node_template;
      auto &&ch = node.childrenMut();
      for (size_t k = 0; k < ch.size(); ++k) {
        ch[k] = regs[atom.child_regs[k]];
      }
      auto found = egraph.lookupCanonical(node);
      if (!found) {
        return;
      }
      Id canon = *found;
      if (prebound[atom.out_reg] && regs[atom.out_reg] != canon) {
        return;
      }
      regs[atom.out_reg] = canon;
      prebound[atom.out_reg] = 1;
    }

    const size_t m = active_atoms_.size();
    llvm::SmallVector<uint32_t, 8> atom_op_ids(m, EGraph<L, A>::kInvalidOpId);
    for (size_t i = 0; i < m; ++i) {
      uint32_t op_id = egraph.findOpId(atoms_[active_atoms_[i]].disc);
      if (op_id == EGraph<L, A>::kInvalidOpId ||
          egraph.opTable(op_id).live_count == 0) {
        return;
      }
      atom_op_ids[i] = op_id;
    }

    size_t remaining = limit;
    if (m == 0) {
      runBareVarsOnly(egraph, regs, prebound, 0, out, remaining, control, true);
      return;
    }

    runBranch(egraph, atom_op_ids, regs, prebound, /*delta_idx=*/-1,
              /*since_version=*/0, out, remaining, control, true);
  }

  // Convert FlatMatches into the legacy `std::vector<SearchMatches<L>>` API.
  template <typename A>
  std::vector<SearchMatches<L>>
  materializeSearchMatches(const FlatMatches &flat,
                           std::shared_ptr<const PatternAst<L>> ast) const {
    std::vector<SearchMatches<L>> results;
    if (flat.empty()) {
      return results;
    }
    const uint32_t st = flat.stride;
    const size_t n = flat.count();
    FlatHashMap<Id, size_t> class_to_pos;
    class_to_pos.reserve(std::min<size_t>(n, 4096));

    for (size_t i = 0; i < n; ++i) {
      const Id *row = &flat.data[i * st];
      Id ec = row[0];
      Subst subst = Subst::withCapacity(vars_.size());
      for (size_t v = 0; v < vars_.size(); ++v) {
        subst.emplaceUnique(vars_[v], row[1 + v]);
      }
      auto [it, inserted] = class_to_pos.try_emplace(ec, results.size());
      if (inserted) {
        results.push_back(SearchMatches<L>{ec, {}, ast});
      }
      results[it->second].substs.push_back(std::move(subst));
    }
    return results;
  }

  std::vector<Subst> materializeSubsts(const FlatMatches &flat) const {
    std::vector<Subst> out;
    const uint32_t st = flat.stride;
    const size_t n = flat.count();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const Id *row = &flat.data[i * st];
      Subst subst = Subst::withCapacity(vars_.size());
      for (size_t v = 0; v < vars_.size(); ++v) {
        subst.emplaceUnique(vars_[v], row[1 + v]);
      }
      out.push_back(std::move(subst));
    }
    return out;
  }

private:
  uint32_t allocReg() noexcept { return num_regs_++; }

  uint32_t getOrAddVarReg(const Var &var) {
    const uint32_t sym = var.name().id();
    for (size_t i = 0; i < vars_.size(); ++i) {
      if (vars_[i].name().id() == sym) {
        return var_regs_[i];
      }
    }
    uint32_t reg = allocReg();
    vars_.push_back(var);
    var_regs_.push_back(reg);
    return reg;
  }

  uint32_t compileAst(const PatternAst<L> &ast, std::optional<Var> binder) {
    const size_t n = ast.size();
    std::vector<uint32_t> node_regs(n, 0);
    std::vector<uint8_t> node_ground(n, 0);

    // Pre-assign binder register for the root if present and root is a node.
    uint32_t binder_reg = UINT32_MAX;
    if (binder) {
      binder_reg = getOrAddVarReg(*binder);
    }

    for (size_t i = 0; i < n; ++i) {
      const auto &item = ast.items()[i];
      const bool is_root = (i + 1 == n);
      if (item.isVar()) {
        uint32_t vreg = getOrAddVarReg(item.var());
        if (is_root && binder_reg != UINT32_MAX && binder_reg != vreg) {
          // Alias binder_reg == vreg: record equality constraint.
          var_aliases_.emplace_back(binder_reg, vreg);
          node_regs[i] = binder_reg;
        } else {
          node_regs[i] = vreg;
        }
        node_ground[i] = 0;
      } else {
        const L &node = item.node();
        bool ground = true;
        llvm::SmallVector<uint32_t, 4> child_regs;
        child_regs.reserve(node.children().size());
        for (Id c : node.children()) {
          child_regs.push_back(node_regs[c.index()]);
          if (!node_ground[c.index()]) {
            ground = false;
          }
        }
        uint32_t out_reg =
            (is_root && binder_reg != UINT32_MAX) ? binder_reg : allocReg();
        node_regs[i] = out_reg;
        node_ground[i] = ground ? 1 : 0;
        atoms_.emplace_back(node, out_reg, std::move(child_regs), ground);
      }
    }
    return node_regs[n - 1];
  }

  void finalize() {
    // Resolve any variable-to-variable aliases (e.g. `?a = ?b` in MultiPattern)
    if (!var_aliases_.empty()) {
      std::vector<uint32_t> parent(num_regs_);
      for (uint32_t i = 0; i < num_regs_; ++i) {
        parent[i] = i;
      }
      auto findR = [&](uint32_t x) {
        while (parent[x] != x) {
          parent[x] = parent[parent[x]];
          x = parent[x];
        }
        return x;
      };
      for (auto [a, b] : var_aliases_) {
        uint32_t ra = findR(a);
        uint32_t rb = findR(b);
        if (ra != rb) {
          parent[rb] = ra;
        }
      }
      root_reg_ = findR(root_reg_);
      for (uint32_t &r : var_regs_) {
        r = findR(r);
      }
      for (auto &atom : atoms_) {
        atom.out_reg = findR(atom.out_reg);
        for (uint32_t &c : atom.child_regs) {
          c = findR(c);
        }
      }
    }

    // Partition atoms into ground (already in bottom-up topological order!) vs
    // active (non-ground).
    std::vector<uint8_t> reg_in_atom(num_regs_, 0);
    for (uint32_t i = 0; i < atoms_.size(); ++i) {
      reg_in_atom[atoms_[i].out_reg] = 1;
      for (uint32_t c : atoms_[i].child_regs) {
        reg_in_atom[c] = 1;
      }
      if (atoms_[i].is_ground) {
        ground_atom_order_.push_back(i);
      } else {
        active_atoms_.push_back(i);
      }
    }

    // Identify bare variable registers that never appear in any atom.
    for (uint32_t r : var_regs_) {
      if (!reg_in_atom[r]) {
        if (std::find(bare_var_regs_.begin(), bare_var_regs_.end(), r) ==
            bare_var_regs_.end()) {
          bare_var_regs_.push_back(r);
        }
      }
    }

    // Detect the super-hot 2-atom parent->child chain pattern!
    // e.g. (Add ?a (Add ?b ?c)), (Mul ?a (Add ?b ?c)), (Diff ?x (Mul ?a ?b)),
    // (Integral (Mul ?a ?b) ?x), etc.
    is_simple_two_atom_chain_ = false;
    if (active_atoms_.size() == 2 && ground_atom_order_.empty() &&
        bare_var_regs_.empty()) {
      const auto &a0 = atoms_[active_atoms_[0]];
      const auto &a1 = atoms_[active_atoms_[1]];
      // Check if one is outer (parent) and the other is inner (child):
      // In bottom-up AST order, atoms_[0] is inner and atoms_[1] is outer!
      uint32_t outer_idx = active_atoms_[1];
      uint32_t inner_idx = active_atoms_[0];
      const auto &outer = atoms_[outer_idx];
      const auto &inner = atoms_[inner_idx];
      if (outer.exact_disc_match && inner.exact_disc_match &&
          outer.child_regs.size() <= 3 && inner.child_regs.size() <= 3 &&
          outer.out_reg == root_reg_) {
        int join_col = -1;
        int join_count = 0;
        for (size_t k = 0; k < outer.child_regs.size(); ++k) {
          if (outer.child_regs[k] == inner.out_reg) {
            join_col = static_cast<int>(k);
            ++join_count;
          }
        }
        // Verify no repeated vars inside either atom or between outer & inner
        // other than the single join register `inner.out_reg` (for pure linear
        // 2-atom rules; non-linear ones like `(Diff ?x (Sin ?x))` or
        // `(Integral (Cos ?x) ?x)` also check `shared_var_check`!).
        if (join_count == 1) {
          // Check all register occurrences across outer and inner.
          llvm::SmallVector<uint8_t, 16> seen(num_regs_, 0);
          bool linear = true;
          seen[outer.out_reg] = 1;
          for (size_t k = 0; k < outer.child_regs.size(); ++k) {
            uint32_t r = outer.child_regs[k];
            if (seen[r]) {
              linear = false;
            }
            seen[r] = 1;
          }
          for (uint32_t r : inner.child_regs) {
            if (seen[r]) {
              linear = false;
            }
            seen[r] = 1;
          }
          if (linear) {
            is_simple_two_atom_chain_ = true;
            chain_outer_atom_ = outer_idx;
            chain_inner_atom_ = inner_idx;
            chain_join_col_ = static_cast<uint32_t>(join_col);
          }
        }
      }
    }

    // Detect 3-atom sibling-join pattern `(Op0 (Op1 ?a ?b) (Op2 ?a ?c))`!
    is_factor_sibling_join_ = false;
    if (active_atoms_.size() == 3 && ground_atom_order_.empty() &&
        bare_var_regs_.empty() && var_regs_.size() == 3) {
      int o_slot = -1;
      for (int k = 0; k < 3; ++k) {
        const auto &a = atoms_[active_atoms_[k]];
        if (a.out_reg == root_reg_ && a.exact_disc_match &&
            a.child_regs.size() == 2) {
          o_slot = k;
          break;
        }
      }
      if (o_slot >= 0) {
        const auto &outer = atoms_[active_atoms_[o_slot]];
        int l_slot = -1;
        int r_slot = -1;
        for (int k = 0; k < 3; ++k) {
          if (k == o_slot) {
            continue;
          }
          const auto &a = atoms_[active_atoms_[k]];
          if (!a.exact_disc_match || a.child_regs.size() != 2) {
            continue;
          }
          if (a.out_reg == outer.child_regs[0] && l_slot < 0) {
            l_slot = k;
          } else if (a.out_reg == outer.child_regs[1] && r_slot < 0) {
            r_slot = k;
          }
        }
        if (l_slot >= 0 && r_slot >= 0 && l_slot != r_slot) {
          const auto &la = atoms_[active_atoms_[l_slot]];
          const auto &ra = atoms_[active_atoms_[r_slot]];
          const uint32_t shared_reg = la.child_regs[0];
          const uint32_t b_reg = la.child_regs[1];
          const uint32_t c_reg = ra.child_regs[1];
          if (shared_reg == ra.child_regs[0] && shared_reg != b_reg &&
              shared_reg != c_reg && b_reg != c_reg &&
              outer.out_reg != la.out_reg && outer.out_reg != ra.out_reg &&
              la.out_reg != ra.out_reg && shared_reg != outer.out_reg &&
              shared_reg != la.out_reg && shared_reg != ra.out_reg &&
              b_reg != outer.out_reg && b_reg != la.out_reg &&
              b_reg != ra.out_reg && c_reg != outer.out_reg &&
              c_reg != la.out_reg && c_reg != ra.out_reg) {
            bool all_mapped = true;
            uint8_t src[3] = {0, 0, 0};
            for (int v = 0; v < 3; ++v) {
              if (var_regs_[v] == shared_reg) {
                src[v] = 0;
              } else if (var_regs_[v] == b_reg) {
                src[v] = 1;
              } else if (var_regs_[v] == c_reg) {
                src[v] = 2;
              } else {
                all_mapped = false;
              }
            }
            if (all_mapped) {
              is_factor_sibling_join_ = true;
              factor_outer_slot_ = static_cast<uint32_t>(o_slot);
              factor_left_slot_ = static_cast<uint32_t>(l_slot);
              factor_right_slot_ = static_cast<uint32_t>(r_slot);
              factor_var_src_[0] = src[0];
              factor_var_src_[1] = src[1];
              factor_var_src_[2] = src[2];
            }
          }
        }
      }
    }
  }

  inline bool emitMatch(const llvm::SmallVector<Id, 16> &regs, FlatMatches &out,
                        size_t &remaining) const {
    out.data.push_back(regs[root_reg_]);
    for (uint32_t vr : var_regs_) {
      out.data.push_back(regs[vr]);
    }
    return --remaining > 0;
  }

  template <typename A>
  void runSingleAtom(const EGraph<L, A> &egraph, uint32_t atom_idx,
                     uint32_t op_id, llvm::SmallVector<Id, 16> &regs,
                     const llvm::SmallVector<uint8_t, 16> &prebound,
                     uint32_t since_version, FlatMatches &out,
                     size_t &remaining, const WorkControl *control) const {
    const auto &atom = atoms_[atom_idx];
    const uint32_t arity = static_cast<uint32_t>(atom.child_regs.size());
    const bool exact = atom.exact_disc_match;
    const bool out_prebound = prebound[atom.out_reg] != 0;

    // Check if any child is prebound (e.g. ground constant in `(Add ?a 0)`):
    // If so, probe `by_col[col]` in O(matches) instead of scanning the entire
    // table!
    int prebound_col = -1;
    for (uint32_t k = 0; k < arity; ++k) {
      if (prebound[atom.child_regs[k]]) {
        prebound_col = static_cast<int>(k);
        break;
      }
    }

    // Build check/bind descriptors for sequential evaluation
    llvm::SmallVector<uint8_t, 4> check_ch(arity, 0);
    llvm::SmallVector<uint8_t, 16> bound = prebound;
    bool check_out = bound[atom.out_reg] != 0;
    bound[atom.out_reg] = 1;
    for (uint32_t k = 0; k < arity; ++k) {
      if (bound[atom.child_regs[k]]) {
        check_ch[k] = 1;
      } else {
        bound[atom.child_regs[k]] = 1;
      }
    }

    auto processRow = [&](const OpRow &row) -> bool {
      if (control && control->poll()) {
        remaining = 0;
        return false;
      }
      if (!exact && !atom.node_template.matches(egraph.enodeAt(row.node_idx))) {
        return true;
      }
      if (check_out) {
        if (row.eclass != regs[atom.out_reg]) {
          return true;
        }
      } else {
        regs[atom.out_reg] = row.eclass;
      }
      for (uint32_t k = 0; k < arity; ++k) {
        Id val = egraph.enodeChild(row, k);
        if (check_ch[k]) {
          if (val != regs[atom.child_regs[k]]) {
            return true;
          }
        } else {
          regs[atom.child_regs[k]] = val;
        }
      }
      return emitMatch(regs, out, remaining);
    };

    if (out_prebound) {
      const auto &csr = egraph.opByEClass(op_id);
      OpCsrRange range = csr.lookup(regs[atom.out_reg]);
      for (uint32_t i = range.begin; i < range.end; ++i) {
        const OpRow &row = csr.entries[i];
        if (row.version <= since_version) {
          continue;
        }
        if (!processRow(row)) {
          return;
        }
      }
      return;
    }

    if (prebound_col >= 0) {
      const auto &csr = egraph.opByCol(op_id, static_cast<size_t>(prebound_col));
      OpCsrRange range = csr.lookup(regs[atom.child_regs[prebound_col]]);
      for (uint32_t i = range.begin; i < range.end; ++i) {
        const OpRow &row = csr.entries[i];
        if (row.version <= since_version) {
          continue;
        }
        if (!processRow(row)) {
          return;
        }
      }
      return;
    }

    const OpTable &table = egraph.opTable(op_id);
    const uint32_t start = table.deltaStart(since_version);
    const uint32_t end = static_cast<uint32_t>(table.rows.size());
    if (start >= end) {
      return;
    }

    // Ultra-tight specialized loop for binary exact-discriminant ops without
    // repeated vars (e.g., `(Add ?a ?b)`, `(Mul ?a ?b)`, `(Sub ?a ?b)`):
    if (exact && arity == 2 && !check_out && !check_ch[0] && !check_ch[1] &&
        !control && var_regs_.size() == 2) {
      const bool normal_order = (var_regs_[0] == atom.child_regs[0] &&
                                 var_regs_[1] == atom.child_regs[1]);
      const OpRow *rows = table.rows.data();
      out.data.reserve(out.data.size() +
                       std::min<size_t>(remaining, end - start) * 3);
      if (normal_order) {
        for (uint32_t i = start; i < end; ++i) {
          const OpRow &row = rows[i];
          if (row.isDead()) {
            continue;
          }
          out.data.push_back(row.eclass);
          out.data.push_back(row.c0);
          out.data.push_back(row.c1);
          if (--remaining == 0) {
            return;
          }
        }
      } else {
        for (uint32_t i = start; i < end; ++i) {
          const OpRow &row = rows[i];
          if (row.isDead()) {
            continue;
          }
          out.data.push_back(row.eclass);
          out.data.push_back(row.c1);
          out.data.push_back(row.c0);
          if (--remaining == 0) {
            return;
          }
        }
      }
      return;
    }

    for (uint32_t i = start; i < end; ++i) {
      const OpRow &row = table.rows[i];
      if (row.isDead()) {
        continue;
      }
      if (!processRow(row)) {
        return;
      }
    }
  }

  // Ultra-tight specialized executor for linear 2-atom parent->child rules
  // like `(Add ?a (Add ?b ?c))`, `(Mul ?a (Add ?b ?c))`, `(Diff ?x (Mul ?a ?b))`,
  // `(Integral (Mul ?a ?b) ?x)` — ZERO recursion, ZERO dynamic dispatch!
  template <typename A>
  void runSimpleTwoAtomChain(const EGraph<L, A> &egraph, uint32_t inner_op_id,
                             uint32_t outer_op_id,
                             llvm::SmallVector<Id, 16> &regs,
                             const llvm::SmallVector<uint8_t, 16> &,
                             uint32_t since_version, FlatMatches &out,
                             size_t &remaining) const {
    const auto &outer = atoms_[chain_outer_atom_];
    const auto &inner = atoms_[chain_inner_atom_];
    const uint32_t jcol = chain_join_col_;
    const uint32_t outer_arity = static_cast<uint32_t>(outer.child_regs.size());
    const uint32_t inner_arity = static_cast<uint32_t>(inner.child_regs.size());

    const OpTable &outer_tbl = egraph.opTable(outer_op_id);
    const OpTable &inner_tbl = egraph.opTable(inner_op_id);

    // Precompute compact source locations for each var_slot to avoid writing
    // into `regs` array on the ultra-hot 3-var binary-binary path!
    // Source encoding: 0..2 = outer c0..c2, 3..5 = inner c0..c2
    llvm::SmallVector<uint8_t, 4> var_src(var_regs_.size(), 0);
    for (size_t v = 0; v < var_regs_.size(); ++v) {
      uint32_t vr = var_regs_[v];
      for (uint32_t k = 0; k < outer_arity; ++k) {
        if (outer.child_regs[k] == vr) {
          var_src[v] = static_cast<uint8_t>(k);
        }
      }
      for (uint32_t k = 0; k < inner_arity; ++k) {
        if (inner.child_regs[k] == vr) {
          var_src[v] = static_cast<uint8_t>(3 + k);
        }
      }
    }

    auto readSrc = [](const OpRow &ro, const OpRow &ri,
                      uint8_t src) noexcept -> Id {
      switch (src) {
      case 0:
        return ro.c0;
      case 1:
        return ro.c1;
      case 2:
        return ro.c2;
      case 3:
        return ri.c0;
      case 4:
        return ri.c1;
      default:
        return ri.c2;
      }
    };

    const bool fast_3var = (var_regs_.size() == 3);
    const uint8_t s0 = fast_3var ? var_src[0] : 0;
    const uint8_t s1 = fast_3var ? var_src[1] : 0;
    const uint8_t s2 = fast_3var ? var_src[2] : 0;

    // Branch 0: outer is NEW (version > since_version), inner is ALL!
    const uint32_t o_start = outer_tbl.deltaStart(since_version);
    const uint32_t o_end = static_cast<uint32_t>(outer_tbl.rows.size());
    if (o_start < o_end && remaining > 0) {
      const size_t o_delta = o_end - o_start;
      if (inner_tbl.live_count * 2 < o_delta) {
        // Inner table is much smaller than outer delta (e.g. Mul(a, Sin(x))):
        // scan inner table and probe outer by_col checking version > since!
        const OpCsrIndex &outer_by_col = egraph.opByCol(outer_op_id, jcol);
        const OpRow *i_rows = inner_tbl.rows.data();
        const uint32_t i_all_end = static_cast<uint32_t>(inner_tbl.rows.size());
        const OpRow *o_entries = outer_by_col.entries.data();
        for (uint32_t i = 0; i < i_all_end; ++i) {
          const OpRow &ri = i_rows[i];
          if (ri.isDead()) {
            continue;
          }
          OpCsrRange r = outer_by_col.lookup(ri.eclass);
          for (uint32_t j = r.begin; j < r.end; ++j) {
            const OpRow &ro = o_entries[j];
            if (ro.version <= since_version) {
              continue;
            }
            out.data.push_back(ro.eclass);
            if (fast_3var) {
              out.data.push_back(readSrc(ro, ri, s0));
              out.data.push_back(readSrc(ro, ri, s1));
              out.data.push_back(readSrc(ro, ri, s2));
            } else {
              for (uint8_t src : var_src) {
                out.data.push_back(readSrc(ro, ri, src));
              }
            }
            if (--remaining == 0) {
              return;
            }
          }
        }
      } else {
        const OpCsrIndex &inner_by_ec = egraph.opByEClass(inner_op_id);
        const OpRow *o_rows = outer_tbl.rows.data();
        const OpRow *i_entries = inner_by_ec.entries.data();
        const bool dense = inner_by_ec.is_dense;
        const OpCsrRange *dense_ptr =
            dense ? inner_by_ec.dense_ranges.data() : nullptr;
        const size_t dense_sz = inner_by_ec.dense_ranges.size();

        auto lookupInner = [&](Id id) noexcept -> OpCsrRange {
          if (dense) {
            size_t idx = id.index();
            return idx < dense_sz ? dense_ptr[idx] : OpCsrRange{};
          }
          return inner_by_ec.lookup(id);
        };

        if (fast_3var && jcol == 1 && s0 == 0 && s1 == 3 && s2 == 4 && dense) {
          // Ultra-fast specialized path for `(Op1 ?a (Op2 ?b ?c))` (add-assoc,
          // mul-assoc, dist-mul-add, diff-add, diff-mul, etc.)!
          for (uint32_t i = o_start; i < o_end; ++i) {
            if (i + 8 < o_end) {
              const Id pf_id = o_rows[i + 8].c1;
              if (pf_id.index() < dense_sz) {
                __builtin_prefetch(dense_ptr + pf_id.index(), 0, 1);
              }
            }
            const OpRow &ro = o_rows[i];
            if (ro.isDead()) {
              continue;
            }
            const size_t jidx = ro.c1.index();
            if (jidx >= dense_sz) {
              continue;
            }
            const OpCsrRange r = dense_ptr[jidx];
            const uint32_t cnt = r.end - r.begin;
            if (cnt == 0) {
              continue;
            }
            const uint32_t take =
                cnt <= remaining ? cnt : static_cast<uint32_t>(remaining);
            const size_t old_sz = out.data.size();
            const size_t add_n = static_cast<size_t>(take) * 4;
            out.data.reserveAdditional(add_n);
            Id *dst = out.data.data() + old_sz;
            const Id ec = ro.eclass;
            const Id rc0 = ro.c0;
            const OpRow *p = i_entries + r.begin;
            for (uint32_t t = 0; t < take; ++t) {
              dst[0] = ec;
              dst[1] = rc0;
              dst[2] = p[t].c0;
              dst[3] = p[t].c1;
              dst += 4;
            }
            out.data.setSizeUnchecked(old_sz + add_n);
            remaining -= take;
            if (remaining == 0) {
              return;
            }
          }
        } else if (fast_3var && jcol == 0 && s0 == 3 && s1 == 4 && s2 == 1 &&
                   dense) {
          // Ultra-fast specialized path for `(Op1 (Op2 ?a ?b) ?c)` (int-add,
          // int-sub, int-parts, etc.)!
          for (uint32_t i = o_start; i < o_end; ++i) {
            const OpRow &ro = o_rows[i];
            if (ro.isDead()) {
              continue;
            }
            const size_t jidx = ro.c0.index();
            if (jidx >= dense_sz) {
              continue;
            }
            const OpCsrRange r = dense_ptr[jidx];
            const uint32_t cnt = r.end - r.begin;
            if (cnt == 0) {
              continue;
            }
            const uint32_t take =
                cnt <= remaining ? cnt : static_cast<uint32_t>(remaining);
            const size_t old_sz = out.data.size();
            const size_t add_n = static_cast<size_t>(take) * 4;
            out.data.reserveAdditional(add_n);
            Id *dst = out.data.data() + old_sz;
            const Id ec = ro.eclass;
            const Id rc1 = ro.c1;
            const OpRow *p = i_entries + r.begin;
            for (uint32_t t = 0; t < take; ++t) {
              dst[0] = ec;
              dst[1] = p[t].c0;
              dst[2] = p[t].c1;
              dst[3] = rc1;
              dst += 4;
            }
            out.data.setSizeUnchecked(old_sz + add_n);
            remaining -= take;
            if (remaining == 0) {
              return;
            }
          }
        } else if (fast_3var) {
          for (uint32_t i = o_start; i < o_end; ++i) {
            const OpRow &ro = o_rows[i];
            if (ro.isDead()) {
              continue;
            }
            Id join_id = (jcol == 0) ? ro.c0 : (jcol == 1 ? ro.c1 : ro.c2);
            OpCsrRange r = lookupInner(join_id);
            for (uint32_t j = r.begin; j < r.end; ++j) {
              const OpRow &ri = i_entries[j];
              out.data.push_back(ro.eclass);
              out.data.push_back(readSrc(ro, ri, s0));
              out.data.push_back(readSrc(ro, ri, s1));
              out.data.push_back(readSrc(ro, ri, s2));
              if (--remaining == 0) {
                return;
              }
            }
          }
        } else {
          for (uint32_t i = o_start; i < o_end; ++i) {
            const OpRow &ro = o_rows[i];
            if (ro.isDead()) {
              continue;
            }
            Id join_id = (jcol == 0) ? ro.c0 : (jcol == 1 ? ro.c1 : ro.c2);
            OpCsrRange r = lookupInner(join_id);
            for (uint32_t j = r.begin; j < r.end; ++j) {
              const OpRow &ri = i_entries[j];
              out.data.push_back(ro.eclass);
              for (uint8_t src : var_src) {
                out.data.push_back(readSrc(ro, ri, src));
              }
              if (--remaining == 0) {
                return;
              }
            }
          }
        }
      }
    }

    // Branch 1 (semi-naive only!): outer is OLD (version <= since_version),
    // inner is NEW (version > since_version)!
    if (since_version > 0 && remaining > 0) {
      const uint32_t i_start = inner_tbl.deltaStart(since_version);
      const uint32_t i_end = static_cast<uint32_t>(inner_tbl.rows.size());
      const uint32_t o_old_end = o_start;
      if (i_start < i_end && o_old_end > 0) {
        const size_t i_delta = i_end - i_start;
        if (o_old_end * 2 < i_delta) {
          // Outer old rows are much fewer than inner delta (e.g. Diff(x, Add(a, b))):
          // scan outer old rows and probe already-built inner_by_ec checking version!
          const OpCsrIndex &inner_by_ec = egraph.opByEClass(inner_op_id);
          const OpRow *o_rows = outer_tbl.rows.data();
          const OpRow *i_entries = inner_by_ec.entries.data();
          const bool dense = inner_by_ec.is_dense;
          const OpCsrRange *dense_ptr =
              dense ? inner_by_ec.dense_ranges.data() : nullptr;
          const size_t dense_sz = inner_by_ec.dense_ranges.size();

          auto lookupInner = [&](Id id) noexcept -> OpCsrRange {
            if (dense) {
              size_t idx = id.index();
              return idx < dense_sz ? dense_ptr[idx] : OpCsrRange{};
            }
            return inner_by_ec.lookup(id);
          };

          for (uint32_t i = 0; i < o_old_end; ++i) {
            const OpRow &ro = o_rows[i];
            if (ro.isDead()) {
              continue;
            }
            Id join_id = (jcol == 0) ? ro.c0 : (jcol == 1 ? ro.c1 : ro.c2);
            OpCsrRange r = lookupInner(join_id);
            for (uint32_t j = r.begin; j < r.end; ++j) {
              const OpRow &ri = i_entries[j];
              if (ri.version <= since_version) {
                continue;
              }
              out.data.push_back(ro.eclass);
              if (fast_3var) {
                out.data.push_back(readSrc(ro, ri, s0));
                out.data.push_back(readSrc(ro, ri, s1));
                out.data.push_back(readSrc(ro, ri, s2));
              } else {
                for (uint8_t src : var_src) {
                  out.data.push_back(readSrc(ro, ri, src));
                }
              }
              if (--remaining == 0) {
                return;
              }
            }
          }
        } else {
          const OpCsrIndex &outer_by_col = egraph.opByCol(outer_op_id, jcol);
          const OpRow *i_rows = inner_tbl.rows.data();
          const OpRow *o_entries = outer_by_col.entries.data();
          const bool dense = outer_by_col.is_dense;
          const OpCsrRange *dense_ptr =
              dense ? outer_by_col.dense_ranges.data() : nullptr;
          const size_t dense_sz = outer_by_col.dense_ranges.size();

          auto lookupOuter = [&](Id id) noexcept -> OpCsrRange {
            if (dense) {
              size_t idx = id.index();
              return idx < dense_sz ? dense_ptr[idx] : OpCsrRange{};
            }
            return outer_by_col.lookup(id);
          };

          if (fast_3var) {
            for (uint32_t i = i_start; i < i_end; ++i) {
              const OpRow &ri = i_rows[i];
              if (ri.isDead()) {
                continue;
              }
              OpCsrRange r = lookupOuter(ri.eclass);
              for (uint32_t j = r.begin; j < r.end; ++j) {
                const OpRow &ro = o_entries[j];
                if (ro.version > since_version) {
                  continue;
                }
                out.data.push_back(ro.eclass);
                out.data.push_back(readSrc(ro, ri, s0));
                out.data.push_back(readSrc(ro, ri, s1));
                out.data.push_back(readSrc(ro, ri, s2));
                if (--remaining == 0) {
                  return;
                }
              }
            }
          } else {
            for (uint32_t i = i_start; i < i_end; ++i) {
              const OpRow &ri = i_rows[i];
              if (ri.isDead()) {
                continue;
              }
              OpCsrRange r = lookupOuter(ri.eclass);
              for (uint32_t j = r.begin; j < r.end; ++j) {
                const OpRow &ro = o_entries[j];
                if (ro.version > since_version) {
                  continue;
                }
                out.data.push_back(ro.eclass);
                for (uint8_t src : var_src) {
                  out.data.push_back(readSrc(ro, ri, src));
                }
                if (--remaining == 0) {
                  return;
                }
              }
            }
          }
        }
      }
    }
  }

  // Ultra-fast zero-recursion 3-atom sibling-join executor for patterns like
  // `(Add (Mul ?a ?b) (Mul ?a ?c))` and `(Mul (Pow ?a ?b) (Pow ?a ?c))`!
  template <typename A>
  void runFactorSiblingJoin(const EGraph<L, A> &egraph, uint32_t outer_op_id,
                            uint32_t left_op_id, uint32_t right_op_id,
                            uint32_t since_version, FlatMatches &out,
                            size_t &remaining) const {
    const OpTable &o_tbl = egraph.opTable(outer_op_id);
    const OpTable &l_tbl = egraph.opTable(left_op_id);
    const OpTable &r_tbl = egraph.opTable(right_op_id);
    if (o_tbl.live_count == 0 || l_tbl.live_count == 0 ||
        r_tbl.live_count == 0 || remaining == 0) {
      return;
    }

    const uint8_t s0 = factor_var_src_[0];
    const uint8_t s1 = factor_var_src_[1];
    const uint8_t s2 = factor_var_src_[2];
    const bool identity_order = (s0 == 0 && s1 == 1 && s2 == 2);

    auto emitTriple = [&](Id root, Id a, Id b, Id c) -> bool {
      const size_t old_sz = out.data.size();
      out.data.reserveAdditional(4);
      Id *dst = out.data.data() + old_sz;
      dst[0] = root;
      if (identity_order) {
        dst[1] = a;
        dst[2] = b;
        dst[3] = c;
      } else {
        const Id vals[3] = {a, b, c};
        dst[1] = vals[s0];
        dst[2] = vals[s1];
        dst[3] = vals[s2];
      }
      out.data.setSizeUnchecked(old_sz + 4);
      return --remaining > 0;
    };

    auto probeSortedC0 = [&](const OpRow *entries, OpCsrRange range, Id target_a,
                             uint32_t max_ver, Id root, Id b,
                             bool right_is_b) -> bool {
      const uint32_t cnt = range.end - range.begin;
      if (cnt == 0) {
        return true;
      }
      if (cnt <= 8) {
        for (uint32_t k = range.begin; k < range.end; ++k) {
          const OpRow &row = entries[k];
          if (row.c0 == target_a && row.version <= max_ver) {
            if (!emitTriple(root, target_a, right_is_b ? row.c1 : b,
                            right_is_b ? b : row.c1)) {
              return false;
            }
          }
        }
        return true;
      }
      const OpRow *lo = std::lower_bound(
          entries + range.begin, entries + range.end, target_a,
          [](const OpRow &row, Id val) { return row.c0.value() < val.value(); });
      const OpRow *hi = entries + range.end;
      for (const OpRow *p = lo; p < hi && p->c0 == target_a; ++p) {
        if (p->version <= max_ver) {
          if (!emitTriple(root, target_a, right_is_b ? p->c1 : b,
                          right_is_b ? b : p->c1)) {
            return false;
          }
        }
      }
      return true;
    };

    const uint32_t o_start = o_tbl.deltaStart(since_version);
    const uint32_t o_end = static_cast<uint32_t>(o_tbl.rows.size());
    const uint32_t l_start = l_tbl.deltaStart(since_version);
    const uint32_t l_end = static_cast<uint32_t>(l_tbl.rows.size());
    const uint32_t r_start = r_tbl.deltaStart(since_version);
    const uint32_t r_end = static_cast<uint32_t>(r_tbl.rows.size());

    // Branch 0: outer is NEW (or ALL when since_version == 0), left & right ALL
    if (o_start < o_end && remaining > 0) {
      const OpCsrIndex &l_ec = egraph.opByEClass(left_op_id);
      const OpCsrIndex &r_ec = egraph.opByEClassSortedC0(right_op_id);
      const OpRow *o_rows = o_tbl.rows.data();
      const OpRow *l_ent = l_ec.entries.data();
      const OpRow *r_ent = r_ec.entries.data();
      const bool l_dense = l_ec.is_dense;
      const bool r_dense = r_ec.is_dense;
      const OpCsrRange *l_dp = l_dense ? l_ec.dense_ranges.data() : nullptr;
      const OpCsrRange *r_dp = r_dense ? r_ec.dense_ranges.data() : nullptr;
      const size_t l_dsz = l_ec.dense_ranges.size();
      const size_t r_dsz = r_ec.dense_ranges.size();

      for (uint32_t i = o_start; i < o_end; ++i) {
        const OpRow &ro = o_rows[i];
        if (ro.isDead()) {
          continue;
        }
        OpCsrRange rl =
            l_dense
                ? (ro.c0.index() < l_dsz ? l_dp[ro.c0.index()] : OpCsrRange{})
                : l_ec.lookup(ro.c0);
        if (rl.empty()) {
          continue;
        }
        OpCsrRange rr =
            r_dense
                ? (ro.c1.index() < r_dsz ? r_dp[ro.c1.index()] : OpCsrRange{})
                : r_ec.lookup(ro.c1);
        if (rr.empty()) {
          continue;
        }
        for (uint32_t j = rl.begin; j < rl.end; ++j) {
          const OpRow &left_row = l_ent[j];
          if (!probeSortedC0(r_ent, rr, left_row.c0, UINT32_MAX, ro.eclass,
                             left_row.c1, /*right_is_b=*/false)) {
            return;
          }
        }
      }
    }

    if (since_version == 0 || remaining == 0) {
      return;
    }

    // Branch 1: left is NEW (> since), outer is OLD (<= since), right is ALL
    if (l_start < l_end && o_start > 0 && remaining > 0) {
      const OpCsrIndex &o_col0 = egraph.opByCol(outer_op_id, 0);
      const OpCsrIndex &r_ec = egraph.opByEClassSortedC0(right_op_id);
      const OpRow *l_rows = l_tbl.rows.data();
      const OpRow *o_ent = o_col0.entries.data();
      const OpRow *r_ent = r_ec.entries.data();
      const bool o_dense = o_col0.is_dense;
      const bool r_dense = r_ec.is_dense;
      const OpCsrRange *o_dp = o_dense ? o_col0.dense_ranges.data() : nullptr;
      const OpCsrRange *r_dp = r_dense ? r_ec.dense_ranges.data() : nullptr;
      const size_t o_dsz = o_col0.dense_ranges.size();
      const size_t r_dsz = r_ec.dense_ranges.size();

      for (uint32_t i = l_start; i < l_end; ++i) {
        const OpRow &left_row = l_rows[i];
        if (left_row.isDead()) {
          continue;
        }
        OpCsrRange ro_rng =
            o_dense ? (left_row.eclass.index() < o_dsz
                           ? o_dp[left_row.eclass.index()]
                           : OpCsrRange{})
                    : o_col0.lookup(left_row.eclass);
        for (uint32_t j = ro_rng.begin; j < ro_rng.end; ++j) {
          const OpRow &ro = o_ent[j];
          if (ro.version > since_version) {
            continue;
          }
          OpCsrRange rr =
              r_dense
                  ? (ro.c1.index() < r_dsz ? r_dp[ro.c1.index()] : OpCsrRange{})
                  : r_ec.lookup(ro.c1);
          if (!probeSortedC0(r_ent, rr, left_row.c0, UINT32_MAX, ro.eclass,
                             left_row.c1, /*right_is_b=*/false)) {
            return;
          }
        }
      }
    }

    // Branch 2: right is NEW (> since), outer is OLD (<= since), left is OLD (<= since)
    if (r_start < r_end && o_start > 0 && l_start > 0 && remaining > 0) {
      const OpCsrIndex &o_col1 = egraph.opByCol(outer_op_id, 1);
      const OpCsrIndex &l_ec = egraph.opByEClassSortedC0(left_op_id);
      const OpRow *r_rows = r_tbl.rows.data();
      const OpRow *o_ent = o_col1.entries.data();
      const OpRow *l_ent = l_ec.entries.data();
      const bool o_dense = o_col1.is_dense;
      const bool l_dense = l_ec.is_dense;
      const OpCsrRange *o_dp = o_dense ? o_col1.dense_ranges.data() : nullptr;
      const OpCsrRange *l_dp = l_dense ? l_ec.dense_ranges.data() : nullptr;
      const size_t o_dsz = o_col1.dense_ranges.size();
      const size_t l_dsz = l_ec.dense_ranges.size();

      for (uint32_t i = r_start; i < r_end; ++i) {
        const OpRow &right_row = r_rows[i];
        if (right_row.isDead()) {
          continue;
        }
        OpCsrRange ro_rng =
            o_dense ? (right_row.eclass.index() < o_dsz
                           ? o_dp[right_row.eclass.index()]
                           : OpCsrRange{})
                    : o_col1.lookup(right_row.eclass);
        for (uint32_t j = ro_rng.begin; j < ro_rng.end; ++j) {
          const OpRow &ro = o_ent[j];
          if (ro.version > since_version) {
            continue;
          }
          OpCsrRange rl =
              l_dense
                  ? (ro.c0.index() < l_dsz ? l_dp[ro.c0.index()] : OpCsrRange{})
                  : l_ec.lookup(ro.c0);
          if (!probeSortedC0(l_ent, rl, right_row.c0, since_version, ro.eclass,
                             right_row.c1, /*right_is_b=*/true)) {
            return;
          }
        }
      }
    }
  }

  template <typename A>
  void runBranch(const EGraph<L, A> &egraph,
                 const llvm::SmallVector<uint32_t, 8> &atom_op_ids,
                 llvm::SmallVector<Id, 16> &regs,
                 const llvm::SmallVector<uint8_t, 16> &prebound, int delta_idx,
                 uint32_t since_version, FlatMatches &out, size_t &remaining,
                 const WorkControl *control, bool sorted_classes) const {
    const size_t m = active_atoms_.size();
    llvm::SmallVector<uint8_t, 16> bound = prebound;
    llvm::SmallVector<uint8_t, 8> placed(m, 0);
    llvm::SmallVector<JoinStep, 8> plan;
    plan.reserve(m + bare_var_regs_.size());

    auto cardOf = [&](size_t i) -> size_t {
      const OpTable &tbl = egraph.opTable(atom_op_ids[i]);
      if (static_cast<int>(i) == delta_idx) {
        size_t s = tbl.deltaStart(since_version);
        return s < tbl.rows.size() ? tbl.rows.size() - s : 0;
      }
      return tbl.live_count;
    };

    // Track which registers are the `out_reg` of any already-placed atom:
    // probing a parent atom via a child's `out_reg` follows a tree edge
    // (small fanout!) and MUST be preferred over joining two siblings on a
    // shared high-degree leaf variable (e.g. Mul(a, b) x Mul(a, c) on a)!
    llvm::SmallVector<uint8_t, 16> is_placed_out_reg(num_regs_, 0);

    // Cost-based greedy relational join planner:
    // Priority:
    // 5. LookupExact (all children bound) -> O(1) hashcons lookup!
    // 4. ProbeEClass with a bound child (out_reg + child bound) -> tiny fanout!
    // 3. ProbeEClass (out_reg bound) -> small e-class fanout!
    // 2. ProbeCol along a parent-child edge (child is out_reg of a placed atom)!
    // 1. ProbeCol along a general shared variable (e.g. graph/MultiPattern join)!
    // 0. ScanTable -> pick smallest-cardinality remaining atom (delta or full)!
    for (size_t step_num = 0; step_num < m; ++step_num) {
      int best_i = -1;
      int best_prio = -1;
      size_t best_cost = std::numeric_limits<size_t>::max();
      JoinStep::Kind best_kind = JoinStep::Kind::ScanTable;
      uint32_t best_col = 0;

      for (size_t i = 0; i < m; ++i) {
        if (placed[i]) {
          continue;
        }
        const auto &atom = atoms_[active_atoms_[i]];
        const uint32_t arity = static_cast<uint32_t>(atom.child_regs.size());
        bool out_b = bound[atom.out_reg] != 0;
        bool all_ch_b = (arity > 0);
        int tree_ch_b = -1;
        int first_ch_b = -1;
        uint32_t n_ch_b = 0;
        for (uint32_t k = 0; k < arity; ++k) {
          if (bound[atom.child_regs[k]]) {
            if (first_ch_b < 0) {
              first_ch_b = static_cast<int>(k);
            }
            if (tree_ch_b < 0 && is_placed_out_reg[atom.child_regs[k]]) {
              tree_ch_b = static_cast<int>(k);
            }
            ++n_ch_b;
          } else {
            all_ch_b = false;
          }
        }

        int prio = 0;
        JoinStep::Kind kind = JoinStep::Kind::ScanTable;
        uint32_t col = 0;
        size_t cost = cardOf(i);

        if (all_ch_b && atom.exact_disc_match && delta_idx < 0) {
          prio = 5;
          kind = JoinStep::Kind::LookupExact;
          cost = 1;
        } else if (out_b && n_ch_b > 0) {
          prio = 4;
          kind = JoinStep::Kind::ProbeEClass;
        } else if (out_b) {
          prio = 3;
          kind = JoinStep::Kind::ProbeEClass;
        } else if (tree_ch_b >= 0) {
          prio = 2;
          kind = JoinStep::Kind::ProbeCol;
          col = static_cast<uint32_t>(tree_ch_b);
        } else if (first_ch_b >= 0) {
          prio = 1;
          kind = JoinStep::Kind::ProbeCol;
          col = static_cast<uint32_t>(first_ch_b);
        } else {
          prio = 0;
          kind = JoinStep::Kind::ScanTable;
        }

        if (prio > best_prio || (prio == best_prio && cost < best_cost)) {
          best_prio = prio;
          best_cost = cost;
          best_i = static_cast<int>(i);
          best_kind = kind;
          best_col = col;
        }
      }

      placed[best_i] = 1;
      const auto &atom = atoms_[active_atoms_[best_i]];
      is_placed_out_reg[atom.out_reg] = 1;
      JoinStep st;
      st.kind = best_kind;
      st.atom_idx = active_atoms_[best_i];
      st.op_id = atom_op_ids[best_i];
      st.probe_col = best_col;
      st.out_reg = atom.out_reg;
      st.arity = static_cast<uint32_t>(atom.child_regs.size());
      st.child_regs = atom.child_regs;
      st.needs_node_match = !atom.exact_disc_match;

      if (delta_idx >= 0) {
        if (best_i < delta_idx) {
          st.min_version = 0;
          st.max_version = since_version; // STRICT OLD
        } else if (best_i == delta_idx) {
          st.min_version = since_version + 1; // STRICT NEW
          st.max_version = UINT32_MAX;
        } else {
          st.min_version = 0;
          st.max_version = UINT32_MAX; // ALL
        }
      }

      st.check_out = bound[atom.out_reg] != 0;
      bound[atom.out_reg] = 1;
      st.c0_bound_for_bsearch =
          (best_kind == JoinStep::Kind::ProbeEClass && st.arity >= 1 &&
           bound[atom.child_regs[0]] != 0);

      st.check_child.resize(st.arity, 0);
      for (uint32_t k = 0; k < st.arity; ++k) {
        if (bound[atom.child_regs[k]]) {
          st.check_child[k] = 1;
        } else {
          bound[atom.child_regs[k]] = 1;
        }
      }
      plan.push_back(std::move(st));
    }

    for (uint32_t br : bare_var_regs_) {
      if (!bound[br]) {
        JoinStep st;
        st.kind = JoinStep::Kind::ScanClasses;
        st.out_reg = br;
        bound[br] = 1;
        plan.push_back(std::move(st));
      }
    }

    executePlan(egraph, plan, 0, regs, out, remaining, control, sorted_classes);
  }

  template <typename A>
  bool executePlan(const EGraph<L, A> &egraph,
                   const llvm::SmallVector<JoinStep, 8> &plan, size_t pc,
                   llvm::SmallVector<Id, 16> &regs, FlatMatches &out,
                   size_t &remaining, const WorkControl *control,
                   bool sorted_classes) const {
    if (pc >= plan.size()) {
      return emitMatch(regs, out, remaining);
    }
    if (control && control->poll()) {
      remaining = 0;
      return false;
    }

    const JoinStep &st = plan[pc];
    const auto &atom = atoms_[st.atom_idx];

    auto testAndRecurse = [&](const OpRow &row) -> bool {
      if (row.version < st.min_version || row.version > st.max_version) {
        return true;
      }
      if (st.needs_node_match &&
          !atom.node_template.matches(egraph.enodeAt(row.node_idx))) {
        return true;
      }
      if (st.check_out) {
        if (row.eclass != regs[st.out_reg]) {
          return true;
        }
      } else {
        regs[st.out_reg] = row.eclass;
      }
      for (uint32_t k = 0; k < st.arity; ++k) {
        Id val = egraph.enodeChild(row, k);
        if (st.check_child[k]) {
          if (val != regs[st.child_regs[k]]) {
            return true;
          }
        } else {
          regs[st.child_regs[k]] = val;
        }
      }
      return executePlan(egraph, plan, pc + 1, regs, out, remaining, control,
                         sorted_classes);
    };

    switch (st.kind) {
    case JoinStep::Kind::ScanTable: {
      const OpTable &table = egraph.opTable(st.op_id);
      const uint32_t start =
          st.min_version > 0 ? table.deltaStart(st.min_version - 1) : 0;
      const uint32_t end =
          st.max_version < UINT32_MAX
              ? std::min<uint32_t>(static_cast<uint32_t>(table.rows.size()),
                                   table.deltaStart(st.max_version))
              : static_cast<uint32_t>(table.rows.size());
      for (uint32_t i = start; i < end; ++i) {
        const OpRow &row = table.rows[i];
        if (row.isDead()) {
          continue;
        }
        if (!testAndRecurse(row)) {
          return false;
        }
      }
      return true;
    }
    case JoinStep::Kind::ProbeEClass: {
      const OpCsrIndex &csr = st.c0_bound_for_bsearch
                                  ? egraph.opByEClassSortedC0(st.op_id)
                                  : egraph.opByEClass(st.op_id);
      OpCsrRange range = csr.lookup(regs[st.out_reg]);
      if (range.empty()) {
        return true;
      }
      if (st.c0_bound_for_bsearch && range.size() > 8) {
        // Binary search on c0 inside the sorted e-class slice!
        const Id target_c0 = regs[st.child_regs[0]];
        const OpRow *base = csr.entries.data();
        const OpRow *lo = std::lower_bound(
            base + range.begin, base + range.end, target_c0,
            [](const OpRow &row, Id val) {
              return row.c0.value() < val.value();
            });
        const OpRow *hi = base + range.end;
        if (pc + 1 == plan.size() && !st.needs_node_match && !control) {
          if (st.arity == 2 && !st.check_child[1]) {
            const uint32_t r1 = st.child_regs[1];
            for (const OpRow *p = lo; p < hi && p->c0 == target_c0; ++p) {
              if (p->version < st.min_version || p->version > st.max_version) {
                continue;
              }
              regs[r1] = p->c1;
              if (!emitMatch(regs, out, remaining)) {
                return false;
              }
            }
            return true;
          }
          if (st.arity == 3 && !st.check_child[1] && !st.check_child[2]) {
            const uint32_t r1 = st.child_regs[1];
            const uint32_t r2 = st.child_regs[2];
            for (const OpRow *p = lo; p < hi && p->c0 == target_c0; ++p) {
              if (p->version < st.min_version || p->version > st.max_version) {
                continue;
              }
              regs[r1] = p->c1;
              regs[r2] = p->c2;
              if (!emitMatch(regs, out, remaining)) {
                return false;
              }
            }
            return true;
          }
        }
        for (const OpRow *p = lo; p < hi && p->c0 == target_c0; ++p) {
          if (!testAndRecurse(*p)) {
            return false;
          }
        }
        return true;
      }
      if (pc + 1 == plan.size() && !st.needs_node_match &&
          st.c0_bound_for_bsearch && !control) {
        if (st.arity == 2 && !st.check_child[1]) {
          const Id target_c0 = regs[st.child_regs[0]];
          const uint32_t r1 = st.child_regs[1];
          const OpRow *entries = csr.entries.data();
          for (uint32_t i = range.begin; i < range.end; ++i) {
            const OpRow &row = entries[i];
            if (row.c0 != target_c0 || row.version < st.min_version ||
                row.version > st.max_version) {
              continue;
            }
            regs[r1] = row.c1;
            if (!emitMatch(regs, out, remaining)) {
              return false;
            }
          }
          return true;
        }
        if (st.arity == 3 && !st.check_child[1] && !st.check_child[2]) {
          const Id target_c0 = regs[st.child_regs[0]];
          const uint32_t r1 = st.child_regs[1];
          const uint32_t r2 = st.child_regs[2];
          const OpRow *entries = csr.entries.data();
          for (uint32_t i = range.begin; i < range.end; ++i) {
            const OpRow &row = entries[i];
            if (row.c0 != target_c0 || row.version < st.min_version ||
                row.version > st.max_version) {
              continue;
            }
            regs[r1] = row.c1;
            regs[r2] = row.c2;
            if (!emitMatch(regs, out, remaining)) {
              return false;
            }
          }
          return true;
        }
      }
      for (uint32_t i = range.begin; i < range.end; ++i) {
        if (!testAndRecurse(csr.entries[i])) {
          return false;
        }
      }
      return true;
    }
    case JoinStep::Kind::ProbeCol: {
      const OpCsrIndex &csr = egraph.opByCol(st.op_id, st.probe_col);
      OpCsrRange range = csr.lookup(regs[st.child_regs[st.probe_col]]);
      for (uint32_t i = range.begin; i < range.end; ++i) {
        if (!testAndRecurse(csr.entries[i])) {
          return false;
        }
      }
      return true;
    }
    case JoinStep::Kind::LookupExact: {
      L node = atom.node_template;
      auto &&ch = node.childrenMut();
      for (uint32_t k = 0; k < st.arity; ++k) {
        ch[k] = regs[st.child_regs[k]];
      }
      auto found = egraph.lookupCanonical(node);
      if (!found) {
        return true;
      }
      if (st.check_out) {
        if (*found != regs[st.out_reg]) {
          return true;
        }
      } else {
        regs[st.out_reg] = *found;
      }
      return executePlan(egraph, plan, pc + 1, regs, out, remaining, control,
                         sorted_classes);
    }
    case JoinStep::Kind::ScanClasses: {
      const auto &ids =
          sorted_classes ? egraph.classIds() : egraph.activeClassIds();
      for (Id cid : ids) {
        regs[st.out_reg] = cid;
        if (!executePlan(egraph, plan, pc + 1, regs, out, remaining, control,
                         sorted_classes)) {
          return false;
        }
      }
      return true;
    }
    }
    return true;
  }

  template <typename A>
  bool runBareVarsOnly(const EGraph<L, A> &egraph,
                       llvm::SmallVector<Id, 16> &regs,
                       const llvm::SmallVector<uint8_t, 16> &prebound,
                       size_t idx, FlatMatches &out, size_t &remaining,
                       const WorkControl *control, bool sorted_classes) const {
    while (idx < bare_var_regs_.size() && prebound[bare_var_regs_[idx]]) {
      ++idx;
    }
    if (idx >= bare_var_regs_.size()) {
      return emitMatch(regs, out, remaining);
    }
    const auto &ids =
        sorted_classes ? egraph.classIds() : egraph.activeClassIds();
    const uint32_t r = bare_var_regs_[idx];
    for (Id cid : ids) {
      if (control && control->poll()) {
        remaining = 0;
        return false;
      }
      regs[r] = cid;
      if (!runBareVarsOnly(egraph, regs, prebound, idx + 1, out, remaining,
                           control, sorted_classes)) {
        return false;
      }
    }
    return true;
  }

  uint32_t num_regs_ = 0;
  uint32_t root_reg_ = 0;
  std::vector<Var> vars_;
  std::vector<uint32_t> var_regs_;
  std::vector<std::pair<uint32_t, uint32_t>> var_aliases_;
  std::vector<QueryAtom<L>> atoms_;
  std::vector<uint32_t> ground_atom_order_;
  std::vector<uint32_t> active_atoms_;
  std::vector<uint32_t> bare_var_regs_;

  // Pre-analyzed 2-atom linear chain specialization
  bool is_simple_two_atom_chain_ = false;
  uint32_t chain_outer_atom_ = 0;
  uint32_t chain_inner_atom_ = 0;
  uint32_t chain_join_col_ = 0;

  // Pre-analyzed 3-atom sibling-join specialization `(Op0 (Op1 ?a ?b) (Op2 ?a ?c))`
  bool is_factor_sibling_join_ = false;
  uint32_t factor_outer_slot_ = 0;
  uint32_t factor_left_slot_ = 0;
  uint32_t factor_right_slot_ = 0;
  uint8_t factor_var_src_[3] = {0, 1, 2};
};

} // namespace detail
} // namespace lotus::egraph
