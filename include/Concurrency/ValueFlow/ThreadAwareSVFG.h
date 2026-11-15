/**
 * @file ThreadAwareSVFG.h
 * @brief Thread-interference overlay for Lotus's sparse value-flow graph.
 */
#pragma once

#include "IR/GraphView.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mhp {
class IMHPAnalysis;
class LockSetAnalysis;
} // namespace mhp

namespace lotus::analysis {

class LockSpanReasoner;
class ThreadCreationTree;

/// Adds guarded Store->Load and symmetric Store<->Store value-flow edges for
/// accesses that may execute concurrently. The base SVFG remains owned by the
/// caller and can be restored with clear().
///
/// Pairs that a common exclusive lock must exclude are additionally filtered by
/// lock-span reasoning: an interference edge is only kept when the stored value
/// can actually survive until the reading span starts. See Config and
/// LockSpanReasoner for the exact rules.
class ThreadAwareSVFGBuilder {
public:
  struct Config {
    /// Drop a store's interference flow for objects that a later store in the
    /// same critical section provably overwrites before the lock is released.
    bool pruneOverwrittenStores = true;
    /// Drop interference flow for objects that a store preceding the sink in
    /// the sink's own critical section already provides.
    bool pruneShadowedSinks = true;
  };

  struct Statistics {
    std::size_t stores = 0;
    std::size_t loads = 0;
    std::size_t aliasCandidates = 0;
    std::size_t parallelCandidates = 0;
    /// MHP + aliasing pairs that a common exclusive lock must exclude.
    std::size_t lockProtectedCandidates = 0;
    /// Object labels removed from candidate guards by lock-span reasoning.
    std::size_t spanPrunedObjects = 0;
    /// Lock-protected pairs whose whole guard was pruned, leaving no edge.
    std::size_t spanPrunedPairs = 0;
    std::size_t edgesAdded = 0;
    std::size_t forkParameterEdges = 0;
    std::size_t forkMemoryEdges = 0;
    std::size_t joinMemoryEdges = 0;
    std::size_t joinMemoryNodes = 0;
  };

  ThreadAwareSVFGBuilder(SVFG &graph, const mhp::IMHPAnalysis &mhp,
                         const mhp::LockSetAnalysis *locks = nullptr,
                         const FilteredSVFGView *scope = nullptr,
                         const ThreadCreationTree *threadTree = nullptr);
  ThreadAwareSVFGBuilder(SVFG &graph, const mhp::IMHPAnalysis &mhp,
                         const mhp::LockSetAnalysis *locks,
                         const FilteredSVFGView *scope,
                         const ThreadCreationTree *threadTree, Config config);
  ~ThreadAwareSVFGBuilder();

  ThreadAwareSVFGBuilder(const ThreadAwareSVFGBuilder &) = delete;
  ThreadAwareSVFGBuilder &operator=(const ThreadAwareSVFGBuilder &) = delete;

  const Statistics &build();
  void clear();

  const Statistics &statistics() const { return stats_; }
  std::size_t edgeCount() const { return overlayEdges_.size(); }

private:
  bool inScope(const SVFGNode *node) const;
  SVFGNodeBS intersectTargets(const SVFGNode &lhs, const SVFGNode &rhs) const;
  bool addInterferenceEdge(SVFGNode &source, SVFGNode &destination,
                           const SVFGNodeBS &guard);
  bool addOwnedEdge(SVFGNode &source, SVFGNode &destination, SVFGEdgeK kind,
                    const llvm::CallBase *callSite,
                    const SVFGNodeBS &guard = {});
  void connectForkJoinMemoryFlow();
  void connectForkFlow();
  void connectJoinFlow();

  /// Applies lock-span pruning to one lock-protected candidate pair and
  /// returns the surviving object labels of `guard`.
  SVFGNodeBS refineGuardForSpan(StoreSVFGNode &source, SVFGNode &destination,
                                const SVFGNodeBS &guard);
  /// Stores reachable from `node` through base (non-interference) indirect
  /// value-flow edges, stopping at statement nodes.
  const std::vector<StoreSVFGNode *> &baseSuccessorStores(SVFGNode &node);
  /// Stores that reach `node` through base (non-interference) indirect
  /// value-flow edges, stopping at statement nodes.
  const std::vector<StoreSVFGNode *> &basePredecessorStores(SVFGNode &node);
  /// Objects that base (non-interference) edges already carry into `node`.
  const SVFGNodeBS &baseIncomingObjects(SVFGNode &node);

  SVFG *graph_;
  const mhp::IMHPAnalysis *mhp_;
  const mhp::LockSetAnalysis *locks_;
  const FilteredSVFGView *scope_;
  const ThreadCreationTree *threadTree_;
  Config config_;
  std::unique_ptr<LockSpanReasoner> spans_;
  std::unordered_set<SVFGEdge *> overlayEdges_;
  std::unordered_map<const SVFGNode *, std::vector<StoreSVFGNode *>>
      baseSuccessorStores_;
  std::unordered_map<const SVFGNode *, std::vector<StoreSVFGNode *>>
      basePredecessorStores_;
  std::unordered_map<const SVFGNode *, SVFGNodeBS> baseIncomingObjects_;
  Statistics stats_;
};

} // namespace lotus::analysis
