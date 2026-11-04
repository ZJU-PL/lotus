#ifndef LOTUS_IR_USETRACESSA_SVFGBRIDGE_H
#define LOTUS_IR_USETRACESSA_SVFGBRIDGE_H

#include "IR/SVFG/SVFG.h"
#include "IR/UseTraceSSA/SVFGImporter.h"

#include <cstdint>
#include <functional>
#include <utility>

namespace lotus {
namespace usetracessa {

enum class NativeHistoryMode { Full, DoubleFree, UseAfterFree, MemoryLeak, FileLeak };
struct NativeHistoryOptions {
  /// False keeps opcode/site identities and debug locations without printing
  /// the whole LLVM module. Exact instruction text remains the API default.
  bool instructionText = true;
  /// Optional phase observer, in milliseconds; it never times upstream SVFG.
  std::function<void(const char *, double)> phaseTiming = {};
};

/// Construct ordered histories from this revision's SVFG and the LLVM CFG.
/// Sites are derived from instructions, memory phases, CFG edges and function
/// boundaries. Select the property before construction; Full explicitly builds
/// the general overlay. Property modes retain their events, source-object
/// universe, and required call/return topology. Unknown source guards prevent
/// finite object pruning. Unsupported or ambiguous facts are recorded as issues,
/// so negative queries remain Unknown. Rebuild for a different property.
SVFGHistoryResult buildUseTraceSSAFromLotusSVFG(
    const analysis::SVFG &svfg, const llvm::Module &module,
    NativeHistoryMode mode, NativeHistoryOptions options = {});

/// Copy metadata carried by a Lotus SVFG edge. The caller assigns an ID and
/// supplies the exact consumer site (or marks a genuine boundary). Empty
/// points-to guards on indirect edges remain unknown, not known-empty.
inline LocatedSVFGEdge locateLotusSVFGEdge(const analysis::SVFGEdge &edge,
                                           NativeID id, SiteID consumerSite,
                                           bool boundary = false) {
  LocatedSVFGEdge located;
  located.id = id;
  located.from = edge.getSrcNode()->getId();
  located.to = edge.getDstNode()->getId();
  located.nativeKind = static_cast<std::uint64_t>(edge.getEdgeKind());
  located.consumerSite = consumerSite;
  located.boundary = boundary;
  if (edge.isThreadMHPEdge()) located.kind = FlowKind::Thread;
  else if (edge.isCallEdge()) located.kind = FlowKind::Call;
  else if (edge.isRetEdge()) located.kind = FlowKind::Return;
  else if (edge.isMemoryEdge() || edge.isIndirectEdge()) located.kind = FlowKind::Memory;
  if (const auto *call = edge.getCallSite())
    located.callSite = reinterpret_cast<std::uintptr_t>(call);
  if (!edge.getPointsTo().empty()) {
    std::vector<ObjectID> objects(edge.getPointsTo().begin(), edge.getPointsTo().end());
    located.objects = ObjectSet::known(std::move(objects));
  }
  return located;
}

/// NodeMapper: const SVFGNode& -> LocatedSVFGNode
/// EdgeMapper: const SVFGEdge& -> LocatedSVFGEdge
///
/// The mappers copy native locations, MemorySSA versions, points-to guards and
/// call-site identities. They must NOT rerun alias analysis or return raw
/// def-use shortcuts. LLVMHistoryResult::graph().program() and siteID()/uses()
/// provide the scalar layout; memory-phi edge locations and mu/chi phases are
/// supplied by the existing SVFG/MemorySSA producer.
///
/// The mappers supply semantic locations and metadata that cannot be recovered
/// from graph adjacency alone. Every native edge is visited exactly once.
template <typename NodeMapper, typename EdgeMapper>
SVFGHistoryResult buildUseTraceSSAFromSVFG(const analysis::SVFG &svfg,
                                         std::vector<FunctionLayout> layouts,
                                         NodeMapper nodeMapper, EdgeMapper edgeMapper) {
  SVFGConstructionInput input;
  input.functions = std::move(layouts);
  for (const auto &item : svfg) {
    const auto &node = *item.second;
    input.nodes.push_back(nodeMapper(node));
    for (const analysis::SVFGEdge *edge : node.getOutEdges())
      input.edges.push_back(edgeMapper(*edge));
  }
  return SVFGImporter::build(input);
}

} // namespace usetracessa
} // namespace lotus
#endif
