#include "CFL/Certificates/LotusAdapter.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace lotus::cfl::certificates {
namespace {
int checkedSize(std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::length_error("CFL certificate dimensions exceed int range");
  return static_cast<int>(size);
}
} // namespace

Query makeQuery(const classical::Grammar &grammar,
                const classical::LabeledGraph &graph, std::size_t source,
                std::size_t target) {
  if (grammar.usesUnidirectionalMetadata())
    throw std::invalid_argument("CFL certificates require a declarative CFG; "
                                "Insert/Follow/Count metadata is unsupported");
  for (const auto &issue : grammar.validate())
    if (issue.severity == classical::GrammarIssueSeverity::Error)
      throw std::invalid_argument(issue.message);
  if (source >= graph.vertexCount() || target >= graph.vertexCount())
    throw std::out_of_range("CFL certificate query endpoint out of range");

  Query query;
  auto &out = query.grammar;
  out.nonterminals = checkedSize(grammar.symbolCount());
  out.terminals = out.nonterminals;
  out.start = checkedSize(grammar.startSymbolId());
  for (int symbol = 0; symbol < out.nonterminals; ++symbol)
    out.productions.push_back({symbol, {cflcert::cfg::Symbol::T(symbol)}});
  for (const auto &entry : grammar.productions()) {
    const int lhs = checkedSize(grammar.symbolId(entry.first));
    for (const auto &rhs : entry.second) {
      cflcert::cfg::GeneralProduction rule;
      rule.lhs = lhs;
      for (const auto &symbol : rhs)
        if (symbol != classical::Grammar::kEpsilonSymbol)
          rule.rhs.push_back(
              cflcert::cfg::Symbol::N(checkedSize(grammar.symbolId(symbol))));
      out.productions.push_back(std::move(rule));
    }
  }
  // ProductionMap is unordered; stable ordering makes certificates portable
  // for a fixed Lotus symbol table in independent verifier processes.
  std::sort(out.productions.begin(), out.productions.end(),
            [](const auto &a, const auto &b) {
              if (a.lhs != b.lhs) return a.lhs < b.lhs;
              return std::lexicographical_compare(
                  a.rhs.begin(), a.rhs.end(), b.rhs.begin(), b.rhs.end(),
                  [](const auto &x, const auto &y) {
                    return std::make_pair(x.terminal, x.id) <
                           std::make_pair(y.terminal, y.id);
                  });
            });
  query.graph.n = checkedSize(graph.vertexCount());
  query.graph.source = checkedSize(source);
  query.graph.target = checkedSize(target);
  for (const auto &edge : graph.edges()) {
    if (!grammar.hasSymbol(edge.label))
      throw std::invalid_argument("Graph uses unknown grammar symbol: " + edge.label);
    query.graph.edges.push_back({checkedSize(edge.source),
                                 checkedSize(edge.target),
                                 checkedSize(grammar.symbolId(edge.label))});
  }
  return query;
}

cflcert::frontend::CFGReachabilityResult
solve(const classical::Grammar &grammar, const classical::LabeledGraph &graph,
      std::size_t source, std::size_t target) {
  const auto query = makeQuery(grammar, graph, source, target);
  return cflcert::frontend::solveCFG(query.grammar, query.graph);
}

bool verify(const classical::Grammar &grammar,
            const classical::LabeledGraph &graph, std::size_t source,
            std::size_t target,
            const cflcert::frontend::CFGReachabilityResult &result) {
  const auto query = makeQuery(grammar, graph, source, target);
  return cflcert::frontend::verifyCFGResult(query.grammar, query.graph, result);
}
} // namespace lotus::cfl::certificates
