#include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAAAdapter.h"

#include <stdexcept>
#include <string>

namespace lotus::cfl::classical::engines::subcubic {

Problem toSubcubicAliasProblem(const LabeledGraph &graph) {
  for (const auto &[label, edges] : graph.symbolPairs()) {
    if (label != "a" && label != "abar" && label != "d" &&
        label != "dbar") {
      throw std::invalid_argument(
          "Subcubic alias analysis supports only a/abar/d/dbar PEG labels; "
          "unsupported label: " +
          label);
    }
    const std::string reverse = LabeledGraph::complementLabel(label);
    for (const auto &[source, target] : edges) {
      if (!graph.hasEdge(target, source, reverse)) {
        throw std::invalid_argument(
            "Subcubic alias analysis requires a bidirected PEG: missing " +
            graph.vertexName(target) + " --" + reverse + "--> " +
            graph.vertexName(source) +
            "; use an explicit bidirectional graph transformation if the "
            "input omits reverse edges");
      }
    }
  }

  Problem problem;
  problem.nodes = graph.vertexCount();
  problem.assignments = graph.edgesForLabel("a");
  problem.dereferences = graph.edgesForLabel("d");
  return problem;
}

} // namespace lotus::cfl::classical::engines::subcubic
