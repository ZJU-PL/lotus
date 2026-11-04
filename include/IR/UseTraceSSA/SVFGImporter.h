#ifndef LOTUS_IR_USETRACESSA_SVFGIMPORTER_H
#define LOTUS_IR_USETRACESSA_SVFGIMPORTER_H

#include "IR/UseTraceSSA/TraceFlowGraph.h"

namespace lotus {
namespace usetracessa {

// ---------------------------------------------------------------------------
// Port resolution: maps SVFG-level identity to TraceFlowGraph node IDs.
// ---------------------------------------------------------------------------

enum class PortKind { Definition, BeforeUse, AfterUse, Version, FlowNode };
struct Port {
  PortKind kind = PortKind::Definition;
  FunctionID function = InvalidID;
  ValueID value = InvalidID;
  SiteID site = InvalidID;
  VersionID version = InvalidID;
  FlowNodeID node = InvalidFlowID;
  static Port definition(FunctionID f, ValueID v);
  static Port afterUse(FunctionID f, SiteID site, ValueID v);
  static Port flowNode(FlowNodeID node);
  FlowNodeID resolve(const TraceFlowGraph &graph) const;
};

/// A node output is a scalar SSA definition, a MemorySSA region/version, or
/// an actual/formal boundary port. Different memory versions MUST be distinct.
struct SVFGNodeRecord {
  NativeID id = NoNativeID;
  Port output;
};
struct SVFGEdgeRecord {
  NativeID id = NoNativeID;
  NativeID from = NoNativeID, to = NoNativeID;
  FlowKind kind = FlowKind::Direct;
  CallSiteID callSite = NoNativeID;
  ObjectSet objects = ObjectSet::unknown();
  std::uint64_t nativeKind = 0;
  std::string guard;
  /// For local def-use flow: the use of the SOURCE channel at the consumer.
  /// Must be AfterUse, so the raw SVFG edge cannot bypass earlier checks/uses.
  std::optional<Port> consumption;
  /// Only for true boundary ports (actual/formal, entry/exit, or summaries),
  /// whose output already denotes state at the transfer point.
  bool sourceIsBoundary = false;
};
struct SVFGSnapshot {
  std::vector<SVFGNodeRecord> nodes;
  std::vector<SVFGEdgeRecord> edges;
  std::vector<std::string> issues;
};
struct SVFGImportResult {
  std::map<NativeID, FlowNodeID> nodes;
  std::map<NativeID, FlowEdgeID> edges;
};

// ---------------------------------------------------------------------------
// Located records: normalised SVFG nodes/edges with site layout information.
// ---------------------------------------------------------------------------

struct FunctionLayout {
  FunctionID id = InvalidID;
  std::string name;
  /// Only topology and operation order are used. An operation can be on a
  /// block or a CFG edge. Give distinct semantic phases distinct sites.
  Program control;
};
using LocatedEventEffect = GuardedEventEffect;
struct LocatedSVFGNode {
  NativeID id = NoNativeID;
  FunctionID function = InvalidID;
  /// InvalidID denotes live-on-entry (formal parameters / memory inputs).
  SiteID definitionSite = InvalidID;
  std::string label;
  Event definitionEvents = Event::None;
  Certainty certainty = Certainty::Must;
  /// Authoritative allocation/resource facts, independent of adjacent edges.
  std::vector<LocatedEventEffect> definitionEffects = {};
};
struct LocatedSVFGEdge {
  NativeID id = NoNativeID, from = NoNativeID, to = NoNativeID;
  FlowKind kind = FlowKind::Direct;
  CallSiteID callSite = NoNativeID;
  ObjectSet objects;
  std::uint64_t nativeKind = 0;
  /// Local uses default to the target definition's site. LLVM PHI / MemorySSA
  /// phi operands MUST explicitly name their incoming-edge site here.
  SiteID consumerSite = InvalidID;
  /// Only actual/formal, entry/exit or explicit summary ports may omit a local
  /// use. Interprocedural transfers must opt in and retain call-site metadata.
  bool boundary = false;
  Event useEvents = Event::None;
  Certainty certainty = Certainty::Must;
  std::string guard;
  /// Ordinary useEvents remain generic. Resource uses carry their own guards.
  std::vector<LocatedEventEffect> useEffects;
};
struct SVFGConstructionInput {
  std::vector<FunctionLayout> functions;
  std::vector<LocatedSVFGNode> nodes;
  std::vector<LocatedSVFGEdge> edges;
  std::vector<std::string> issues;
};
struct SVFGHistoryResult {
  TraceFlowGraph graph;
  SVFGImportResult native;
  std::map<NativeID, ValueID> channels;
  std::map<FunctionID, std::map<SiteID, SiteID>> sites;
};

// ---------------------------------------------------------------------------
// SVFGImporter: builds history SSA from located SVFG records and imports
// the resulting typed transfers into a TraceFlowGraph.
// ---------------------------------------------------------------------------

/// Builds history SSA for the existing SVFG's scalar and MemorySSA-version
/// channels, then REPLACES each local raw def-use transfer by an after-use
/// transfer. Input contains no alias solver and no recomputed points-to sets.
///
/// This normalized API is deliberately independent of LOTUS SVFG subclasses.
/// A native frontend supplies instruction/edge locations and copies upstream
/// edge metadata; there is no safe way to recover use order from adjacency alone.
class SVFGImporter {
public:
  static SVFGHistoryResult build(const SVFGConstructionInput &input);

  /// Strict, transactional import. Invalid/missing use bindings throw; the
  /// supplied graph is unchanged. No pointer analysis is run or replaced.
  static SVFGImportResult append(TraceFlowGraph &graph, const SVFGSnapshot &snapshot);

  /// Generic SVF-style graph iterator bridge.
  template <typename NativeGraph, typename NodeMapper, typename EdgeMapper>
  static SVFGImportResult appendNative(TraceFlowGraph &graph, const NativeGraph &native,
                                      NodeMapper nodeMapper, EdgeMapper edgeMapper) {
    SVFGSnapshot view;
    for (const auto &item : native) {
      const auto &node = *item.second;
      view.nodes.push_back(nodeMapper(node));
      for (auto it = node.OutEdgeBegin(); it != node.OutEdgeEnd(); ++it)
        view.edges.push_back(edgeMapper(**it));
    }
    return append(graph, view);
  }
};

} // namespace usetracessa
} // namespace lotus
#endif
