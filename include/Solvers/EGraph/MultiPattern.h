#pragma once

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

template <typename L> class MultiPattern {
public:
  MultiPattern() = default;
  explicit MultiPattern(std::vector<std::pair<Var, Pattern<L>>> clauses)
      : clauses_(std::move(clauses)) {
    std::vector<std::pair<Var, PatternAst<L>>> asts;
    asts.reserve(clauses_.size());
    for (const auto &[var, pattern] : clauses_) {
      asts.emplace_back(var, pattern.ast());
    }
    program_ = std::make_shared<PatternProgram<L>>(
        PatternProgram<L>::compileFromMultiPattern(asts));
    query_ = std::make_shared<detail::RelationalQuery<L>>(
        detail::RelationalQuery<L>::compileFromMultiPattern(asts));
  }

  const detail::RelationalQuery<L> &relationalQuery() const { return *query_; }
  const std::vector<std::pair<Var, Pattern<L>>> &clauses() const {
    return clauses_;
  }

  static MultiPattern parse(std::string_view input) {
    std::vector<std::pair<Var, Pattern<L>>> clauses;
    std::string text(input);
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find(',', start);
      std::string clause =
          trim(end == std::string::npos
                   ? std::string_view(text).substr(start)
                   : std::string_view(text).substr(start, end - start));
      if (!clause.empty()) {
        std::vector<std::string> parts;
        size_t part_start = 0;
        while (part_start <= clause.size()) {
          size_t eq = clause.find('=', part_start);
          std::string part =
              trim(eq == std::string::npos
                       ? std::string_view(clause).substr(part_start)
                       : std::string_view(clause).substr(part_start,
                                                         eq - part_start));
          if (!part.empty()) {
            parts.push_back(std::move(part));
          }
          if (eq == std::string::npos) {
            break;
          }
          part_start = eq + 1;
        }

        if (parts.size() < 2) {
          throw std::runtime_error("Malformed multipattern clause");
        }

        Var var = Var::parse(parts.front());
        for (size_t i = 1; i < parts.size(); ++i) {
          clauses.emplace_back(var, Pattern<L>::parse(parts[i]));
        }
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return MultiPattern(std::move(clauses));
  }

  template <typename A>
  std::vector<Subst> search(const EGraph<L, A> &egraph,
                            const WorkControl *control = nullptr) const {
    if (clauses_.empty()) {
      throw std::runtime_error("empty multipattern");
    }
    if (clauses_.front().second.ast().size() == 1 &&
        clauses_.front().second.ast().items().front().isVar()) {
      throw std::runtime_error(
          "Bare pattern variable cannot be first in multipattern");
    }
    if (!egraph.clean()) {
      throw std::runtime_error("Tried to search a dirty e-graph");
    }
    if (!egraph.analysis().allowEMatchingCycles()) {
      std::vector<Subst> out;
      for (const auto &match : searchWithLimit(
               egraph, std::numeric_limits<size_t>::max(), control)) {
        out.insert(out.end(), match.substs.begin(), match.substs.end());
      }
      return out;
    }
    FlatMatches flat;
    query_->runFlat(egraph, flat, std::numeric_limits<size_t>::max(), 0,
                    control, true);
    return query_->materializeSubsts(flat);
  }

  template <typename A>
  std::vector<SearchMatches<L>>
  searchWithLimit(const EGraph<L, A> &egraph, size_t limit,
                  const WorkControl *control = nullptr) const {
    if (clauses_.empty()) {
      throw std::runtime_error("empty multipattern");
    }
    if (limit == 0) {
      return {};
    }

    if (clauses_.front().second.ast().size() == 1 &&
        clauses_.front().second.ast().items().front().isVar()) {
      throw std::runtime_error(
          "Bare pattern variable cannot be first in multipattern");
    }
    if (!egraph.clean()) {
      throw std::runtime_error("Tried to search a dirty e-graph");
    }
    if (!egraph.analysis().allowEMatchingCycles()) {
      return program_->runEClassesWithLimit(egraph, egraph.classIds(), limit,
                                            control, nullptr);
    }
    FlatMatches flat;
    query_->runFlat(egraph, flat, limit, 0, control, true);
    return query_->template materializeSearchMatches<A>(flat, nullptr);
  }

  template <typename A>
  std::optional<SearchMatches<L>>
  searchEClassWithLimit(const EGraph<L, A> &egraph, Id eclass, size_t limit,
                        const WorkControl *control = nullptr) const {
    if (clauses_.empty()) {
      throw std::runtime_error("empty multipattern");
    }
    if (limit == 0) {
      return std::nullopt;
    }
    if (clauses_.front().second.ast().size() == 1 &&
        clauses_.front().second.ast().items().front().isVar()) {
      throw std::runtime_error(
          "Bare pattern variable cannot be first in multipattern");
    }
    if (!egraph.clean()) {
      throw std::runtime_error("Tried to search a dirty e-graph");
    }
    std::vector<Subst> results;
    if (!egraph.analysis().allowEMatchingCycles()) {
      results = program_->runWithLimit(egraph, eclass, limit, control);
    } else {
      FlatMatches flat;
      query_->runSingleEClassFlat(egraph, eclass, flat, limit, control);
      results = query_->materializeSubsts(flat);
    }
    if (results.empty()) {
      return std::nullopt;
    }
    return SearchMatches<L>{eclass, std::move(results), nullptr};
  }

  template <typename A> size_t nMatches(const EGraph<L, A> &egraph) const {
    size_t total = 0;
    for (const auto &match :
         searchWithLimit(egraph, std::numeric_limits<size_t>::max())) {
      total += match.substs.size();
    }
    return total;
  }

  template <typename A>
  std::vector<Id> apply(EGraph<L, A> &egraph,
                        const std::vector<Subst> &matches) const {
    if (egraph.areExplanationsEnabled()) {
      throw std::runtime_error(
          "Multipattern application does not support explanations");
    }
    std::vector<Id> added;
    added.reserve(matches.size());
    for (const auto &match : matches) {
      Subst subst = match;
      for (size_t i = 0; i < clauses_.size(); ++i) {
        Id id = clauses_[i].second.apply(egraph, subst);
        if (const Id *existing = subst.get(clauses_[i].first)) {
          egraph.unite(id, *existing);
        } else {
          subst.insert(clauses_[i].first, id);
        }
        if (i == 0) {
          added.push_back(id);
        }
      }
    }
    return added;
  }

  std::vector<Var> vars() const {
    std::vector<Var> vars;
    for (const auto &[bound, pat] : clauses_) {
      vars.push_back(bound);
      for (const auto &var : pat.vars()) {
        vars.push_back(var);
      }
    }
    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    return vars;
  }

  std::vector<Var> applierVars() const {
    std::unordered_set<Var> bound;
    std::vector<Var> vars;
    for (const auto &[bound_var, pat] : clauses_) {
      for (const auto &var : pat.vars()) {
        if (!bound.count(var)) {
          vars.push_back(var);
        }
      }
      bound.insert(bound_var);
    }
    std::sort(vars.begin(), vars.end());
    vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
    return vars;
  }

  template <typename A>
  std::vector<Id>
  applyMatches(EGraph<L, A> &egraph,
               const std::vector<SearchMatches<L>> &matches) const {
    if (egraph.areExplanationsEnabled()) {
      throw std::runtime_error(
          "Multipattern application does not support explanations");
    }
    std::vector<Id> added;
    size_t expected = 0;
    for (const auto &match : matches) {
      expected += match.substs.size();
    }
    added.reserve(expected);
    for (const auto &match : matches) {
      for (const auto &subst : match.substs) {
        Subst current = subst;
        for (size_t i = 0; i < clauses_.size(); ++i) {
          Id id = clauses_[i].second.apply(egraph, current);
          if (auto existing = current.insert(clauses_[i].first, id)) {
            egraph.unite(id, *existing);
          }
          if (i == 0) {
            added.push_back(id);
          }
        }
      }
    }
    return added;
  }

private:
  std::vector<std::pair<Var, Pattern<L>>> clauses_;
  std::shared_ptr<PatternProgram<L>> program_;
  std::shared_ptr<detail::RelationalQuery<L>> query_;
};

} // namespace lotus::egraph
