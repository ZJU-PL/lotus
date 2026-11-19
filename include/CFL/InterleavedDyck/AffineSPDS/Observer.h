#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Matrix.h"
#include "CFL/InterleavedDyck/Core/Graph.h"

#include <iosfwd>
#include <map>
#include <utility>

namespace lotus::cfl::interleaved_dyck::affine {
struct EdgeLess {
  bool operator()(const Edge &a, const Edge &b) const;
};
struct ObserverOptions {
  // This limits observed history features, never path length or stack depth.
  std::size_t max_events = 4;
  std::size_t max_order_pairs = 2;
  std::size_t max_matrix_dimension =
      0; // Checked before automatic matrix allocation.
};

// A total, shared map from ORIGINAL graph edges to matrices. Missing edges
// carry identity. Duplicate Graph edges have the same event identity, as in
// Core::Graph. No synthetic PDS transition becomes a new observed event.
class HistoryObserver {
public:
  explicit HistoryObserver(std::size_t dimension = 1);
  std::size_t dimension() const { return identity_.dimension(); }
  void set(const Edge &edge, const Matrix &matrix);
  const Matrix &matrix(const Edge &edge) const;
  void validate(const Graph &graph) const;
  // Returns a compact joint layout only when every assigned matrix respects
  // the declared blocks. Cross-block custom observers use dense coordinates.
  std::shared_ptr<const MatrixLayout> compactLayout() const;
  const std::map<Edge, Matrix, EdgeLess> &assignments() const {
    return matrices_;
  }
  // Disjoint diagonal blocks, for joint-vs-independent readout ablations.
  // Custom observers have one block; directSum preserves component boundaries.
  const std::vector<std::pair<std::size_t, std::size_t>> &blocks() const {
    return blocks_;
  }
  Matrix trace(const std::vector<Edge> &edges) const;

  static HistoryObserver parity(const std::vector<Edge> &events);
  static HistoryObserver orderedPair(const std::vector<Edge> &first,
                                     const std::vector<Edge> &second);
  static HistoryObserver cyclic(const std::vector<Edge> &events,
                                std::size_t modulus);
  static HistoryObserver
  directSum(const std::vector<HistoryObserver> &observers);
  // Symmetric 2*groups parity blocks (dimension 4*groups): paren IDs mod groups
  // in blocks [0, groups) and bracket IDs mod groups in [groups, 2*groups).
  static HistoryObserver labelParity(const Graph &graph, unsigned groups = 8);
  // Single-sided groups parity blocks (dimension 2*groups, e.g. 16 for groups=8):
  // observes only brackets (for P_call) or only parentheses (for P_field).
  static HistoryObserver singleSidedParity(const Graph &graph,
                                           unsigned groups = 8,
                                           bool observe_brackets = true);
  // Stable graph-only policy: one non-default edge per branch first, then
  // remaining edges; finite budgets, no random hashes or client-written laws.
  static HistoryObserver automatic(const Graph &graph,
                                   ObserverOptions options = {});
  void write(std::ostream &out) const;
  static HistoryObserver read(std::istream &input);

private:
  Matrix identity_;
  std::map<Edge, Matrix, EdgeLess> matrices_;
  std::vector<std::pair<std::size_t, std::size_t>> blocks_;
};
} // namespace lotus::cfl::interleaved_dyck::affine
