// SPDX-License-Identifier: MIT
#pragma once
#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Core/Relation.h"
#include <memory>

namespace lotus::cfl::classical::engines::sparse_subset {
/// All-pairs CFL saturation using the paper's two-sided incremental set-pair
/// reports. The returned relation contains every terminal and nonterminal fact.
std::unique_ptr<Relation> solveAllPairs(const LabeledGraph &graph,
                                        const Grammar &grammar,
                                        double epsilon = 0.25);
} // namespace lotus::cfl::classical::engines::sparse_subset
