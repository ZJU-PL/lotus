#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfGrammar.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfGraph.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfTypes.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::staged_bounds::detail {

namespace mr = mutual_refinement;

struct LabelHash {
  std::size_t operator()(const Label &label) const {
    std::size_t seed = static_cast<std::size_t>(label.kind);
    seed ^= std::hash<unsigned>{}(label.id) + 0x9e3779b9U + (seed << 6U) +
            (seed >> 2U);
    return seed;
  }
};
struct EncodedGrammar {
  mr::CnfGrammar grammar;
  std::unordered_map<Vertex, int> vertex_to_dense;
  std::vector<Vertex> dense_to_vertex;
  std::unordered_map<Label, int, LabelHash> label_to_terminal;
  std::unordered_map<int, Label> terminal_to_label;
  std::unordered_set<mr::Edge, mr::EdgeHasher> edges;
  int next_symbol = 0;

  explicit EncodedGrammar(const Graph &graph) {
    for (Vertex vertex : graph.vertices()) {
      const int dense = static_cast<int>(dense_to_vertex.size());
      vertex_to_dense.emplace(vertex, dense);
      dense_to_vertex.push_back(vertex);
    }

    std::vector<Label> labels;
    std::unordered_set<Label, LabelHash> seen;
    for (const Edge &edge : graph.edges()) {
      if (seen.insert(edge.label).second) {
        labels.push_back(edge.label);
      }
    }
    std::sort(labels.begin(), labels.end(),
              [](const Label &left, const Label &right) {
                return std::tie(left.kind, left.id) <
                       std::tie(right.kind, right.id);
              });
    for (const Label &label : labels) {
      const int symbol = next_symbol++;
      label_to_terminal.emplace(label, symbol);
      terminal_to_label.emplace(symbol, label);
      grammar.addTerminal(symbol);
    }
    for (const Edge &edge : graph.edges()) {
      edges.insert(std::make_tuple(vertex_to_dense.at(edge.source),
                                   label_to_terminal.at(edge.label),
                                   vertex_to_dense.at(edge.target)));
    }
  }

  int nonterminal() {
    const int symbol = next_symbol++;
    grammar.addNonterminal(symbol);
    return symbol;
  }

  std::optional<int> terminal(const Label &label) const {
    const auto found = label_to_terminal.find(label);
    if (found == label_to_terminal.end()) {
      return std::nullopt;
    }
    return found->second;
  }
};

void addTerminalProduction(EncodedGrammar &encoded, int lhs,
                           const Label &label) {
  if (const auto terminal = encoded.terminal(label)) {
    encoded.grammar.addUnaryProduction(lhs, *terminal);
  }
}

void addWrappedProductions(EncodedGrammar &encoded, int relation,
                           Alphabet alphabet,
                           const std::vector<unsigned> &ids) {
  for (unsigned id : ids) {
    const auto open_terminal = encoded.terminal(openLabel(alphabet, id));
    const auto close_terminal = encoded.terminal(closeLabel(alphabet, id));
    if (!open_terminal || !close_terminal) {
      continue;
    }
    const int open_nonterminal = encoded.nonterminal();
    const int close_nonterminal = encoded.nonterminal();
    const int tail = encoded.nonterminal();
    encoded.grammar.addUnaryProduction(open_nonterminal, *open_terminal);
    encoded.grammar.addUnaryProduction(close_nonterminal, *close_terminal);
    encoded.grammar.addBinaryProduction(tail, relation, close_nonterminal);
    encoded.grammar.addBinaryProduction(relation, open_nonterminal, tail);
  }
}

EncodedGrammar buildClassicGrammar(const Graph &graph, Alphabet balanced) {
  EncodedGrammar encoded(graph);
  const int start = encoded.nonterminal();
  encoded.grammar.addStartSymbol(start);
  encoded.grammar.addEmptyProduction(start);
  addTerminalProduction(encoded, start, Label::neutral());

  const Alphabet ignored = balanced == Alphabet::Parenthesis
                               ? Alphabet::Bracket
                               : Alphabet::Parenthesis;
  for (const auto &[label, terminal] : encoded.label_to_terminal) {
    if (belongsTo(label.kind, ignored)) {
      encoded.grammar.addUnaryProduction(start, terminal);
    }
  }
  encoded.grammar.addBinaryProduction(start, start, start);
  addWrappedProductions(encoded, start, balanced,
                        matchedLabelIds(graph, balanced));
  encoded.grammar.initFastIndices();
  return encoded;
}

EncodedGrammar buildCombinedGrammar(const Graph &graph) {
  EncodedGrammar encoded(graph);
  const int start = encoded.nonterminal();
  encoded.grammar.addStartSymbol(start);
  encoded.grammar.addEmptyProduction(start);
  addTerminalProduction(encoded, start, Label::neutral());
  encoded.grammar.addBinaryProduction(start, start, start);
  addWrappedProductions(encoded, start, Alphabet::Parenthesis,
                        matchedLabelIds(graph, Alphabet::Parenthesis));
  addWrappedProductions(encoded, start, Alphabet::Bracket,
                        matchedLabelIds(graph, Alphabet::Bracket));
  encoded.grammar.initFastIndices();
  return encoded;
}

std::size_t parityState(unsigned mask, bool leading_close, bool trailing_open) {
  return (static_cast<std::size_t>(mask) * 2U +
          static_cast<unsigned>(leading_close)) *
             2U +
         static_cast<unsigned>(trailing_open);
}

EncodedGrammar buildParityGrammar(const Graph &graph, Alphabet balanced,
                                  unsigned parity_groups) {
  if (parity_groups == 0 || parity_groups > detail::MAX_PARITY_GROUPS) {
    throw std::invalid_argument("parity_groups must be between 1 and " +
                                std::to_string(detail::MAX_PARITY_GROUPS));
  }

  EncodedGrammar encoded(graph);
  const int start = encoded.nonterminal();
  const int empty = encoded.nonterminal();
  const unsigned mask_count = 1U << parity_groups;
  std::vector<int> states(static_cast<std::size_t>(mask_count) * 4U);
  for (int &state : states) {
    state = encoded.nonterminal();
  }

  encoded.grammar.addStartSymbol(start);
  encoded.grammar.addUnaryProduction(start, empty);
  encoded.grammar.addUnaryProduction(start,
                                     states[parityState(0, false, false)]);
  encoded.grammar.addEmptyProduction(empty);
  addTerminalProduction(encoded, empty, Label::neutral());
  encoded.grammar.addBinaryProduction(empty, empty, empty);

  const Alphabet ignored = balanced == Alphabet::Parenthesis
                               ? Alphabet::Bracket
                               : Alphabet::Parenthesis;
  const auto ignored_ids = labelIds(graph, ignored);
  std::unordered_map<unsigned, unsigned> group;
  for (std::size_t index = 0; index < ignored_ids.size(); ++index) {
    group[ignored_ids[index]] = static_cast<unsigned>(index) % parity_groups;
  }
  for (const auto &[label, terminal] : encoded.label_to_terminal) {
    if (!belongsTo(label.kind, ignored)) {
      continue;
    }
    const unsigned mask = 1U << group.at(label.id);
    const bool leading_close = !isOpen(label.kind);
    const bool trailing_open = isOpen(label.kind);
    encoded.grammar.addUnaryProduction(
        states[parityState(mask, leading_close, trailing_open)], terminal);
  }

  const auto balanced_ids = matchedLabelIds(graph, balanced);
  addWrappedProductions(encoded, empty, balanced, balanced_ids);
  for (int state : states) {
    addWrappedProductions(encoded, state, balanced, balanced_ids);
    encoded.grammar.addBinaryProduction(state, state, empty);
    encoded.grammar.addBinaryProduction(state, empty, state);
  }

  for (unsigned left_mask = 0; left_mask < mask_count; ++left_mask) {
    for (unsigned left_close = 0; left_close < 2; ++left_close) {
      for (unsigned left_open = 0; left_open < 2; ++left_open) {
        const int left =
            states[parityState(left_mask, left_close != 0, left_open != 0)];
        for (unsigned right_mask = 0; right_mask < mask_count; ++right_mask) {
          for (unsigned right_close = 0; right_close < 2; ++right_close) {
            for (unsigned right_open = 0; right_open < 2; ++right_open) {
              const int right = states[parityState(right_mask, right_close != 0,
                                                   right_open != 0)];
              const int output = states[parityState(
                  left_mask ^ right_mask, left_close != 0, right_open != 0)];
              encoded.grammar.addBinaryProduction(output, left, right);
            }
          }
        }
      }
    }
  }

  encoded.grammar.initFastIndices();
  return encoded;
}

ReachabilityRun
runGrammar(EncodedGrammar encoded, bool trace, bool factorized_tracing = false,
           const std::optional<Pair> &trace_pair = std::nullopt) {
  if (encoded.dense_to_vertex.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error("too many vertices for the CFL engine");
  }

  std::vector<mr::Edge> raw_result;
  std::unordered_set<mr::Edge, mr::EdgeHasher> closure;
  {
    // Release the saturated graph (and eager records) before materializing
    // the public PairSet and Graph. Otherwise both complete representations
    // coexist at the conversion peak, including in the untraced stages.
    mr::CnfGraph engine;
    engine.reinit(static_cast<int>(encoded.dense_to_vertex.size()),
                  encoded.edges);
    std::unordered_map<mr::Edge, std::unordered_set<int>, mr::EdgeHasher>
        unary_record;
    std::unordered_map<
        mr::Edge,
        std::unordered_set<std::tuple<int, int, int>, mr::IntTripleHasher>,
        mr::EdgeHasher>
        binary_record;
    if (trace && !factorized_tracing) {
      raw_result = engine.runCFLReachabilityCompact(
          encoded.grammar, unary_record, binary_record);
    } else {
      raw_result = engine.runCFLReachabilityCompact(encoded.grammar);
    }

    raw_result.erase(std::remove_if(raw_result.begin(), raw_result.end(),
                                    [](const mr::Edge &edge) {
                                      return std::get<0>(edge) ==
                                             std::get<2>(edge);
                                    }),
                     raw_result.end());
    if (trace) {
      // Reuse the start relation as the all-pairs roots. A targeted trace
      // needs only a singleton; do not copy the entire relation for either.
      std::vector<mr::Edge> target_roots;
      if (trace_pair) {
        const auto source = encoded.vertex_to_dense.find(trace_pair->source);
        const auto target = encoded.vertex_to_dense.find(trace_pair->target);
        if (source != encoded.vertex_to_dense.end() &&
            target != encoded.vertex_to_dense.end()) {
          const mr::Edge root{source->second, encoded.grammar.startSymbol,
                              target->second};
          if (source->second != target->second && engine.hasEdge(root)) {
            target_roots.push_back(root);
          }
        }
      }
      const auto &roots = trace_pair ? target_roots : raw_result;
      closure =
          factorized_tracing
              ? engine.getFactorizedEdgeClosureCompact(encoded.grammar, roots)
              : engine.getEdgeClosureCompact(encoded.grammar, roots,
                                             unary_record, binary_record);
    }
  }

  ReachabilityRun result;
  for (const mr::Edge &edge : raw_result) {
    result.pairs.insert({encoded.dense_to_vertex.at(std::get<0>(edge)),
                         encoded.dense_to_vertex.at(std::get<2>(edge))});
  }
  for (const mr::Edge &edge : closure) {
    const auto label = encoded.terminal_to_label.find(std::get<1>(edge));
    if (label == encoded.terminal_to_label.end()) {
      continue;
    }
    result.used_edges.addEdge(encoded.dense_to_vertex.at(std::get<0>(edge)),
                              encoded.dense_to_vertex.at(std::get<2>(edge)),
                              label->second);
  }
  return result;
}

ReachabilityRun runProjected(const Graph &graph, Alphabet balanced,
                             GrammarStrength strength, unsigned parity_groups,
                             bool trace, bool factorized_tracing,
                             const std::optional<Pair> &trace_pair) {
  if (strength == GrammarStrength::Parity) {
    return runGrammar(buildParityGrammar(graph, balanced, parity_groups), trace,
                      factorized_tracing, trace_pair);
  }
  return runGrammar(buildClassicGrammar(graph, balanced), trace,
                    factorized_tracing, trace_pair);
}

PairSet runClassicProjectedMapped(
    const Graph &graph, Alphabet balanced,
    const std::function<std::optional<Pair>(const Pair &)> &map_pair) {
  const auto encoded = buildClassicGrammar(graph, balanced);
  if (encoded.dense_to_vertex.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error("too many vertices for the CFL engine");
  }
  mr::CnfGraph engine;
  engine.reinit(static_cast<int>(encoded.dense_to_vertex.size()),
                encoded.edges);
  PairSet result;
  engine.runCFLReachabilityVisit(encoded.grammar, [&](const mr::Edge &edge) {
    const Pair pair{encoded.dense_to_vertex.at(std::get<0>(edge)),
                    encoded.dense_to_vertex.at(std::get<2>(edge))};
    if (pair.source != pair.target) {
      if (const auto mapped = map_pair(pair)) {
        result.insert(*mapped);
      }
    }
  });
  return result;
}

PairSet runCombined(const Graph &graph) {
  return runGrammar(buildCombinedGrammar(graph), false).pairs;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::detail

namespace lotus::cfl::interleaved_dyck::staged_bounds {

PairSet Solver::projectedReachability(const Graph &graph,
                                      Alphabet balanced_alphabet,
                                      GrammarStrength strength,
                                      unsigned parity_groups) const {
  return detail::runProjected(graph, balanced_alphabet, strength, parity_groups)
      .pairs;
}

PairSet Solver::intersection(const Graph &graph, GrammarStrength strength,
                             unsigned parity_groups) const {
  const PairSet alpha = projectedReachability(graph, Alphabet::Parenthesis,
                                              strength, parity_groups);
  const PairSet beta =
      projectedReachability(graph, Alphabet::Bracket, strength, parity_groups);
  return detail::intersect(alpha, beta);
}

PairSet Solver::underapproximation(const Graph &graph) const {
  return detail::runCombined(graph);
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds
