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

#include "Alias/InclusionBased/FlowSensitive/Support/ObjectClustering.h"
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
    // Rejects on-the-fly indirect-call edges that the pre-analysis does not
    // consider feasible. SVF bounds these with the auxiliary Andersen call
    // graph before materialising an edge.
    using IndirectCallFilter =
        std::function<bool(const llvm::CallBase *, const llvm::Function *)>;
    // Points-to provider used to seed meld labels (SVF's `prelabel()` reads
    // the auxiliary Andersen analysis: `ander->getPts(store LHS)`).  When
    // empty, the MemorySSA region sets recorded by the builder are used, with
    // the value's pre-analysis object set as a fallback.
    using PointsToProvider = std::function<PointsToSet(const llvm::Value *)>;
    const lotus::analysis::FilteredSVFGView *scope = nullptr;
    IndirectCallConnector connectIndirectCall;
    IndirectCallFilter isIndirectCallTargetAllowed;
    PointsToProvider preAnalysisTargets;
    // Labels footprint representatives in occurrence-weight order (heaviest
    // first). This is *not* SVF's `cluster()`: no objects, equivalents, or
    // NodeIDs are merged here.
    bool orderVersionedObjectsByWeight = true;
    // Threads used to compute object version labels. Zero selects
    // hardware_concurrency(); one runs sequentially.
    unsigned versioningThreads = 0;
    // Wall-clock budget for solve() in seconds. Zero disables it. On expiry
    // the solver keeps the current partial solution and sets timedOut().
    double timeLimitSeconds = 0;
    // Object clustering (SVF's FlowSensitive::cluster()). `None` is the
    // identity mapping (SVF plainMap()). `Footprint` merges objects with
    // identical access footprints, which is exactly the partition the solver
    // already computes for label sharing (an analogue of SVF's regioned
    // clustering). `Type` is a coarser Lotus-specific policy that merges base
    // objects with the same LLVM type and kind. Merging is sound for a
    // may-points-to analysis and disables strong updates on merged classes.
    enum class ClusterMethod { None, Footprint, Type };
    ClusterMethod clusterObjects = ClusterMethod::None;
    // Model only pointer-typed loads/stores, like SVF's pointer-only SVFG
    // (memSSA.buildPTROnlySVFG). A non-pointer access cannot change pointer
    // contents; processing it only adds empty version facts and transfer work.
    // Set to false to reproduce the pre-parity behaviour.
    bool pointerOnlySVFG = true;
  };

  struct Statistics {
    std::size_t nodes = 0;
    std::size_t nodeProcesses = 0;
    // SVFG SCC decomposition used to order the statement worklist
    // (SVF: numOfSCC / maxSCCSize).
    std::size_t sccs = 0;
    std::size_t maxSccSize = 0;
    // Completed worklist sweeps (SVF: SolveIterations).
    std::size_t solveIterations = 0;
    std::size_t versionedObjects = 0;
    std::size_t equivalentObjects = 0;
    std::size_t versions = 0;
    std::size_t versionedFacts = 0;
    std::size_t versionPropagations = 0;
    std::size_t statementReliances = 0;
    std::size_t strongUpdates = 0;
    std::size_t weakUpdates = 0;
    std::size_t indirectCallEdges = 0;
    // Edges the pre-analysis filter rejected (SVF drops these silently).
    std::size_t indirectCallsRejected = 0;
    // On-the-fly edges materialised without a target filter (unbounded).
    std::size_t unboundedIndirectCalls = 0;
    // Indirect callsites for which no connector was configured.
    std::size_t unresolvedIndirectCalls = 0;
    // Objects collapsed to a field-insensitive representative while solving.
    std::size_t fieldInsensitiveCollapses = 0;
    std::size_t deltaNodes = 0;
    std::size_t deltaSourceNodes = 0;
    std::size_t deltaVersionUpdates = 0;
    std::size_t deltaRelabelings = 0;
    std::size_t statementWorklistPushes = 0;
    std::size_t relabelings = 0;
    // Version-points-to sizes (SVF: TotalNonEmptyVPts, MaxVersionPtsSize,
    // AvgVersionPtsSize).
    std::size_t nonEmptyVersions = 0;
    std::size_t maxVersionPtsSize = 0;
    double averageVersionPtsSize = 0.0;
    // SVF-named version and population statistics
    // (VersionedFlowSensitiveStat).
    std::size_t totalVersions = 0;           // TotalVersions
    std::size_t maxVersionsForObject = 0;    // MaxVersionsForObj
    std::size_t singleVersionObjects = 0;    // TotalSingleVObjs
    std::size_t totalExistingVPts = 0;       // TotalExistingVPts
    std::size_t totalEmptyVPts = 0;          // TotalEmptyVPts
    std::size_t totalPointers = 0;           // TotalPointers
    std::size_t totalObjects = 0;            // TotalObjects
    std::size_t fieldInsensitiveObjects = 0; // FIObjNum
    std::size_t fieldSensitiveObjects = 0;   // FSObjNum
    std::size_t maxPtsSize = 0;              // MaxPtsSize
    double averagePtsSize = 0.0;             // AvgPtsSize
    std::size_t addrTakenVars = 0;           // NumOfAddrTakeVar
    std::size_t maxAddrTakenPtsSize = 0;     // MaxAddrTakenVarPts
    double averageAddrTakenPtsSize = 0.0;    // AvgAddrTakenVarPtsSize
    // Objects merged into a cluster representative, and non-singleton classes.
    std::size_t clusteredObjects = 0;
    std::size_t mergedObjectClusters = 0;
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
  /// Canonical identity for result reporting: a dedicated field-insensitive
  /// representative maps to the base it collapses (SVF has no separate FI
  /// object). Not used for solving.
  ObjectID canonicalObject(ObjectID object) const;
  const std::vector<NodeID> &getDependentStatements(ObjectID object,
                                                    Version version) const;

  bool writeVersionedAnalysisResultToFile(const std::string &filename) const;
  bool readVersionedAnalysisResultFromFile(const std::string &filename);

  const Statistics &statistics() const { return stats_; }
  bool timedOut() const { return stats_.timedOut; }
  /// Object partition used by the solver (empty = identity mapping).
  const flow_sensitive::ObjectClustering &objectClustering() const {
    return objectClustering_;
  }
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
  /// Field-insensitive representative for a base object, collapsing the struct
  /// on first use exactly like SVF's setObjFieldInsensitive/getFIObjVar.
  ObjectID fieldInsensitiveRepresentative(ObjectID base);
  /// Rebuild the per-object edge index used by versionFootprint() and
  /// computeObjectLabels() so labelling is proportional to an object's
  /// footprint instead of the whole graph.
  void buildObjectIndex();
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
  // Object clustering support (SVF's FlowSensitive::cluster()).
  ObjectID mapObject(ObjectID object) const;
  PointsToSet mapObjects(const PointsToSet &objects) const;
  void buildObjectClustering(const PointsToSet &objects);
  /// Pre-analysis targets of a store's LHS: Config::preAnalysisTargets when
  /// configured (SVF's Andersen-based prelabel seed), else the builder's
  /// MemorySSA region set, else the value's object set.
  PointsToSet
  storeTargetObjects(const lotus::analysis::StoreSVFGNode &store) const;
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
  PointsToSet gepTransfer(const lotus::analysis::GepSVFGNode &gep);

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
  /// Object partition (empty = identity mapping).
  flow_sensitive::ObjectClustering objectClustering_;
  std::vector<ObjectID> versionProcessingOrder_;
  /// Cached field-insensitive representative per base object.
  std::unordered_map<ObjectID, ObjectID> fieldInsensitiveObjects_;
  /// In-scope indirect edges that carry each object. Rebuilt by
  /// buildObjectIndex() before every labelling pass.
  std::unordered_map<ObjectID, std::vector<const lotus::analysis::SVFGEdge *>>
      objectIndex_;
  std::vector<const lotus::analysis::SVFGNode *> allNodes_;
  std::vector<const lotus::analysis::SVFGNode *> statementNodes_;
  std::vector<const lotus::analysis::StoreSVFGNode *> storeNodes_;
  std::vector<const lotus::analysis::ActualOutSVFGNode *> actualOutNodes_;
  std::vector<const lotus::analysis::MSSAPhiSVFGNode *> memoryPhiNodes_;
  /// Indirect edges guarded by the unknown object: they carry every object and
  /// therefore cannot be indexed precisely.
  std::vector<const lotus::analysis::SVFGEdge *> wildcardEdges_;
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
  /// Field-insensitive identity map loaded from a result file.
  std::unordered_map<ObjectID, ObjectID> loadedCanonicalObjects_;
  /// Intrinsic actual-outs (memcpy/memmove/memset) read memory which is not
  /// part of their def set, so every version change of these objects must
  /// re-evaluate them.
  std::unordered_map<ObjectID, std::unordered_set<NodeID>> intrinsicDependents_;
  /// Pending version propagations (SVF's VersionedVarToPropNode worklist).
  std::queue<VersionedObject> versionPropagationQueue_;
  std::unordered_set<VersionedObject, VersionedObjectHash>
      queuedVersionPropagations_;
  /// Statements affected by a version change (SVF's stmtReliance worklist).
  /// Ordered by SVFG SCC (reverse topological, like SVF's `WPAFSSolver`), then
  /// by node id for determinism.
  std::priority_queue<std::pair<std::size_t, NodeID>,
                      std::vector<std::pair<std::size_t, NodeID>>,
                      std::greater<>>
      statementWorklist_;
  std::unordered_set<NodeID> queuedStatements_;
  /// NodeID -> SVFG SCC index, refreshed whenever the topology changes.
  std::unordered_map<NodeID, std::size_t> sccComponentOf_;
  bool topologyChanged_ = false;
  bool hasDeadline_ = false;
  std::chrono::steady_clock::time_point deadline_;
  Statistics stats_;
};

} // namespace lotus::alias
