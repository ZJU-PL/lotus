#pragma once

#include "CFL/ConditionalDyck/ConditionalDyck.h"

#include <deque>
#include <functional>
#include <queue>
#include <tuple>

namespace lotus::cfl::conditional_dyck::detail {

// Tail_i normalizes S -> Open_i S Close_i into two binary productions.
enum class Symbol { Neutral, Open, Close, S, L, R, D, Tail };
struct Fact {
  Node source = 0;
  Node target = 0;
  Symbol symbol = Symbol::S;
  CallSite call_site = 0;
  bool operator<(const Fact &other) const {
    return std::tie(source, target, symbol, call_site) <
           std::tie(other.source, other.target, other.symbol, other.call_site);
  }
  bool operator==(const Fact &other) const {
    return source == other.source && target == other.target &&
           symbol == other.symbol && call_site == other.call_site;
  }
};
struct Premise {
  // Guard is an externally supplied fact; Reach is discharged by saturation.
  enum class Kind { Guard, Reach };
  Kind kind = Kind::Reach;
  Condition condition = 0;
  Fact fact;
  static Premise guard(Condition condition) {
    return {Kind::Guard, condition, {}};
  }
  static Premise reach(Fact fact) { return {Kind::Reach, 0, fact}; }
  bool operator<(const Premise &other) const {
    return std::tie(kind, condition, fact) <
           std::tie(other.kind, other.condition, other.fact);
  }
  bool operator==(const Premise &other) const {
    return kind == other.kind && condition == other.condition &&
           fact == other.fact;
  }
};
using Premises = std::vector<Premise>;
struct ConditionalFact {
  Fact fact;
  Premises premises; // sorted, unique
};
using FactId = std::size_t;

Fact terminal(const Edge &edge);
std::optional<Language> language(Symbol symbol);

/// Port of summarize_f/summarize_k: premise-priority worklist, subsumption,
/// and a dependency map for bounded-premise bridging (paper Algorithms 1/3).
/// The storage and normalized Dyck rule dispatch are shared by both variants.
class Saturation {
public:
  explicit Saturation(std::optional<std::size_t> bound = std::nullopt)
      : bound_(bound) {}
  void seed(Fact fact, Premises premises = {});
  void seedNodes(const std::set<Node> &nodes);
  void run(const std::function<void(const Fact &)> &on_fact = {});
  const std::deque<ConditionalFact> &facts() const { return facts_; }
  const std::map<Fact, std::set<FactId>> &bridges() const { return bridges_; }

private:
  void derive(Fact fact, Premises premises,
              const std::vector<FactId> &dependencies);
  void join(FactId left, FactId right);
  bool subsumed(const Fact &fact, const Premises &premises) const;

  std::optional<std::size_t> bound_;
  std::deque<ConditionalFact> facts_;
  std::map<Fact, std::vector<FactId>> known_;
  std::map<Node, std::vector<FactId>> outgoing_, incoming_;
  std::map<Fact, std::set<FactId>> bridges_;
  using Work = std::pair<std::size_t, FactId>;
  std::priority_queue<Work, std::vector<Work>, std::greater<Work>> work_;
};

// Boundary summaries plus the transitive closure of their bridge dependencies.
std::vector<ConditionalFact>
extract(const Saturation &engine, const std::map<Node, unsigned> &boundaries);

} // namespace lotus::cfl::conditional_dyck::detail
