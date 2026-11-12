#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/LCL/Solver.h"

#include <algorithm>
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
#include <tuple>
#include <vector>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace lcl = lotus::cfl::interleaved_dyck::lcl;

namespace {

struct CommandLine {
  std::string input;
  lcl::Options options;
  bool print_upper = false;
  bool bidirect = false;
  std::optional<dyck::Pair> query;
};

void usage(std::ostream &out) {
  out << "usage: lotus-cfl-interleaved-dyck lcl [options] <graph.dot>\n\n"
         "POPL 2017 LCL upper bound for directed, typed interleaved Dyck.\n"
         "A retained pair is MAY-REACH, not a certified balanced witness.\n\n"
         "options:\n"
         "  --baseline                 Algorithm 1 white-node baseline\n"
         "  --no-feasibility           disable Section 5.3 endpoint filters\n"
         "  --query SOURCE TARGET      report may-reach or unreachable\n"
         "  --print-upper              print sorted upper-bound pairs\n"
         "  --bidirect                 add missing complement reverse arcs\n"
         "  --max-summaries N           fail above N summaries (0: unlimited)\n"
         "  --max-normalized-edges N    cap epsilon expansion (0: unlimited)\n"
         "  --                         end option parsing\n"
         "  -h, --help                 show this help\n";
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
    if (!positional_only && argument == "--baseline") {
      result.options.algorithm = lcl::Algorithm::Baseline;
      continue;
    }
    if (!positional_only && argument == "--no-feasibility") {
      result.options.enable_feasibility = false;
      continue;
    }
    if (!positional_only && argument == "--print-upper") {
      result.print_upper = true;
      continue;
    }
    if (!positional_only && argument == "--bidirect") {
      result.bidirect = true;
      continue;
    }
    if (!positional_only && argument == "--query") {
      if (argc - i <= 2 || result.query) {
        throw std::invalid_argument("--query requires exactly one SOURCE TARGET pair");
      }
      const auto source = parseInteger<dyck::Vertex>(argv[++i], argument);
      const auto target = parseInteger<dyck::Vertex>(argv[++i], argument);
      result.query = dyck::Pair{source, target};
      continue;
    }
    if (!positional_only &&
        (argument == "--max-summaries" || argument == "--max-normalized-edges")) {
      if (++i == argc) {
        throw std::invalid_argument("missing value for " + std::string(argument));
      }
      const auto value = parseInteger<std::size_t>(argv[i], argument);
      if (argument == "--max-summaries") {
        result.options.max_summaries = value;
      } else {
        result.options.max_normalized_edges = value;
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
  return result;
}

} // namespace

int runInterleavedDyckLCL(int argc, char **argv) {
  try {
    const auto command = parseCommandLine(argc, argv);
    auto graph = dyck::Graph::parseDotFile(command.input);
    if (command.bidirect) {
      graph.makeBidirected();
    }
    if (command.query &&
        (!graph.containsVertex(command.query->source) ||
         !graph.containsVertex(command.query->target))) {
      throw std::invalid_argument("query vertex is absent from the input graph");
    }
    const auto start = std::chrono::steady_clock::now();
    const auto result = lcl::Solver{}.analyze(graph, command.options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    const auto &stats = result.statistics;
    std::cout << "engine: "
              << (command.options.algorithm == lcl::Algorithm::Baseline
                      ? "lcl-baseline"
                      : "lcl-refined")
              << "\nsemantics: sound upper bound\n"
              << "input vertices: " << stats.input_vertices << '\n'
              << "input edges: " << stats.input_edges << '\n'
              << "normalized edges: " << stats.normalized_edges << '\n'
              << "epsilon pairs: " << stats.epsilon_pairs << '\n'
              << "summaries: " << stats.summaries << '\n'
              << "gray summaries: " << stats.gray_summaries << '\n'
              << "summary upgrades: " << stats.summary_upgrades << '\n'
              << "worklist pops: " << stats.worklist_pops << '\n'
              << "upper-bound pairs: " << result.upper_bound.size() << '\n'
              << "time (ms): " << elapsed.count() << '\n';
    if (command.query) {
      std::cout << "query: "
                << (result.mayReach(command.query->source, command.query->target)
                        ? "may-reach"
                        : "unreachable")
                << '\n';
    }
    if (command.print_upper) {
      std::vector<dyck::Pair> sorted(result.upper_bound.begin(), result.upper_bound.end());
      std::sort(sorted.begin(), sorted.end(), [](const dyck::Pair &a, const dyck::Pair &b) {
        return std::tie(a.source, a.target) < std::tie(b.source, b.target);
      });
      for (const dyck::Pair &pair : sorted) {
        std::cout << pair.source << ' ' << pair.target << '\n';
      }
    }
    if (!std::cout) {
      throw std::runtime_error("failed to write results");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck lcl: " << error.what() << '\n';
    return 1;
  }
}
