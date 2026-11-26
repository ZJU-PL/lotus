#pragma once


#include "IR/UseTraceSSA/TraceFlowGraph.h"
#include "IR/UseTraceSSA/SearchBudget.h"
#include <llvm/ADT/BitVector.h>
#include <memory>

namespace lotus {
namespace usetracessa {

enum class ContextMode {
  Insensitive,
  /// All calls/returns on a witness must match; empty initial/final stack.
  Balanced,
  /// Any contiguous segment of a well-nested execution. Allows unmatched
  /// returns at the beginning and unmatched calls at the end, never mismatches.
  Realizable
};
enum class QueryStatus { Found, NotFound, Unknown };

/// Deterministic event automaton. May-events additionally have a no-effect
/// transition. Bounded/context-insensitive searches build the product lazily
/// while solving; unbounded searches tabulate matched-call summaries.
struct Automaton {
  ID states = 1;
  ID initial = 0;
  std::vector<ID> accepting = {0};
  std::function<ID(ID, Event)> transition;
  static Automaton ordered(std::vector<Event> sequence);
  static Automaton doubleFree();
  static Automaton useAfterFree();
  static Automaton memoryLeak();
  static Automaton fileLeak();
};

struct Query {
  std::vector<FlowNodeID> sources;
  std::vector<FlowNodeID> sinks;
  /// Structural traps include endpoints and unconditionally block the node.
  std::vector<FlowNodeID> traps;
  /// Must events block; May events retain the no-effect alternative.
  Event trapEvents = Event::None;
  /// Optional event required on the accepting sink for the selected object.
  Event sinkEvents = Event::None;
  ContextMode context = ContextMode::Realizable;
  /// Defaults to depth 3. Unset uses unbounded Dyck summaries. A supplied k
  /// retains call strings using Saber's limit semantics: k=0 merges context
  /// from the first call.
  std::optional<std::size_t> contextLimit = DEFAULT_CONTEXT_LIMIT;
  Automaton automaton;
  bool requireNonEmpty = false;
  /// Threads have no sequential call-stack interpretation. Ignoring a reached
  /// thread edge yields Unknown on a negative result, not a safety proof.
  bool includeThreadEdges = false;
  /// Optional, FIXED memory-object filter. Never intersects points-to sets
  /// globally along a taint path: different accesses can address different objects.
  std::optional<ObjectID> memoryObject;
  std::function<bool(const FlowEdge &)> edgeFilter;
  /// Resource limits are opt-in; zero means unlimited.
  std::size_t maxProductStates = 0;
  std::size_t maxSummaryPairs = 0;
  /// Rendering limit only: truncating evidence does not interrupt the search.
  std::size_t maxWitnessEdges = 1000000;
};

inline void SearchLimits::apply(Query &query) const {
  query.maxProductStates = maxProductStates;
  query.maxSummaryPairs = maxSummaryPairs;
}

struct QueryResult {
  QueryStatus status = QueryStatus::NotFound;
  SearchCompletion completion;
  std::vector<FlowNodeID> nodes;
  std::vector<FlowEdgeID> edges;
  std::vector<ID> automatonStates;
  bool witnessComplete = true;
  std::size_t productStates = 0;
  std::size_t edgesExamined = 0;
  std::size_t summaryPairs = 0;
  std::string message;
  /// Found means a witness IN THIS ABSTRACTION, not a proof of concrete feasibility.
  bool found() const { return status == QueryStatus::Found; }
};

struct QueryScanResult {
  QueryStatus status = QueryStatus::NotFound;
  SearchCompletion completion;
  std::map<FlowNodeID, QueryResult> foundAt;
  /// False when modeling or budgets prevent exhaustive sink enumeration.
  bool complete = true;
  std::size_t productStates = 0, edgesExamined = 0, summaryPairs = 0;
  std::string message;
};

/// Complements exist only inside this finite query universe. Graph ObjectSet
/// retains its TOP/BOTTOM lattice; in particular TOP minus a finite set is
/// never written back to the graph.
using ObjectMask = llvm::BitVector;
class ObjectUniverse {
public:
  explicit ObjectUniverse(std::vector<ObjectID> objects = {});
  const std::vector<ObjectID> &objects() const { return Objects; }
  ObjectMask all() const { return ObjectMask(Objects.size(), true); }
  ObjectMask none() const { return ObjectMask(Objects.size(), false); }
  ObjectMask mask(const ObjectSet &set) const;
  std::size_t index(ObjectID object) const;
private:
  std::vector<ObjectID> Objects;
};
struct ObjectBatchQuery {
  /// This API explicitly requests same-object semantics. Ordinary run(Query)
  /// stays generic unless Query::memoryObject is set. A batch must not also
  /// specify memoryObject; its universe is the only source of object identity.
  Query query;
  ObjectUniverse universe;
  /// Retain shared path provenance for bounded-context and context-insensitive
  /// queries. Unbounded Dyck queries use runToSinks() for witness construction.
  bool retainWitnesses = false;
  /// Existential search. Unvisited objects remain Unknown and enumeration is
  /// explicitly incomplete when the first valid sink/object is accepted.
  bool stopAfterFirstFinding = false;
  /// Avoid exporting a dense public bitmap per sink for count-only scans.
  bool retainSinkMasks = true;
};
struct ObjectSinkSummary {
  FlowNodeID sink;
  ObjectID representative;
  std::size_t objects;
};
struct ObjectBatchStatistics {
  std::size_t productStates = 0, productEdges = 0, edgesExamined = 0;
  std::size_t summaryPairs = 0, maskIntersections = 0, maskUnions = 0;
  std::size_t nonemptyDeltas = 0, candidateObjects = 0;
  std::size_t foundObjects = 0, notFoundObjects = 0, unknownObjects = 0;
};
struct ObjectWitnessData;
struct ObjectBatchResult {
  SearchCompletion completion;
  ObjectUniverse universe;
  ObjectMask found, notFound, unknown;
  /// Accepted masks per sink allow one shared scan with lazy witnesses.
  std::map<FlowNodeID, ObjectMask> foundAt;
  std::vector<ObjectSinkSummary> sinks;
  ObjectBatchStatistics statistics;
  /// Stronger than unknown.none(): a budget can stop enumeration after every
  /// object already has one witness, while other accepting sites remain unseen.
  bool complete = true;
  std::string message;
  QueryStatus status(ObjectID object) const;
  bool hasWitnesses() const { return bool(Witnesses); }
  /// Recover evidence for an accepted sink/object without another graph search.
  /// Returns Unknown when provenance was not retained for this query mode.
  QueryResult witness(FlowNodeID sink, ObjectID object) const;
private:
  friend class QueryEngine;
  std::shared_ptr<ObjectWitnessData> Witnesses;
};

enum class CoverageStatus { SinkUnreachable, AllPathsTrapped, UntrappedPath, Unknown };
struct CoverageResult {
  CoverageStatus status = CoverageStatus::Unknown;
  QueryResult evidence;
};

class QueryEngine {
public:
  explicit QueryEngine(const TraceFlowGraph &graph) : G(graph) {}
  QueryResult run(const Query &query) const;
  /// Build the product and context summaries once, retaining one witness for
  /// each reachable sink. Supports generic and fixed-object queries.
  QueryScanResult runToSinks(const Query &query) const;
  ObjectBatchResult runObjects(const ObjectBatchQuery &query) const;
  std::vector<QueryResult> runBatch(const std::vector<Query> &queries) const;
  /// Distinguishes vacuous coverage (sink unreachable even without traps).
  CoverageResult allPathsHitTraps(const Query &query) const;
  /// Context-insensitive structural slices. Use run() for context-sensitive
  /// source/sink witnesses. Reverse slicing never silently claims realizability.
  std::vector<FlowNodeID> slice(const std::vector<FlowNodeID> &seeds,
                              bool backward = false,
                              const std::vector<FlowNodeID> &traps = {}) const;
private:
  QueryResult runImpl(const Query &query, QueryScanResult *scan) const;
  const TraceFlowGraph &G;
};

} // namespace usetracessa
} // namespace lotus
