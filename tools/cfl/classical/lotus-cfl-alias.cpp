#include "CFL/Classical/Clients/Alias/LLVMAliasAnalysis.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

using namespace lotus::cfl::classical;

namespace {

struct Options {
  std::string input;
  LLVMAliasOptions analysis;
  bool json_stats = false;
  bool print_points_to = false;
  bool check_annotations = false;
  std::string dump_cfl_graph;
  std::string dump_cfl_grammar;
  std::string query_lhs;
  std::string query_rhs;
};

void usage(std::ostream &stream) {
  stream << "Usage: lotus-cfl-alias [options] INPUT.{ll,bc}\n"
            "Options:\n"
            "  --solver sparse-set|sparse-bitvector|sparse-subset|graspan|sqid|"
            "skewed|cat|iea|iea-ocr|transitive-closure|pocr|hpocr|focr|"
            "endpoint-quotient|cert\n"
            "  --encoding pag|peg|cfl-peg  Default: cfl-peg\n"
            "      pag: constraint-level Lotus grammar\n"
            "      peg: extended Lotus ArrayPath/Memcpy grammar\n"
            "      cfl-peg: standard a/d/f_i interchange grammar\n"
            "  --entry FUNCTION\n"
            "  --max-callgraph-rounds N\n"
            "  --query LHS,RHS\n"
            "  --dump-cfl-graph FILE\n"
            "  --dump-cfl-grammar FILE\n"
            "  --print-points-to\n"
            "  --check-annotations\n"
            "  --json-stats\n";
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
    if (argument == "--solver") {
      options.analysis.backend = parseSolverBackend(value());
    } else if (argument == "--encoding") {
      const std::string selected = value();
      if (selected == "pag") {
        options.analysis.encoding = AliasEncodingMode::PAG;
      } else if (selected == "peg") {
        options.analysis.encoding = AliasEncodingMode::PEG;
      } else if (selected == "cfl-peg") {
        options.analysis.encoding = AliasEncodingMode::CFLPEG;
      } else {
        throw std::invalid_argument("Unknown encoding: " + selected);
      }
    } else if (argument == "--entry") {
      options.analysis.entry = value();
    } else if (argument == "--max-callgraph-rounds") {
      options.analysis.max_callgraph_rounds = std::stoul(value());
    } else if (argument == "--query") {
      const std::string query = value();
      const auto comma = query.find(',');
      if (comma == std::string::npos) {
        throw std::invalid_argument("Alias query must be LHS,RHS");
      }
      options.query_lhs = query.substr(0, comma);
      options.query_rhs = query.substr(comma + 1);
    } else if (argument == "--dump-cfl-graph") {
      options.dump_cfl_graph = value();
    } else if (argument == "--dump-cfl-grammar") {
      options.dump_cfl_grammar = value();
    } else if (argument == "--print-points-to") {
      options.print_points_to = true;
    } else if (argument == "--check-annotations") {
      options.check_annotations = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else if (!argument.empty() && argument.front() == '-') {
      throw std::invalid_argument("Unknown option: " + argument);
    } else if (options.input.empty()) {
      options.input = argument;
    } else {
      throw std::invalid_argument("Multiple input modules were provided");
    }
  }
  if (options.input.empty()) {
    throw std::invalid_argument("An input LLVM module is required");
  }
  return options;
}

const llvm::Value *findNamedValue(const llvm::Module &module,
                                  const std::string &name) {
  if (const auto *global = module.getNamedGlobal(name)) {
    return global;
  }
  if (const auto *function = module.getFunction(name)) {
    return function;
  }
  for (const llvm::Function &function : module) {
    for (const llvm::Argument &argument : function.args()) {
      if (argument.getName() == name) {
        return &argument;
      }
    }
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      if (instruction.getName() == name) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

void printPointsTo(const llvm::Module &module,
                   const LLVMCFLAliasAnalysis &analysis) {
  for (const llvm::Function &function : module) {
    for (const llvm::Argument &argument : function.args()) {
      if (!argument.getType()->isPointerTy() || !argument.hasName()) {
        continue;
      }
      std::cout << argument.getName().str() << ':';
      for (const llvm::Value *target : analysis.pointsTo(&argument)) {
        std::cout << ' '
                  << (target->hasName() ? target->getName().str()
                                        : std::string("<unnamed>"));
      }
      std::cout << '\n';
    }
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      if (!instruction.getType()->isPointerTy() || !instruction.hasName()) {
        continue;
      }
      std::cout << instruction.getName().str() << ':';
      for (const llvm::Value *target : analysis.pointsTo(&instruction)) {
        std::cout << ' '
                  << (target->hasName() ? target->getName().str()
                                        : std::string("<unnamed>"));
      }
      std::cout << '\n';
    }
  }
}

std::pair<std::size_t, std::size_t>
checkAnnotations(const llvm::Module &module,
                 const LLVMCFLAliasAnalysis &analysis) {
  std::size_t total = 0;
  std::size_t failures = 0;
  for (const llvm::Function &function : module) {
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      const llvm::Function *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || call->arg_size() < 2) {
        continue;
      }
      const llvm::StringRef name = callee->getName();
      const bool expect_alias = name == "__aser_alias__" || name == "MAYALIAS";
      const bool expect_no_alias =
          name == "__aser_no_alias__" || name == "NOALIAS";
      if (!expect_alias && !expect_no_alias) {
        continue;
      }
      ++total;
      const bool actual =
          analysis.mayAlias(call->getArgOperand(0), call->getArgOperand(1));
      const bool passed = expect_alias ? actual : !actual;
      failures += passed ? 0 : 1;
      std::cout << "annotation=" << (passed ? "pass" : "fail")
                << " expected=" << (expect_alias ? "alias" : "no-alias")
                << " function=" << function.getName().str();
      if (!passed) {
        const auto lhs = analysis.nodeForValue(call->getArgOperand(0));
        const auto rhs = analysis.nodeForValue(call->getArgOperand(1));
        std::cout << " lhs_node="
                  << (lhs ? std::to_string(*lhs) : std::string("missing"))
                  << " rhs_node="
                  << (rhs ? std::to_string(*rhs) : std::string("missing"));
        if (lhs) {
          std::cout << " lhs_pts=";
          for (std::size_t target : analysis.client().pointsTo(*lhs)) {
            std::cout << target << ';';
          }
        }
        if (rhs) {
          std::cout << " rhs_pts=";
          for (std::size_t target : analysis.client().pointsTo(*rhs)) {
            std::cout << target << ';';
          }
        }
      }
      std::cout << '\n';
    }
  }
  return {total, failures};
}

const char *endpointQuotientRuleKind(std::size_t kind) {
  switch (kind) {
  case 0:
    return "epsilon";
  case 1:
    return "unary";
  case 2:
    return "binary";
  default:
    return "unknown";
  }
}

const char *endpointQuotientSccClass(std::size_t classification) {
  switch (classification) {
  case 0:
    return "acyclic";
  case 1:
    return "unary-recursive";
  case 2:
    return "left-linear";
  case 3:
    return "right-linear";
  case 4:
    return "transitive";
  case 5:
    return "general";
  default:
    return "unknown";
  }
}

const char *aliasEncodingName(const LLVMAliasOptions &options) {
  switch (options.encoding) {
  case AliasEncodingMode::PAG:
    return "pag";
  case AliasEncodingMode::PEG:
    return "peg";
  case AliasEncodingMode::CFLPEG:
    return "cfl-peg";
  }
  return "unknown";
}

void printEndpointQuotientProfiles(std::ostream &stream,
                                   const ReachabilityStats &stats) {
  stream << ",\"eq_rule_profiles\":[";
  bool first = true;
  for (const auto &rule : stats.endpoint_quotient_per_rule) {
    if (!first)
      stream << ',';
    first = false;
    stream << "{\"id\":" << rule.rule_id << ",\"kind\":\""
           << endpointQuotientRuleKind(rule.kind) << "\",\"lhs\":"
           << rule.lhs << ",\"left\":" << rule.left << ",\"right\":"
           << rule.right << ",\"delta_rows\":" << rule.delta_rows
           << ",\"delta_cells\":" << rule.delta_cells << ",\"joins\":"
           << rule.joins << ",\"propagations\":" << rule.propagations
           << ",\"successful_propagations\":"
           << rule.successful_propagations
           << ",\"duplicate_propagations\":"
           << (rule.propagations >= rule.successful_propagations
                   ? rule.propagations - rule.successful_propagations
                   : 0)
           << ",\"repeated_outputs\":"
           << rule.repeated_outputs << ",\"join_word_operations\":"
           << rule.join_word_operations << '}';
  }
  stream << "],\"eq_scc_profiles\":[";
  first = true;
  for (const auto &scc : stats.endpoint_quotient_per_scc) {
    if (!first)
      stream << ',';
    first = false;
    stream << "{\"id\":" << scc.scc_id << ",\"class\":\""
           << endpointQuotientSccClass(scc.classification)
           << "\",\"symbols\":" << scc.symbols << ",\"rules\":"
           << scc.rules << ",\"delta_rows\":" << scc.delta_rows
           << ",\"delta_cells\":" << scc.delta_cells << ",\"joins\":"
           << scc.joins << ",\"propagations\":" << scc.propagations
           << ",\"successful_propagations\":"
           << scc.successful_propagations
           << ",\"duplicate_propagations\":"
           << (scc.propagations >= scc.successful_propagations
                   ? scc.propagations - scc.successful_propagations
                   : 0)
           << ",\"repeated_outputs\":"
           << scc.repeated_outputs << ",\"join_word_operations\":"
           << scc.join_word_operations << '}';
  }
  stream << ']';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(options.input, diagnostic, context);
    if (!module) {
      diagnostic.print(argv[0], llvm::errs());
      return 2;
    }

    LLVMCFLAliasAnalysis analysis(options.analysis);
    const ReachabilityStats stats = analysis.analyze(*module);
    if (!options.dump_cfl_graph.empty()) {
      analysis.client().graph().writeTextFile(options.dump_cfl_graph);
    }
    if (!options.dump_cfl_grammar.empty()) {
      analysis.client().grammar().writeTextFile(options.dump_cfl_grammar);
    }
    const GraphSimplificationStatistics &simplification =
        analysis.client().simplificationStatistics();
    if (!options.query_lhs.empty()) {
      const llvm::Value *lhs = findNamedValue(*module, options.query_lhs);
      const llvm::Value *rhs = findNamedValue(*module, options.query_rhs);
      if (!lhs || !rhs) {
        throw std::invalid_argument("Alias query names were not found");
      }
      std::cout << "alias=" << (analysis.mayAlias(lhs, rhs) ? "may" : "no")
                << '\n';
    }
    if (options.print_points_to) {
      printPointsTo(*module, analysis);
    }
    std::size_t annotation_total = 0;
    std::size_t annotation_failures = 0;
    if (options.check_annotations) {
      std::tie(annotation_total, annotation_failures) =
          checkAnnotations(*module, analysis);
    }
    if (options.json_stats) {
      std::cout
          << "{\"solver\":\"" << solverBackendName(options.analysis.backend)
          << "\",\"encoding\":\"" << aliasEncodingName(options.analysis)
          << "\",\"nodes\":" << stats.graph_nodes
          << ",\"base_edges\":" << stats.base_graph_edges
          << ",\"grammar_symbols\":" << stats.grammar_symbols
          << ",\"grammar_productions\":" << stats.grammar_productions
          << ",\"relation_edges\":" << stats.relation_edges
          << ",\"start_edges\":" << stats.start_symbol_edges
          << ",\"callgraph_rounds\":" << stats.solver_rounds
          << ",\"processed_items\":" << stats.processed_work_items
          << ",\"classical_iterations\":" << stats.classical_iterations
          << ",\"duplicate_edges\":" << stats.duplicate_edges
          << ",\"peak_worklist\":" << stats.peak_worklist_size
          << ",\"transitive_pairs\":" << stats.transitive_propagated_pairs
          << ",\"pocr_tree_nodes\":" << stats.pocr_tree_nodes
          << ",\"pocr_traversal_steps\":" << stats.pocr_traversal_steps
          << ",\"pocr_tree_join_visits\":" << stats.pocr_tree_join_visits
          << ",\"preprocess_input_nodes\":" << simplification.original_nodes
          << ",\"preprocess_reduced_nodes\":" << simplification.reduced_nodes
          << ",\"scc_nodes_merged\":" << simplification.scc_nodes_merged
          << ",\"folded_nodes\":" << simplification.folded_nodes
          << ",\"deref_nodes_merged\":"
          << simplification.common_dereference_nodes_merged
          << ",\"focr_critical_edges\":" << stats.fully_ordered_critical_edges
          << ",\"focr_reachability_checks\":"
          << stats.fully_ordered_reachability_checks
          << ",\"focr_tree_join_visits\":"
          << stats.fully_ordered_tree_join_visits
          << ",\"focr_cycle_simplifications\":"
          << stats.fully_ordered_cycle_simplifications
          << ",\"graspan_epochs\":" << stats.graspan_epochs
          << ",\"cert_levels\":" << stats.cert_cfl_levels
          << ",\"cert_blocks\":" << stats.cert_cfl_blocks
          << ",\"cert_peak_tiles\":" << stats.cert_cfl_peak_tiles
          << ",\"cert_updates\":" << stats.cert_cfl_updates
          << ",\"cert_promotions\":" << stats.cert_cfl_promotions
          << ",\"cert_genuine_promotions\":"
          << stats.cert_cfl_genuine_promotions
          << ",\"eq_cells\":" << stats.endpoint_quotient_cells
          << ",\"eq_insert_attempts\":"
          << stats.endpoint_quotient_insert_attempts
          << ",\"eq_duplicate_inserts\":"
          << stats.endpoint_quotient_duplicate_inserts
          << ",\"eq_binary_joins\":" << stats.endpoint_quotient_binary_joins
          << ",\"eq_binary_join_words\":"
          << stats.endpoint_quotient_binary_join_words
          << ",\"eq_preprocess_us\":" << stats.endpoint_quotient_preprocess_us
          << ",\"eq_saturation_us\":" << stats.endpoint_quotient_saturation_us
          << ",\"eq_count_us\":" << stats.endpoint_quotient_count_us
          << ",\"eq_dependency_sccs\":"
          << stats.endpoint_quotient_dependency_sccs
          << ",\"eq_acyclic_sccs\":" << stats.endpoint_quotient_acyclic_sccs
          << ",\"eq_unary_recursive_sccs\":"
          << stats.endpoint_quotient_unary_recursive_sccs
          << ",\"eq_transitive_sccs\":"
          << stats.endpoint_quotient_transitive_sccs
          << ",\"eq_linear_sccs\":" << stats.endpoint_quotient_linear_sccs
          << ",\"eq_general_sccs\":" << stats.endpoint_quotient_general_sccs
          << ",\"eq_max_scc_symbols\":"
          << stats.endpoint_quotient_max_scc_symbols
          << ",\"eq_max_scc_rules\":" << stats.endpoint_quotient_max_scc_rules
          << ",\"eq_hottest_rule_id\":"
          << stats.endpoint_quotient_hottest_rule_id
          << ",\"eq_hottest_rule_joins\":"
          << stats.endpoint_quotient_hottest_rule_joins
          << ",\"eq_hottest_scc_id\":" << stats.endpoint_quotient_hottest_scc_id
          << ",\"eq_hottest_scc_joins\":"
          << stats.endpoint_quotient_hottest_scc_joins;
      printEndpointQuotientProfiles(std::cout, stats);
      std::cout << ",\"annotation_total\":" << annotation_total
                << ",\"annotation_failures\":" << annotation_failures
                << ",\"frontend_us\":" << stats.frontend_time_microseconds
                << ",\"client_init_us\":"
                << stats.client_initialization_microseconds
                << ",\"discovery_us\":" << stats.client_discovery_microseconds
                << ",\"solve_us\":" << stats.solve_time_microseconds << "}\n";
    } else {
      std::cout << "solver=" << solverBackendName(options.analysis.backend)
                << " encoding=" << aliasEncodingName(options.analysis)
                << " nodes=" << stats.graph_nodes
                << " base_edges=" << stats.base_graph_edges
                << " grammar_symbols=" << stats.grammar_symbols
                << " grammar_productions=" << stats.grammar_productions
                << " relation_edges=" << stats.relation_edges
                << " start_edges=" << stats.start_symbol_edges
                << " callgraph_rounds=" << stats.solver_rounds
                << " processed_items=" << stats.processed_work_items
                << " classical_iterations=" << stats.classical_iterations
                << " duplicate_edges=" << stats.duplicate_edges
                << " peak_worklist=" << stats.peak_worklist_size
                << " pocr_tree_nodes=" << stats.pocr_tree_nodes
                << " pocr_traversal_steps=" << stats.pocr_traversal_steps
                << " pocr_tree_join_visits=" << stats.pocr_tree_join_visits
                << " preprocess_input_nodes=" << simplification.original_nodes
                << " preprocess_reduced_nodes=" << simplification.reduced_nodes
                << " scc_nodes_merged=" << simplification.scc_nodes_merged
                << " folded_nodes=" << simplification.folded_nodes
                << " deref_nodes_merged="
                << simplification.common_dereference_nodes_merged
                << " focr_critical_edges=" << stats.fully_ordered_critical_edges
                << " focr_tree_join_visits="
                << stats.fully_ordered_tree_join_visits
                << " graspan_epochs=" << stats.graspan_epochs
                << " cert_levels=" << stats.cert_cfl_levels
                << " cert_blocks=" << stats.cert_cfl_blocks
                << " cert_peak_tiles=" << stats.cert_cfl_peak_tiles
                << " cert_updates=" << stats.cert_cfl_updates
                << " cert_promotions=" << stats.cert_cfl_promotions
                << " cert_genuine_promotions="
                << stats.cert_cfl_genuine_promotions
                << " eq_cells=" << stats.endpoint_quotient_cells
                << " eq_preprocess_us=" << stats.endpoint_quotient_preprocess_us
                << " eq_saturation_us=" << stats.endpoint_quotient_saturation_us
                << " annotation_total=" << annotation_total
                << " annotation_failures=" << annotation_failures
                << " frontend_us=" << stats.frontend_time_microseconds
                << " client_init_us="
                << stats.client_initialization_microseconds
                << " discovery_us=" << stats.client_discovery_microseconds
                << " solve_us=" << stats.solve_time_microseconds << '\n';
    }
    return annotation_failures == 0 ? 0 : 3;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
