#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/StagedBounds/Solver.h"

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

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace approximation = lotus::cfl::interleaved_dyck::staged_bounds;
namespace interleaved_dyck = lotus::cfl::interleaved_dyck;

namespace {

enum class PrintedPairs { None, Lower, Result };

struct CommandLine {
  std::string input;
  std::string output;
  std::string relation_directory;
  approximation::Method method = approximation::Method::All;
  approximation::BenchmarkKind analysis = approximation::BenchmarkKind::Taint;
  unsigned parity_groups = 2;
  bool factorized_tracing = false;
  bool stage_stats = false;
  PrintedPairs printed_pairs = PrintedPairs::None;
};

void usage(std::ostream &output) {
  output
      << "usage: lotus-cfl-interleaved-dyck staged-bounds [options] "
         "<graph.dot>\n"
         "\n"
         "Compute staged lower and upper bounds for typed interleaved-Dyck\n"
         "reachability on a single DOT graph. Edges use op--N / cp--N for\n"
         "parentheses, ob--N / cb--N for brackets, and normal for neutral\n"
         "value-flow edges.\n"
         "\n"
         "options:\n"
         "  --analysis NAME    client analysis: taint (default) or "
         "value-flow.\n"
         "                     taint uses the general regularization "
         "automaton\n"
         "                     derived from the bracket labels in each "
         "graph\n"
         "                     component. value-flow drops vertices "
         "outside\n"
         "                     bracket source-to-sink paths and applies "
         "the\n"
         "                     value-flow product transformation.\n"
         "  --method NAME      last stage to run: regularization, "
         "intersection,\n"
         "                     underapproximation, mutual-refinement,\n"
         "                     stronger-grammar, on-demand, or all "
         "(default:\n"
         "                     all). Stages run in the listed order.\n"
         "  --parity-groups N  parity groups for the stronger grammar, 1-4\n"
         "                     (default: 2). Larger values tighten the "
         "upper\n"
         "                     bound but grow the grammar to 4 * 2^N "
         "states\n"
         "                     per projection.\n"
         "  --factorized-tracing\n"
         "                     reconstruct provenance from the CFL closure\n"
         "                     instead of recording derivations eagerly;\n"
         "                     avoids storing derivation records.\n"
         "  --stage-stats      report stage time and cumulative process peak\n"
         "                     RSS to stderr (the RSS high-water mark does\n"
         "                     not reset between stages).\n"
         "  --print-lower      print certified lower-bound pairs\n"
         "  --print-result     print pairs produced by the selected method\n"
         "  --dump-relations DIR\n"
         "                     write sorted union.pairs and, for a final\n"
         "                     run, on-demand.pairs\n"
         "  -o FILE            write output to FILE\n"
         "  -h, --help         show this help\n"
         "\n"
         "A pair in the underapproximation is definitely reachable. A "
         "pair\n"
         "absent from the final upper bound is definitely unreachable. A\n"
         "pair inside the final upper bound but outside the lower bound\n"
         "remains unresolved.\n";
}

approximation::Method parseMethod(std::string_view text) {
  if (text == "all") {
    return approximation::Method::All;
  }
  if (text == "regularization") {
    return approximation::Method::Regularization;
  }
  if (text == "intersection") {
    return approximation::Method::Intersection;
  }
  if (text == "underapproximation") {
    return approximation::Method::Underapproximation;
  }
  if (text == "mutual-refinement") {
    return approximation::Method::MutualRefinement;
  }
  if (text == "stronger-grammar") {
    return approximation::Method::StrongerGrammar;
  }
  if (text == "on-demand") {
    return approximation::Method::OnDemand;
  }
  throw std::invalid_argument("unknown method: " + std::string(text));
}

approximation::BenchmarkKind parseAnalysis(std::string_view text) {
  if (text == "taint") {
    return approximation::BenchmarkKind::Taint;
  }
  if (text == "value-flow") {
    return approximation::BenchmarkKind::ValueFlow;
  }
  throw std::invalid_argument("unknown analysis: " + std::string(text));
}

unsigned parseUnsigned(std::string_view text, std::string_view option) {
  unsigned value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
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
    if (argument == "--analysis") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --analysis");
      }
      result.analysis = parseAnalysis(argv[i]);
      continue;
    }
    if (argument == "--method") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --method");
      }
      result.method = parseMethod(argv[i]);
      continue;
    }
    if (argument == "--parity-groups") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --parity-groups");
      }
      result.parity_groups = parseUnsigned(argv[i], argument);
      continue;
    }
    if (argument == "--factorized-tracing") {
      result.factorized_tracing = true;
      continue;
    }
    if (argument == "--stage-stats") {
      result.stage_stats = true;
      continue;
    }
    if (argument == "--print-lower") {
      result.printed_pairs = PrintedPairs::Lower;
      continue;
    }
    if (argument == "--print-result") {
      result.printed_pairs = PrintedPairs::Result;
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
  if (!result.relation_directory.empty() &&
      result.method != approximation::Method::All &&
      result.method != approximation::Method::OnDemand &&
      result.method != approximation::Method::Underapproximation) {
    throw std::invalid_argument(
        "--dump-relations requires --method underapproximation, all, or "
        "--method on-demand");
  }
  return result;
}

void writePairs(const std::filesystem::path &path,
                const approximation::PairSet &pairs) {
  std::vector<approximation::Pair> ordered(pairs.begin(), pairs.end());
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

void dumpRelations(const std::string &directory, approximation::Method method,
                   const approximation::ApproximationResult &result) {
  if (directory.empty()) {
    return;
  }
  const std::filesystem::path path(directory);
  std::filesystem::create_directories(path);
  writePairs(path / "union.pairs", result.underapproximation);
  if (method != approximation::Method::Underapproximation) {
    writePairs(path / "on-demand.pairs", result.on_demand);
  }
}

std::optional<std::uint64_t> peakRssBytes() {
#if defined(__unix__) || defined(__APPLE__)
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    std::uint64_t bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__unix__)
    bytes *= 1024;
#endif
    return bytes;
  }
#endif
  return std::nullopt;
}

const char *methodLabel(approximation::Method method) {
  switch (method) {
  case approximation::Method::Regularization:
    return "regularization";
  case approximation::Method::Intersection:
    return "intersection upper bound";
  case approximation::Method::Underapproximation:
    return "certified lower bound";
  case approximation::Method::MutualRefinement:
    return "mutual-refinement upper bound";
  case approximation::Method::StrongerGrammar:
    return "stronger-grammar upper bound";
  case approximation::Method::OnDemand:
  case approximation::Method::All:
    return "final upper bound";
  }
  throw std::logic_error("unhandled approximation method");
}

const approximation::PairSet &
methodResult(const approximation::ApproximationResult &result,
             approximation::Method method) {
  switch (method) {
  case approximation::Method::Regularization:
    return result.regularization;
  case approximation::Method::Intersection:
    return result.intersection;
  case approximation::Method::Underapproximation:
    return result.underapproximation;
  case approximation::Method::MutualRefinement:
    return result.mutual_refinement;
  case approximation::Method::StrongerGrammar:
    return result.stronger_grammar;
  case approximation::Method::OnDemand:
  case approximation::Method::All:
    return result.on_demand;
  }
  throw std::logic_error("unhandled approximation method");
}

void printResult(std::ostream &output, const CommandLine &command_line,
                 const approximation::ApproximationResult &result,
                 std::int64_t elapsed_ms) {
  if (command_line.method == approximation::Method::All) {
    output << "regularization: " << result.regularization.size() << '\n'
           << "intersection upper bound: " << result.intersection.size() << '\n'
           << "certified lower bound: " << result.underapproximation.size()
           << '\n'
           << "mutual-refinement upper bound: "
           << result.mutual_refinement.size() << '\n'
           << "stronger-grammar upper bound: " << result.stronger_grammar.size()
           << '\n'
           << "final upper bound: " << result.on_demand.size() << '\n';
  } else {
    output << methodLabel(command_line.method) << ": "
           << methodResult(result, command_line.method).size() << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';

  const approximation::PairSet *pairs = nullptr;
  if (command_line.printed_pairs == PrintedPairs::Lower) {
    pairs = &result.underapproximation;
  } else if (command_line.printed_pairs == PrintedPairs::Result) {
    pairs = &methodResult(result, command_line.method);
  }
  if (pairs != nullptr) {
    for (const approximation::Pair &pair : *pairs) {
      output << pair.source << ' ' << pair.target << '\n';
    }
  }
}

} // namespace

int runInterleavedDyckStagedBounds(int argc, char **argv) {
  try {
    const CommandLine command_line = parseCommandLine(argc, argv);
    const interleaved_dyck::Graph graph =
        interleaved_dyck::Graph::parseDotFile(command_line.input);

    approximation::Options options;
    options.method = command_line.method;
    options.parity_groups = command_line.parity_groups;
    options.factorized_tracing = command_line.factorized_tracing;
    const auto start = std::chrono::steady_clock::now();
    if (command_line.stage_stats) {
      options.stage_completed = [last = start](
                                    approximation::Method stage) mutable {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
        std::cerr << "Stage " << methodLabel(stage)
                  << ": time_ms=" << elapsed.count();
#if defined(__unix__) || defined(__APPLE__)
        struct rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
          std::uint64_t bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#if !defined(__APPLE__)
          bytes *= 1024;
#endif
          std::cerr << " cumulative_peak_rss_mib="
                    << static_cast<double>(bytes) / (1024.0 * 1024.0);
        }
#endif
        std::cerr << '\n';
        last = std::chrono::steady_clock::now();
      };
    }
    const approximation::ApproximationResult result =
        approximation::Solver{}.analyze(graph, command_line.analysis, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    const std::optional<std::uint64_t> peak_rss = peakRssBytes();

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
    dumpRelations(command_line.relation_directory, command_line.method, result);
    printResult(*output, command_line, result, elapsed.count());
    if (peak_rss) {
      *output << "process peak RSS (bytes): " << *peak_rss << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck staged-bounds: " << error.what()
              << '\n';
    return 1;
  }
}
