#pragma once

#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAA.h"

namespace lotus::cfl::classical::engines::subcubic {

/// Convert a bidirected a/abar/d/dbar PEG without changing its vertex IDs.
/// Unsupported labels or missing reverse edges throw std::invalid_argument.
/// For a graph containing only physical a/d edges, callers may explicitly
/// apply graph.transformed(EdgeDirection::Bidirectional) before conversion.
/// solve() checks the PEG's assignment-target structural precondition.
Problem toSubcubicAliasProblem(const LabeledGraph &graph);

} // namespace lotus::cfl::classical::engines::subcubic
