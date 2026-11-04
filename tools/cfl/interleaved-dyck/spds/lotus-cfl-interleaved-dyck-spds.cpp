#include "CFL/InterleavedDyck/SPDS/Solver.h"
#include "CFL/InterleavedDyck/SPDS/ValueFlow.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace spds = dyck::spds;
namespace {
template <class T> T number(std::string_view text) {
  T result{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer: " + std::string(text));
  return result;
}
void usage() {
  std::cout
      << "Usage: lotus-cfl-interleaved-dyck spds [options] graph.dot\n"
         "  --analysis NAME         dyck (default) or value-flow\n"
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
} // namespace
int runInterleavedDyckSPDS(int argc, char **argv) {
  try {
    spds::Options options;
    std::optional<dyck::Vertex> source, target;
    std::vector<dyck::Vertex> vertices;
    bool single_query = false, all_pairs = false, pairs = false,
         timings = false, positional = false;
    std::string path, queries_path, client = "dyck";
    spds::DemandDirection demand_direction = spds::DemandDirection::Auto;
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
        return 0;
      }
      if (!positional && arg == "--analysis") {
        client = argument(i);
        if (client != "dyck" && client != "value-flow")
          throw std::invalid_argument(
              "invalid --analysis; use dyck or value-flow");
      } else if (!positional && arg == "--all-pairs") {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        all_pairs = true;
      } else if (!positional && arg == "--query") {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        source = number<dyck::Vertex>(argument(i));
        target = number<dyck::Vertex>(argument(i));
        single_query = true;
      } else if (!positional && (arg == "--source" || arg == "--target")) {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        auto value = number<dyck::Vertex>(argument(i));
        if (arg == "--source")
          source = value;
        else
          target = value;
      } else if (!positional && arg == "--queries") {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        queries_path = argument(i);
      } else if (!positional && arg == "--direction") {
        const auto value = argument(i);
        if (value == "auto")
          demand_direction = spds::DemandDirection::Auto;
        else if (value == "post")
          demand_direction = spds::DemandDirection::Post;
        else if (value == "pre")
          demand_direction = spds::DemandDirection::Pre;
        else
          throw std::invalid_argument("invalid --direction");
      } else if (!positional && arg == "--call-prefix")
        options.parentheses = spds::StackAcceptance::Any;
      else if (!positional && arg == "--field-prefix")
        options.brackets = spds::StackAcceptance::Any;
      else if (!positional && arg == "--pairs")
        pairs = true;
      else if (!positional && arg == "--timings")
        timings = true;
      else if (!positional && arg == "--vertex")
        vertices.push_back(number<dyck::Vertex>(argument(i)));
      else if (!positional && arg == "--max-states")
        options.limits.max_states = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-transitions")
        options.limits.max_transitions = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-updates")
        options.limits.max_updates = number<std::size_t>(argument(i));
      else if (!positional && !arg.empty() && arg.front() == '-')
        throw std::invalid_argument("unknown option: " + std::string(arg));
      else {
        if (!path.empty())
          throw std::invalid_argument("multiple graph files");
        path = arg;
      }
    }
    if (path.empty())
      throw std::invalid_argument("missing graph.dot; use --help");
    if (!all_pairs && !source && !target && queries_path.empty())
      throw std::invalid_argument(
          "missing query scope; use --all-pairs, --query, --source, "
          "--target, or --queries");
    if (all_pairs && client == "dyck" &&
        demand_direction != spds::DemandDirection::Auto)
      throw std::invalid_argument(
          "--direction is only valid for demand queries");
    if (source && !target && demand_direction == spds::DemandDirection::Pre)
      throw std::invalid_argument("--source requires post direction");
    if (target && !source && demand_direction == spds::DemandDirection::Post)
      throw std::invalid_argument("--target requires pre direction");
    const bool reverse =
        (target && !source) ||
        (single_query && demand_direction == spds::DemandDirection::Pre);
    if (reverse && (options.parentheses != spds::StackAcceptance::Empty ||
                    options.brackets != spds::StackAcceptance::Empty))
      throw std::invalid_argument("prefix flags require a forward query");
    if (!queries_path.empty() &&
        (options.parentheses != spds::StackAcceptance::Empty ||
         options.brackets != spds::StackAcceptance::Empty) &&
        demand_direction == spds::DemandDirection::Pre)
      throw std::invalid_argument("prefix flags require post direction");
    dyck::Graph graph = dyck::Graph::parseDotFile(path);
    for (auto v : vertices)
      graph.addVertex(v);
    if ((source && !graph.containsVertex(*source)) ||
        (target && !graph.containsVertex(*target)))
      throw std::invalid_argument(
          "query vertex is not in graph; use --vertex for isolated vertices");
    if (client == "value-flow") {
      spds::ValueFlowAnalysis analysis(graph, options);
      spds::Result result;
      std::vector<dyck::Pair> demands;
      if (!queries_path.empty()) {
        demands = readDemands(queries_path);
        result = analysis.analyzeDemands(demands, demand_direction);
      } else if (single_query)
        result =
            analysis.analyzeDemands({{*source, *target}}, demand_direction);
      else if (source)
        result = analysis.analyzeFrom(*source);
      else if (target)
        result = analysis.analyzeTo(*target);
      else
        result = analysis.analyzeAll(demand_direction);
      std::cout << "engine: spds\nanalysis: value-flow\n"
                   "semantics: sound upper bound\ncall-stack: "
                   "empty\nfield-stack: empty\n";
      if (!queries_path.empty())
        std::cout << "requested-pairs: " << demands.size() << '\n';
      if (single_query) {
        const dyck::Pair p{*source, *target};
        std::cout << "query: "
                  << (result.mayReach(*source, *target) ? "may-reach"
                                                        : "unreachable")
                  << "\ncall-projection: "
                  << (result.parenthesis_pairs.count(p) ? "accept" : "reject")
                  << "\nfield-projection: "
                  << (result.bracket_pairs.count(p) ? "accept" : "reject")
                  << '\n';
      }
      std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
      printStats(result.statistics, timings);
      if (pairs)
        printPairs(result.upper_bound);
      return 0;
    }
    spds::Solver solver(options);
    auto analysis = solver.prepare(graph);
    // Do not print result headers until saturation has completed successfully.
    const auto header = [&] {
      std::cout << "engine: spds\nsemantics: sound upper bound\n"
                << "scope: "
                << (all_pairs               ? "all-pairs"
                    : !queries_path.empty() ? "batch"
                    : single_query          ? "pair"
                    : source                ? "source"
                                            : "target")
                << '\n'
                << "direction: "
                << (!queries_path.empty() &&
                            demand_direction == spds::DemandDirection::Auto
                        ? "auto"
                    : reverse ? "pre*"
                              : "post*")
                << '\n'
                << "call-stack: "
                << (options.parentheses == spds::StackAcceptance::Empty
                        ? "empty"
                        : "any")
                << '\n'
                << "field-stack: "
                << (options.brackets == spds::StackAcceptance::Empty ? "empty"
                                                                     : "any")
                << '\n';
    };
    if (!queries_path.empty()) {
      auto demands = readDemands(queries_path);
      auto direction = demand_direction;
      if (direction == spds::DemandDirection::Auto &&
          (options.parentheses != spds::StackAcceptance::Empty ||
           options.brackets != spds::StackAcceptance::Empty))
        direction = spds::DemandDirection::Post;
      auto result = analysis.analyzeDemands(demands, direction);
      header();
      std::cout << "requested-pairs: " << demands.size()
                << "\ncandidate-pairs: " << result.upper_bound.size() << '\n';
      printStats(result.statistics, timings);
      if (pairs)
        printPairs(result.upper_bound);
    } else if (source || target) {
      if (single_query) {
        auto result =
            reverse ? analysis.queryTo(*target) : analysis.queryFrom(*source);
        header();
        auto v = reverse ? *source : *target;
        std::cout << "query: "
                  << (result.mayReach(v) ? "may-reach" : "unreachable") << '\n'
                  << "call-projection: "
                  << (result.parenthesisReachable(v) ? "accept" : "reject")
                  << '\n'
                  << "field-projection: "
                  << (result.bracketReachable(v) ? "accept" : "reject") << '\n';
        if (pairs && result.mayReach(v))
          std::cout << "pair: " << *source << ' ' << *target << '\n';
        std::cout << "call-automaton\n";
        printStats(result.callAutomaton().statistics(), timings);
        std::cout << "field-automaton\n";
        printStats(result.fieldAutomaton().statistics(), timings);
      } else {
        auto result = reverse ? analysis.analyzeTo(*target)
                              : analysis.analyzeFrom(*source);
        header();
        std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
        printStats(result.statistics, timings);
        if (pairs)
          printPairs(result.upper_bound);
      }
    } else {
      const auto result = analysis.analyzeAll();
      header();
      std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
      printStats(result.statistics, timings);
      if (pairs)
        printPairs(result.upper_bound);
    }
    return 0;
  } catch (const spds::ResourceLimit &error) {
    std::cerr << "resource-limit: " << error.what() << "; no result returned\n";
    return 3;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
