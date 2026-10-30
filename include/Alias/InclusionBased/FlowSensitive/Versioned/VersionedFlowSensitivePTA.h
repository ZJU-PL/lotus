/**
 * @file VersionedFlowSensitivePTA.h
 * @brief Object-versioned sparse flow-sensitive pointer analysis.
 *
 * This is the Lotus counterpart of SVF's VersionedFlowSensitive analysis.
 * Memory facts are keyed by (abstract object, meld version), rather than by
 * (SVFG location, abstract object) as in the conventional flow-sensitive
 * solver.
 *
 * The solver uses SVF's delta-version architecture: version labels are
 * computed once over the pre-analysis SVFG, and every node which may gain
 * indirect edges during on-the-fly call-graph construction (a "delta" node)
 * is prelabelled with a dedicated version whose consume and yield are the
 * same.  Materialising a new edge therefore only appends to the reliance maps
 * and propagates the affected version; it never requires relabelling.
 */
#pragma once

#include "IR/GraphView.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::alias {

class VersionedFlowSensitivePTA {
public:
  using ObjectID = std::uint32_t;
  using NodeID = std::uint32_t;
  using Version = std::uint32_t;
  using PointsToSet = lotus::analysis::SVFGNodeBS;

  static constexpr Version InvalidVersion = 0;

  struct Config {
    using IndirectCallConnector =
        std::function<bool(const llvm::CallBase *, const llvm::Function *)>;
    const lotus::analysis::FilteredSVFGView *scope = nullptr;
    IndirectCallConnector connectIndirectCall;
    bool clusterVersionedObjects = true;
    // Threads used to compute object version labels. Zero selects
    // hardware_concurrency(); one runs sequentially.
    unsigned versioningThreads = 0;
    // Wall-clock budget for solve() in seconds. Zero disables it. On expiry
    // the solver keeps the current partial solution and sets timedOut().
    double timeLimitSeconds = 0;
  };

  struct Statistics {
    std::size_t nodes = 0;
    std::size_t nodeProcesses = 0;
    std::size_t versionedObjects = 0;
    std::size_t equivalentObjects = 0;
    std::size_t versions = 0;
    std::size_t versionedFacts = 0;
    std::size_t versionPropagations = 0;
    std::size_t statementReliances = 0;
    std::size_t strongUpdates = 0;
    std::size_t weakUpdates = 0;
    std::size_t indirectCallEdges = 0;
    std::size_t deltaNodes = 0;
    std::size_t deltaSourceNodes = 0;
    std::size_t deltaVersionUpdates = 0;
    std::size_t deltaRelabelings = 0;
    std::size_t statementWorklistPushes = 0;
    std::size_t relabelings = 0;
    // Set when Config::timeLimitSeconds expired before a fixed point.
    bool timedOut = false;
    // Set when a store in scope has a unique target (a strong update).
    bool svfgHasSU = false;
    double solveSeconds = 0;
    double labelingSeconds = 0;
    double otfSeconds = 0;
  };

  explicit VersionedFlowSensitivePTA(const lotus::analysis::SVFG &graph);
  VersionedFlowSensitivePTA(const lotus::analysis::SVFG &graph, Config config);

  const Statistics &solve();

  const PointsToSet &pointsTo(const lotus::analysis::SVFGNode *node) const;
  std::optional<PointsToSet> pointsTo(const llvm::Value *value) const;
  std::optional<bool> mayAlias(const llvm::Value *lhs,
                               const llvm::Value *rhs) const;

  Version getConsume(NodeID location, ObjectID object) const;
  Version getYield(NodeID location, ObjectID object) const;
  const PointsToSet &versionedPointsTo(ObjectID object, Version version) const;
  const std::vector<Version> &getReliantVersions(ObjectID object,
                                                 Version version) const;
  ObjectID canonicalVersionObject(ObjectID object) const;
  const std::vector<NodeID> &getDependentStatements(ObjectID object,
                                                    Version version) const;

  bool writeVersionedAnalysisResultToFile(const std::string &filename) const;
  bool readVersionedAnalysisResultFromFile(const std::string &filename);

  const Statistics &statistics() const { return stats_; }
  bool timedOut() const { return stats_.timedOut; }
  /// Human-readable statistics report.
  std::string statisticsReport() const;

private:
  struct VersionedObject {
    ObjectID object = 0;
    Version version = InvalidVersion;

    bool operator==(const VersionedObject &other) const {
      return object == other.object && version == other.version;
    }
  };

  struct VersionedObjectHash {
    std::size_t operator()(const VersionedObject &value) const {
      const std::size_t objectHash = std::hash<ObjectID>{}(value.object);
      const std::size_t versionHash = std::hash<Version>{}(value.version);
      return objectHash ^ (versionHash + 0x9e3779b9U + (objectHash << 6) +
                           (objectHash >> 2));
    }
  };

  using LocationVersionMap =
      std::unordered_map<NodeID, std::unordered_map<ObjectID, Version>>;
  using VersionRelianceMap =
      std::unordered_map<ObjectID,
                         std::unordered_map<Version, std::vector<Version>>>;
  using StatementRelianceMap =
      std::unordered_map<ObjectID,
                         std::unordered_map<Version, std::vector<NodeID>>>;
  using MeldSet = PointsToSet;
  struct FootprintEntry {
    std::uint8_t kind = 0;
    NodeID source = 0;
    NodeID destination = 0;

    bool operator<(const FootprintEntry &other) const {
      if (kind != other.kind)
        return kind < other.kind;
      if (source != other.source)
        return source < other.source;
      return destination < other.destination;
    }
    bool operator==(const FootprintEntry &other) const {
      return kind == other.kind && source == other.source &&
             destination == other.destination;
    }
  };

  bool inScope(const lotus::analysis::SVFGNode *node) const;
  bool timeLimitExceeded() const;
  void noteTimeout() { stats_.timedOut = true; }
  void initializeRecursiveFunctions();
  /// Returns true if a function with a strong-update site changed its
  /// recursion status, which invalidates the version labels.
  bool recursionChangeAffectsStrongUpdates(
      const std::unordered_set<const llvm::Function *> &previous) const;
  bool functionHasKnownCaller(const llvm::Function *function) const;
  void buildDeltaMaps();
  void buildVersionLabels();
  /// Version labels for a single object, accumulated without touching shared
  /// solver state so several objects can be labelled concurrently.
  struct ObjectLabels {
    std::vector<MeldSet> melds;
    Version initialVersion = InvalidVersion;
    std::unordered_map<NodeID, Version> consume;
    std::unordered_map<NodeID, Version> yield;
    std::unordered_map<Version, std::vector<Version>> reliance;
    std::unordered_map<Version, std::vector<NodeID>> statements;
    std::unordered_set<NodeID> intrinsicDependents;

    Version intern(const MeldSet &meld) {
      for (std::size_t i = 0; i < melds.size(); ++i)
        if (melds[i] == meld)
          return static_cast<Version>(i + 1);
      melds.push_back(meld);
      return static_cast<Version>(melds.size());
    }
    Version consumeAt(NodeID location) const {
      auto it = consume.find(location);
      return it == consume.end() ? InvalidVersion : it->second;
    }
    Version yieldAt(NodeID location) const {
      auto it = yield.find(location);
      return it == yield.end() ? InvalidVersion : it->second;
    }
    bool addReliance(Version source, Version destination) {
      if (source == InvalidVersion || destination == InvalidVersion ||
          source == destination)
        return false;
      auto &destinations = reliance[source];
      if (std::find(destinations.begin(), destinations.end(), destination) ==
          destinations.end()) {
        destinations.push_back(destination);
        return true;
      }
      return false;
    }
  };
  void labelObject(ObjectID object);
  void computeObjectLabels(ObjectID object, ObjectLabels &labels) const;
  void mergeObjectLabels(ObjectID object, ObjectLabels &&labels);
  bool labelObjectIncremental(ObjectID object);
  std::vector<FootprintEntry> versionFootprint(ObjectID object) const;
  void copyVersionLabels(ObjectID canonical, ObjectID object);
  PointsToSet relevantObjects() const;
  PointsToSet expandIndirectObjects(const PointsToSet &objects) const;
  bool edgeCarriesObject(const lotus::analysis::SVFGEdge &edge,
                         ObjectID object) const;
  bool storeMayTarget(const lotus::analysis::StoreSVFGNode &store,
                      ObjectID object) const;
  bool intrinsicMayDefine(const lotus::analysis::ActualOutSVFGNode &actualOut,
                          ObjectID object) const;
  bool memoryPhiNeedsInitial(const lotus::analysis::MSSAPhiSVFGNode &phi,
                             ObjectID object) const;
  /// True if \p node is a delta node: it may acquire new incoming indirect
  /// edges through on-the-fly call-graph construction.
  bool isDeltaNode(NodeID node) const;
  /// True if \p node may be the source of an indirect edge to a delta node.
  bool isDeltaSource(NodeID node) const;
  /// True if the memory region of delta \p node carries \p object.
  bool deltaCarriesObject(const lotus::analysis::SVFGNode &node,
                          ObjectID object) const;
  PointsToSet objectsWithFields(const llvm::Value *pointer) const;
  bool addReliance(ObjectID object, Version source, Version destination);
  void buildVersionProcessingOrder();
  void updateConnectedNodes(
      const std::vector<const lotus::analysis::SVFGEdge *> &newEdges);
  /// Re-materialise the indirect-call edges a loaded solution depends on.
  void reconnectIndirectCallsAfterRead();

  bool solveCurrentTopology();
  /// Propagate o:v into every version which relies on it.
  void propagateVersion(ObjectID object, Version version);
  /// Propagate o:source into o:destination.  Queues a follow-up propagation
  /// when o:destination changes.
  void propagateVersion(ObjectID object, Version source, Version destination);
  /// Drain the version-propagation queue.  Returns true if anything changed.
  bool drainVersionPropagation();
  /// Enqueue the direct successors of \p node (SVF's propagate()).
  void propagate(const lotus::analysis::SVFGNode &node);
  /// Enqueue \p node onto the statement worklist.
  void enqueueStatement(NodeID node);
  /// Enqueue every statement which consumes o:version.
  void enqueueDependentStatements(ObjectID object, Version version);
  bool processNode(const lotus::analysis::SVFGNode &node);
  bool processIntrinsicActualOut(
      const lotus::analysis::ActualOutSVFGNode &actualOut);
  PointsToSet directInput(const lotus::analysis::SVFGNode &node) const;
  PointsToSet pointerTargets(const llvm::Value *pointer) const;
  PointsToSet accessTargets(const llvm::Value *pointer,
                            const PointsToSet &memoryPointsTo) const;
  bool isStrongUpdate(const PointsToSet &targets) const;
  bool resolveIndirectCalls(const lotus::analysis::SVFGNode &node,
                            const PointsToSet &pointsTo);
  PointsToSet gepTransfer(const lotus::analysis::GepSVFGNode &gep) const;

  void initializeGlobalMemory();
  PointsToSet constantPointsTo(const llvm::Constant *constant) const;
  Version initialVersion(ObjectID object) const;
  static const llvm::Value *accessPointer(const llvm::Instruction *instruction);
  static const llvm::Value *storedValue(const llvm::Instruction *instruction);

  const lotus::analysis::SVFG *graph_;
  Config config_;
  std::unordered_map<NodeID, PointsToSet> topLevelPointsTo_;
  std::unordered_map<VersionedObject, PointsToSet, VersionedObjectHash>
      versionedPointsTo_;
  LocationVersionMap consume_;
  LocationVersionMap yield_;
  VersionRelianceMap versionReliance_;
  StatementRelianceMap statementReliance_;
  std::unordered_map<ObjectID, Version> initialVersion_;
  std::unordered_map<ObjectID, std::vector<MeldSet>> versionMelds_;
  std::unordered_map<ObjectID, ObjectID> equivalentObject_;
  std::vector<ObjectID> versionProcessingOrder_;
  std::unordered_set<NodeID> strongUpdateSites_;
  std::unordered_set<NodeID> weakUpdateSites_;
  std::unordered_set<const llvm::Function *> recursiveFunctions_;
  /// Nodes which may gain incoming indirect edges through OTF call-graph
  /// construction (SVF's deltaMap).
  std::unordered_set<NodeID> deltaNodes_;
  /// Nodes which may gain outgoing indirect edges to a delta node (SVF's
  /// deltaSourceMap).
  std::unordered_set<NodeID> deltaSourceNodes_;
  /// Objects which have labels (either directly or through an equivalent
  /// canonical object).
  std::unordered_set<ObjectID> labelledObjects_;
  /// Intrinsic actual-outs (memcpy/memmove/memset) read memory which is not
  /// part of their def set, so every version change of these objects must
  /// re-evaluate them.
  std::unordered_map<ObjectID, std::unordered_set<NodeID>> intrinsicDependents_;
  /// Pending version propagations (SVF's VersionedVarToPropNode worklist).
  std::queue<VersionedObject> versionPropagationQueue_;
  std::unordered_set<VersionedObject, VersionedObjectHash>
      queuedVersionPropagations_;
  /// Statements affected by a version change (SVF's stmtReliance worklist).
  std::queue<NodeID> statementWorklist_;
  std::unordered_set<NodeID> queuedStatements_;
  bool topologyChanged_ = false;
  bool hasDeadline_ = false;
  std::chrono::steady_clock::time_point deadline_;
  Statistics stats_;
};

} // namespace lotus::alias
