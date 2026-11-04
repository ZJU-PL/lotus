#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/MCFL/InterleavedDyck.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace mcfl = lotus::cfl::interleaved_dyck::mcfl;
namespace interleaved_dyck = lotus::cfl::interleaved_dyck;

namespace {

struct CommandLine {
  std::string input;
  std::string output;
  std::string relation_directory;
  unsigned dimension = 2;
  mcfl::InterleavedGrammarVariant variant =
      mcfl::InterleavedGrammarVariant::Full;
  mcfl::CondensationExpansionPolicy expansion_policy =
      mcfl::CondensationExpansionPolicy::ReachabilityFiltered;
  bool condense = true;
  bool stats = false;
  bool print_pairs = false;
};

void usage(std::ostream &output) {
  output << "usage: lotus-cfl-interleaved-dyck mcfl [options] <graph.dot>\n"
            "\n"
            "Compute the MCFL underapproximation of typed interleaved-Dyck "
            "reachability.\n"
            "\n"
            "options:\n"
            "  -d, --dimension N  run staged G_1 ... G_N (default: 2)\n"
            "  --simple           use G_d^circ instead of full G_d^+\n"
            "  --no-condense      disable the artifact's cycle elimination\n"
            "  --artifact-compatible\n"
            "                     use the artifact's condensed cross-product\n"
            "  --stats            print saturation statistics\n"
            "  --print-pairs      print final non-reflexive reachable pairs\n"
            "  --dump-relations DIR\n"
            "                     write one sorted pair file per dimension\n"
            "  -o FILE            write output to FILE\n"
            "  -h, --help         show this help\n";
}

unsigned parseUnsigned(std::string_view text, std::string_view option) {
  unsigned value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || value == 0) {
    throw std::invalid_argument("invalid value for " + std::string(option));
  }
  return value;
}

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      usage(std::cout);
      std::exit(0);
    }
    if (argument == "-d" || argument == "--dimension") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --dimension");
      }
      result.dimension = parseUnsigned(argv[i], argument);
      continue;
    }
    if (argument == "--simple") {
      result.variant = mcfl::InterleavedGrammarVariant::Simple;
      continue;
    }
    if (argument == "--no-condense") {
      result.condense = false;
      continue;
    }
    if (argument == "--artifact-compatible") {
      result.expansion_policy =
          mcfl::CondensationExpansionPolicy::ArtifactCompatible;
      continue;
    }
    if (argument == "--stats") {
      result.stats = true;
      continue;
    }
    if (argument == "--print-pairs") {
      result.print_pairs = true;
      continue;
    }
    if (argument == "--dump-relations") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --dump-relations");
      }
      result.relation_directory = argv[i];
      continue;
    }
    if (argument == "-o") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for -o");
      }
      result.output = argv[i];
      continue;
    }
    if (!argument.empty() && argument.front() == '-') {
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

void writePairs(const std::filesystem::path &path, const mcfl::PairSet &pairs) {
  std::vector<mcfl::Pair> ordered(pairs.begin(), pairs.end());
  std::sort(
      ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        return left.source < right.source ||
               (left.source == right.source && left.target < right.target);
      });
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("cannot open relation file: " + path.string());
  }
  for (const auto &pair : ordered) {
    output << pair.source << ' ' << pair.target << '\n';
  }
  output.flush();
  if (!output) {
    throw std::runtime_error("cannot write relation file: " + path.string());
  }
}

void dumpRelations(const CommandLine &command_line,
                   const mcfl::InterleavedAnalysisResult &result) {
  if (command_line.relation_directory.empty()) {
    return;
  }
  const std::filesystem::path directory(command_line.relation_directory);
  std::filesystem::create_directories(directory);
  const char *family =
      command_line.variant == mcfl::InterleavedGrammarVariant::Full ? "plus"
                                                                    : "circ";
  for (const auto &dimension : result.dimensions) {
    writePairs(directory / ("g-" + std::string(family) + "-" +
                            std::to_string(dimension.dimension) + ".pairs"),
               dimension.reachable_pairs);
  }
}

std::optional<std::uint64_t> peakRssBytes() {
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    std::uint64_t bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__linux__)
    bytes *= 1024;
#endif
    return bytes;
  }
#endif
  return std::nullopt;
}

void printResult(std::ostream &output, const CommandLine &command_line,
                 const mcfl::InterleavedAnalysisResult &result,
                 std::int64_t elapsed_ms) {
  const char *grammar_suffix =
      command_line.variant == mcfl::InterleavedGrammarVariant::Full ? "+"
                                                                    : "circ";
  for (const mcfl::InterleavedDimensionResult &dimension : result.dimensions) {
    output << "Reachable pairs (L(G^" << grammar_suffix << "_"
           << dimension.dimension << ")): " << dimension.reachable_pairs.size()
           << '\n';
    if (command_line.stats) {
      output << "  facts: " << dimension.stats.facts << '\n'
             << "  worklist pops: " << dimension.stats.worklist_pops << '\n'
             << "  type-5 combinations: " << dimension.stats.type5_combinations
             << '\n'
             << "  unreachable tuples pruned: "
             << dimension.stats.rejected_unreachable_gaps << '\n';
    }
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    for (const mcfl::Pair &pair : result.reachablePairs()) {
      output << pair.source << ' ' << pair.target << '\n';
    }
  }
}

} // namespace

int runInterleavedDyckMCFL(int argc, char **argv) {
  try {
    const CommandLine command_line = parseCommandLine(argc, argv);
    const interleaved_dyck::Graph graph =
        interleaved_dyck::Graph::parseDotFile(command_line.input);
    std::ofstream output_file;
    std::ostream *output = &std::cout;
    if (!command_line.output.empty()) {
      output_file.open(command_line.output);
      if (!output_file) {
        throw std::runtime_error("cannot open output file: " +
                                 command_line.output);
      }
      output = &output_file;
    }

    mcfl::InterleavedOptions options;
    options.max_dimension = command_line.dimension;
    options.variant = command_line.variant;
    options.condense = command_line.condense;
    options.expansion_policy = command_line.expansion_policy;

    const auto start = std::chrono::steady_clock::now();
    const mcfl::InterleavedAnalysisResult result =
        mcfl::InterleavedDyckSolver{}.solve(graph, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    const std::optional<std::uint64_t> peak_rss = peakRssBytes();
    dumpRelations(command_line, result);
    printResult(*output, command_line, result, elapsed.count());
    if (peak_rss) {
      *output << "process peak RSS (bytes): " << *peak_rss << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck mcfl: " << error.what() << '\n';
    return 1;
  }
}
