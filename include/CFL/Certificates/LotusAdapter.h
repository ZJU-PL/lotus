#pragma once

#include "CFL/Certificates/Frontend.h"
#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

namespace lotus::cfl::certificates {

/// A single source/target query using the Lotus start symbol. Dense symbol IDs
/// follow Grammar::symbolId; graph vertex IDs are preserved. Every Lotus symbol
/// has a terminal bridge so edges labeled by nonterminals remain valid seeds.
struct Query {
  cflcert::cfg::GeneralGrammar grammar;
  cflcert::cfg::Graph graph;
};

/// Throws for unknown edge labels, invalid endpoints, dimensions
/// exceeding int, invalid grammars, or operational Insert/Follow/Count
/// metadata.
Query makeQuery(const classical::Grammar &grammar,
                const classical::LabeledGraph &graph, std::size_t source,
                std::size_t target);

cflcert::frontend::CFGReachabilityResult
solve(const classical::Grammar &grammar, const classical::LabeledGraph &graph,
      std::size_t source, std::size_t target);

/// Rebuild the query from the original Lotus inputs before checking a result.
bool verify(const classical::Grammar &grammar,
            const classical::LabeledGraph &graph, std::size_t source,
            std::size_t target,
            const cflcert::frontend::CFGReachabilityResult &result);

} // namespace lotus::cfl::certificates
