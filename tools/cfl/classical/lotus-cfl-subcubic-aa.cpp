#include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAAAdapter.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lotus::cfl::classical;
namespace subcubic = lotus::cfl::classical::engines::subcubic;

namespace {

enum class AliasRelation { Memory, Value };

struct Options {
  std::string graph;
  subcubic::Options solver;
  AliasRelation relation = AliasRelation::Value;
  bool relation_explicit = false;
  bool bidirectional = false;
  bool print_aliases = false;
  bool json_stats = false;
  std::optional<std::pair<std::string, std::string>> query;
};

void usage(std::ostream &stream) {
  stream <<
      "Usage: lotus-cfl-subcubic-aa [options] GRAPH\n"
      "       lotus-cfl-subcubic-aa [options] --graph GRAPH\n"
      "OOPSLA 2014 PEG memory/value alias analysis (Algorithms 1 and 2).\n"
      "Input: Lotus text/PEG, labeled DOT, or JSON with a/abar/d/dbar edges.\n"
      "Every assignment target must have an incoming d edge.\n"
      "Options:\n"
      "  --graph FILE              Input graph (or supply it positionally)\n"
      "  --bidirectional           Add missing reverse a/abar/d/dbar edges\n"
      "  --memory-only             Run Algorithm 1 only; default relation M\n"
      "  --no-fast-sets            Use scalar set-difference traversal\n"
      "  --no-components           Solve the whole graph as one component\n"
      "  --relation memory|value   Relation for queries/output (default value)\n"
      "  --query LHS,RHS           Query two exact vertex names\n"
      "  --print-aliases           Print all ordered pairs of that relation\n"
      "  --json-stats              Emit one JSON object with statistics and\n"
      "                            any requested query/pairs\n"
      "  --help, -h                Show this help\n";
}

Options parseOptions(int argc, char **argv) {
  Options options;
  bool positional_only = false;
  auto setGraph = [&](const std::string &path) {
    if (path.empty()) {
      throw std::invalid_argument("The graph path must not be empty");
    }
    if (!options.graph.empty()) {
      throw std::invalid_argument("Multiple input graphs were provided");
    }
    options.graph = path;
  };
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto value = [&]() -> std::string {
      if (++index >= argc) {
        throw std::invalid_argument("Missing value for " + argument);
      }
      return argv[index];
    };
    if (positional_only) {
      setGraph(argument);
    } else if (argument == "--") {
      positional_only = true;
    } else if (argument == "--graph") {
      setGraph(value());
    } else if (argument == "--bidirectional") {
      options.bidirectional = true;
    } else if (argument == "--memory-only") {
      options.solver.compute_value_aliases = false;
    } else if (argument == "--no-fast-sets") {
      options.solver.use_fast_sets = false;
    } else if (argument == "--no-components") {
      options.solver.decompose_components = false;
    } else if (argument == "--relation") {
      const std::string selected = value();
      options.relation_explicit = true;
      if (selected == "memory") {
        options.relation = AliasRelation::Memory;
      } else if (selected == "value") {
        options.relation = AliasRelation::Value;
      } else {
        throw std::invalid_argument("Unknown relation: " + selected +
                                    "; expected memory or value");
      }
    } else if (argument == "--query") {
      if (options.query) {
        throw std::invalid_argument("Multiple queries were provided");
      }
      const std::string selected = value();
      const auto comma = selected.find(',');
      if (comma == std::string::npos || comma == 0 ||
          comma + 1 == selected.size() ||
          selected.find(',', comma + 1) != std::string::npos) {
        throw std::invalid_argument(
            "Alias query must be LHS,RHS with two nonempty vertex names");
      }
      options.query =
          std::make_pair(selected.substr(0, comma), selected.substr(comma + 1));
    } else if (argument == "--print-aliases") {
      options.print_aliases = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else if (!argument.empty() && argument.front() == '-') {
      throw std::invalid_argument("Unknown option: " + argument);
    } else {
      setGraph(argument);
    }
  }
  if (options.graph.empty()) {
    throw std::invalid_argument("An input graph is required");
  }
  if (!options.solver.compute_value_aliases) {
    if (options.relation_explicit &&
        options.relation == AliasRelation::Value) {
      throw std::invalid_argument(
          "--relation value is incompatible with --memory-only");
    }
    options.relation = AliasRelation::Memory;
  }
  return options;
}

const char *relationName(AliasRelation relation) {
  return relation == AliasRelation::Memory ? "memory" : "value";
}

void jsonString(std::ostream &stream, const std::string &value) {
  constexpr char hex[] = "0123456789abcdef";
  stream << '"';
  for (unsigned char character : value) {
    if (character == '"' || character == '\\') {
      stream << '\\' << static_cast<char>(character);
    } else if (character < 0x20) {
      stream << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
    } else {
      stream << static_cast<char>(character);
    }
  }
  stream << '"';
}

template <typename Visitor>
void visitNamedPairs(const LabeledGraph &graph, const subcubic::Result &result,
                     AliasRelation relation, Visitor &&visitor) {
  std::vector<subcubic::Node> sources(graph.vertexCount());
  std::iota(sources.begin(), sources.end(), subcubic::Node{0});
  auto nameLess = [&](subcubic::Node lhs, subcubic::Node rhs) {
    return graph.vertexName(lhs) < graph.vertexName(rhs);
  };
  std::sort(sources.begin(), sources.end(), nameLess);
  std::vector<subcubic::Node> targets;
  for (subcubic::Node source : sources) {
    targets.clear();
    auto collect = [&](subcubic::Node target) {
      targets.push_back(target);
      return true;
    };
    if (relation == AliasRelation::Memory) {
      result.visitMemorySuccessors(source, collect);
    } else {
      result.visitValueSuccessors(source, collect);
    }
    std::sort(targets.begin(), targets.end(), nameLess);
    for (subcubic::Node target : targets) {
      visitor(graph.vertexName(source), graph.vertexName(target));
    }
  }
}

void printResult(const Options &options, const LabeledGraph &graph,
                 const subcubic::Result &result,
                 const std::optional<bool> &query_result) {
  const auto &stats = result.statistics();
  if (options.json_stats) {
    std::cout << "{\"algorithm\":\"subcubic-aa\",\"nodes\":" << stats.nodes
              << ",\"assignment_edges\":" << stats.assignment_edges
              << ",\"dereference_edges\":" << stats.dereference_edges
              << ",\"connected_components\":" << stats.connected_components
              << ",\"largest_component\":" << stats.largest_component
              << ",\"memory_aliases\":" << result.memoryAliasCount()
              << ",\"value_aliases\":";
    if (result.valueAliasesComputed()) {
      std::cout << result.valueAliasCount();
    } else {
      std::cout << "null";
    }
    std::cout << ",\"value_aliases_computed\":"
              << (result.valueAliasesComputed() ? "true" : "false")
              << ",\"fast_sets\":"
              << (options.solver.use_fast_sets ? "true" : "false")
              << ",\"component_decomposition\":"
              << (options.solver.decompose_components ? "true" : "false")
              << ",\"processed_items\":" << stats.processed_items
              << ",\"peak_worklist\":" << stats.peak_worklist
              << ",\"difference_words\":" << stats.difference_words
              << ",\"candidate_checks\":" << stats.candidate_checks
              << ",\"relation\":\"" << relationName(options.relation) << '"';
    if (options.query) {
      std::cout << ",\"query\":{\"lhs\":";
      jsonString(std::cout, options.query->first);
      std::cout << ",\"rhs\":";
      jsonString(std::cout, options.query->second);
      std::cout << ",\"alias\":" << (*query_result ? "true" : "false") << '}';
    }
    if (options.print_aliases) {
      std::cout << ",\"aliases\":[";
      bool first = true;
      visitNamedPairs(graph, result, options.relation,
                      [&](const std::string &source, const std::string &target) {
                        if (!first) {
                          std::cout << ',';
                        }
                        first = false;
                        std::cout << '[';
                        jsonString(std::cout, source);
                        std::cout << ',';
                        jsonString(std::cout, target);
                        std::cout << ']';
                      });
      std::cout << ']';
    }
    std::cout << "}\n";
    return;
  }

  std::cout << "nodes=" << stats.nodes
            << " assignment_edges=" << stats.assignment_edges
            << " dereference_edges=" << stats.dereference_edges
            << " components=" << stats.connected_components
            << " largest_component=" << stats.largest_component
            << " memory_aliases=" << result.memoryAliasCount()
            << " value_aliases=";
  if (result.valueAliasesComputed()) {
    std::cout << result.valueAliasCount();
  } else {
    std::cout << "not-computed";
  }
  std::cout << " fast_sets=" << (options.solver.use_fast_sets ? "on" : "off")
            << " processed_items=" << stats.processed_items
            << " difference_words=" << stats.difference_words
            << " candidate_checks=" << stats.candidate_checks << '\n';
  if (options.query) {
    std::cout << "relation=" << relationName(options.relation)
              << " alias=" << (*query_result ? "yes" : "no")
              << " lhs=" << options.query->first
              << " rhs=" << options.query->second << '\n';
  }
  if (options.print_aliases) {
    const char label = options.relation == AliasRelation::Memory ? 'M' : 'V';
    visitNamedPairs(graph, result, options.relation,
                    [&](const std::string &source, const std::string &target) {
                      std::cout << label << '\t' << source << '\t' << target
                                << '\n';
                    });
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    const GraphLoadOptions loading{
        GraphMode::Plain, options.bidirectional ? EdgeDirection::Bidirectional
                                               : EdgeDirection::Plain};
    const LabeledGraph graph = LabeledGraph::parseFromFile(options.graph, loading);
    const subcubic::Problem problem = subcubic::toSubcubicAliasProblem(graph);
    std::optional<std::pair<subcubic::Node, subcubic::Node>> query_nodes;
    if (options.query) {
      query_nodes = std::make_pair(graph.vertexId(options.query->first),
                                  graph.vertexId(options.query->second));
    }
    const subcubic::Result result = subcubic::solve(problem, options.solver);
    std::optional<bool> query_result;
    if (query_nodes) {
      query_result = options.relation == AliasRelation::Memory
                         ? result.memoryAlias(query_nodes->first,
                                              query_nodes->second)
                         : result.valueAlias(query_nodes->first,
                                             query_nodes->second);
    }
    printResult(options, graph, result, query_result);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-subcubic-aa: " << error.what() << '\n';
    return 1;
  }
}
