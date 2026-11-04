#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace lotus::cfl::classical::engines::subcubic {

using Node = std::size_t;

/// A pointer expression graph for Zhang et al., "Efficient Subcubic Alias
/// Analysis for C", OOPSLA 2014, DOI: 10.1145/2660193.2660213.
///
/// Nodes are dense IDs in [0, nodes). An assignment goes from the RHS to the
/// LHS expression; a dereference goes from e to *e. Reverse edges are implicit.
/// Every assignment target must have an incoming dereference (the PEG
/// condition used in Lemmas 5 and 6). Duplicate edges are harmless.
///
/// Multiple dereference successors are supported: all pairs of successors of
/// a common source seed M, as required by dbar V d with nullable V. Canonical
/// expression graphs have at most one such successor.
struct Problem {
  std::size_t nodes = 0;
  std::vector<std::pair<Node, Node>> assignments;
  std::vector<std::pair<Node, Node>> dereferences;
};

struct Options {
  /// Section 3.4: subtract already-known facts a machine word at a time.
  /// False uses element-by-element membership checks over the same storage.
  bool use_fast_sets = true;
  /// Solve weakly connected components of the original PEG independently.
  bool decompose_components = true;
  /// False stops after Algorithm 1; all value-relation operations then throw.
  bool compute_value_aliases = true;
};

struct Statistics {
  std::size_t nodes = 0;
  std::size_t assignment_edges = 0;
  std::size_t dereference_edges = 0;
  /// Number of independently processed graphs (one when decomposition is off).
  std::size_t connected_components = 0;
  std::size_t largest_component = 0;
  /// Counts include diagonal pairs and both orientations.
  std::size_t memory_aliases = 0;
  std::size_t value_aliases = 0;
  std::size_t processed_items = 0;
  std::size_t peak_worklist = 0;
  std::size_t difference_words = 0;
  std::size_t candidate_checks = 0;
};

/// Immutable all-pairs result. M and V are symmetric, but are NOT transitive.
/// V includes every empty path; M includes only actual dbar V d paths.
///
/// Results retain packed, component-local rows rather than expanding pairs.
/// Queries use the original Problem IDs and take constant time. Traversals
/// visit each ordered pair once, stopping and returning false on cancellation.
/// All node arguments are range checked. Value queries/counts/traversals throw
/// std::logic_error when compute_value_aliases was false.
class Result {
public:
  using NodeVisitor = std::function<bool(Node)>;
  using EdgeVisitor = std::function<bool(Node, Node)>;

  Result(Result &&) noexcept;
  Result &operator=(Result &&) noexcept;
  Result(const Result &) = delete;
  Result &operator=(const Result &) = delete;
  ~Result();

  std::size_t nodeCount() const;
  bool valueAliasesComputed() const;
  bool memoryAlias(Node source, Node target) const;
  bool valueAlias(Node source, Node target) const;
  std::size_t memoryAliasCount() const;
  std::size_t valueAliasCount() const;
  bool visitMemorySuccessors(Node source, const NodeVisitor &visitor) const;
  bool visitValueSuccessors(Node source, const NodeVisitor &visitor) const;
  bool visitMemoryAliases(const EdgeVisitor &visitor) const;
  bool visitValueAliases(const EdgeVisitor &visitor) const;
  const Statistics &statistics() const;

private:
  struct Impl;
  explicit Result(std::unique_ptr<Impl> impl);
  const Impl &get() const;
  std::unique_ptr<Impl> impl_;
  friend Result solve(const Problem &, const Options &);
};

/// Algorithms 1 and 2, with the fast-set differences from Section 3.4.
/// This is a specialized PEG analysis, not a general CFG-reachability solver.
/// Throws std::out_of_range for invalid node IDs and std::invalid_argument
/// when an assignment target has no incoming dereference.
Result solve(const Problem &problem, const Options &options = {});

} // namespace lotus::cfl::classical::engines::subcubic
