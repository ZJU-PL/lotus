#pragma once

#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/SPDS/Pushdown.h"

#include <memory>
#include <optional>

namespace lotus::cfl::interleaved_dyck::spds {

enum class StackAcceptance { Empty, Any };
enum class DemandDirection { Auto, Post, Pre };
struct Options {
  // Empty/Empty is the balanced interleaved-Dyck upper bound. Any allows
  // unmatched opens at the endpoint, but NEVER an underflow or mismatched pop.
  StackAcceptance parentheses = StackAcceptance::Empty;
  StackAcceptance brackets = StackAcceptance::Empty;
  Limits limits; // per individual PDS saturation; no partial results on failure
};
struct Result {
  PairSet upper_bound;
  PairSet parenthesis_pairs;
  PairSet bracket_pairs;
  Statistics statistics; // totals over individual projection runs
  bool mayReach(Vertex from, Vertex to) const {
    return upper_bound.count({from, to}) != 0;
  }
};

class QueryResult {
public:
  Vertex anchor() const { return anchor_; }
  Direction direction() const { return direction_; }
  // Post: anchor -> vertex. Pre: vertex -> anchor. The anchor's stacks are
  // empty; Options controls acceptance at the queried vertex in either case.
  bool mayReach(Vertex vertex) const;
  bool parenthesisReachable(Vertex vertex) const;
  bool bracketReachable(Vertex vertex) const;
  // For post*, query a precise pair of endpoint stacks. For pre*, query
  // precise predecessor stacks. Both vectors are top first; no bottom needed.
  bool mayAccept(Vertex vertex, const std::vector<unsigned> &parentheses,
                 const std::vector<unsigned> &brackets) const;
  const Automaton<BooleanSemiring> &callAutomaton() const { return calls_; }
  const Automaton<BooleanSemiring> &fieldAutomaton() const { return fields_; }

private:
  friend class PreparedAnalysis;
  QueryResult(Vertex anchor, Direction direction, Options options,
              std::shared_ptr<const std::map<Vertex, State>> controls,
              Automaton<BooleanSemiring> calls,
              Automaton<BooleanSemiring> fields);
  bool reachable(const Automaton<BooleanSemiring> &automaton, Vertex vertex,
                 StackAcceptance acceptance) const;
  Vertex anchor_;
  Direction direction_;
  Options options_;
  std::shared_ptr<const std::map<Vertex, State>> controls_;
  Automaton<BooleanSemiring> calls_, fields_;
  std::uint64_t projection_microseconds_ = 0;
};

// Graph specialization of Definition 4 (POPL 2019, Spath/Ali/Bodden).
// Each projection is exact. Their conjunction is a sound upper bound, NOT
// an exact same-path two-stack reachability procedure.
class PreparedAnalysis {
public:
  QueryResult queryFrom(Vertex source) const;
  QueryResult queryTo(Vertex target) const;
  Result analyzeFrom(Vertex source) const;
  Result analyzeTo(Vertex target) const;
  Result analyzeAll() const;
  // Evaluate a Cartesian query scope without materializing all demands.
  Result analyzeBetween(const std::vector<Vertex> &sources,
                        const std::vector<Vertex> &targets,
                        DemandDirection direction = DemandDirection::Auto) const;
  Result
  analyzeDemands(const std::vector<Pair> &demands,
                 DemandDirection direction = DemandDirection::Auto) const;

private:
  friend class Solver;
  PreparedAnalysis(Options options, const Graph &graph,
                   std::map<Vertex, State> controls,
                   PushdownSystem<BooleanSemiring> calls,
                   PushdownSystem<BooleanSemiring> fields);
  QueryResult query(Vertex anchor, Direction direction,
                    bool slice_graph = true) const;
  std::optional<Graph> relevantGraph(Vertex anchor, Direction direction) const;
  Options options_;
  std::shared_ptr<const std::map<Vertex, State>> controls_;
  PushdownSystem<BooleanSemiring> calls_, fields_;
  std::vector<Vertex> vertices_;
  std::vector<Edge> edges_;
  std::vector<std::vector<State>> successors_, predecessors_;
  std::uint64_t projection_microseconds_ = 0;
};

class Solver {
public:
  explicit Solver(Options options = {}) : options_(options) {}
  PreparedAnalysis prepare(const Graph &graph) const;

private:
  Options options_;
};

} // namespace lotus::cfl::interleaved_dyck::spds
