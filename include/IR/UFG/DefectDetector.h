#pragma once

#include "IR/UFG/UFG.h"
#include "IR/UseTraceSSA/DefectDetector.h"

namespace lotus {
namespace ufg {

using usetracessa::DefectKind;
using usetracessa::DefectReport;
using usetracessa::DefectScan;

/// Resource scans run one UFG tabulation per object and collect every accepted
/// site. Automata and report types remain compatible with UseTraceSSA.
class DefectDetector {
public:
  explicit DefectDetector(const UFGGraph &graph,
                          std::optional<std::size_t> contextLimit =
                              usetracessa::DEFAULT_CONTEXT_LIMIT,
                          usetracessa::SearchLimits limits = {},
                          usetracessa::ContextMode context = usetracessa::ContextMode::Realizable)
      : Graph(graph), ContextLimit(contextLimit), Limits(limits), Context(context) {}
  DefectReport run(DefectKind kind, std::vector<FlowNodeID> roots = {},
                   std::vector<FlowNodeID> uses = {}) const;
  DefectScan scan(DefectKind kind) const;

private:
  const UFGGraph &Graph;
  std::optional<std::size_t> ContextLimit;
  usetracessa::SearchLimits Limits;
  usetracessa::ContextMode Context;
};

} // namespace ufg
} // namespace lotus
