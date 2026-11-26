#pragma once

#include "IR/UseTraceSSA/Models.h"

namespace lotus {
namespace usetracessa {

enum class DefectKind {
  DoubleFree,
  UseAfterFree,
  Taint,
  UncheckedUse,
  MemoryLeak,
  FileLeak
};

struct DefectReport {
  DefectKind kind = DefectKind::DoubleFree;
  QueryResult result;
  FlowNodeID sink = InvalidFlowID;
  std::optional<ObjectID> witnessObject;
  std::vector<ObjectID> objects;
  std::size_t objectCount = 0;
  /// Native SVFG IDs present on witness nodes. History psi/phi nodes have no
  /// native ID, so this list need not have one entry per witness node.
  std::vector<NativeID> nativeWitness;
};
struct DefectScanOptions {
  bool materializeWitnesses = true;
  bool retainWitnesses = true;
  bool firstFinding = false;
  /// Disable only for count-only scans; other modes retain object identities.
  bool retainObjects = true;
};

struct DefectScan {
  QueryStatus status = QueryStatus::NotFound;
  SearchCompletion completion;
  std::vector<DefectReport> findings;
  ObjectBatchStatistics statistics;
  bool exhaustive = true;
  double searchMilliseconds = 0, reportingMilliseconds = 0;
  std::string message;
  /// Deferred proof recovery. Graph must be the unchanged graph used to scan.
  DefectReport materialize(std::size_t finding,
                           const TraceFlowGraph &graph) const;

private:
  friend class DefectDetector;
  std::shared_ptr<ObjectBatchResult> Witnesses;
  std::optional<Query> WitnessQuery;
  const TraceFlowGraph *SourceGraph = nullptr;
  std::uint64_t GraphRevision = 0;
};

/// Run a defect rule over an already constructed and annotated UseTraceSSA graph.
/// Found means a potential defect in the supplied abstraction. No required
/// facts, incomplete modeling, or exhausted budgets yield Unknown, never Safe.
/// Clients supply generic events or guarded temporal effects before calling run.
class DefectDetector {
public:
  explicit DefectDetector(const TraceFlowGraph &graph,
                          std::optional<std::size_t> contextLimit = DEFAULT_CONTEXT_LIMIT,
                          SearchLimits limits = {}, ContextMode context = ContextMode::Realizable)
      : Graph(graph), ContextLimit(contextLimit), Limits(limits), Context(context) {}
  DefectReport run(DefectKind kind,
                   std::vector<FlowNodeID> roots = {},
                   std::vector<FlowNodeID> uses = {}) const;
  /// Enumerate one witness per resource sink site.
  /// An issue can make the negative result Unknown even after all modeled
  /// sites have been visited.
  DefectScan scan(DefectKind kind, DefectScanOptions options = {}) const;
  /// Stop at any valid resource finding; scan() retains deterministic sink
  /// order.
  DefectReport findAny(DefectKind kind) const;

private:
  DefectScan scanImpl(DefectKind kind, bool firstOnly,
                      DefectScanOptions options = {}) const;
  const TraceFlowGraph &Graph;
  std::optional<std::size_t> ContextLimit;
  SearchLimits Limits;
  ContextMode Context;
};

} // namespace usetracessa
} // namespace lotus
