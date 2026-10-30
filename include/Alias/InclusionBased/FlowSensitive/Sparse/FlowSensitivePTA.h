/**
 * @file FlowSensitivePTA.h
 * @brief General sparse flow-sensitive inclusion-based pointer analysis.
 */
#pragma once

#include "Alias/InclusionBased/FlowSensitive/Support/ObjectClustering.h"
#include "Alias/Infrastructure/PtsSet/HashConsedPointsToSet.h"
#include "IR/GraphView.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::alias {

class FlowSensitivePTA {
public:
  using ObjectID = std::uint32_t;
  using NodeID = std::uint32_t;
  using PointsToSet = lotus::analysis::SVFGNodeBS;

  struct Config {
    using IndirectCallConnector =
        std::function<bool(const llvm::CallBase *, const llvm::Function *)>;
    // Rejects on-the-fly indirect-call edges that the pre-analysis does not
    // consider feasible. SVF applies the same bound with its auxiliary
    // Andersen call graph before materialising an edge; leaving this empty
    // removes the bound and is only safe for graphs without indirect calls.
    using IndirectCallFilter =
        std::function<bool(const llvm::CallBase *, const llvm::Function *)>;
    PointsToSetBackend setBackend = PointsToSetBackend::Mutable;
    const lotus::analysis::FilteredSVFGView *scope = nullptr;
    IndirectCallConnector connectIndirectCall;
    IndirectCallFilter isIndirectCallTargetAllowed;
    // Pipeline transfers over immutable, validated effect snapshots. Graph
    // mutation through connectIndirectCall occurs only between solving epochs.
    bool parallel = false;
    // Research ablation: unordered commits can choose a different fixed point
    // for non-monotone transfers. Default commits follow the reference
    // schedule.
    bool unorderedParallel = false;
    // Share immutable effect values between worker snapshots. Disabling this
    // retains copied-set storage for performance ablation.
    bool shareParallelSets = true;
    // Transfers evaluated with local effect forwarding in a worker block.
    // One disables forwarding and gives single-transfer speculation.
    unsigned parallelBlockSize = 8;
    // Reuse an unchanged transfer result while its complete read certificate
    // remains valid. Reference worklist retirement is still preserved.
    bool parallelMemoize = true;
    // Validate precise memory guards by object, including absence and
    // wildcard fallback; whole-channel validation remains an ablation.
    bool parallelObjectCertificates = true;
    // Zero selects hardware_concurrency(); includes the calling thread.
    unsigned workerThreads = 0;
    // Wall-clock budget for solve() in seconds. Zero disables it. On expiry
    // the solver stops at a worklist boundary, keeps the partial solution
    // queryable, and sets timedOut().
    double timeLimitSeconds = 0;
    // Object clustering (SVF's FlowSensitive::cluster()). `None` is the
    // identity mapping and corresponds to SVF's plainMap(); `Type` merges
    // base objects with the same LLVM type and kind. Merging is sound for a
    // may-points-to analysis and disables strong updates on merged classes.
    enum class ClusterMethod { None, Type };
    ClusterMethod clusterObjects = ClusterMethod::None;
    // Model only pointer-typed loads/stores, like SVF's pointer-only SVFG
    // (memSSA.buildPTROnlySVFG): its `isInterestedSVFVar` keeps pointer-typed
    // variables only, so a non-pointer access never has a VFG node. Such an
    // access cannot change pointer contents, but transferring it materialises
    // empty memory facts and forces the transfer to walk the whole target set.
    // Set to false to reproduce the pre-parity behaviour.
    bool pointerOnlySVFG = true;
  };

  struct Statistics {
    std::size_t nodes = 0;
    std::size_t sccs = 0;
    std::size_t maxSccSize = 0;
    std::size_t nodeProcesses = 0;
    // Per-node-kind transfer counts (SVF: ProcessedAddr, ProcessedCopy, ...).
    std::size_t processedAddr = 0;
    std::size_t processedCopy = 0;
    std::size_t processedGep = 0;
    std::size_t processedLoad = 0;
    std::size_t processedStore = 0;
    std::size_t processedPhi = 0;
    std::size_t processedActualParam = 0;
    std::size_t processedFormalRet = 0;
    std::size_t processedMSSA = 0;
    // SCC-worklist rounds, counted the way SVF counts SolveIterations.
    std::size_t solveIterations = 0;
    std::size_t topLevelFacts = 0;
    std::size_t memoryInFacts = 0;
    std::size_t memoryOutFacts = 0;
    std::size_t strongUpdates = 0;
    std::size_t weakUpdates = 0;
    std::size_t strongUpdateExecutions = 0;
    std::size_t weakUpdateExecutions = 0;
    std::size_t indirectCallEdges = 0;
    // Edges the pre-analysis filter rejected (SVF: silently dropped).
    std::size_t indirectCallsRejected = 0;
    // On-the-fly edges materialised without a target filter. Non-zero means
    // the call graph is unbounded, unlike SVF's auxiliary-Andersen bound.
    std::size_t unboundedIndirectCalls = 0;
    // Indirect callsites for which no connector was configured.
    std::size_t unresolvedIndirectCalls = 0;
    // Objects collapsed to a field-insensitive representative during solving.
    std::size_t fieldInsensitiveCollapses = 0;
    // Points-to set sizes over top-level nodes (SVF: MaxPtsSize/AvgPtsSize).
    std::size_t maxTopLevelPtsSize = 0;
    double averageTopLevelPtsSize = 0.0;
    // SVF-named population and IN/OUT statistics (FlowSensitiveStat).
    std::size_t totalPointers = 0;           // TotalPointers
    std::size_t totalObjects = 0;            // TotalObjects
    std::size_t fieldInsensitiveObjects = 0; // FIObjNum
    std::size_t fieldSensitiveObjects = 0;   // FSObjNum
    std::size_t maxPtsSize = 0;              // MaxPtsSize over IN/OUT facts
    double averagePtsSize = 0.0;             // AvgPtsSize over IN/OUT facts
    // Store LHS pointers with a non-empty points-to set, and their sizes
    // (SVF: NumOfAddrTakeVar / MaxAddrTakenVarPts / AvgAddrTakenVarPtsSize).
    std::size_t addrTakenVars = 0;
    std::size_t maxAddrTakenPtsSize = 0;
    double averageAddrTakenPtsSize = 0.0;
    // Objects merged into a cluster representative, and the number of
    // non-singleton classes (SVF: cluster() statistics).
    std::size_t clusteredObjects = 0;
    std::size_t mergedObjectClusters = 0;
    std::size_t hashConsedUniqueSets = 0;
    std::size_t hashConsedUnionCacheHits = 0;
    std::size_t parallelThreads = 0;
    std::size_t parallelCommits = 0;
    std::size_t parallelConflicts = 0;
    std::size_t parallelPublications = 0;
    std::size_t parallelNotifications = 0;
    std::size_t parallelPeakWorkers = 0;
    std::size_t parallelBlocks = 0;
    std::size_t parallelForwardedReads = 0;
    std::size_t parallelAcceptedForwardedReads = 0;
    std::size_t parallelMemoHits = 0;
    std::size_t parallelObjectReads = 0;
    std::size_t parallelNegativeReads = 0;
    std::size_t parallelWildcardReads = 0;
    std::size_t parallelObjectSalvages = 0;
    std::size_t topologyEpochs = 0;
    // Set when Config::timeLimitSeconds expired before a fixed point.
    bool timedOut = false;
    // Set when a store in scope has a unique target (a strong update).
    bool svfgHasSU = false;
    double solveSeconds = 0;
    double initializeSeconds = 0;
    double otfSeconds = 0;
  };

  explicit FlowSensitivePTA(const lotus::analysis::SVFG &graph);
  FlowSensitivePTA(const lotus::analysis::SVFG &graph, Config config);

  const Statistics &solve();
  const PointsToSet &pointsTo(const lotus::analysis::SVFGNode *node) const;
  std::optional<PointsToSet> pointsTo(const llvm::Value *value) const;
  const PointsToSet &memoryIn(const lotus::analysis::SVFGNode *node,
                              ObjectID object) const;
  const PointsToSet &memoryOut(const lotus::analysis::SVFGNode *node,
                               ObjectID object) const;
  std::optional<bool> mayAlias(const llvm::Value *lhs,
                               const llvm::Value *rhs) const;

  const Statistics &statistics() const { return stats_; }
  bool timedOut() const { return stats_.timedOut; }
  PointsToSetBackend setBackend() const { return config_.setBackend; }
  /// Object partition used by the solver (empty = identity mapping, SVF's
  /// plainMap()).
  const flow_sensitive::ObjectClustering &objectClustering() const {
    return objectClustering_;
  }
  /// Human-readable statistics report.
  std::string statisticsReport() const;

  /// Serialize the solved points-to and IN/OUT memory facts as
  /// LOTUS_SPARSEPTA version 1 (Lotus object IDs).
  bool writeAnalysisResultToFile(const std::string &filename) const;
  /// Load facts written by writeAnalysisResultToFile(). Indirect-call edges
  /// are not stored; callers must rebuild the graph and reconnect them.
  bool readAnalysisResultFromFile(const std::string &filename);
  /// Canonical identity for result reporting: a dedicated field-insensitive
  /// representative maps to the base object it collapses. Do not use this for
  /// solving -- expandIndirectObjects() is the precision-preserving path.
  ObjectID canonicalObject(ObjectID object) const;
  // Full differential check of public queries. A missing memory entry and an
  // explicit empty entry both answer the empty set. Analyses must share an
  // SVFG.
  bool hasSameSolution(const FlowSensitivePTA &other,
                       std::string *difference = nullptr) const;
  // Re-evaluate every equation against immutable current inputs, without
  // changing this result or connecting new calls.
  bool isFixedPoint(std::string *difference = nullptr) const;

private:
  class ParallelExecution;
  class OrderedExecution;
  bool compareSolution(const FlowSensitivePTA &other, bool comparePresence,
                       std::string *difference) const;
  const Statistics &solveParallel();
  const Statistics &solveOrderedParallel();
  struct StoredSet {
    PointsToSet mutableSet;
    // Worker snapshots share immutable sets; transfer mutations create a new
    // value. Public mutable/hash-consed result storage remains unchanged.
    std::shared_ptr<const PointsToSet> sharedSet;
    HashConsedPointsToSetArena::SetID interned =
        HashConsedPointsToSetArena::EmptySet;
  };
  using MemoryState = std::unordered_map<ObjectID, StoredSet>;
  struct SCCInfo {
    std::vector<std::vector<const lotus::analysis::SVFGNode *>> components;
    std::unordered_map<NodeID, std::size_t> nodeToComponent;
    std::vector<std::vector<std::size_t>> successors;
  };

  bool inScope(const lotus::analysis::SVFGNode *node) const;
  bool timeLimitExceeded() const;
  void noteTimeout() { stats_.timedOut = true; }
  // Field-insensitive representative for a base object, collapsing the struct
  // on first use exactly like SVF's setObjFieldInsensitive/getFIObjVar.
  ObjectID fieldInsensitiveRepresentative(ObjectID base);
  StoredSet singleton(ObjectID object);
  const PointsToSet &materialize(const StoredSet &set) const;
  bool merge(StoredSet &destination, const StoredSet &source);
  bool assign(StoredSet &destination, const StoredSet &source);
  bool mergeState(MemoryState &destination, const MemoryState &source);
  bool assignState(MemoryState &destination, const MemoryState &source);
  const StoredSet &topSet(const lotus::analysis::SVFGNode *node) const;
  const MemoryState &outState(const lotus::analysis::SVFGNode *node,
                              const PointsToSet *objects = nullptr) const;
  const MemoryState &inState(const lotus::analysis::SVFGNode *node,
                             const PointsToSet *objects = nullptr) const;
  // Private read interception keeps the transfer semantics shared by the
  // sequential solver and worker-local parallel evaluators.
  std::function<const StoredSet &(const lotus::analysis::SVFGNode *)> topRead_;
  std::function<const MemoryState &(const lotus::analysis::SVFGNode *, bool,
                                    const PointsToSet *)>
      memoryRead_;
  PointsToSet expandIndirectObjects(const PointsToSet &objects) const;
  void initializeRecursiveFunctions();
  void initializeGlobalMemory();
  StoredSet constantPointsTo(const llvm::Constant *constant);
  SCCInfo computeSCCs() const;
  /// Fill the SVF-named population / points-to statistics from the solved
  /// state. Called by both the sequential and parallel solve paths.
  void collectStatistics();
  // Object clustering support (SVF's FlowSensitive::cluster()).
  ObjectID mapObject(ObjectID object) const;
  PointsToSet mapObjects(const PointsToSet &objects) const;
  StoredSet buildStored(const PointsToSet &set);
  void mapStoredInPlace(StoredSet &set);
  void buildObjectClustering();
  bool transfer(const lotus::analysis::SVFGNode &node);
  StoredSet directInput(const lotus::analysis::SVFGNode &node);
  StoredSet pointerTargets(const llvm::Value *pointer);
  PointsToSet accessTargets(const llvm::Value *pointer,
                            const PointsToSet &memoryPointsTo);
  bool isStrongUpdate(const PointsToSet &targets) const;
  bool resolveIndirectCalls(const lotus::analysis::SVFGNode &node,
                            const StoredSet &pointsTo);
  /// Re-materialise the indirect-call edges a loaded solution depends on.
  void reconnectIndirectCallsAfterRead();
  static const llvm::Value *accessPointer(const llvm::Instruction *instruction);
  static const llvm::Value *storedValue(const llvm::Instruction *instruction);

  const lotus::analysis::SVFG *graph_;
  Config config_;
  bool persistentSets_ = false;
  HashConsedPointsToSetArena arena_;
  std::unordered_map<NodeID, StoredSet> topLevelPointsTo_;
  std::unordered_map<NodeID, MemoryState> dfIn_;
  std::unordered_map<NodeID, MemoryState> dfOut_;
  MemoryState initialMemory_;
  std::unordered_map<ObjectID, ObjectID> fieldInsensitiveObjects_;
  /// Field-insensitive identity map loaded from a result file (version 2).
  std::unordered_map<ObjectID, ObjectID> loadedCanonicalObjects_;
  /// Object partition (empty = identity mapping).
  flow_sensitive::ObjectClustering objectClustering_;
  std::unordered_set<const llvm::Function *> recursiveFunctions_;
  std::unordered_set<NodeID> strongUpdateSites_;
  std::unordered_set<NodeID> weakUpdateSites_;
  bool topologyChanged_ = false;
  bool hasDeadline_ = false;
  std::chrono::steady_clock::time_point deadline_;
  Statistics stats_;
};

} // namespace lotus::alias
