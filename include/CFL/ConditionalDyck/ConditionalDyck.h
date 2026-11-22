#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace lotus::cfl::conditional_dyck {

using Node = std::uint64_t;
using CallSite = std::uint64_t;
using Condition = std::uint64_t;

enum class EdgeKind { Neutral, Open, Close };

struct Label {
  EdgeKind kind = EdgeKind::Neutral;
  CallSite call_site = 0;
  static Label neutral() { return {}; }
  static Label open(CallSite site) { return {EdgeKind::Open, site}; }
  static Label close(CallSite site) { return {EdgeKind::Close, site}; }
  bool operator==(const Label &other) const;
  bool operator<(const Label &other) const;
};

struct Edge {
  Node source = 0;
  Node target = 0;
  Label label;
  bool operator==(const Edge &other) const;
  bool operator<(const Edge &other) const;
};

/// A directed graph over one family of typed parentheses. IDs need not be
/// dense.
class Graph {
public:
  void addNode(Node node);
  bool addEdge(Node source, Node target, Label label = Label::neutral());
  const std::set<Node> &nodes() const { return nodes_; }
  const std::vector<Edge> &edges() const { return edges_; }

private:
  std::set<Node> nodes_;
  std::set<Edge> edge_set_;
  std::vector<Edge> edges_;
};

/// A node can have several roles; markBoundary accumulates them.
enum class Boundary { Input, Output, Call, Return };

struct ConditionalInput {
  Edge edge;
  Condition condition = 0;
};

class Library {
public:
  Graph &graph() { return graph_; }
  const Graph &graph() const { return graph_; }
  void markBoundary(Node node, Boundary role);
  /// A client satisfying this condition enables the edge. Conditions can be
  /// shared by several edges, e.g. the argument/return edges of a call target.
  void addConditionalEdge(Node source, Node target, Label label,
                          Condition condition);
  /// Declare possible balanced reachability through a client callback. Also
  /// marks source as Call and target as Return; it does not assert
  /// reachability.
  void addCallback(Node source, Node target);

private:
  friend class Solver;
  Graph graph_;
  std::map<Node, unsigned> boundaries_;
  std::vector<ConditionalInput> conditional_;
  std::set<std::pair<Node, Node>> callbacks_;
};

/// S: balanced; L/R: unmatched opens/closes only; D: unmatched closes followed
/// by unmatched opens. All four languages include the empty path.
enum class Language { Balanced, Left, Right, Extended };

struct Options {
  /// nullopt selects ConCRA-f; positive k selects ConCRA-k. Zero is invalid.
  std::optional<std::size_t> max_premises = 1;
};

struct SummaryStatistics {
  std::size_t conditional_facts = 0;
  std::size_t summary_facts = 0;
  std::size_t bridging_facts = 0;
  std::size_t largest_premise_set = 0;
};

/// Immutable, owning summary; copies share read-only storage. It can be reused
/// concurrently for different clients, independently of the input Library.
class Summary {
public:
  SummaryStatistics statistics() const;
  Options options() const;

private:
  friend class Solver;
  struct Impl;
  explicit Summary(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};

class Reachability {
public:
  bool contains(Node source, Node target,
                Language language = Language::Balanced) const;
  const std::set<std::pair<Node, Node>> &pairs(Language language) const;

private:
  friend class Solver;
  std::array<std::set<std::pair<Node, Node>>, 4> pairs_;
};

class Solver {
public:
  Summary summarize(const Library &library, Options options = {}) const;

  /// Client contains its own nodes/edges and edges to library boundary nodes;
  /// it must not contain library-internal edges. satisfied enables conditional
  /// library edges. Callbacks are discharged by derived balanced reachability.
  /// Only client-to-client facts are exported. Invalid boundary crossings,
  /// undeclared callbacks/conditions and reused cross-scope call IDs throw
  /// std::invalid_argument. Library internals need not be supplied again.
  Reachability analyze(const Summary &summary, const Graph &client,
                       const std::set<Condition> &satisfied = {}) const;

  /// Ordinary whole-graph Dyck reachability, also useful as a baseline.
  Reachability solve(const Graph &graph) const;
};

} // namespace lotus::cfl::conditional_dyck
