#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Unary/Solver.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace interleaved_dyck = lotus::cfl::interleaved_dyck;
namespace unary = lotus::cfl::interleaved_dyck::unary;

namespace {

struct CommandLine {
  std::string input;
  std::string output;
  std::string component_output;
  unary::Algorithm algorithm = unary::Algorithm::Adaptive;
  bool sparsify = true;
  bool add_reverse_edges = false;
  std::optional<std::size_t> shallow_threshold;
  bool stats = false;
  bool phase_timing = false;
  bool print_pairs = false;
};

void usage(std::ostream &output) {
  output
      << "usage: lotus-cfl-interleaved-dyck unary [options] <graph.dot>\n"
         "\n"
         "Compute exact bidirected unary D1-interleaved-D1 reachability.\n"
         "\n"
         "options:\n"
         "  --algorithm NAME  adaptive (default) or fixed-counter\n"
         "  --direct         skip quotient sparsification\n"
         "  --bidirect       add missing complement reverse arcs; the\n"
         "                   result overapproximates the original graph\n"
         "  --shallow K      adaptive only: solve min(counter1,counter2) <= K\n"
         "  --stats          print construction and backend statistics\n"
         "  --phase-timing   adaptive only: collect RQ2.2 phase times\n"
         "  --print-pairs    materialize non-reflexive component pairs\n"
         "  --dump-components FILE\n"
         "                   write sorted vertex-to-component identifiers\n"
         "  -o FILE          write output to FILE\n"
         "  -h, --help       show this help\n";
}

std::size_t parseSize(std::string_view text, std::string_view option) {
  std::size_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid value for " + std::string(option));
  }
  return value;
}

unary::Algorithm parseAlgorithm(std::string_view name) {
  if (name == "adaptive") {
    return unary::Algorithm::Adaptive;
  }
  if (name == "fixed-counter") {
    return unary::Algorithm::FixedCounter;
  }
  throw std::invalid_argument("unknown algorithm '" + std::string(name) +
                              "' (expected adaptive or fixed-counter)");
}

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      usage(std::cout);
      std::exit(0);
    }
    if (argument == "--algorithm") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --algorithm");
      }
      result.algorithm = parseAlgorithm(argv[i]);
      continue;
    }
    if (argument == "--direct") {
      result.sparsify = false;
      continue;
    }
    if (argument == "--bidirect") {
      result.add_reverse_edges = true;
      continue;
    }
    if (argument == "--shallow") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --shallow");
      }
      result.shallow_threshold = parseSize(argv[i], argument);
      continue;
    }
    if (argument == "--stats") {
      result.stats = true;
      continue;
    }
    if (argument == "--phase-timing") {
      result.phase_timing = true;
      continue;
    }
    if (argument == "--print-pairs") {
      result.print_pairs = true;
      continue;
    }
    if (argument == "--dump-components") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --dump-components");
      }
      result.component_output = argv[i];
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
  if (result.algorithm == unary::Algorithm::FixedCounter &&
      result.shallow_threshold) {
    throw std::invalid_argument(
        "--shallow is available only with --algorithm adaptive");
  }
  if (result.algorithm == unary::Algorithm::FixedCounter &&
      result.phase_timing) {
    throw std::invalid_argument(
        "--phase-timing is available only with --algorithm adaptive");
  }
  if (result.phase_timing && !result.stats) {
    throw std::invalid_argument("--phase-timing requires --stats");
  }
  return result;
}

template <typename Result> std::size_t componentCount(const Result &result) {
  std::unordered_set<std::size_t> components;
  for (const auto &[_, component] : result.components()) {
    components.insert(component);
  }
  return components.size();
}

template <typename Result>
void dumpComponents(const std::string &path, const Result &result) {
  if (path.empty()) {
    return;
  }
  using Entry = std::pair<interleaved_dyck::Vertex, std::size_t>;
  std::vector<Entry> ordered(result.components().begin(),
                             result.components().end());
  std::sort(ordered.begin(), ordered.end(),
            [](const Entry &left, const Entry &right) {
              return left.first < right.first;
            });
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("cannot open component file: " + path);
  }
  for (const auto &[vertex, component] : ordered) {
    output << vertex << ' ' << component << '\n';
  }
  output.flush();
  if (!output) {
    throw std::runtime_error("cannot write component file: " + path);
  }
}

template <typename Result>
void printPairs(std::ostream &output, const interleaved_dyck::Graph &graph,
                const Result &result) {
  std::unordered_map<std::size_t, std::vector<interleaved_dyck::Vertex>> groups;
  for (auto vertex : graph.vertices())
    groups[result.component(vertex)].push_back(vertex);
  for (const auto &[_, vertices] : groups)
    for (auto source : vertices)
      for (auto target : vertices)
        if (source != target)
          output << source << ' ' << target << '\n';
}

void printExecution(std::ostream &output,
                    const interleaved_dyck::UnaryExecutionStats &stats) {
  output << "  weak components/largest: " << stats.weak_components << '/'
         << stats.largest_component_vertices << '\n'
         << "  trivial/single-counter components: " << stats.trivial_components
         << '/' << stats.single_counter_components << '\n'
         << "  projection/preprocessing/decomposition (us): "
         << stats.projection_us << '/' << stats.preprocessing_us << '/'
         << stats.decomposition_us << '\n'
         << "  solving/lifting/total (us): " << stats.solving_us << '/'
         << stats.lifting_us << '/' << stats.total_us << '\n'
         << "  peak construction payload estimate (bytes): "
         << stats.peak_working_bytes << '\n';
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    std::uint64_t bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__linux__)
    bytes *= 1024;
#endif
    output << "  process peak RSS (bytes): " << bytes << '\n';
  }
#endif
}

void printDyck(std::ostream &output, std::string_view name,
               const interleaved_dyck::BidirectedDyckStats &stats) {
  output << "  " << name << " epsilon components: " << stats.epsilon_components
         << '\n'
         << "  " << name
         << " generated/stored/scanned closing edges: " << stats.closing_edges
         << '/' << stats.stored_closing_edges << '/'
         << stats.scanned_closing_edges << '\n'
         << "  " << name
         << " epsilon/closing/saturation (us): " << stats.epsilon_us << '/'
         << stats.closing_us << '/' << stats.saturation_us << '\n';
}

template <typename Result>
void printPreamble(std::ostream &output, std::string_view algorithm,
                   const Result &result) {
  output << "Algorithm: " << algorithm << '\n'
         << "Components: " << componentCount(result) << '\n'
         << "Guarantee: "
         << (result.stats().overapproximates_original
                 ? "sound overapproximation of original directed graph"
                 : "exact for original bidirected graph")
         << '\n';
}

void printAdaptiveResult(std::ostream &output, const CommandLine &command_line,
                         const interleaved_dyck::Graph &graph,
                         const unary::AdaptiveResult &result,
                         std::int64_t elapsed_ms) {
  printPreamble(
      output, command_line.shallow_threshold ? "adaptive-shallow" : "adaptive",
      result);
  if (command_line.stats) {
    const unary::AdaptiveStats &stats = result.stats();
    printExecution(output, stats.execution);
    output << "Phase timing: "
           << (stats.phase_timing.enabled ? "enabled" : "disabled") << '\n';
    if (stats.phase_timing.enabled) {
      const unary::AdaptivePhaseTiming &phase = stats.phase_timing;
      output << "  phase projection (us): " << phase.projection_us << '\n'
             << "  phase quotient sparsification (us): "
             << phase.quotient_sparsification_us << '\n'
             << "  phase decomposition (us): " << phase.decomposition_us << '\n'
             << "  phase vertical construction (us): "
             << phase.vertical_construction_us << '\n'
             << "  phase vertical solving (us): " << phase.vertical_solving_us
             << '\n'
             << "  phase horizontal construction (us): "
             << phase.horizontal_construction_us << '\n'
             << "  phase horizontal solving (us): "
             << phase.horizontal_solving_us << '\n'
             << "  phase parent-map labeling (us): "
             << phase.parent_map_labeling_us << '\n'
             << "  phase boundary unions (us): " << phase.boundary_unions_us
             << '\n'
             << "  phase output lifting (us): " << phase.output_lifting_us
             << '\n';
    }
    output << "  vertical/horizontal/merge (us): " << stats.vertical_us << '/'
           << stats.horizontal_us << '/' << stats.merge_us << '\n';
    printDyck(output, "vertical", stats.vertical_dyck);
    printDyck(output, "horizontal", stats.horizontal_dyck);
    printDyck(output, "single-counter", stats.single_counter_dyck);
    printDyck(output, "quotient", stats.quotient_dyck);
    output << "  input vertices/arcs: " << stats.input_vertices << '/'
           << stats.input_arcs << '\n'
           << "  quotient vertices/arcs: " << stats.quotient_vertices << '/'
           << stats.quotient_arcs << '\n'
           << "  threshold: " << stats.threshold << '\n'
           << "  vertical states/arcs: " << stats.vertical_control_states << '/'
           << stats.vertical_arcs << '\n'
           << "  horizontal states/arcs: " << stats.horizontal_control_states
           << '/' << stats.horizontal_arcs << '\n'
           << "  vertical Dyck pops/unions: "
           << stats.vertical_dyck.worklist_pops << '/'
           << stats.vertical_dyck.component_unions << '\n'
           << "  horizontal Dyck pops/unions: "
           << stats.horizontal_dyck.worklist_pops << '/'
           << stats.horizontal_dyck.component_unions << '\n'
           << "  quotient Dyck pops/unions: "
           << stats.quotient_dyck.worklist_pops << '/'
           << stats.quotient_dyck.component_unions << '\n'
           << "  added reverse arcs: " << stats.added_reverse_arcs << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    printPairs(output, graph, result);
  }
}

void printFixedCounterResult(std::ostream &output,
                             const CommandLine &command_line,
                             const interleaved_dyck::Graph &graph,
                             const unary::FixedCounterResult &result,
                             std::int64_t elapsed_ms) {
  printPreamble(output, "fixed-counter", result);
  if (command_line.stats) {
    const unary::FixedCounterStats &stats = result.stats();
    printExecution(output, stats.execution);
    printDyck(output, "fixed-counter", stats.dyck);
    printDyck(output, "single-counter", stats.single_counter_dyck);
    printDyck(output, "quotient", stats.quotient_dyck);
    output << "  input vertices/arcs: " << stats.input_vertices << '/'
           << stats.input_arcs << '\n'
           << "  quotient vertices/arcs: " << stats.quotient_vertices << '/'
           << stats.quotient_arcs << '\n'
           << "  counter bound: " << stats.counter_bound << '\n'
           << "  control states: " << stats.control_states << '\n'
           << "  translated arcs: " << stats.translated_arcs << '\n'
           << "  epsilon/closing edges: " << stats.epsilon_edges << '/'
           << stats.closing_edges << '\n'
           << "  Dyck worklist pops/unions: " << stats.dyck.worklist_pops << '/'
           << stats.dyck.component_unions << '\n'
           << "  quotient Dyck pops/unions: "
           << stats.quotient_dyck.worklist_pops << '/'
           << stats.quotient_dyck.component_unions << '\n'
           << "  added reverse arcs: " << stats.added_reverse_arcs << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    printPairs(output, graph, result);
  }
}

} // namespace

int runInterleavedDyckUnary(int argc, char **argv) {
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

    const auto start = std::chrono::steady_clock::now();
    if (command_line.algorithm == unary::Algorithm::Adaptive) {
      unary::AdaptiveOptions options;
      options.sparsify = command_line.sparsify;
      options.collect_phase_timing = command_line.phase_timing;
      if (command_line.add_reverse_edges) {
        options.input_policy =
            unary::BidirectedInputPolicy::AddMissingReverseEdges;
      }
      unary::AdaptiveResult result;
      if (command_line.shallow_threshold) {
        result = unary::AdaptiveSolver{}.solveShallow(
            graph, *command_line.shallow_threshold, options);
      } else {
        result = unary::AdaptiveSolver{}.solve(graph, options);
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start);
      printAdaptiveResult(*output, command_line, graph, result,
                          elapsed.count());
      dumpComponents(command_line.component_output, result);
    } else {
      unary::FixedCounterOptions options;
      options.sparsify = command_line.sparsify;
      if (command_line.add_reverse_edges) {
        options.input_policy =
            unary::BidirectedInputPolicy::AddMissingReverseEdges;
      }
      const unary::FixedCounterResult result =
          unary::FixedCounterSolver{}.solve(graph, options);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start);
      printFixedCounterResult(*output, command_line, graph, result,
                              elapsed.count());
      dumpComponents(command_line.component_output, result);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck unary: " << error.what() << '\n';
    return 1;
  }
}
