#pragma once

#include "CFL/InterleavedDyck/StagedBounds/CnfGrammar.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfTypes.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement {

/* Graph on which CFL-reachability is run
 * The type Edge is an alias for std::tuple<int, int, int>,
 * which uses (i, A, j) to represent the edge i --A--> j */
struct CnfGraph {
  using EdgeSet = std::unordered_set<Edge, EdgeHasher>;
  using UnaryRecord =
      std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>;
  // For i --A--> j --B--> k, the recorded triple is (A, j, B).
  using BinaryRecord = std::unordered_map<
      Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
      EdgeHasher>;

  void reinit(int n, const EdgeSet &edges);
  void addEdge(const Edge &e);
  bool hasEdge(const Edge &e) const;
  // Normal and eagerly traced CFL reachability (return the set of S edges).
  EdgeSet runCFLReachability(const CnfGrammar &grammar);
  EdgeSet runCFLReachability(const CnfGrammar &grammar,
                             UnaryRecord &singleRecord,
                             BinaryRecord &binaryRecord);
  // The pipeline consumes each S edge once: export a unique vector without
  // allocating another hash table over the already-deduplicated relation.
  std::vector<Edge> runCFLReachabilityCompact(const CnfGrammar &grammar);
  std::vector<Edge> runCFLReachabilityCompact(const CnfGrammar &grammar,
                                              UnaryRecord &singleRecord,
                                              BinaryRecord &binaryRecord);
  // Stream saturated S edges to a filtering/projecting consumer, without
  // materializing the entire start relation a second time.
  void
  runCFLReachabilityVisit(const CnfGrammar &grammar,
                          const std::function<void(const Edge &)> &visitor);
  // Get original edges contributing to the supplied S edges.
  EdgeSet getEdgeClosure(const CnfGrammar &grammar, const EdgeSet &result,
                         const UnaryRecord &singleRecord,
                         const BinaryRecord &binaryRecord) const;
  EdgeSet getEdgeClosureCompact(const CnfGrammar &grammar,
                                const std::vector<Edge> &result,
                                const UnaryRecord &singleRecord,
                                const BinaryRecord &binaryRecord) const;
  // Reconstruct contributing edges after saturation. Reuses the saturation
  // indices and assigns compact visited slots in place.
  EdgeSet getFactorizedEdgeClosure(const CnfGrammar &grammar,
                                   const EdgeSet &result);
  EdgeSet getFactorizedEdgeClosureCompact(const CnfGrammar &grammar,
                                          const std::vector<Edge> &result);

private:
  // Small rows use sorted endpoints, larger rows use occupied bitmap words,
  // and sufficiently populated word sets become dense. No vertex-by-symbol
  // or vertex-by-vertex matrix is eagerly allocated; only populated rows exist.
  struct RelationRow {
    std::vector<int> sparse;
    // With no words, sparse holds individual vertices. Otherwise it holds
    // occupied word indices; an empty index denotes a fully dense bitmap.
    std::vector<std::uint64_t> words;
    std::size_t count = 0;
    std::size_t traceIndex = 0;

    std::uint64_t wordAt(int index) const;
    bool contains(int vertex) const;
    bool insert(int vertex, std::size_t vertex_count);
    std::size_t traceSlot(int vertex) const;
    template <class Fn> void forEachWord(Fn &&fn) const;
    template <class Fn> void forEach(Fn &&fn) const;
    template <class Fn>
    void forEachCommon(const RelationRow &other, Fn &&fn) const;
    template <class Fn>
    void forEachMissing(const RelationRow *known, Fn &&fn) const;
  };
  struct Relation {
    std::unordered_map<int, RelationRow> outgoing;
    std::unordered_map<int, RelationRow> incoming;
  };
  std::size_t vertexCount = 0;
  std::unordered_map<int, Relation> relations;
  std::vector<std::vector<int>> outgoingSymbols;
  std::vector<std::vector<int>> incomingSymbols;
  bool insertEdge(const Edge &edge);
  std::vector<Edge> runCFLReachabilityCore(
      const CnfGrammar &grammar, UnaryRecord *singleRecord,
      BinaryRecord *binaryRecord,
      const std::function<void(const Edge &)> *visitor = nullptr);
};

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement
