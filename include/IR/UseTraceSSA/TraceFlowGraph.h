#ifndef LOTUS_IR_USETRACESSA_TRACEFLOWGRAPH_H
#define LOTUS_IR_USETRACESSA_TRACEFLOWGRAPH_H

#include "IR/UseTraceSSA/UseTraceSSA.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace lotus {
namespace usetracessa {

// The object-expanded UFG shares these records and can exceed 2^32 nodes or
// edges. Intraprocedural history and automaton IDs remain independently sized.
using FlowNodeID = std::uint64_t;
using FlowEdgeID = std::uint64_t;
constexpr FlowNodeID InvalidFlowID = std::numeric_limits<FlowNodeID>::max();
using FunctionID = ID;
using ObjectID = std::uint64_t;
using NativeID = std::uint64_t;
using CallSiteID = std::uint64_t;
constexpr NativeID NoNativeID = std::numeric_limits<NativeID>::max();

/// Unknown is TOP, not the empty set. A known empty set is BOTTOM. IDs are
/// opaque IDs from the upstream pointer analysis, never equivalence classes.
class ObjectSet {
public:
  static ObjectSet unknown();
  static ObjectSet known(std::vector<ObjectID> objects);
  bool isUnknown() const { return Unknown; }
  bool empty() const { return !Unknown && !Objects; }
  bool contains(ObjectID object) const;
  bool intersects(const ObjectSet &other) const;
  const std::vector<ObjectID> &objects() const;

private:
  bool Unknown = true;
  std::shared_ptr<const std::vector<ObjectID>> Objects;
};

enum class Event : std::uint32_t {
  None = 0, Source = 1u << 0, Sink = 1u << 1, Sanitize = 1u << 2,
  Allocate = 1u << 3, Release = 1u << 4, Dereference = 1u << 5,
  Read = 1u << 6, Write = 1u << 7, NonNull = 1u << 8,
  IsNull = 1u << 9,
  UnknownEffect = 1u << 10, Exit = 1u << 11,
  Open = 1u << 12, Close = 1u << 13, Escape = 1u << 14
};
inline Event operator|(Event a, Event b) {
  return static_cast<Event>(static_cast<std::uint32_t>(a) |
                            static_cast<std::uint32_t>(b));
}
inline bool hasEvent(Event value, Event mask) {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(mask)) != 0;
}

enum class Certainty { Must, May };
/// Separate records are intentional: certainty is combined only after selecting
/// one object (or partitioning a finite query universe).
struct GuardedEventEffect {
  Event events = Event::None;
  ObjectSet objects = ObjectSet::unknown();
  Certainty certainty = Certainty::Must;
};
struct EffectiveEvent {
  Event events = Event::None;
  Certainty certainty = Certainty::Must;
};
constexpr ObjectID UnknownResource = std::numeric_limits<ObjectID>::max();
enum class FlowKind { History, Direct, Memory, Call, Return, Summary,
                      Dependence, Thread };

struct FlowNode {
  FlowNodeID id = InvalidFlowID;
  std::string label;
  /// Structured debug location; instruction operands can also contain brackets.
  std::string sourceLocation;
  FunctionID function = InvalidID;
  VersionID version = InvalidID;
  NativeID native = NoNativeID;
  Event events = Event::None;
  Certainty certainty = Certainty::Must;
  std::vector<GuardedEventEffect> effects = {};
};

struct FlowEdge {
  FlowEdgeID id = InvalidFlowID;
  FlowNodeID from = InvalidFlowID, to = InvalidFlowID;
  FlowKind kind = FlowKind::Direct;
  CallSiteID callSite = NoNativeID;
  ObjectSet objects = ObjectSet::unknown();
  NativeID native = NoNativeID;
  /// Retains the original upstream enum without making assumptions about it.
  std::uint64_t nativeKind = 0;
  RegionID predecessor = InvalidID;
  std::string guard;
  bool enabled = true;
};

struct HistoryLayer {
  FunctionID function = InvalidID;
  std::string name;
  Graph history;
  std::vector<FlowNodeID> versions;
};

struct FlowStatistics {
  std::size_t historyNodes = 0, historyPsiNodes = 0, historyPhiNodes = 0;
  std::size_t flowEdges = 0, guardedEffects = 0;
  std::size_t knownObjectCardinality = 0, unknownObjectSets = 0;
};

/// Non-mutating overlay: history edges encode temporal uses; imported SVFG
/// edges encode value transfer. Keep them typed: this is NOT a transitive
/// closure of may-alias, nor a new pointer analysis.
class TraceFlowGraph {
public:
  FlowNodeID addNode(FlowNode node);
  FlowEdgeID addEdge(FlowEdge edge);
  /// One layer per channel family; no layer or value encodes object identity.
  void addHistory(FunctionID function, std::string name, Graph history);
  FlowNodeID version(FunctionID function, VersionID version) const;
  FlowNodeID definition(FunctionID function, ValueID value) const;
  FlowNodeID before(FunctionID function, SiteID site, ValueID value) const;
  FlowNodeID after(FunctionID function, SiteID site, ValueID value) const;
  const HistoryLayer &layer(FunctionID function) const;
  void annotate(FlowNodeID id, Event events, Certainty certainty = Certainty::Must);
  void annotate(FlowNodeID id, Event events, ObjectSet objects,
                Certainty certainty = Certainty::Must);
  EffectiveEvent effectiveEvent(FlowNodeID id, ObjectID object) const;
  /// Uses the property universe when supplied. Otherwise includes the sentinel
  /// for unknown guarded effects, not ordinary TOP history edges. TOP alone
  /// never makes the graph incomplete.
  std::vector<ObjectID> resourceCandidates() const;
  /// Restrict candidates to the sources of one property. Event/topology edits
  /// invalidate this derived universe. TOP guards remain TOP in the graph.
  void setResourceUniverse(std::vector<ObjectID> objects);
  FlowStatistics statistics() const;
  void setNative(FlowNodeID id, NativeID native);
  void setSourceLocation(FlowNodeID id, std::string location);
  void disableEdge(FlowEdgeID id);
  void addIssue(std::string issue);
  bool complete() const { return Issues.empty(); }
  const std::vector<std::string> &issues() const { return Issues; }
  const std::vector<FlowNode> &nodes() const { return Nodes; }
  const std::vector<FlowEdge> &edges() const { return Edges; }
  const FlowNode &node(FlowNodeID id) const { return Nodes.at(id); }
  const FlowEdge &edge(FlowEdgeID id) const { return Edges.at(id); }
  const std::vector<FlowEdgeID> &outgoing(FlowNodeID id) const { return Out.at(id); }
  const std::vector<FlowEdgeID> &incoming(FlowNodeID id) const { return In.at(id); }
  std::vector<FlowNodeID> select(Event events) const;
  std::uint64_t revision() const { return Revision; }
  bool verify(std::string *error = nullptr) const;
  void printDOT(std::ostream &out) const;
  void printJSON(std::ostream &out) const;
private:
  std::vector<FlowNode> Nodes;
  std::vector<FlowEdge> Edges;
  std::vector<std::vector<FlowEdgeID>> Out, In;
  std::map<FunctionID, HistoryLayer> Layers;
  std::vector<std::string> Issues;
  std::optional<std::vector<ObjectID>> ResourceUniverse;
  std::uint64_t Revision = 0;
};

} // namespace usetracessa
} // namespace lotus
#endif
