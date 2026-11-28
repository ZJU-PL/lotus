#pragma once

#include "Solvers/EGraph/MultiPattern.h"

namespace lotus::egraph {

template <typename L, typename A> class Searcher;
template <typename L, typename A> class Applier;

template <typename L, typename A>
using Condition = std::function<bool(EGraph<L, A> &, Id, const Subst &)>;

namespace detail {

template <typename L> struct CompiledRhsStep {
  bool is_var = false;
  bool is_ground = false;
  bool is_binary_exact = false;
  uint32_t var_slot = 0;
  std::optional<L> node_template;
  llvm::SmallVector<uint32_t, 4> child_steps;
};

template <typename L> struct CompiledRhs {
  std::vector<CompiledRhsStep<L>> steps;
  bool single_var = false;
  bool all_binary_ops = false;
  uint32_t single_var_slot = 0;

  static CompiledRhs compile(const PatternAst<L> &ast,
                             const RelationalQuery<L> &query) {
    CompiledRhs out;
    out.steps.reserve(ast.size());
    bool all_bin = true;
    for (const auto &item : ast.items()) {
      CompiledRhsStep<L> st;
      if (item.isVar()) {
        st.is_var = true;
        st.var_slot = query.varSlot(item.var());
        st.is_ground = false;
      } else {
        const L &node = item.node();
        st.is_var = false;
        st.node_template.emplace(node);
        bool ground = true;
        st.child_steps.reserve(node.children().size());
        for (Id c : node.children()) {
          uint32_t ci = static_cast<uint32_t>(c.index());
          st.child_steps.push_back(ci);
          if (!out.steps[ci].is_ground) {
            ground = false;
          }
        }
        st.is_ground = ground;
        st.is_binary_exact =
            !ground && node.children().size() == 2 &&
            discriminantImpliesMatch(node);
        if (!ground && !st.is_binary_exact) {
          all_bin = false;
        }
      }
      out.steps.push_back(std::move(st));
    }
    if (out.steps.size() == 1 && out.steps[0].is_var) {
      out.single_var = true;
      out.single_var_slot = out.steps[0].var_slot;
      out.all_binary_ops = false;
    } else {
      out.all_binary_ops = all_bin && !out.steps.empty() &&
                           out.steps.back().is_binary_exact;
    }
    return out;
  }

  template <typename A>
  void accumulateReserveHint(EGraph<L, A> &egraph, size_t match_count,
                             llvm::SmallVectorImpl<size_t> &op_extra,
                             size_t &total_nodes,
                             size_t &total_classes) const {
    if (!all_binary_ops || match_count == 0) {
      return;
    }
    const size_t nsteps = steps.size();
    for (size_t s = 0; s < nsteps; ++s) {
      const auto &st = steps[s];
      if (!st.is_binary_exact) {
        continue;
      }
      uint32_t op_id = egraph.getOrCreateOpIdForNode(*st.node_template);
      if (!egraph.opUsesBinaryMemo(op_id)) {
        continue;
      }
      if (op_id >= op_extra.size()) {
        op_extra.resize(op_id + 1, 0);
      }
      op_extra[op_id] += match_count;
      total_nodes += match_count;
      if (s + 1 < nsteps) {
        total_classes += match_count;
      }
    }
  }

  template <typename A, typename StopCheck>
  size_t applyFlat(EGraph<L, A> &egraph, const FlatMatches &matches,
                   const Symbol &rule_name, size_t node_limit,
                   StopCheck &&should_stop) const {
    if (matches.empty() || steps.empty()) {
      return 0;
    }
    const uint32_t stride = matches.stride;
    const size_t count = matches.count();
    const Id *data = matches.data.data();
    size_t applied = 0;

    if (single_var) {
      const uint32_t voff = 1 + single_var_slot;
      for (size_t i = 0; i < count; ++i) {
        const Id *row = data + i * stride;
        if constexpr (std::is_same_v<A, NoAnalysis<L>>) {
          if (egraph.uniteFast(row[0], row[voff]).second) {
            ++applied;
          }
        } else {
          if (egraph.uniteChecked(row[0], row[voff], rule_name).second) {
            ++applied;
          }
        }
        if (((i + 1) & 4095u) == 0 && should_stop()) {
          break;
        }
      }
      return applied;
    }

    const size_t nsteps = steps.size();
    llvm::SmallVector<uint32_t, 16> local_op_ids(nsteps,
                                                 EGraph<L, A>::kInvalidOpId);
    llvm::SmallVector<Id, 16> ground_ids(nsteps, Id(0));
    llvm::SmallVector<Id, 16> step_ids(nsteps, Id(0));

    // Pre-materialize any ground RHS subtrees ONCE for the entire batch!
    for (size_t s = 0; s < nsteps; ++s) {
      const auto &st = steps[s];
      if (st.is_ground) {
        L node = *st.node_template;
        auto &&ch = node.childrenMut();
        for (size_t k = 0; k < ch.size(); ++k) {
          ch[k] = ground_ids[st.child_steps[k]];
        }
        ground_ids[s] =
            egraph.addCanonicalWithOpId(std::move(node), local_op_ids[s]);
      } else if (!st.is_var) {
        local_op_ids[s] = egraph.getOrCreateOpIdForNode(*st.node_template);
      }
    }

    // Ultra-fast step-major prefetched batch path for pure binary-op RHS rules
    // (covers Add/Mul comm, assoc, dist, factor, diff, integral, etc.)!
    if constexpr (std::is_same_v<A, NoAnalysis<L>>) {
      if (all_binary_ops && nsteps <= 16 &&
          !egraph.areExplanationsEnabled() &&
          egraph.opUsesBinaryMemo(local_op_ids[nsteps - 1])) {
        // Reserve capacity hints once before large batches to avoid mid-loop
        // reallocations without over-doubling hash tables past power-of-2 cliffs.
        if (count >= 4096 && egraph.totalSize() + count <= node_limit) {
          for (size_t s = 0; s < nsteps; ++s) {
            if (steps[s].is_binary_exact) {
              egraph.reserveBinaryBatch(local_op_ids[s], count,
                                        /*creates_classes=*/s + 1 < nsteps);
            }
          }
        }

        constexpr size_t kBatch = 32;
        Id batch_ids[kBatch][16];
        Id bc0[kBatch];
        Id bc1[kBatch];
        uint64_t bkey[kBatch];
        uint64_t bh[kBatch];

        for (size_t base = 0; base < count; base += kBatch) {
          const size_t bn = std::min(kBatch, count - base);

          for (size_t s = 0; s < nsteps; ++s) {
            const auto &st = steps[s];
            if (st.is_var) {
              const uint32_t voff = 1 + st.var_slot;
              for (size_t b = 0; b < bn; ++b) {
                batch_ids[b][s] = data[(base + b) * stride + voff];
              }
              continue;
            }
            if (st.is_ground) {
              const Id gid = ground_ids[s];
              for (size_t b = 0; b < bn; ++b) {
                batch_ids[b][s] = gid;
              }
              continue;
            }

            const uint32_t op_id = local_op_ids[s];
            const uint32_t cs0 = st.child_steps[0];
            const uint32_t cs1 = st.child_steps[1];
            const bool is_root = (s + 1 == nsteps);

            // Pass A: Canonicalize, hash, and prefetch all `bn` BinaryMemoTable
            // probes (and target ClassMeta for root writes!) concurrently.
            for (size_t b = 0; b < bn; ++b) {
              const Id c0 = egraph.findMut(batch_ids[b][cs0]);
              const Id c1 = egraph.findMut(batch_ids[b][cs1]);
              const uint64_t key = BinaryMemoTable::packKey(c0, c1);
              const uint64_t h = fastMix64(key);
              bc0[b] = c0;
              bc1[b] = c1;
              bkey[b] = key;
              bh[b] = h;
              egraph.prefetchBinary(op_id, h);
              if (is_root) {
                const Id lhs_ec = data[(base + b) * stride];
                egraph.prefetchUnionFind(lhs_ec);
                egraph.prefetchClassMeta(lhs_ec);
              }
            }

            // Pass B: Execute all `bn` probes/inserts in ONE hoisted batch call
            // (zero per-item capacity checks, zero per-item markOpDirty)!
            if (!is_root) {
              Id out_step[kBatch];
              egraph.applyBinaryStepBatch(op_id, bn, bc0, bc1, bkey, bh,
                                          out_step);
              for (size_t b = 0; b < bn; ++b) {
                batch_ids[b][s] = out_step[b];
              }
            } else {
              applied += egraph.applyBinaryRootBatch(
                  op_id, bn, data + base * stride, stride, bc0, bc1, bkey, bh);
            }
          }

          if (egraph.totalSize() > node_limit) {
            break;
          }
          if (((base + bn) & 4095u) == 0 && should_stop()) {
            break;
          }
        }
        return applied;
      }
    }

    const bool fuse_root =
        std::is_same_v<A, NoAnalysis<L>> && !steps.back().is_var &&
        !steps.back().is_ground;
    const size_t build_end = fuse_root ? (nsteps - 1) : nsteps;

    for (size_t i = 0; i < count; ++i) {
      const Id *row = data + i * stride;
      for (size_t s = 0; s < build_end; ++s) {
        const auto &st = steps[s];
        if (st.is_var) {
          step_ids[s] = row[1 + st.var_slot];
        } else if (st.is_ground) {
          step_ids[s] = ground_ids[s];
        } else {
          L node = *st.node_template;
          auto &&ch = node.childrenMut();
          const size_t arity = ch.size();
          if (arity == 2) {
            ch[0] = step_ids[st.child_steps[0]];
            ch[1] = step_ids[st.child_steps[1]];
          } else if (arity == 1) {
            ch[0] = step_ids[st.child_steps[0]];
          } else {
            for (size_t k = 0; k < arity; ++k) {
              ch[k] = step_ids[st.child_steps[k]];
            }
          }
          step_ids[s] = egraph.addCanonicalWithOpId(std::move(node),
                                                    local_op_ids[s]);
        }
      }
      if constexpr (std::is_same_v<A, NoAnalysis<L>>) {
        if (fuse_root) {
          const auto &st = steps[nsteps - 1];
          L node = *st.node_template;
          auto &&ch = node.childrenMut();
          const size_t arity = ch.size();
          if (arity == 2) {
            ch[0] = step_ids[st.child_steps[0]];
            ch[1] = step_ids[st.child_steps[1]];
          } else if (arity == 1) {
            ch[0] = step_ids[st.child_steps[0]];
          } else {
            for (size_t k = 0; k < arity; ++k) {
              ch[k] = step_ids[st.child_steps[k]];
            }
          }
          if (egraph.addAndUniteFast(row[0], std::move(node),
                                     local_op_ids[nsteps - 1])) {
            ++applied;
          }
        } else {
          if (egraph.uniteFast(row[0], step_ids[nsteps - 1]).second) {
            ++applied;
          }
        }
      } else {
        const Id rhs_id = step_ids[nsteps - 1];
        if (egraph.uniteChecked(row[0], rhs_id, rule_name).second) {
          ++applied;
        }
      }
      if (((i + 1) & 63u) == 0) {
        if (egraph.totalSize() > node_limit) {
          break;
        }
        if (((i + 1) & 4095u) == 0 && should_stop()) {
          break;
        }
      }
    }
    return applied;
  }
};

template <typename L> struct CompiledMultiRhsClause {
  uint32_t binder_slot = 0;
  bool binder_initially_bound = false;
  std::vector<CompiledRhsStep<L>> steps;
};

template <typename L> struct CompiledMultiRhs {
  uint32_t total_slots = 0;
  uint32_t query_var_count = 0;
  std::vector<CompiledMultiRhsClause<L>> clauses;

  static CompiledMultiRhs compile(const MultiPattern<L> &mp,
                                  const RelationalQuery<L> &query) {
    CompiledMultiRhs out;
    std::vector<Var> all_vars = query.vars();
    out.query_var_count = static_cast<uint32_t>(all_vars.size());

    auto findOrAddSlot = [&](const Var &v, bool &was_bound) -> uint32_t {
      const uint32_t sym = v.name().id();
      for (uint32_t i = 0; i < all_vars.size(); ++i) {
        if (all_vars[i].name().id() == sym) {
          was_bound = true;
          return i;
        }
      }
      was_bound = false;
      uint32_t slot = static_cast<uint32_t>(all_vars.size());
      all_vars.push_back(v);
      return slot;
    };

    for (const auto &[binder, pat] : mp.clauses()) {
      CompiledMultiRhsClause<L> clause;
      const auto &ast = pat.ast();
      clause.steps.reserve(ast.size());
      for (const auto &item : ast.items()) {
        CompiledRhsStep<L> st;
        if (item.isVar()) {
          bool ignored = false;
          st.is_var = true;
          st.var_slot = findOrAddSlot(item.var(), ignored);
          st.is_ground = false;
        } else {
          const L &node = item.node();
          st.is_var = false;
          st.node_template.emplace(node);
          bool ground = true;
          st.child_steps.reserve(node.children().size());
          for (Id c : node.children()) {
            uint32_t ci = static_cast<uint32_t>(c.index());
            st.child_steps.push_back(ci);
            if (!clause.steps[ci].is_ground) {
              ground = false;
            }
          }
          st.is_ground = ground;
        }
        clause.steps.push_back(std::move(st));
      }
      clause.binder_slot =
          findOrAddSlot(binder, clause.binder_initially_bound);
      out.clauses.push_back(std::move(clause));
    }
    out.total_slots = static_cast<uint32_t>(all_vars.size());
    return out;
  }

  template <typename A, typename StopCheck>
  size_t applyFlat(EGraph<L, A> &egraph, const FlatMatches &matches,
                   size_t node_limit, StopCheck &&should_stop) const {
    if (matches.empty() || clauses.empty()) {
      return 0;
    }
    const uint32_t stride = matches.stride;
    const size_t count = matches.count();
    const Id *data = matches.data.data();

    llvm::SmallVector<Id, 16> env(total_slots, Id(0));
    llvm::SmallVector<uint8_t, 16> env_bound(total_slots, 0);
    llvm::SmallVector<Id, 16> step_ids;
    size_t applied = 0;

    for (size_t i = 0; i < count; ++i) {
      const Id *row = data + i * stride;
      for (uint32_t v = 0; v < query_var_count; ++v) {
        env[v] = row[1 + v];
        env_bound[v] = 1;
      }
      for (uint32_t v = query_var_count; v < total_slots; ++v) {
        env_bound[v] = 0;
      }
      for (size_t c = 0; c < clauses.size(); ++c) {
        const auto &cl = clauses[c];
        step_ids.resize(cl.steps.size());
        for (size_t s = 0; s < cl.steps.size(); ++s) {
          const auto &st = cl.steps[s];
          if (st.is_var) {
            step_ids[s] = env[st.var_slot];
          } else {
            L node = *st.node_template;
            auto &&ch = node.childrenMut();
            for (size_t k = 0; k < ch.size(); ++k) {
              ch[k] = step_ids[st.child_steps[k]];
            }
            step_ids[s] = egraph.add(node);
          }
        }
        Id val = step_ids.back();
        if (env_bound[cl.binder_slot]) {
          egraph.unite(val, env[cl.binder_slot]);
        } else {
          env[cl.binder_slot] = val;
          env_bound[cl.binder_slot] = 1;
        }
        if (c == 0) {
          ++applied;
        }
      }
      if (((i + 1) & 63u) == 0) {
        if (egraph.totalSize() > node_limit) {
          break;
        }
        if (((i + 1) & 4095u) == 0 && should_stop()) {
          break;
        }
      }
    }
    return applied;
  }
};

template <typename T, typename = void>
struct HasVarsMethod : std::false_type {};

template <typename T>
struct HasVarsMethod<T, std::void_t<decltype(std::declval<const T &>().vars())>>
    : std::true_type {};

template <typename T>
inline std::vector<Var> conditionVars(const T &condition) {
  if constexpr (HasVarsMethod<T>::value) {
    return condition.vars();
  } else {
    return {};
  }
}

template <typename L, typename A, typename S>
inline std::vector<SearchMatches<L>>
searchEclassesWithLimit(const S &searcher, const EGraph<L, A> &egraph,
                        const std::vector<Id> &eclasses, size_t limit,
                        const WorkControl *control) {
  std::vector<SearchMatches<L>> matches;
  for (Id eclass : eclasses) {
    if (limit == 0 || (control && control->poll())) {
      break;
    }
    auto found = searcher.searchEClassWithLimit(egraph, eclass, limit, control);
    if (!found || found->substs.empty()) {
      continue;
    }
    limit -= std::min(limit, found->substs.size());
    matches.push_back(std::move(*found));
  }
  return matches;
}

inline std::vector<Var> sortedUnique(std::vector<Var> vars) {
  std::sort(vars.begin(), vars.end());
  vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
  return vars;
}

template <typename L, typename A>
inline void validateRewriteBoundVars(std::string_view name,
                                     const Searcher<L, A> &searcher,
                                     const Applier<L, A> &applier) {
  auto bound_vars = searcher.vars();
  for (const auto &var : applier.vars()) {
    if (std::find(bound_vars.begin(), bound_vars.end(), var) ==
        bound_vars.end()) {
      throw std::runtime_error("Rewrite " + std::string(name) +
                               " refers to unbound var " +
                               std::string(var.name().view()));
    }
  }
}

template <typename T>
inline std::shared_ptr<const T> borrowShared(const T &obj) {
  return std::shared_ptr<const T>(std::shared_ptr<const T>{}, &obj);
}

} // namespace detail

template <typename L, typename A = NoAnalysis<L>> class Searcher {
public:
  virtual ~Searcher() = default;

  virtual std::optional<SearchMatches<L>>
  searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass, size_t limit,
                        const WorkControl *control = nullptr) const = 0;

  virtual std::optional<SearchMatches<L>>
  searchEClass(const EGraph<L, A> &egraph, Id eclass) const {
    return searchEClassWithLimit(egraph, eclass,
                                 std::numeric_limits<size_t>::max(), nullptr);
  }

  virtual std::vector<SearchMatches<L>>
  searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                  const WorkControl *control = nullptr) const {
    return detail::searchEclassesWithLimit<L, A>(
        *this, egraph, egraph.classIds(), limit, control);
  }

  virtual std::vector<SearchMatches<L>>
  search(const EGraph<L, A> &egraph) const {
    return searchWithLimit(egraph, std::numeric_limits<size_t>::max());
  }

  virtual size_t nMatches(const EGraph<L, A> &egraph) const {
    size_t total = 0;
    for (const auto &match : search(egraph)) {
      total += match.substs.size();
    }
    return total;
  }

  virtual std::vector<Var> vars() const = 0;

  virtual const PatternAst<L> *getPatternAst() const { return nullptr; }

  virtual const detail::RelationalQuery<L> *relationalQuery() const {
    return nullptr;
  }
};

template <typename L, typename A = NoAnalysis<L>> class Applier {
public:
  virtual ~Applier() = default;

  virtual std::vector<Id> applyOne(EGraph<L, A> &egraph, Id eclass,
                                   const Subst &subst,
                                   const PatternAst<L> *match_ast,
                                   const Symbol &rule_name) const = 0;

  virtual std::vector<Id>
  applyMatches(EGraph<L, A> &egraph,
               const std::vector<SearchMatches<L>> &matches,
               const Symbol &rule_name) const {
    std::vector<Id> ids;
    size_t expected = 0;
    for (const auto &match : matches) {
      expected += match.substs.size();
    }
    ids.reserve(expected);
    for (const auto &match : matches) {
      const PatternAst<L> *match_ast = nullptr;
      if (egraph.areExplanationsEnabled()) {
        match_ast = match.ast.get();
      }
      for (const auto &subst : match.substs) {
        auto added =
            applyOne(egraph, match.eclass, subst, match_ast, rule_name);
        ids.insert(ids.end(), added.begin(), added.end());
      }
    }
    return ids;
  }

  virtual const PatternAst<L> *getPatternAst() const { return nullptr; }
  virtual bool isUnconditionalPatternApplier() const { return false; }
  virtual const MultiPattern<L> *getUnconditionalMultiPattern() const {
    return nullptr;
  }

  virtual std::vector<Var> vars() const { return {}; }
};

template <typename L, typename A = NoAnalysis<L>>
class PatternSearcher final : public Searcher<L, A> {
public:
  explicit PatternSearcher(Pattern<L> pattern) : pattern_(std::move(pattern)) {}

  std::optional<SearchMatches<L>>
  searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass, size_t limit,
                        const WorkControl *control = nullptr) const override {
    return pattern_.searchEClassWithLimit(egraph, eclass, limit, control);
  }

  std::vector<SearchMatches<L>>
  searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                  const WorkControl *control = nullptr) const override {
    return pattern_.searchWithLimit(egraph, limit, control);
  }

  std::vector<Var> vars() const override { return pattern_.vars(); }

  const PatternAst<L> *getPatternAst() const override {
    return &pattern_.ast();
  }

  const detail::RelationalQuery<L> *relationalQuery() const override {
    return &pattern_.relationalQuery();
  }

private:
  Pattern<L> pattern_;
};

template <typename L, typename A = NoAnalysis<L>>
class PatternApplier final : public Applier<L, A> {
public:
  explicit PatternApplier(Pattern<L> pattern) : pattern_(std::move(pattern)) {}

  std::vector<Id> applyOne(EGraph<L, A> &egraph, Id eclass, const Subst &subst,
                           const PatternAst<L> *match_ast,
                           const Symbol &rule_name) const override {
    if (match_ast && egraph.areExplanationsEnabled()) {
      auto [merged, changed] = egraph.unionInstantiations(
          *match_ast, pattern_.ast(), subst, rule_name);
      return changed ? std::vector<Id>{merged} : std::vector<Id>{};
    }

    Id rhs = pattern_.apply(egraph, subst);
    auto [merged, changed] = egraph.uniteChecked(eclass, rhs, rule_name);
    (void)merged;
    return changed ? std::vector<Id>{eclass} : std::vector<Id>{};
  }

  const PatternAst<L> *getPatternAst() const override {
    return &pattern_.ast();
  }

  bool isUnconditionalPatternApplier() const override { return true; }

  std::vector<Var> vars() const override { return pattern_.vars(); }

private:
  Pattern<L> pattern_;
};

template <typename L, typename A = NoAnalysis<L>>
class MultiPatternSearcher final : public Searcher<L, A> {
public:
  explicit MultiPatternSearcher(MultiPattern<L> pattern)
      : pattern_(std::move(pattern)) {}

  std::optional<SearchMatches<L>>
  searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass, size_t limit,
                        const WorkControl *control = nullptr) const override {
    return pattern_.searchEClassWithLimit(egraph, eclass, limit, control);
  }

  std::vector<SearchMatches<L>>
  searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                  const WorkControl *control = nullptr) const override {
    return pattern_.searchWithLimit(egraph, limit, control);
  }

  std::vector<Var> vars() const override { return pattern_.vars(); }

  const detail::RelationalQuery<L> *relationalQuery() const override {
    return &pattern_.relationalQuery();
  }

private:
  MultiPattern<L> pattern_;
};

template <typename L, typename A = NoAnalysis<L>>
class MultiPatternApplier final : public Applier<L, A> {
public:
  explicit MultiPatternApplier(MultiPattern<L> pattern)
      : pattern_(std::move(pattern)) {}

  std::vector<Id> applyOne(EGraph<L, A> &, Id, const Subst &,
                           const PatternAst<L> *,
                           const Symbol &) const override {
    throw std::runtime_error(
        "MultiPatternApplier does not support applyOne; use applyMatches");
  }

  std::vector<Id> applyMatches(EGraph<L, A> &egraph,
                               const std::vector<SearchMatches<L>> &matches,
                               const Symbol &) const override {
    return pattern_.applyMatches(egraph, matches);
  }

  const MultiPattern<L> *getUnconditionalMultiPattern() const override {
    return &pattern_;
  }

  std::vector<Var> vars() const override { return pattern_.applierVars(); }

private:
  MultiPattern<L> pattern_;
};

template <typename L, typename A = NoAnalysis<L>> class ConditionEqual {
public:
  ConditionEqual() = default;
  ConditionEqual(Pattern<L> lhs, Pattern<L> rhs)
      : lhs_(std::move(lhs)), rhs_(std::move(rhs)) {}

  static ConditionEqual parse(std::string_view lhs, std::string_view rhs) {
    return ConditionEqual(Pattern<L>::parse(lhs), Pattern<L>::parse(rhs));
  }

  bool operator()(EGraph<L, A> &egraph, Id, const Subst &subst) const {
    return egraph.addInstantiation(lhs_.ast(), subst) ==
           egraph.addInstantiation(rhs_.ast(), subst);
  }

  std::vector<Var> vars() const {
    auto vars = lhs_.vars();
    auto rhs_vars = rhs_.vars();
    vars.insert(vars.end(), rhs_vars.begin(), rhs_vars.end());
    return detail::sortedUnique(std::move(vars));
  }

private:
  Pattern<L> lhs_;
  Pattern<L> rhs_;
};

template <typename L, typename A = NoAnalysis<L>>
class ConditionalApplier final : public Applier<L, A> {
public:
  ConditionalApplier(Condition<L, A> condition,
                     std::shared_ptr<const Applier<L, A>> applier,
                     std::vector<Var> required_vars = {})
      : condition_(std::move(condition)), applier_(std::move(applier)),
        required_vars_(detail::sortedUnique(std::move(required_vars))) {}

  ConditionalApplier(Condition<L, A> condition, Pattern<L> applier,
                     std::vector<Var> required_vars = {})
      : ConditionalApplier(
            std::move(condition),
            std::make_shared<PatternApplier<L, A>>(std::move(applier)),
            std::move(required_vars)) {}

  std::vector<Id> applyOne(EGraph<L, A> &egraph, Id eclass, const Subst &subst,
                           const PatternAst<L> *match_ast,
                           const Symbol &rule_name) const override {
    if (!condition_(egraph, eclass, subst)) {
      return {};
    }
    return applier_->applyOne(egraph, eclass, subst, match_ast, rule_name);
  }

  std::vector<Id> applyMatches(EGraph<L, A> &egraph,
                               const std::vector<SearchMatches<L>> &matches,
                               const Symbol &rule_name) const override {
    std::vector<Id> ids;
    size_t expected = 0;
    for (const auto &match : matches) {
      expected += match.substs.size();
    }
    ids.reserve(expected);
    for (const auto &match : matches) {
      const PatternAst<L> *match_ast = nullptr;
      if (egraph.areExplanationsEnabled()) {
        match_ast = match.ast.get();
      }
      for (const auto &subst : match.substs) {
        if (!condition_(egraph, match.eclass, subst)) {
          continue;
        }
        auto added = applier_->applyOne(egraph, match.eclass, subst, match_ast,
                                        rule_name);
        ids.insert(ids.end(), added.begin(), added.end());
      }
    }
    return ids;
  }

  const PatternAst<L> *getPatternAst() const override {
    return applier_->getPatternAst();
  }

  std::vector<Var> vars() const override {
    auto vars = applier_->vars();
    vars.insert(vars.end(), required_vars_.begin(), required_vars_.end());
    return detail::sortedUnique(std::move(vars));
  }

private:
  Condition<L, A> condition_;
  std::shared_ptr<const Applier<L, A>> applier_;
  std::vector<Var> required_vars_;
};

template <typename L, typename A, typename C>
class ConditionalApplierT final : public Applier<L, A> {
public:
  ConditionalApplierT(C condition, std::shared_ptr<const Applier<L, A>> applier)
      : condition_(std::move(condition)), applier_(std::move(applier)) {}

  ConditionalApplierT(C condition, Pattern<L> applier)
      : ConditionalApplierT(
            std::move(condition),
            std::make_shared<PatternApplier<L, A>>(std::move(applier))) {}

  std::vector<Id> applyOne(EGraph<L, A> &egraph, Id eclass, const Subst &subst,
                           const PatternAst<L> *match_ast,
                           const Symbol &rule_name) const override {
    if (!condition_(egraph, eclass, subst)) {
      return {};
    }
    return applier_->applyOne(egraph, eclass, subst, match_ast, rule_name);
  }

  std::vector<Id> applyMatches(EGraph<L, A> &egraph,
                               const std::vector<SearchMatches<L>> &matches,
                               const Symbol &rule_name) const override {
    std::vector<Id> ids;
    size_t expected = 0;
    for (const auto &match : matches) {
      expected += match.substs.size();
    }
    ids.reserve(expected);
    for (const auto &match : matches) {
      const PatternAst<L> *match_ast = nullptr;
      if (egraph.areExplanationsEnabled()) {
        match_ast = match.ast.get();
      }
      for (const auto &subst : match.substs) {
        if (!condition_(egraph, match.eclass, subst)) {
          continue;
        }
        auto added = applier_->applyOne(egraph, match.eclass, subst, match_ast,
                                        rule_name);
        ids.insert(ids.end(), added.begin(), added.end());
      }
    }
    return ids;
  }

  const PatternAst<L> *getPatternAst() const override {
    return applier_->getPatternAst();
  }

  std::vector<Var> vars() const override {
    auto vars = applier_->vars();
    auto required_vars = detail::conditionVars(condition_);
    vars.insert(vars.end(), required_vars.begin(), required_vars.end());
    return detail::sortedUnique(std::move(vars));
  }

private:
  C condition_;
  std::shared_ptr<const Applier<L, A>> applier_;
};

template <typename L, typename A = NoAnalysis<L>> class RewriteBorrow {
public:
  RewriteBorrow() = default;

  RewriteBorrow(Symbol name, std::shared_ptr<const Searcher<L, A>> searcher,
                std::shared_ptr<const Applier<L, A>> applier)
      : name_(name), searcher_(std::move(searcher)),
        applier_(std::move(applier)) {
    validateBoundVars();
    compileFastPath();
  }

  template <typename S, typename Ap,
            typename = std::enable_if_t<std::is_base_of_v<Searcher<L, A>, S> &&
                                        std::is_base_of_v<Applier<L, A>, Ap>>>
  RewriteBorrow(Symbol name, const S &searcher, const Ap &applier)
      : RewriteBorrow(name, detail::borrowShared(searcher),
                      detail::borrowShared(applier)) {}

  const Symbol &name() const { return name_; }

  const Searcher<L, A> &searcher() const { return *searcher_; }
  const Applier<L, A> &applier() const { return *applier_; }

  std::vector<SearchMatches<L>> search(const EGraph<L, A> &egraph) const {
    return searcher_->search(egraph);
  }

  std::vector<SearchMatches<L>>
  searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                  const WorkControl *control = nullptr) const {
    return searcher_->searchWithLimit(egraph, limit, control);
  }

  std::vector<Id> apply(EGraph<L, A> &egraph,
                        const std::vector<SearchMatches<L>> &matches) const {
    return applier_->applyMatches(egraph, matches, name_);
  }

  bool hasFastFlatPath() const noexcept {
    return fast_query_ != nullptr &&
           (compiled_rhs_ != nullptr || compiled_multi_rhs_ != nullptr);
  }

  bool supportsSemiNaive() const noexcept {
    return compiled_rhs_ != nullptr || compiled_multi_rhs_ != nullptr;
  }

  const detail::RelationalQuery<L> *fastQuery() const noexcept {
    return fast_query_;
  }

  void accumulateFlatReserveHint(EGraph<L, A> &egraph,
                                 const FlatMatches &matches,
                                 llvm::SmallVectorImpl<size_t> &op_extra,
                                 size_t &total_nodes,
                                 size_t &total_classes) const {
    if (compiled_rhs_ && !matches.empty()) {
      compiled_rhs_->accumulateReserveHint(egraph, matches.count(), op_extra,
                                           total_nodes, total_classes);
    }
  }

  template <typename StopCheck>
  size_t applyFlat(EGraph<L, A> &egraph, const FlatMatches &matches,
                   size_t node_limit, StopCheck &&should_stop) const {
    if (compiled_rhs_) {
      return compiled_rhs_->applyFlat(egraph, matches, name_, node_limit,
                                      std::forward<StopCheck>(should_stop));
    }
    if (compiled_multi_rhs_) {
      return compiled_multi_rhs_->applyFlat(
          egraph, matches, node_limit, std::forward<StopCheck>(should_stop));
    }
    return 0;
  }

protected:
  RewriteBorrow(Symbol name, std::shared_ptr<const Searcher<L, A>> searcher)
      : name_(name), searcher_(std::move(searcher)) {}

  void validateBoundVars() const {
    detail::validateRewriteBoundVars(std::string_view(name_.view()), *searcher_,
                                     *applier_);
  }

  void compileFastPath() {
    fast_query_ = searcher_ ? searcher_->relationalQuery() : nullptr;
    if (!fast_query_ || !applier_) {
      return;
    }
    if (applier_->isUnconditionalPatternApplier()) {
      if (const auto *rhs_ast = applier_->getPatternAst()) {
        compiled_rhs_ = std::make_shared<detail::CompiledRhs<L>>(
            detail::CompiledRhs<L>::compile(*rhs_ast, *fast_query_));
      }
    } else if (const auto *rhs_mp = applier_->getUnconditionalMultiPattern()) {
      compiled_multi_rhs_ = std::make_shared<detail::CompiledMultiRhs<L>>(
          detail::CompiledMultiRhs<L>::compile(*rhs_mp, *fast_query_));
    }
  }

  Symbol name_;
  std::shared_ptr<const Searcher<L, A>> searcher_;
  std::shared_ptr<const Applier<L, A>> applier_;
  const detail::RelationalQuery<L> *fast_query_ = nullptr;
  std::shared_ptr<const detail::CompiledRhs<L>> compiled_rhs_;
  std::shared_ptr<const detail::CompiledMultiRhs<L>> compiled_multi_rhs_;
};

template <typename L, typename A = NoAnalysis<L>>
class Rewrite : public RewriteBorrow<L, A> {
public:
  using RewriteBorrow<L, A>::RewriteBorrow;

  Rewrite(Symbol name, Pattern<L> searcher, Pattern<L> applier,
          std::vector<Condition<L, A>> conditions = {})
      : RewriteBorrow<L, A>(name, std::make_shared<PatternSearcher<L, A>>(
                                      std::move(searcher))) {
    std::shared_ptr<const Applier<L, A>> wrapped =
        std::make_shared<PatternApplier<L, A>>(std::move(applier));
    for (auto it = conditions.rbegin(); it != conditions.rend(); ++it) {
      wrapped = std::make_shared<ConditionalApplier<L, A>>(*it, wrapped);
    }
    this->applier_ = std::move(wrapped);
    this->validateBoundVars();
    this->compileFastPath();
  }

  Rewrite(Symbol name, Pattern<L> searcher, ConditionalApplier<L, A> applier)
      : Rewrite(
            name, std::make_shared<PatternSearcher<L, A>>(std::move(searcher)),
            std::make_shared<ConditionalApplier<L, A>>(std::move(applier))) {}

  Rewrite(Symbol name, MultiPattern<L> searcher, MultiPattern<L> applier)
      : Rewrite(
            name,
            std::make_shared<MultiPatternSearcher<L, A>>(std::move(searcher)),
            std::make_shared<MultiPatternApplier<L, A>>(std::move(applier))) {}

  template <typename S, typename Ap,
            typename = std::enable_if_t<std::is_base_of_v<Searcher<L, A>, S> &&
                                        std::is_base_of_v<Applier<L, A>, Ap>>>
  Rewrite(Symbol name, S searcher, Ap applier)
      : Rewrite(name, std::make_shared<S>(std::move(searcher)),
                std::make_shared<Ap>(std::move(applier))) {}
};

template <typename L, typename A = NoAnalysis<L>>
inline Rewrite<L, A> makeRewrite(Symbol name, std::string_view lhs,
                                 std::string_view rhs,
                                 std::vector<Condition<L, A>> conditions = {}) {
  return Rewrite<L, A>(name, Pattern<L>::parse(lhs), Pattern<L>::parse(rhs),
                       std::move(conditions));
}

template <typename L, typename A = NoAnalysis<L>, typename C>
inline Rewrite<L, A> makeConditionalRewrite(Symbol name, std::string_view lhs,
                                            std::string_view rhs, C condition) {
  auto searcher = Pattern<L>::parse(lhs);
  auto bound_vars = searcher.vars();
  auto required_vars = detail::conditionVars(condition);
  for (const auto &var : required_vars) {
    if (std::find(bound_vars.begin(), bound_vars.end(), var) ==
        bound_vars.end()) {
      throw std::runtime_error("Rewrite condition refers to unbound variable");
    }
  }
  return Rewrite<L, A>(name, std::move(searcher),
                       ConditionalApplierT<L, A, std::decay_t<C>>(
                           std::move(condition), Pattern<L>::parse(rhs)));
}

template <typename L, typename A = NoAnalysis<L>>
inline Rewrite<L, A> makeMultiRewrite(Symbol name, MultiPattern<L> searcher,
                                      MultiPattern<L> applier) {
  return Rewrite<L, A>(name, std::move(searcher), std::move(applier));
}

template <typename L, typename A = NoAnalysis<L>, typename S, typename Ap,
          typename = std::enable_if_t<std::is_base_of_v<Searcher<L, A>, S> &&
                                      std::is_base_of_v<Applier<L, A>, Ap>>>
inline RewriteBorrow<L, A> makeRewriteBorrow(Symbol name, const S &searcher,
                                             const Ap &applier) {
  return RewriteBorrow<L, A>(name, searcher, applier);
}

template <typename L, typename A = NoAnalysis<L>, typename S,
          typename = std::enable_if_t<std::is_base_of_v<Searcher<L, A>, S>>>
inline RewriteBorrow<L, A> makeRewriteBorrow(Symbol name, const S &searcher,
                                             Pattern<L> applier) {
  return RewriteBorrow<L, A>(
      name, std::make_shared<S>(searcher),
      std::make_shared<PatternApplier<L, A>>(std::move(applier)));
}

template <typename L, typename A = NoAnalysis<L>, typename C>
inline RewriteBorrow<L, A>
makeConditionalRewriteBorrow(Symbol name, std::string_view lhs,
                             std::string_view rhs, const C &condition) {
  auto searcher = Pattern<L>::parse(lhs);
  auto bound_vars = searcher.vars();
  auto required_vars = detail::conditionVars(condition);
  for (const auto &var : required_vars) {
    if (std::find(bound_vars.begin(), bound_vars.end(), var) ==
        bound_vars.end()) {
      throw std::runtime_error("Rewrite condition refers to unbound variable");
    }
  }
  auto owned_searcher =
      std::make_shared<PatternSearcher<L, A>>(std::move(searcher));
  auto applier = std::make_shared<ConditionalApplierT<L, A, C>>(
      condition, Pattern<L>::parse(rhs));
  return RewriteBorrow<L, A>(name, std::move(owned_searcher),
                             std::move(applier));
}

} // namespace lotus::egraph
