#include "CFL/Classical/Core/Validation.h"
#include "CFL/Classical/Solvers/Preprocessing/GraphSimplification.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

using namespace lotus::cfl::classical;

namespace {

enum class ResultScope {
  All,
  Start,
  Count,
};

struct Options {
  std::string grammar;
  std::string graph;
  SolverBackend backend = SolverBackend::SparseSet;
  GraphLoadOptions graph_options;
  GrammarParseOptions grammar_options;
  bool dump_relation = false;
  bool start_only = false;
  bool count_only = false;
  bool json_stats = false;
  bool validate_only = false;
  bool unidirectional = false;
  bool simplify_focr_cycles = false;
  ResultScope result_scope = ResultScope::All;
  std::vector<std::pair<std::string, std::string>> pearl_inverse_relations;
  GraphSimplificationOptions simplification;
  std::string relation_output;
  std::string stats_output;
  std::string graph_output;
  std::string stg_spec;
};

void usage(std::ostream &stream) {
  stream
      << "Usage: lotus-cfl-solve --grammar FILE --graph FILE [options]\n"
         "Options:\n"
         "  --solver sparse-set|sparse-bitvector|sparse-subset|graspan|sqid|"
         "skewed|stg|cat|iea|iea-ocr|transitive-closure|pocr|hpocr|focr|"
         "endpoint-quotient|cert\n"
         "  --graph-mode plain|matrix|pag-matrix\n"
         "  --direction plain|reverse|bidirectional\n"
         "  --attribute-domain var:i=N,N,...  Variable-specific domain\n"
         "  --attribute-domain kind:call=N,N  Symbol-kind domain\n"
         "  --max-attribute-expansions N      Expansion safety limit\n"
         "  --dump-relation\n"
         "  --relation-output FILE\n"
         "  --graph-output FILE          Write normalized/preprocessed graph\n"
         "  --start-only\n"
         "  --count-only                 Emit POCR Count symbols only\n"
         "  --json-stats\n"
         "  --stats-output FILE\n"
         "  --unidirectional            Honor POCR Insert/Follow metadata\n"
         "  --result-scope all|start|count\n"
         "                              Select the reported CFL relation\n"
         "  --stg-spec FILE             Explicit staged-decomposition JSON\n"
         "  --focr-scc                  Simplify FOCR critical-graph SCCs\n"
         "  --pearl-inverse X,XBAR      Pair inverse PEARL relations\n"
         "  --simplification-flavor alias|value-flow\n"
         "  --scc-elimination           Condense direct-edge SCCs\n"
         "  --graph-folding             Apply POCR client graph folding\n"
         "  --interdyck-pruning         Prune non-contributing Dyck edges\n"
         "  --simplify-graph            Enable SCC elimination and folding\n"
         "  --validate-only\n";
}

std::vector<std::uint32_t> parseAttributes(const std::string &value) {
  std::vector<std::uint32_t> result;
  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, ',')) {
    const auto attribute = parseAttributeValue(token);
    if (!attribute) {
      throw std::invalid_argument("Invalid attribute: " + token);
    }
    result.push_back(*attribute);
  }
  return result;
}

void parseAttributeDomain(const std::string &spec,
                          GrammarParseOptions &options) {
  const auto separator = spec.find('=');
  if (separator == std::string::npos || separator + 1 == spec.size()) {
    throw std::invalid_argument(
        "Attribute domain must have the form var:i=1,2 or kind:call=1,2");
  }
  const std::string name = spec.substr(0, separator);
  const auto domain = parseAttributes(spec.substr(separator + 1));
  if (name.rfind("var:", 0) == 0 && name.size() == 5) {
    options.variable_attributes[name.back()] = domain;
    return;
  }
  if (name.rfind("kind:", 0) == 0 && name.size() > 5) {
    options.symbol_attributes[name.substr(5)] = domain;
    return;
  }
  throw std::invalid_argument(
      "Attribute domain must have the form var:i=1,2 or kind:call=1,2");
}

GrammarParseOptions mergeGrammarOptions(GrammarParseOptions inferred,
                                        const GrammarParseOptions &explicit_) {
  for (const auto &[variable, domain] : explicit_.variable_attributes) {
    inferred.variable_attributes[variable] = domain;
  }
  for (const auto &[kind, domain] : explicit_.symbol_attributes) {
    inferred.symbol_attributes[kind] = domain;
  }
  inferred.max_attribute_expansions = explicit_.max_attribute_expansions;
  return inferred;
}

std::ostream *openOutput(const std::string &path, std::ofstream &file) {
  if (path.empty() || path == "-") {
    return &std::cout;
  }
  file.open(path);
  if (!file) {
    throw std::runtime_error("Failed to open output file: " + path);
  }
  return &file;
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto value = [&]() -> std::string {
      if (++index >= argc) {
        throw std::invalid_argument("Missing value for " + argument);
      }
      return argv[index];
    };

    if (argument == "--grammar") {
      options.grammar = value();
    } else if (argument == "--graph") {
      options.graph = value();
    } else if (argument == "--solver") {
      options.backend = parseSolverBackend(value());
    } else if (argument == "--graph-mode") {
      const std::string selected = value();
      if (selected == "plain") {
        options.graph_options.mode = GraphMode::Plain;
      } else if (selected == "matrix") {
        options.graph_options.mode = GraphMode::Matrix;
      } else if (selected == "pag-matrix") {
        options.graph_options.mode = GraphMode::PAGMatrix;
      } else {
        throw std::invalid_argument("Unknown graph mode: " + selected);
      }
    } else if (argument == "--direction") {
      const std::string selected = value();
      if (selected == "plain") {
        options.graph_options.direction = EdgeDirection::Plain;
      } else if (selected == "reverse") {
        options.graph_options.direction = EdgeDirection::Reverse;
      } else if (selected == "bidirectional") {
        options.graph_options.direction = EdgeDirection::Bidirectional;
      } else {
        throw std::invalid_argument("Unknown direction: " + selected);
      }
    } else if (argument == "--attribute-domain") {
      parseAttributeDomain(value(), options.grammar_options);
    } else if (argument == "--max-attribute-expansions") {
      options.grammar_options.max_attribute_expansions = std::stoull(value());
    } else if (argument == "--dump-relation") {
      options.dump_relation = true;
    } else if (argument == "--relation-output") {
      options.relation_output = value();
      options.dump_relation = true;
    } else if (argument == "--graph-output") {
      options.graph_output = value();
    } else if (argument == "--start-only") {
      options.start_only = true;
    } else if (argument == "--count-only") {
      options.count_only = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--stats-output") {
      options.stats_output = value();
    } else if (argument == "--unidirectional") {
      options.unidirectional = true;
    } else if (argument == "--result-scope") {
      const std::string selected = value();
      if (selected == "all") {
        options.result_scope = ResultScope::All;
      } else if (selected == "start") {
        options.result_scope = ResultScope::Start;
        options.start_only = true;
      } else if (selected == "count") {
        options.result_scope = ResultScope::Count;
        options.count_only = true;
      } else {
        throw std::invalid_argument("Unknown result scope: " + selected);
      }
    } else if (argument == "--stg-spec") {
      options.stg_spec = value();
    } else if (argument == "--focr-scc") {
      options.simplify_focr_cycles = true;
    } else if (argument == "--pearl-inverse") {
      const std::string relation_pair = value();
      const std::size_t comma = relation_pair.find(',');
      if (comma == std::string::npos || comma == 0 ||
          comma + 1 == relation_pair.size()) {
        throw std::invalid_argument("PEARL inverse relation must be X,XBAR");
      }
      options.pearl_inverse_relations.emplace_back(
          relation_pair.substr(0, comma), relation_pair.substr(comma + 1));
    } else if (argument == "--simplification-flavor") {
      const std::string selected = value();
      if (selected == "alias") {
        options.simplification.flavor = GraphSimplificationFlavor::Alias;
      } else if (selected == "value-flow") {
        options.simplification.flavor = GraphSimplificationFlavor::ValueFlow;
      } else {
        throw std::invalid_argument("Unknown simplification flavor: " +
                                    selected);
      }
    } else if (argument == "--scc-elimination") {
      options.simplification.eliminate_sccs = true;
    } else if (argument == "--graph-folding") {
      options.simplification.fold_graph = true;
    } else if (argument == "--interdyck-pruning") {
      options.simplification.prune_interdyck = true;
    } else if (argument == "--simplify-graph") {
      options.simplification.eliminate_sccs = true;
      options.simplification.fold_graph = true;
    } else if (argument == "--validate-only") {
      options.validate_only = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown option: " + argument);
    }
  }
  if (options.grammar.empty() || options.graph.empty()) {
    throw std::invalid_argument("--grammar and --graph are required");
  }
  if (options.start_only && options.count_only) {
    throw std::invalid_argument("--start-only and --count-only are exclusive");
  }
  if (!options.stg_spec.empty() && options.backend != SolverBackend::Stg) {
    throw std::invalid_argument("--stg-spec requires --solver stg");
  }
  return options;
}

std::string requiredString(const llvm::json::Object &object,
                           llvm::StringRef key) {
  const auto value = object.getString(key);
  if (!value) {
    throw std::invalid_argument("STG object requires string field '" +
                                key.str() + "'");
  }
  return value->str();
}

SymbolId requiredSymbol(const llvm::json::Object &object, llvm::StringRef key,
                        const Grammar &grammar) {
  const std::string name = requiredString(object, key);
  if (!grammar.hasSymbol(name)) {
    throw std::invalid_argument("Unknown STG grammar symbol: " + name);
  }
  return grammar.symbolId(name);
}

std::vector<SymbolId> parseSymbolArray(const llvm::json::Array *array,
                                       const Grammar &grammar,
                                       const std::string &context) {
  if (!array) {
    throw std::invalid_argument(context + " requires a symbol array");
  }
  std::vector<SymbolId> symbols;
  for (const llvm::json::Value &value : *array) {
    const auto name = value.getAsString();
    if (!name || !grammar.hasSymbol(name->str())) {
      throw std::invalid_argument(context +
                                  " contains an unknown/non-string symbol");
    }
    symbols.push_back(grammar.symbolId(name->str()));
  }
  return symbols;
}

engines::stg::RegularProduction
parseRegularProduction(const llvm::json::Object &object,
                       const Grammar &grammar) {
  engines::stg::RegularProduction production;
  production.lhs = requiredSymbol(object, "lhs", grammar);
  const llvm::json::Array *alternatives = object.getArray("alternatives");
  if (!alternatives) {
    throw std::invalid_argument("STG regular production requires alternatives");
  }
  for (const llvm::json::Value &alternative_value : *alternatives) {
    const llvm::json::Array *alternative = alternative_value.getAsArray();
    if (!alternative) {
      throw std::invalid_argument("STG alternative must be an atom array");
    }
    engines::stg::RegularSequence sequence;
    for (const llvm::json::Value &atom_value : *alternative) {
      const llvm::json::Object *atom = atom_value.getAsObject();
      if (!atom) {
        throw std::invalid_argument("STG regular atom must be an object");
      }
      engines::stg::RegularAtom parsed;
      parsed.symbols =
          parseSymbolArray(atom->getArray("symbols"), grammar, "STG atom");
      parsed.kleene_star =
          atom->getBoolean("kleene_star").getValueOr(false);
      sequence.push_back(std::move(parsed));
    }
    production.alternatives.push_back(std::move(sequence));
  }
  return production;
}

std::vector<engines::stg::RegularProduction>
parseRegularProductions(const llvm::json::Array *array, const Grammar &grammar,
                        const char *section) {
  std::vector<engines::stg::RegularProduction> productions;
  if (!array) {
    return productions;
  }
  for (const llvm::json::Value &value : *array) {
    const llvm::json::Object *object = value.getAsObject();
    if (!object) {
      throw std::invalid_argument(std::string(section) +
                                  " entries must be objects");
    }
    productions.push_back(parseRegularProduction(*object, grammar));
  }
  return productions;
}

engines::stg::StagedSpecification
parseStgSpecification(const std::string &path, const Grammar &grammar) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open STG specification: " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  auto parsed = llvm::json::parse(text);
  if (!parsed) {
    throw std::invalid_argument("Invalid STG JSON: " +
                                llvm::toString(parsed.takeError()));
  }
  const llvm::json::Object *root = parsed->getAsObject();
  if (!root) {
    throw std::invalid_argument("STG specification root must be an object");
  }

  engines::stg::StagedSpecification specification;
  specification.phase_l_regular = parseRegularProductions(
      root->getArray("phase_l_regular"), grammar, "phase_l_regular");
  specification.phase_r =
      parseRegularProductions(root->getArray("phase_r"), grammar, "phase_r");

  if (const llvm::json::Array *patterns = root->getArray("dyck_patterns")) {
    for (const llvm::json::Value &value : *patterns) {
      const llvm::json::Object *object = value.getAsObject();
      if (!object) {
        throw std::invalid_argument("STG Dyck pattern must be an object");
      }
      engines::stg::DyckCfp pattern;
      pattern.summary = requiredSymbol(*object, "summary", grammar);
      pattern.body_symbols = parseSymbolArray(object->getArray("body_symbols"),
                                              grammar, "STG Dyck body");
      const llvm::json::Array *delimiters = object->getArray("delimiters");
      if (!delimiters) {
        throw std::invalid_argument("STG Dyck pattern requires delimiters");
      }
      for (const llvm::json::Value &delimiter_value : *delimiters) {
        const llvm::json::Array *delimiter = delimiter_value.getAsArray();
        if (!delimiter || delimiter->size() != 2) {
          throw std::invalid_argument("STG delimiter must be [open, close]");
        }
        const auto open = (*delimiter)[0].getAsString();
        const auto close = (*delimiter)[1].getAsString();
        if (!open || !close || !grammar.hasSymbol(open->str()) ||
            !grammar.hasSymbol(close->str())) {
          throw std::invalid_argument("STG delimiter uses an unknown symbol");
        }
        pattern.delimiters.push_back(
            {grammar.symbolId(open->str()), grammar.symbolId(close->str())});
      }
      specification.dyck_patterns.push_back(std::move(pattern));
    }
  }

  if (const llvm::json::Array *patterns = root->getArray("alias_patterns")) {
    for (const llvm::json::Value &value : *patterns) {
      const llvm::json::Object *object = value.getAsObject();
      if (!object) {
        throw std::invalid_argument("STG alias pattern must be an object");
      }
      specification.alias_patterns.push_back(
          {requiredSymbol(*object, "summary", grammar),
           requiredSymbol(*object, "open", grammar),
           requiredSymbol(*object, "close", grammar),
           requiredSymbol(*object, "reverse_forward", grammar),
           requiredSymbol(*object, "center", grammar),
           requiredSymbol(*object, "backward", grammar)});
    }
  }
  if (specification.phase_l_regular.empty() &&
      specification.dyck_patterns.empty() &&
      specification.alias_patterns.empty() && specification.phase_r.empty()) {
    throw std::invalid_argument("STG specification is empty");
  }
  return specification;
}

const char *resultScopeName(ResultScope scope) {
  if (scope == ResultScope::All) {
    return "all";
  }
  return scope == ResultScope::Count ? "count" : "start";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    const auto total_start = std::chrono::steady_clock::now();
    const auto graph_start = std::chrono::steady_clock::now();
    LabeledGraph graph =
        LabeledGraph::parseFromFile(options.graph, options.graph_options);
    GraphSimplificationStatistics simplification_stats;
    simplification_stats.original_nodes = graph.vertexCount();
    simplification_stats.original_edges = graph.edgeCount();
    simplification_stats.reduced_nodes = graph.vertexCount();
    simplification_stats.reduced_edges = graph.edgeCount();
    if (options.simplification.eliminate_sccs ||
        options.simplification.fold_graph ||
        options.simplification.prune_interdyck) {
      GraphSimplificationResult simplified =
          simplifyGraph(graph, options.simplification);
      simplification_stats = simplified.statistics;
      graph = std::move(simplified.graph);
    }
    if (!options.graph_output.empty()) {
      graph.writeTextFile(options.graph_output);
    }
    const auto graph_load_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - graph_start)
            .count();

    const GrammarParseOptions grammar_options = mergeGrammarOptions(
        inferGrammarAttributes(graph), options.grammar_options);
    const auto grammar_start = std::chrono::steady_clock::now();
    const Grammar grammar =
        Grammar::parseFromFile(options.grammar, grammar_options);
    const auto grammar_load_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - grammar_start)
            .count();

    bool invalid = false;
    for (const GrammarIssue &issue : validateGraph(graph, grammar)) {
      const bool error = issue.severity == GrammarIssueSeverity::Error;
      std::cerr << (error ? "error: " : "warning: ") << issue.message << '\n';
      invalid = invalid || error;
    }
    if (invalid) {
      return 2;
    }
    if (options.validate_only) {
      std::cout << "validation=ok nodes=" << graph.vertexCount()
                << " symbols=" << grammar.symbolCount()
                << " productions=" << grammar.productionCount() << '\n';
      return 0;
    }

    SolverOptions solver_options;
    solver_options.backend = options.backend;
    solver_options.unidirectional = options.unidirectional;
    solver_options.simplify_focr_cycles = options.simplify_focr_cycles;
    solver_options.pearl_inverse_relations = options.pearl_inverse_relations;
    if (options.backend == SolverBackend::Stg) {
      if (options.stg_spec.empty()) {
        throw std::invalid_argument("--solver stg requires --stg-spec FILE");
      }
      solver_options.stg = parseStgSpecification(options.stg_spec, grammar);
    }
    if (options.backend == SolverBackend::Skewed &&
        options.result_scope != ResultScope::All) {
      solver_options.skewed.scope = skewed::Scope::TargetsOnly;
      if (options.result_scope == ResultScope::Count) {
        if (grammar.countSymbols().empty()) {
          throw std::invalid_argument(
              "Count result scope requires grammar Count metadata");
        }
        for (const std::string &symbol : grammar.countSymbols()) {
          solver_options.skewed.targets.push_back(grammar.symbolId(symbol));
        }
      } else {
        solver_options.skewed.targets.push_back(grammar.startSymbolId());
      }
      if (!grammar.followSymbols().empty()) {
        std::vector<skewed::Symbol> propagating;
        for (const std::string &symbol : grammar.followSymbols()) {
          propagating.push_back(grammar.symbolId(symbol));
        }
        solver_options.skewed.propagating_symbols = std::move(propagating);
      }
    }
    SolverSession session(graph, grammar, solver_options);
    const ReachabilityStats stats = session.solve();
    if (options.dump_relation) {
      std::ofstream relation_file;
      std::ostream &relation_output =
          *openOutput(options.relation_output, relation_file);
      auto edges = session.relation().edges();
      std::sort(edges.begin(), edges.end(),
                [](const RelationEdge &lhs, const RelationEdge &rhs) {
                  if (lhs.source != rhs.source) {
                    return lhs.source < rhs.source;
                  }
                  if (lhs.target != rhs.target) {
                    return lhs.target < rhs.target;
                  }
                  return lhs.symbol < rhs.symbol;
                });
      for (const RelationEdge &edge : edges) {
        if (options.start_only && edge.symbol != grammar.startSymbolId()) {
          continue;
        }
        if (options.count_only &&
            !grammar.isCountSymbol(grammar.symbolName(edge.symbol))) {
          continue;
        }
        if (options.count_only && edge.source == edge.target) {
          continue;
        }
        relation_output << graph.vertexName(edge.source) << ','
                        << graph.vertexName(edge.target) << ','
                        << grammar.symbolName(edge.symbol) << '\n';
      }
    }

    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - total_start)
                              .count();
    const std::size_t result_edges =
        options.result_scope == ResultScope::Start
            ? stats.start_symbol_edges
            : (options.result_scope == ResultScope::Count
                   ? stats.count_symbol_edges
                   : stats.relation_edges);
    const std::size_t inserted_summary_edges =
        options.backend == SolverBackend::Skewed
            ? stats.skewed_inserted_summary_edges
            : stats.added_edges;
    std::ofstream stats_file;
    std::ostream &stats_output = *openOutput(options.stats_output, stats_file);
    if (options.json_stats) {
      stats_output
          << "{\"solver\":\"" << solverBackendName(options.backend)
          << "\",\"result_scope\":\"" << resultScopeName(options.result_scope)
          << '"' << ",\"nodes\":" << graph.vertexCount()
          << ",\"base_edges\":" << stats.base_graph_edges
          << ",\"grammar_symbols\":" << stats.grammar_symbols
          << ",\"grammar_terminals\":" << stats.grammar_terminals
          << ",\"grammar_nonterminals\":" << stats.grammar_nonterminals
          << ",\"grammar_productions\":" << stats.grammar_productions
          << ",\"grammar_nullable\":" << stats.grammar_nullable_symbols
          << ",\"grammar_transitive\":" << stats.grammar_transitive_symbols
          << ",\"input_edges\":" << stats.input_edges
          << ",\"derived_edges\":" << stats.added_edges
          << ",\"inserted_summary_edges\":" << inserted_summary_edges
          << ",\"relation_edges\":" << stats.relation_edges
          << ",\"result_edges\":" << result_edges
          << ",\"start_edges\":" << stats.start_symbol_edges
          << ",\"count_edges\":" << stats.count_symbol_edges
          << ",\"checks\":" << stats.classical_iterations
          << ",\"processed_items\":" << stats.processed_work_items
          << ",\"duplicates\":" << stats.duplicate_edges
          << ",\"peak_worklist\":" << stats.peak_worklist_size
          << ",\"candidate_relation_edges\":" << stats.candidate_relation_edges
          << ",\"unidirectional\":"
          << (options.unidirectional ? "true" : "false")
          << ",\"relation_payload_bytes_estimate\":"
          << stats.relation_payload_bytes_estimate
          << ",\"transitive_instances\":" << stats.transitive_closure_instances
          << ",\"transitive_edges\":" << stats.transitive_relation_edges
          << ",\"transitive_arcs\":" << stats.transitive_arc_insertions
          << ",\"transitive_propagated_pairs\":"
          << stats.transitive_propagated_pairs
          << ",\"transitive_duplicate_pairs\":"
          << stats.transitive_duplicate_pairs
          << ",\"transitive_payload_bytes_estimate\":"
          << stats.transitive_payload_bytes_estimate
          << ",\"pocr_tree_roots\":" << stats.pocr_tree_roots
          << ",\"pocr_tree_nodes\":" << stats.pocr_tree_nodes
          << ",\"pocr_tree_edges\":" << stats.pocr_tree_edges
          << ",\"pocr_traversal_steps\":" << stats.pocr_traversal_steps
          << ",\"pocr_tree_join_visits\":" << stats.pocr_tree_join_visits
          << ",\"focr_critical_edges\":" << stats.fully_ordered_critical_edges
          << ",\"focr_reachability_checks\":"
          << stats.fully_ordered_reachability_checks
          << ",\"focr_tree_join_visits\":"
          << stats.fully_ordered_tree_join_visits
          << ",\"focr_critical_edge_insertions\":"
          << stats.fully_ordered_critical_edge_insertions
          << ",\"focr_critical_edge_removals\":"
          << stats.fully_ordered_critical_edge_removals
          << ",\"focr_cycle_simplifications\":"
          << stats.fully_ordered_cycle_simplifications
          << ",\"graspan_epochs\":" << stats.graspan_epochs
          << ",\"skewed_indexed_facts\":" << stats.skewed_indexed_facts
          << ",\"skewed_propagating_facts\":" << stats.skewed_propagating_facts
          << ",\"skewed_propagating_symbols\":"
          << stats.skewed_propagating_symbols
          << ",\"skewed_dynamic_eligible_symbols\":"
          << stats.skewed_dynamic_eligible_symbols
          << ",\"skewed_static_pe_insertions\":"
          << stats.skewed_static_pe_insertions
          << ",\"skewed_dynamic_pe_insertions\":"
          << stats.skewed_dynamic_pe_insertions
          << ",\"skewed_promotions_to_indexed\":"
          << stats.skewed_promotions_to_indexed
          << ",\"skewed_unary_applications\":"
          << stats.skewed_unary_applications
          << ",\"skewed_binary_join_pairs\":" << stats.skewed_binary_join_pairs
          << ",\"skewed_inserted_summary_edges\":"
          << stats.skewed_inserted_summary_edges
          << ",\"skewed_output_facts\":" << stats.skewed_output_facts
          << ",\"stg_phase_l_rounds\":" << stats.stg_phase_l_rounds
          << ",\"stg_phase_l_regular_edges\":"
          << stats.stg_phase_l_regular_edges
          << ",\"stg_dyck_path_edges\":" << stats.stg_dyck_path_edges
          << ",\"stg_alias_forward_path_edges\":"
          << stats.stg_alias_forward_path_edges
          << ",\"stg_alias_backward_path_edges\":"
          << stats.stg_alias_backward_path_edges
          << ",\"stg_summary_edges\":" << stats.stg_summary_edges
          << ",\"stg_phase_r_productions\":" << stats.stg_phase_r_productions
          << ",\"stg_phase_r_edges\":" << stats.stg_phase_r_edges
          << ",\"stg_ordered_scc_propagations\":"
          << stats.stg_ordered_scc_propagations
          << ",\"batch_stored_facts\":" << stats.batch_stored_facts
          << ",\"cat_graph_degree\":" << stats.cat_graph_degree
          << ",\"cat_fully_pruned_attempts\":"
          << stats.cat_fully_pruned_attempts
          << ",\"cat_context_annotations\":" << stats.cat_context_annotations
          << ",\"cat_rewrites\":" << stats.cat_rewrites
          << ",\"ieoce_quotient_nodes\":" << stats.ieoce_quotient_nodes
          << ",\"ieoce_epochs\":" << stats.ieoce_epochs
          << ",\"ieoce_merged_nodes\":" << stats.ieoce_merged_nodes
          << ",\"ieoce_graph_facts\":" << stats.ieoce_graph_facts
          << ",\"ieoce_meg_edges\":" << stats.ieoce_meg_edges
          << ",\"ieoce_meg_edges_removed\":" << stats.ieoce_meg_edges_removed
          << ",\"ieoce_ordered_steps\":" << stats.ieoce_ordered_steps
          << ",\"ieoce_ordinary_fallback\":"
          << (stats.ieoce_ordinary_fallback ? "true" : "false")
          << ",\"cert_cfl_levels\":" << stats.cert_cfl_levels
          << ",\"cert_cfl_blocks\":" << stats.cert_cfl_blocks
          << ",\"cert_cfl_peak_tiles\":" << stats.cert_cfl_peak_tiles
          << ",\"cert_cfl_updates\":" << stats.cert_cfl_updates
          << ",\"cert_cfl_promotions\":" << stats.cert_cfl_promotions
          << ",\"cert_cfl_genuine_promotions\":"
          << stats.cert_cfl_genuine_promotions
          << ",\"endpoint_quotient_cells\":" << stats.endpoint_quotient_cells
          << ",\"endpoint_quotient_facts\":" << stats.endpoint_quotient_facts
          << ",\"endpoint_quotient_seed_facts\":"
          << stats.endpoint_quotient_seed_facts
          << ",\"endpoint_quotient_inferred_facts\":"
          << stats.endpoint_quotient_inferred_facts
          << ",\"endpoint_quotient_binary_joins\":"
          << stats.endpoint_quotient_binary_joins
          << ",\"endpoint_quotient_bridge_pairs\":"
          << stats.endpoint_quotient_bridge_pairs
          << ",\"endpoint_quotient_source_classes\":"
          << stats.endpoint_quotient_source_classes
          << ",\"endpoint_quotient_target_classes\":"
          << stats.endpoint_quotient_target_classes
          << ",\"endpoint_quotient_nullable_symbols\":"
          << stats.endpoint_quotient_nullable_symbols
          << ",\"endpoint_quotient_preprocess_us\":"
          << stats.endpoint_quotient_preprocess_us
          << ",\"endpoint_quotient_saturation_us\":"
          << stats.endpoint_quotient_saturation_us
          << ",\"endpoint_quotient_count_us\":"
          << stats.endpoint_quotient_count_us
          << ",\"endpoint_quotient_insert_attempts\":"
          << stats.endpoint_quotient_insert_attempts
          << ",\"endpoint_quotient_duplicate_inserts\":"
          << stats.endpoint_quotient_duplicate_inserts
          << ",\"endpoint_quotient_binary_propagations\":"
          << stats.endpoint_quotient_binary_propagations
          << ",\"endpoint_quotient_successful_binary_propagations\":"
          << stats.endpoint_quotient_successful_binary_propagations
          << ",\"endpoint_quotient_repeated_binary_outputs\":"
          << stats.endpoint_quotient_repeated_binary_outputs
          << ",\"endpoint_quotient_binary_join_words\":"
          << stats.endpoint_quotient_binary_join_words
          << ",\"endpoint_quotient_partitions_built\":"
          << stats.endpoint_quotient_partitions_built
          << ",\"endpoint_quotient_bridges_built\":"
          << stats.endpoint_quotient_bridges_built
          << ",\"endpoint_quotient_lifts_built\":"
          << stats.endpoint_quotient_lifts_built
          << ",\"simplified_nodes\":" << simplification_stats.reduced_nodes
          << ",\"scc_nodes_merged\":" << simplification_stats.scc_nodes_merged
          << ",\"folded_nodes\":" << simplification_stats.folded_nodes
          << ",\"common_dereference_nodes_merged\":"
          << simplification_stats.common_dereference_nodes_merged
          << ",\"interdyck_edges_pruned\":"
          << simplification_stats.interdyck_edges_pruned
          << ",\"graph_load_us\":" << graph_load_us
          << ",\"grammar_load_us\":" << grammar_load_us
          << ",\"solve_us\":" << stats.solve_time_microseconds
          << ",\"total_us\":" << total_us
          << ",\"rounds\":" << stats.solver_rounds << "}\n";
    } else {
      stats_output
          << "solver=" << solverBackendName(options.backend)
          << " result_scope=" << resultScopeName(options.result_scope)
          << " nodes=" << graph.vertexCount()
          << " base_edges=" << stats.base_graph_edges
          << " grammar_symbols=" << stats.grammar_symbols
          << " productions=" << stats.grammar_productions
          << " input_edges=" << stats.input_edges
          << " derived_edges=" << stats.added_edges
          << " inserted_summary_edges=" << inserted_summary_edges
          << " relation_edges=" << stats.relation_edges
          << " result_edges=" << result_edges
          << " start_edges=" << stats.start_symbol_edges
          << " count_edges=" << stats.count_symbol_edges
          << " checks=" << stats.classical_iterations
          << " processed_items=" << stats.processed_work_items
          << " duplicates=" << stats.duplicate_edges
          << " peak_worklist=" << stats.peak_worklist_size
          << " candidate_relation_edges=" << stats.candidate_relation_edges
          << " unidirectional=" << options.unidirectional
          << " relation_payload_bytes_estimate="
          << stats.relation_payload_bytes_estimate
          << " transitive_instances=" << stats.transitive_closure_instances
          << " transitive_pairs=" << stats.transitive_propagated_pairs
          << " transitive_payload_bytes_estimate="
          << stats.transitive_payload_bytes_estimate
          << " pocr_tree_nodes=" << stats.pocr_tree_nodes
          << " pocr_traversal_steps=" << stats.pocr_traversal_steps
          << " pocr_tree_join_visits=" << stats.pocr_tree_join_visits
          << " focr_critical_edges=" << stats.fully_ordered_critical_edges
          << " focr_reachability_checks="
          << stats.fully_ordered_reachability_checks
          << " focr_tree_join_visits=" << stats.fully_ordered_tree_join_visits
          << " graspan_epochs=" << stats.graspan_epochs
          << " skewed_indexed_facts=" << stats.skewed_indexed_facts
          << " skewed_propagating_facts=" << stats.skewed_propagating_facts
          << " skewed_dynamic_pe_insertions="
          << stats.skewed_dynamic_pe_insertions
          << " skewed_inserted_summary_edges="
          << stats.skewed_inserted_summary_edges
          << " skewed_output_facts=" << stats.skewed_output_facts
          << " stg_phase_l_rounds=" << stats.stg_phase_l_rounds
          << " stg_summary_edges=" << stats.stg_summary_edges
          << " stg_phase_r_edges=" << stats.stg_phase_r_edges
          << " batch_stored_facts=" << stats.batch_stored_facts
          << " cat_graph_degree=" << stats.cat_graph_degree
          << " cat_fully_pruned_attempts=" << stats.cat_fully_pruned_attempts
          << " ieoce_quotient_nodes=" << stats.ieoce_quotient_nodes
          << " ieoce_epochs=" << stats.ieoce_epochs
          << " ieoce_merged_nodes=" << stats.ieoce_merged_nodes
          << " ieoce_meg_edges=" << stats.ieoce_meg_edges
          << " cert_cfl_levels=" << stats.cert_cfl_levels
          << " cert_cfl_blocks=" << stats.cert_cfl_blocks
          << " cert_cfl_peak_tiles=" << stats.cert_cfl_peak_tiles
          << " cert_cfl_updates=" << stats.cert_cfl_updates
          << " cert_cfl_promotions=" << stats.cert_cfl_promotions
          << " cert_cfl_genuine_promotions="
          << stats.cert_cfl_genuine_promotions
          << " simplified_nodes=" << simplification_stats.reduced_nodes
          << " scc_nodes_merged=" << simplification_stats.scc_nodes_merged
          << " folded_nodes=" << simplification_stats.folded_nodes
          << " interdyck_edges_pruned="
          << simplification_stats.interdyck_edges_pruned
          << " graph_load_us=" << graph_load_us
          << " grammar_load_us=" << grammar_load_us
          << " solve_us=" << stats.solve_time_microseconds
          << " total_us=" << total_us << " rounds=" << stats.solver_rounds
          << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
