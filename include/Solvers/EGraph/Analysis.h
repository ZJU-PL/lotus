#pragma once

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Util.h"

#include <optional>
#include <variant>

namespace lotus::egraph {

struct DidMerge {
  bool left_changed = false;
  bool right_changed = false;
};

struct Justification {
  enum class Kind {
    Congruence,
    Rule,
  };

  Kind kind = Kind::Congruence;
  Symbol rule;

  static Justification congruence() { return Justification{}; }

  static Justification ruleJustification(Symbol reason) {
    Justification justification;
    justification.kind = Kind::Rule;
    justification.rule = reason;
    return justification;
  }

  bool isCongruence() const { return kind == Kind::Congruence; }
  bool isRule() const { return kind == Kind::Rule; }

  friend bool operator==(const Justification &lhs, const Justification &rhs) {
    return lhs.kind == rhs.kind && lhs.rule == rhs.rule;
  }

  friend bool operator!=(const Justification &lhs, const Justification &rhs) {
    return !(lhs == rhs);
  }
};

template <typename L> struct NoAnalysis;
template <typename L, typename AnalysisT = NoAnalysis<L>> class EGraph;

template <typename L> struct NoAnalysis {
  using Data = std::monostate;

  template <typename AnyAnalysis>
  static Data make(EGraph<L, AnyAnalysis> &, const L &, Id) {
    return {};
  }

  template <typename AnyAnalysis>
  static Data remake(EGraph<L, AnyAnalysis> &, const L &, Id) {
    return {};
  }

  template <typename AnyAnalysis>
  void preUnion(const EGraph<L, AnyAnalysis> &, Id, Id,
                const std::optional<Justification> &) const {}

  DidMerge merge(Data &, Data) { return {}; }

  template <typename AnyAnalysis>
  void modify(EGraph<L, AnyAnalysis> &, Id) const {}

  bool allowEMatchingCycles() const { return true; }
};

template <typename T>
inline DidMerge mergeMax(T &target, T source) {
  if (source > target) {
    target = std::move(source);
    return {true, false};
  }
  if (source == target) {
    return {false, false};
  }
  return {false, true};
}

template <typename T>
inline DidMerge mergeMin(T &target, T source) {
  if (source < target) {
    target = std::move(source);
    return {true, false};
  }
  if (source == target) {
    return {false, false};
  }
  return {false, true};
}

template <typename T, typename F>
inline DidMerge mergeOption(std::optional<T> &target, std::optional<T> source,
                            F &&on_both) {
  if (!source) {
    return {false, false};
  }
  if (!target) {
    target = std::move(source);
    return {true, false};
  }
  return on_both(*target, *source);
}

} // namespace lotus::egraph
