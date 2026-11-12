#include "CFL/InterleavedDyck/SPDS/Solver.h"
#include "CFL/InterleavedDyck/SPDS/ValueFlow.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace spds = dyck::spds;

namespace {

enum class BenchmarkKind { Taint, ValueFlow };

struct CommandLine {
  std::string path;
  std::string queries_path;
  BenchmarkKind analysis = BenchmarkKind::Taint;
  spds::Options options;
  spds::DemandDirection demand_direction = spds::DemandDirection::Auto;
  std::optional<dyck::Vertex> source;
  std::optional<dyck::Vertex> target;
  std::vector<dyck::Vertex> vertices;
  bool all_pairs = false;
  bool single_query = false;
  bool pairs = false;
  bool timings = false;
  bool bidirect = false;
};

template <class T> T number(std::string_view text) {
  T result{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer: " + std::string(text));
  return result;
}

BenchmarkKind parseAnalysis(std::string_view text) {
  if (text == "taint")
    return BenchmarkKind::Taint;
  if (text == "value-flow")
    return BenchmarkKind::ValueFlow;
  throw std::invalid_argument("invalid --analysis; use taint or value-flow");
}

void usage() {
  std::cout
      << "Usage: lotus-cfl-interleaved-dyck spds [options] graph.dot\n"
         "  --analysis NAME         client analysis: taint (default) or "
         "value-flow\n"
         "  --all-pairs             analyze every source/target pair\n"
         "  --query SOURCE TARGET   one balanced reachability query\n"
         "  --source V              candidate successors of V (post*)\n"
         "  --target V              candidate predecessors of V (pre*)\n"
         "  --queries FILE          batch SOURCE TARGET pairs; '-' reads "
         "stdin\n"
         "  --direction D           auto (default), post, or pre for "
         "batch/query\n"
         "  --call-prefix           allow pending calls at a forward endpoint\n"
         "  --field-prefix          allow pending stores at a forward "
         "endpoint\n"
         "  --bidirect              add missing complement reverse arcs\n"
         "  --vertex V              add a vertex (also supports isolated "
         "vertices)\n"
         "  --pairs                 print sorted candidate pairs\n"
         "  --timings               print instrumented phase times\n"
         "  --max-states N          state limit per projection (0 = "
         "unlimited)\n"
         "  --max-transitions N     transition limit per projection\n"
         "  --max-updates N         weight-update limit per projection\n"
         "  --help                  show this help\n"
         "A query scope is required. Results are a sound upper bound.\n"
         "value-flow requires bracket-ended balanced paths, excludes self-pairs,\n"
         "and rejects prefix flags. --direction also applies to its all-pairs scope.\n"
         "Exit codes: 0 = complete; 2 = input error; 3 = resource limit (no "
         "result).\n";
}

std::vector<dyck::Pair> readDemands(const std::string &path) {
  std::ifstream file;
  std::istream *input = &std::cin;
  if (path != "-") {
    file.open(path);
    if (!file)
      throw std::invalid_argument("cannot open demand file");
    input = &file;
  }
  std::vector<dyck::Pair> result;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(*input, line)) {
    ++line_number;
    const auto comment = line.find('#');
    if (comment != std::string::npos)
      line.resize(comment);
    std::istringstream row(line);
    dyck::Pair pair;
    std::string extra;
    if (!(row >> pair.source)) {
      row.clear();
      row >> std::ws;
      if (row.eof())
        continue;
      throw std::invalid_argument("invalid demand on line " +
                                  std::to_string(line_number));
    }
    if (!(row >> pair.target) || (row >> extra))
      throw std::invalid_argument("invalid demand on line " +
                                  std::to_string(line_number));
    result.push_back(pair);
  }
  if (!input->eof())
    throw std::runtime_error("failed while reading demand file");
  return result;
}

void printStats(const spds::Statistics &s, bool timings) {
  std::cout << "states: " << s.states << "\ntransitions: " << s.transitions
            << "\nweight-updates: " << s.updates
            << "\nprocessed: " << s.processed << "\nrules: " << s.rules << '\n';
  if (timings)
    std::cout << "setup-us: " << s.setup_microseconds
              << "\nsaturation-us: " << s.saturation_microseconds
              << "\nreadout-us: " << s.readout_microseconds
              << "\nprojection-us: " << s.projection_microseconds << '\n';
}

void printPairs(const dyck::PairSet &pairs) {
  std::vector<dyck::Pair> ordered(pairs.begin(), pairs.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source, a.target) < std::tie(b.source, b.target);
  });
  for (const auto &p : ordered)
    std::cout << "pair: " << p.source << ' ' << p.target << '\n';
}

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine cmd;
  bool positional = false;
  auto argument = [&](int &i) -> std::string_view {
    if (++i >= argc)
      throw std::invalid_argument("missing option argument");
    return argv[i];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (!positional && arg == "--") {
      positional = true;
      continue;
    }
    if (!positional && arg == "--help") {
      usage();
      std::exit(0);
    }
    if (!positional && arg == "--analysis") {
      cmd.analysis = parseAnalysis(argument(i));
    } else if (!positional && arg == "--all-pairs") {
      if (cmd.all_pairs || cmd.source || cmd.target || !cmd.queries_path.empty())
        throw std::invalid_argument("multiple query scopes");
      cmd.all_pairs = true;
    } else if (!positional && arg == "--query") {
      if (cmd.all_pairs || cmd.source || cmd.target || !cmd.queries_path.empty())
        throw std::invalid_argument("multiple query scopes");
      cmd.source = number<dyck::Vertex>(argument(i));
      cmd.target = number<dyck::Vertex>(argument(i));
      cmd.single_query = true;
    } else if (!positional && (arg == "--source" || arg == "--target")) {
      if (cmd.all_pairs || cmd.source || cmd.target || !cmd.queries_path.empty())
        throw std::invalid_argument("multiple query scopes");
      auto value = number<dyck::Vertex>(argument(i));
      if (arg == "--source")
        cmd.source = value;
      else
        cmd.target = value;
    } else if (!positional && arg == "--queries") {
      if (cmd.all_pairs || cmd.source || cmd.target || !cmd.queries_path.empty())
        throw std::invalid_argument("multiple query scopes");
      cmd.queries_path = argument(i);
    } else if (!positional && arg == "--direction") {
      const auto value = argument(i);
      if (value == "auto")
        cmd.demand_direction = spds::DemandDirection::Auto;
      else if (value == "post")
        cmd.demand_direction = spds::DemandDirection::Post;
      else if (value == "pre")
        cmd.demand_direction = spds::DemandDirection::Pre;
      else
        throw std::invalid_argument("invalid --direction");
    } else if (!positional && arg == "--call-prefix") {
      cmd.options.parentheses = spds::StackAcceptance::Any;
    } else if (!positional && arg == "--field-prefix") {
      cmd.options.brackets = spds::StackAcceptance::Any;
    } else if (!positional && arg == "--bidirect") {
      cmd.bidirect = true;
    } else if (!positional && arg == "--pairs") {
      cmd.pairs = true;
    } else if (!positional && arg == "--timings") {
      cmd.timings = true;
    } else if (!positional && arg == "--vertex") {
      cmd.vertices.push_back(number<dyck::Vertex>(argument(i)));
    } else if (!positional && arg == "--max-states") {
      cmd.options.limits.max_states = number<std::size_t>(argument(i));
    } else if (!positional && arg == "--max-transitions") {
      cmd.options.limits.max_transitions = number<std::size_t>(argument(i));
    } else if (!positional && arg == "--max-updates") {
      cmd.options.limits.max_updates = number<std::size_t>(argument(i));
    } else if (!positional && !arg.empty() && arg.front() == '-') {
      throw std::invalid_argument("unknown option: " + std::string(arg));
    } else {
      if (!cmd.path.empty())
        throw std::invalid_argument("multiple graph files");
      cmd.path = arg;
    }
  }

  if (cmd.path.empty())
    throw std::invalid_argument("missing graph.dot; use --help");
  if (!cmd.all_pairs && !cmd.source && !cmd.target && cmd.queries_path.empty())
    throw std::invalid_argument(
        "missing query scope; use --all-pairs, --query, --source, "
        "--target, or --queries");
  if (cmd.all_pairs && cmd.analysis == BenchmarkKind::Taint &&
      cmd.demand_direction != spds::DemandDirection::Auto)
    throw std::invalid_argument(
        "--direction is only valid for demand queries");
  if (cmd.source && !cmd.target && cmd.demand_direction == spds::DemandDirection::Pre)
    throw std::invalid_argument("--source requires post direction");
  if (cmd.target && !cmd.source && cmd.demand_direction == spds::DemandDirection::Post)
    throw std::invalid_argument("--target requires pre direction");

  const bool reverse =
      (cmd.target && !cmd.source) ||
      (cmd.single_query && cmd.demand_direction == spds::DemandDirection::Pre);
  if (reverse && (cmd.options.parentheses != spds::StackAcceptance::Empty ||
                  cmd.options.brackets != spds::StackAcceptance::Empty))
    throw std::invalid_argument("prefix flags require a forward query");
  if (!cmd.queries_path.empty() &&
      (cmd.options.parentheses != spds::StackAcceptance::Empty ||
       cmd.options.brackets != spds::StackAcceptance::Empty) &&
      cmd.demand_direction == spds::DemandDirection::Pre)
    throw std::invalid_argument("prefix flags require post direction");

  return cmd;
}

int runValueFlow(const CommandLine &cmd, const dyck::Graph &graph) {
  spds::ValueFlowAnalysis analysis(graph, cmd.options);
  spds::Result result;
  std::vector<dyck::Pair> demands;

  if (!cmd.queries_path.empty()) {
    demands = readDemands(cmd.queries_path);
    result = analysis.analyzeDemands(demands, cmd.demand_direction);
  } else if (cmd.single_query) {
    result = analysis.analyzeDemands({{*cmd.source, *cmd.target}},
                                    cmd.demand_direction);
  } else if (cmd.source) {
    result = analysis.analyzeFrom(*cmd.source);
  } else if (cmd.target) {
    result = analysis.analyzeTo(*cmd.target);
  } else {
    result = analysis.analyzeAll(cmd.demand_direction);
  }

  std::cout << "engine: spds\nanalysis: value-flow\n"
               "semantics: sound upper bound\ncall-stack: "
               "empty\nfield-stack: empty\n";
  if (!cmd.queries_path.empty())
    std::cout << "requested-pairs: " << demands.size() << '\n';
  if (cmd.single_query) {
    const dyck::Pair p{*cmd.source, *cmd.target};
    std::cout << "query: "
              << (result.mayReach(*cmd.source, *cmd.target) ? "may-reach"
                                                            : "unreachable")
              << "\ncall-projection: "
              << (result.parenthesis_pairs.count(p) ? "accept" : "reject")
              << "\nfield-projection: "
              << (result.bracket_pairs.count(p) ? "accept" : "reject") << '\n';
  }
  std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
  printStats(result.statistics, cmd.timings);
  if (cmd.pairs)
    printPairs(result.upper_bound);
  return 0;
}

int runTaint(const CommandLine &cmd, const dyck::Graph &graph) {
  spds::Solver solver(cmd.options);
  auto analysis = solver.prepare(graph);

  const bool reverse =
      (cmd.target && !cmd.source) ||
      (cmd.single_query && cmd.demand_direction == spds::DemandDirection::Pre);

  const auto header = [&] {
    std::cout << "engine: spds\nsemantics: sound upper bound\n"
              << "scope: "
              << (cmd.all_pairs               ? "all-pairs"
                  : !cmd.queries_path.empty() ? "batch"
                  : cmd.single_query          ? "pair"
                  : cmd.source                ? "source"
                                              : "target")
              << '\n'
              << "direction: "
              << (!cmd.queries_path.empty() &&
                          cmd.demand_direction == spds::DemandDirection::Auto
                      ? "auto"
                  : reverse ? "pre*"
                            : "post*")
              << '\n'
              << "call-stack: "
              << (cmd.options.parentheses == spds::StackAcceptance::Empty
                      ? "empty"
                      : "any")
              << '\n'
              << "field-stack: "
              << (cmd.options.brackets == spds::StackAcceptance::Empty ? "empty"
                                                                       : "any")
              << '\n';
  };

  if (!cmd.queries_path.empty()) {
    auto demands = readDemands(cmd.queries_path);
    auto direction = cmd.demand_direction;
    if (direction == spds::DemandDirection::Auto &&
        (cmd.options.parentheses != spds::StackAcceptance::Empty ||
         cmd.options.brackets != spds::StackAcceptance::Empty))
      direction = spds::DemandDirection::Post;
    auto result = analysis.analyzeDemands(demands, direction);
    header();
    std::cout << "requested-pairs: " << demands.size()
              << "\ncandidate-pairs: " << result.upper_bound.size() << '\n';
    printStats(result.statistics, cmd.timings);
    if (cmd.pairs)
      printPairs(result.upper_bound);
  } else if (cmd.source || cmd.target) {
    if (cmd.single_query) {
      auto result =
          reverse ? analysis.queryTo(*cmd.target) : analysis.queryFrom(*cmd.source);
      header();
      auto v = reverse ? *cmd.source : *cmd.target;
      std::cout << "query: "
                << (result.mayReach(v) ? "may-reach" : "unreachable") << '\n'
                << "call-projection: "
                << (result.parenthesisReachable(v) ? "accept" : "reject")
                << '\n'
                << "field-projection: "
                << (result.bracketReachable(v) ? "accept" : "reject") << '\n';
      if (cmd.pairs && result.mayReach(v))
        std::cout << "pair: " << *cmd.source << ' ' << *cmd.target << '\n';
      std::cout << "call-automaton\n";
      printStats(result.callAutomaton().statistics(), cmd.timings);
      std::cout << "field-automaton\n";
      printStats(result.fieldAutomaton().statistics(), cmd.timings);
    } else {
      auto result = reverse ? analysis.analyzeTo(*cmd.target)
                            : analysis.analyzeFrom(*cmd.source);
      header();
      std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
      printStats(result.statistics, cmd.timings);
      if (cmd.pairs)
        printPairs(result.upper_bound);
    }
  } else {
    const auto result = analysis.analyzeAll();
    header();
    std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
    printStats(result.statistics, cmd.timings);
    if (cmd.pairs)
      printPairs(result.upper_bound);
  }
  return 0;
}

} // namespace

int runInterleavedDyckSPDS(int argc, char **argv) {
  try {
    CommandLine cmd = parseCommandLine(argc, argv);
    dyck::Graph graph = dyck::Graph::parseDotFile(cmd.path);
    if (cmd.bidirect)
      graph.makeBidirected();
    for (auto v : cmd.vertices)
      graph.addVertex(v);
    if ((cmd.source && !graph.containsVertex(*cmd.source)) ||
        (cmd.target && !graph.containsVertex(*cmd.target)))
      throw std::invalid_argument(
          "query vertex is not in graph; use --vertex for isolated vertices");

    if (cmd.analysis == BenchmarkKind::ValueFlow)
      return runValueFlow(cmd, graph);
    return runTaint(cmd, graph);
  } catch (const spds::ResourceLimit &error) {
    std::cerr << "resource-limit: " << error.what() << "; no result returned\n";
    return 3;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
