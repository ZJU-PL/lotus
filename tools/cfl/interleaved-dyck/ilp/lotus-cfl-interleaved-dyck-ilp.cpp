#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/ILP/Solver.h"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace ilp = lotus::cfl::interleaved_dyck::ilp;

namespace {

struct CommandLine {
  std::string input;
  ilp::Options options;
  std::optional<dyck::Pair> query;
};

const char *backendName(ilp::Backend backend) {
  switch (backend) {
  case ilp::Backend::Z3:
    return "z3";
  case ilp::Backend::Gurobi:
    return "gurobi";
  }
  return "unknown";
}

void usage(std::ostream &out) {
  out << "usage: lotus-cfl-interleaved-dyck ilp --query SOURCE TARGET "
         "[options] <graph.dot>\n\n"
         "POPL 2023 single-source/single-target ILP approximation for "
         "directed,\n"
         "typed interleaved Dyck. A may-reach answer is a candidate, not a\n"
         "certified balanced witness; unreachable is a rejection.\n\n"
         "options:\n"
         "  --query SOURCE TARGET       required source/target pair\n"
         "  --backend NAME              linear-programming backend: "
#ifdef LOTUS_HAVE_GUROBI
         "z3 (default) or gurobi\n"
#else
         "z3 (default); gurobi is not built in\n"
#endif
         "  --no-lcl-refinement         disable POPL 2017 LCL pruning\n"
         "  --allow-unmatched-opens     accept nonnegative final counts\n"
         "  --max-disjuncts N           fail above N DNF disjuncts (0: "
         "unlimited)\n"
         "  --max-expression-nodes N    cap path-expression nodes (0: "
         "unlimited)\n"
         "  --                          end option parsing\n"
         "  -h, --help                  show this help\n";
}

template <typename Integer>
Integer parseInteger(std::string_view text, std::string_view option) {
  Integer value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid integer for " + std::string(option) +
                                ": " + std::string(text));
  }
  return value;
}

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine result;
  bool positional_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (!positional_only && (argument == "-h" || argument == "--help")) {
      usage(std::cout);
      std::exit(0);
    }
    if (!positional_only && argument == "--") {
      positional_only = true;
      continue;
    }
    if (!positional_only && argument == "--no-lcl-refinement") {
      result.options.enable_lcl_refinement = false;
      continue;
    }
    if (!positional_only && argument == "--allow-unmatched-opens") {
      result.options.allow_unmatched_opens = true;
      continue;
    }
    if (!positional_only && argument == "--backend") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for " +
                                    std::string(argument));
      }
      const std::string_view value = argv[i];
      if (value == "z3") {
        result.options.backend = ilp::Backend::Z3;
      } else if (value == "gurobi") {
        result.options.backend = ilp::Backend::Gurobi;
      } else {
        throw std::invalid_argument("invalid --backend: " + std::string(value));
      }
      continue;
    }
    if (!positional_only && argument == "--query") {
      if (argc - i <= 2 || result.query) {
        throw std::invalid_argument(
            "--query requires exactly one SOURCE TARGET pair");
      }
      const auto source = parseInteger<dyck::Vertex>(argv[++i], argument);
      const auto target = parseInteger<dyck::Vertex>(argv[++i], argument);
      result.query = dyck::Pair{source, target};
      continue;
    }
    if (!positional_only && (argument == "--max-disjuncts" ||
                             argument == "--max-expression-nodes")) {
      if (++i == argc) {
        throw std::invalid_argument("missing value for " +
                                    std::string(argument));
      }
      const auto value = parseInteger<std::size_t>(argv[i], argument);
      if (argument == "--max-disjuncts") {
        result.options.max_disjuncts = value;
      } else {
        result.options.max_expression_nodes = value;
      }
      continue;
    }
    if (!positional_only && !argument.empty() && argument.front() == '-') {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
    if (!result.input.empty()) {
      throw std::invalid_argument("more than one input graph was provided");
    }
    result.input = argument;
  }
  if (result.input.empty()) {
    throw std::invalid_argument("no input graph was provided");
  }
  if (!result.query) {
    throw std::invalid_argument("--query SOURCE TARGET is required");
  }
  return result;
}

} // namespace

int runInterleavedDyckILP(int argc, char **argv) {
  try {
    const auto command = parseCommandLine(argc, argv);
#ifndef LOTUS_HAVE_GUROBI
    if (command.options.backend == ilp::Backend::Gurobi) {
      throw std::invalid_argument(
          "--backend gurobi is not available in this build; configure with "
          "-DLOTUS_ENABLE_GUROBI=ON");
    }
#endif
    const auto graph = dyck::Graph::parseDotFile(command.input);
    if (!graph.containsVertex(command.query->source) ||
        !graph.containsVertex(command.query->target)) {
      throw std::invalid_argument(
          "query vertex is absent from the input graph");
    }
    const auto start = std::chrono::steady_clock::now();
    const auto result = ilp::Solver{}.analyze(
        graph, command.query->source, command.query->target, command.options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    const auto &stats = result.statistics;
    std::cout << "engine: ilp\n"
              << "backend: " << backendName(command.options.backend) << '\n'
              << "semantics: over-approximation (may-reach is not a witness)\n"
              << "input vertices: " << graph.vertices().size() << '\n'
              << "input edges: " << graph.edges().size() << '\n'
              << "path-expression nodes: " << stats.path_expression_nodes
              << '\n'
              << "ilp disjuncts: " << stats.ilp_disjuncts << '\n'
              << "rejected bad subexpressions: "
              << stats.rejected_bad_subexpressions << '\n'
              << "integer variables: " << stats.integer_variables << '\n'
              << "integer constraints: " << stats.integer_constraints << '\n'
              << "time (ms): " << elapsed.count() << '\n'
              << "query: " << command.query->source << ' '
              << command.query->target << '\n'
              << "result: " << (result.may_reach ? "may-reach" : "unreachable")
              << '\n';
    if (!std::cout) {
      throw std::runtime_error("failed to write results");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck ilp: " << error.what() << '\n';
    return 1;
  }
}
