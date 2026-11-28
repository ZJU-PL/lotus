#pragma once

#include "Solvers/EGraph/EGraph.h"
#include "Solvers/EGraph/Subst.h"

#include <variant>

namespace lotus::egraph {

template <typename L> class PatternProgram;
namespace detail {
template <typename L> class RelationalQuery;
}

template <typename L> struct ENodeOrVarDiscriminant {
  std::variant<typename L::Discriminant, Var> value;

  friend bool operator==(const ENodeOrVarDiscriminant &lhs,
                         const ENodeOrVarDiscriminant &rhs) {
    return lhs.value == rhs.value;
  }

  friend bool operator!=(const ENodeOrVarDiscriminant &lhs,
                         const ENodeOrVarDiscriminant &rhs) {
    return !(lhs == rhs);
  }
};

template <typename L> struct ENodeOrVar {
  std::variant<L, Var> value;

  ENodeOrVar() = delete;
  explicit ENodeOrVar(const L &node) : value(node) {}
  explicit ENodeOrVar(const Var &var) : value(var) {}

  bool isVar() const { return std::holds_alternative<Var>(value); }
  bool isNode() const { return std::holds_alternative<L>(value); }

  const Var &var() const { return std::get<Var>(value); }
  const L &node() const { return std::get<L>(value); }
  L &node() { return std::get<L>(value); }

  llvm::ArrayRef<Id> children() const {
    if (isVar()) {
      return {};
    }
    return node().children();
  }

  llvm::MutableArrayRef<Id> childrenMut() {
    if (isVar()) {
      throw std::runtime_error("Pattern variable has no mutable children");
    }
    return node().childrenMut();
  }

  bool matches(const ENodeOrVar<L> &other) const {
    if (isVar() || other.isVar()) {
      return false;
    }
    return node().matches(other.node());
  }

  ENodeOrVarDiscriminant<L> discriminant() const {
    if (isVar()) {
      return ENodeOrVarDiscriminant<L>{var()};
    }
    return ENodeOrVarDiscriminant<L>{node().discriminant()};
  }

  template <typename F> ENodeOrVar mapChildren(F &&fn) const {
    if (isVar()) {
      return *this;
    }
    return ENodeOrVar(node().mapChildren(std::forward<F>(fn)));
  }

  friend bool operator==(const ENodeOrVar<L> &lhs, const ENodeOrVar<L> &rhs) {
    return lhs.value == rhs.value;
  }

  friend bool operator!=(const ENodeOrVar<L> &lhs, const ENodeOrVar<L> &rhs) {
    return !(lhs == rhs);
  }
};

template <typename L> struct LanguageOps<ENodeOrVar<L>> {
  static std::optional<ENodeOrVar<L>> fromOp(std::string_view op,
                                             const std::vector<Id> &children) {
    if (op.size() > 1 && op.front() == '?') {
      if (!children.empty()) {
        return std::nullopt;
      }
      return ENodeOrVar<L>(Var::parse(op));
    }
    auto node = LanguageOps<L>::fromOp(op, children);
    if (!node) {
      return std::nullopt;
    }
    return ENodeOrVar<L>(*node);
  }

  static std::string display(const ENodeOrVar<L> &item) {
    return item.isVar() ? std::string(item.var().name().view())
                        : displayNode(item.node());
  }
};

template <typename L> using PatternAst = RecExpr<ENodeOrVar<L>>;

struct WorkControl {
  std::function<bool()> should_stop;
  size_t check_interval = 1024;
  mutable size_t work = 0;

  bool poll() const {
    if (!should_stop) {
      return false;
    }
    ++work;
    return work % std::max<size_t>(1, check_interval) == 0 && should_stop();
  }
};

template <typename L> struct SearchMatches {
  Id eclass;
  std::vector<Subst> substs;
  std::shared_ptr<const PatternAst<L>> ast;
};

template <typename L> class Pattern {
public:
  Pattern() = default;
  explicit Pattern(PatternAst<L> ast);
  explicit Pattern(const RecExpr<L> &expr);

  static Pattern parse(std::string_view input) {
    return Pattern(PatternAst<L>::parse(input));
  }

  const PatternAst<L> &ast() const { return *ast_; }

  PatternAst<L> alphaRename() const {
    std::unordered_map<Var, Var> vars;
    PatternAst<L> renamed;

    auto mkvar = [](size_t i) {
      static constexpr const char *kNames[] = {"?x", "?y", "?z", "?w"};
      if (i < std::size(kNames)) {
        return Var::parse(kNames[i]);
      }
      return Var::parse("?v" + std::to_string(i - std::size(kNames)));
    };

    for (const auto &node : ast_->items()) {
      if (!node.isVar()) {
        renamed.add(node);
        continue;
      }
      auto [it, inserted] = vars.try_emplace(node.var(), mkvar(vars.size()));
      (void)inserted;
      renamed.add(ENodeOrVar<L>(it->second));
    }
    return renamed;
  }

  std::vector<Var> vars() const {
    std::vector<Var> vars;
    for (const auto &item : ast_->items()) {
      if (item.isVar()) {
        if (std::find(vars.begin(), vars.end(), item.var()) == vars.end()) {
          vars.push_back(item.var());
        }
      }
    }
    return vars;
  }

  template <typename A>
  std::vector<SearchMatches<L>>
  search(const EGraph<L, A> &egraph,
         const WorkControl *control = nullptr) const {
    return searchWithLimit(egraph, std::numeric_limits<size_t>::max(), control);
  }

  template <typename A>
  std::vector<SearchMatches<L>>
  searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                  const WorkControl *control = nullptr) const;

  template <typename A>
  std::optional<SearchMatches<L>>
  searchEClass(const EGraph<L, A> &egraph, Id eclass,
               const WorkControl *control = nullptr) const {
    return searchEClassWithLimit(egraph, eclass,
                                 std::numeric_limits<size_t>::max(), control);
  }

  template <typename A>
  std::optional<SearchMatches<L>>
  searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass, size_t limit,
                        const WorkControl *control = nullptr) const;

  const detail::RelationalQuery<L> &relationalQuery() const { return *query_; }

  template <typename A> size_t nMatches(const EGraph<L, A> &egraph) const {
    size_t total = 0;
    for (const auto &match : search(egraph)) {
      total += match.substs.size();
    }
    return total;
  }

  template <typename A>
  Id apply(EGraph<L, A> &egraph, const Subst &subst) const {
    llvm::SmallVector<Id, 16> ids;
    ids.reserve(ast_->size());
    for (const auto &item : ast_->items()) {
      if (item.isVar()) {
        ids.push_back(subst.at(item.var()));
      } else {
        auto materialized = item.node().mapChildren(
            [&](Id child) { return ids[child.index()]; });
        ids.push_back(egraph.add(materialized));
      }
    }
    return ids.back();
  }

  template <typename A>
  bool containsEClass(const EGraph<L, A> &egraph, const Subst &subst,
                      Id target) const {
    return containsEClass(egraph, ast_->root(), subst, egraph.find(target));
  }

private:
  void compact() {
    std::vector<Id> ids;
    std::unordered_map<ENodeOrVar<L>, Id> seen;
    std::vector<ENodeOrVar<L>> compacted;
    compacted.reserve(ast_->size());

    for (size_t i = 0; i < ast_->size(); ++i) {
      ENodeOrVar<L> node = ast_->items()[i].mapChildren(
          [&](Id id) { return ids.at(id.index()); });

      auto [it, inserted] =
          seen.try_emplace(node, Id::fromIndex(compacted.size()));
      if (inserted) {
        compacted.push_back(node);
      }
      ids.push_back(it->second);
    }

    ast_ = std::make_shared<PatternAst<L>>(std::move(compacted));
  }

  template <typename A>
  Id applyNode(EGraph<L, A> &egraph, Id ast_id, const Subst &subst) const {
    const auto &item = (*ast_)[ast_id];
    if (item.isVar()) {
      return subst.at(item.var());
    }

    const auto &node = item.node();
    auto materialized = node.mapChildren(
        [&](Id child) { return applyNode(egraph, child, subst); });
    return egraph.add(materialized);
  }

  template <typename A>
  bool containsEClass(const EGraph<L, A> &egraph, Id ast_id, const Subst &subst,
                      Id target) const {
    const auto &item = (*ast_)[ast_id];
    if (item.isVar()) {
      return egraph.find(subst.at(item.var())) == target;
    }

    for (Id child : item.node().children()) {
      if (containsEClass(egraph, child, subst, target)) {
        return true;
      }
    }
    return false;
  }

  std::shared_ptr<PatternAst<L>> ast_ = std::make_shared<PatternAst<L>>();
  std::shared_ptr<PatternProgram<L>> program_;
  std::shared_ptr<detail::RelationalQuery<L>> query_;
};

template <typename L> inline Pattern<L> pattern(std::string_view text) {
  return Pattern<L>::parse(text);
}

} // namespace lotus::egraph

#include "Solvers/EGraph/PatternMachine.h"
#include "Solvers/EGraph/RelationalQuery.h"

namespace lotus::egraph {

template <typename L>
inline Pattern<L>::Pattern(PatternAst<L> ast)
    : ast_(std::make_shared<PatternAst<L>>(std::move(ast))) {
  compact();
  program_ = std::make_shared<PatternProgram<L>>(
      PatternProgram<L>::compileFromPattern(*this));
  query_ = std::make_shared<detail::RelationalQuery<L>>(
      detail::RelationalQuery<L>::compileFromPattern(*ast_));
}

template <typename L> inline Pattern<L>::Pattern(const RecExpr<L> &expr) {
  PatternAst<L> ast;
  for (const auto &node : expr.items()) {
    ast.add(ENodeOrVar<L>(node));
  }
  ast_ = std::make_shared<PatternAst<L>>(std::move(ast));
  compact();
  program_ = std::make_shared<PatternProgram<L>>(
      PatternProgram<L>::compileFromPattern(*this));
  query_ = std::make_shared<detail::RelationalQuery<L>>(
      detail::RelationalQuery<L>::compileFromPattern(*ast_));
}

template <typename L>
template <typename A>
inline std::vector<SearchMatches<L>>
Pattern<L>::searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                            const WorkControl *control) const {
  if (limit == 0 || ast_->empty()) {
    return {};
  }
  if (!egraph.clean()) {
    throw std::runtime_error("Tried to search a dirty e-graph");
  }

  auto match_ast = egraph.areExplanationsEnabled()
                       ? std::static_pointer_cast<const PatternAst<L>>(ast_)
                       : nullptr;

  if (!egraph.analysis().allowEMatchingCycles()) {
    const auto &root = ast_->items().back();
    const std::vector<Id> *candidates =
        root.isVar() ? &egraph.classIds()
                     : &egraph.classesForOp(root.node().discriminant());
    return program_->runEClassesWithLimit(egraph, *candidates, limit, control,
                                          std::move(match_ast));
  }

  FlatMatches flat;
  query_->runFlat(egraph, flat, limit, /*since_version=*/0, control,
                  /*sorted_classes=*/true);
  return query_->template materializeSearchMatches<A>(flat,
                                                      std::move(match_ast));
}

template <typename L>
template <typename A>
inline std::optional<SearchMatches<L>>
Pattern<L>::searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass,
                                  size_t limit,
                                  const WorkControl *control) const {
  if (limit == 0 || ast_->empty()) {
    return std::nullopt;
  }
  if (!egraph.clean()) {
    throw std::runtime_error("Tried to search a dirty e-graph");
  }

  std::vector<Subst> substs;
  if (!egraph.analysis().allowEMatchingCycles()) {
    substs = program_->runWithLimit(egraph, eclass, limit, control);
  } else {
    FlatMatches flat;
    query_->runSingleEClassFlat(egraph, eclass, flat, limit, control);
    substs = query_->materializeSubsts(flat);
  }
  if (substs.empty()) {
    return std::nullopt;
  }
  SearchMatches<L> matches{
      eclass, std::move(substs),
      egraph.areExplanationsEnabled()
          ? std::static_pointer_cast<const PatternAst<L>>(ast_)
          : nullptr};
  return matches;
}

} // namespace lotus::egraph

template <typename L> struct std::hash<lotus::egraph::ENodeOrVar<L>> {
  size_t operator()(const lotus::egraph::ENodeOrVar<L> &value) const noexcept {
    size_t seed = 0;
    if (value.isVar()) {
      lotus::egraph::hashCombine(seed, value.var());
      return seed;
    }
    seed ^= lotus::egraph::LanguageHash<L>{}(value.node()) +
            0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
  }
};

template <typename L>
struct std::hash<lotus::egraph::ENodeOrVarDiscriminant<L>> {
  size_t operator()(
      const lotus::egraph::ENodeOrVarDiscriminant<L> &value) const noexcept {
    size_t seed = value.value.index();
    if (std::holds_alternative<typename L::Discriminant>(value.value)) {
      lotus::egraph::hashCombine(
          seed, std::get<typename L::Discriminant>(value.value));
    } else {
      lotus::egraph::hashCombine(seed,
                                 std::get<lotus::egraph::Var>(value.value));
    }
    return seed;
  }
};
