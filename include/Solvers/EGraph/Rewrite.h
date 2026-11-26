#pragma once

#include "Solvers/EGraph/MultiPattern.h"

namespace lotus::egraph {

template <typename L, typename A> class Searcher;
template <typename L, typename A> class Applier;

template <typename L, typename A>
using Condition = std::function<bool(EGraph<L, A> &, Id, const Subst &)>;

namespace detail {

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

protected:
  RewriteBorrow(Symbol name, std::shared_ptr<const Searcher<L, A>> searcher)
      : name_(name), searcher_(std::move(searcher)) {}

  void validateBoundVars() const {
    detail::validateRewriteBoundVars(std::string_view(name_.view()), *searcher_,
                                     *applier_);
  }

  Symbol name_;
  std::shared_ptr<const Searcher<L, A>> searcher_;
  std::shared_ptr<const Applier<L, A>> applier_;
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
