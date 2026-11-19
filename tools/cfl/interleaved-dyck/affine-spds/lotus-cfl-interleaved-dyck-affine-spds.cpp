#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace affine = dyck::affine;
namespace spds = dyck::spds;
namespace {
template <class T> T number(std::string_view text) {
  T value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer: " + std::string(text));
  return value;
}
std::vector<unsigned> stack(std::string_view text) {
  std::vector<unsigned> result;
  if (text.empty())
    return result;
  while (true) {
    auto comma = text.find(',');
    result.push_back(number<unsigned>(text.substr(0, comma)));
    if (comma == std::string_view::npos)
      break;
    text.remove_prefix(comma + 1);
  }
  return result;
}
void usage() {
  std::cout
      << "Usage: lotus-cfl-interleaved-dyck affine-spds [options] graph.dot\n"
         "  --all-pairs             analyze every source/target pair\n"
         "  --query SOURCE TARGET   one query (default post*)\n"
         "  --source V              query all successors using post*\n"
         "  --target V              query all predecessors using pre*\n"
         "  --queries FILE          batch SOURCE TARGET pairs; '-' reads "
         "stdin\n"
         "  --direction D           auto (default), post, or pre for "
         "batch/query\n"
         "  --call-prefix           any call stack at queried vertex\n"
         "  --field-prefix          any field stack at queried vertex\n"
         "  --call-stack IDS        exact comma-separated stack, top first "
         "(--query)\n"
         "  --field-stack IDS       exact stack; empty string means empty "
         "(--query)\n"
         "  --events N              automatic event budget (default 4)\n"
         "  --order-pairs N         automatic ordered-pair budget (default 2)\n"
         "  --identity              identity observer, equivalent to Boolean "
         "SPDS\n"
         "  --label-parity N        in-memory label-parity observer with N "
         "paren + N bracket groups\n"
         "  --single-sided-parity N asymmetric N-block observer per projection "
         "(2*N dim, e.g. 64 for N=32)\n"
         "  --corridor-rehash       rehash active matched delimiters per "
         "corridor (with --single-sided-parity)\n"
         "  --best-observer         best single-sided parity observer "
         "(--single-sided-parity 32 --corridor-rehash)\n"
         "  --observer FILE         read a shared original-edge matrix map\n"
         "  --dump-observer FILE    save the map, including independent-block "
         "metadata\n"
         "  --mode MODE             joint (default), independent, or spds\n"
         "  --certificate           include a separating affine equation when "
         "available\n"
         "  --json                  emit machine-readable results and "
         "statistics\n"
         "  --pairs                 include sorted candidate pairs\n"
         "  --timings               include instrumented phase times\n"
         "  --vertex V              add a vertex (including isolated "
         "vertices)\n"
         "  --max-dimension N       matrix-dimension cap; 0 = unlimited\n"
         "  --max-states N          SPDS state limit per projection; 0 = "
         "unlimited\n"
         "  --max-transitions N     SPDS transition limit per projection\n"
         "  --max-updates N         SPDS weight-promotion limit per "
         "projection\n"
         "  --help                  display this help\n"
         "A query scope is required. Every mode is an upper bound.\n"
         "Pre* queries refer to predecessor stacks; the target stacks are "
         "empty.\n"
         "Exit codes: 0 complete; 2 invalid input/I/O; 3 resource failure (no "
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
const char *reason(affine::Verdict verdict) {
  switch (verdict) {
  case affine::Verdict::MayReach:
    return "overlapping-histories";
  case affine::Verdict::ProjectionRejected:
    return "projection-rejected";
  case affine::Verdict::AffineSeparated:
    return "affine-separated";
  }
  throw std::logic_error("invalid verdict");
}
std::vector<dyck::Pair> ordered(const dyck::PairSet &set) {
  std::vector<dyck::Pair> result(set.begin(), set.end());
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source, a.target) < std::tie(b.source, b.target);
  });
  return result;
}
const char *boolean(bool b) { return b ? "true" : "false"; }
const char *answer(bool b) { return b ? "may-reach" : "unreachable"; }
void statistics(std::ostream &out, const affine::Statistics &stats, bool json,
                bool timings, std::uint64_t observer_us = 0) {
  const auto &s = stats.saturation;
  if (json) {
    out << "\"statistics\":{\"matrix_dimension\":" << stats.matrix_dimension
        << ",\"coordinate_dimension\":" << stats.coordinate_dimension
        << ",\"slice_cache_hits\":" << stats.slice_cache_hits
        << ",\"compiled_rules\":" << stats.compiled_rules
        << ",\"prepared_weights\":" << stats.prepared_weights
        << ",\"matrix_products\":" << stats.algebra.matrix_products
        << ",\"basis_reductions\":" << stats.algebra.basis_reductions
        << ",\"basis_insertions\":" << stats.algebra.basis_insertions
        << ",\"cow_detaches\":" << stats.algebra.cow_detaches
        << ",\"intersection_tests\":" << stats.algebra.intersection_tests
        << ",\"intersection_fast_paths\":"
        << stats.algebra.intersection_fast_paths
        << ",\"max_affine_rank\":" << stats.maximum_affine_rank
        << ",\"states\":" << s.states << ",\"transitions\":" << s.transitions
        << ",\"updates\":" << s.updates << ",\"processed\":" << s.processed
        << ",\"rules\":" << s.rules;
    if (timings)
      out << ",\"setup_us\":" << s.setup_microseconds
          << ",\"saturation_us\":" << s.saturation_microseconds
          << ",\"readout_us\":" << s.readout_microseconds
          << ",\"projection_us\":" << s.projection_microseconds
          << ",\"observer_us\":" << stats.observer_microseconds + observer_us
          << ",\"intersection_us\":" << stats.algebra.intersection_us
          << ",\"certificate_us\":" << stats.algebra.certificate_us;
    out << '}';
  } else {
    out << "matrix-dimension: " << stats.matrix_dimension
        << "\ncoordinate-dimension: " << stats.coordinate_dimension
        << "\nslice-cache-hits: " << stats.slice_cache_hits
        << "\ncompiled-rules: " << stats.compiled_rules
        << "\nprepared-weights: " << stats.prepared_weights
        << "\nmatrix-products: " << stats.algebra.matrix_products
        << "\nbasis-reductions: " << stats.algebra.basis_reductions
        << "\nbasis-insertions: " << stats.algebra.basis_insertions
        << "\ncow-detaches: " << stats.algebra.cow_detaches
        << "\nintersection-tests: " << stats.algebra.intersection_tests
        << "\nmax-affine-rank: " << stats.maximum_affine_rank
        << "\nstates: " << s.states << "\ntransitions: " << s.transitions
        << "\nweight-updates: " << s.updates << "\nprocessed: " << s.processed
        << "\nrules: " << s.rules << '\n';
    if (timings)
      out << "setup-us: " << s.setup_microseconds
          << "\nsaturation-us: " << s.saturation_microseconds
          << "\nreadout-us: " << s.readout_microseconds
          << "\nprojection-us: " << s.projection_microseconds
          << "\nobserver-us: " << stats.observer_microseconds + observer_us
          << "\nintersection-us: " << stats.algebra.intersection_us
          << "\ncertificate-us: " << stats.algebra.certificate_us << '\n';
  }
}
void certificate(std::ostream &out, const affine::HistoryComparison &comparison,
                 bool json) {
  const auto &c = comparison.certificate();
  if (json)
    out << ",\"certificate\":";
  if (!c) {
    if (json)
      out << "null";
    else
      out << "certificate: none\n";
    return;
  }
  if (!c->verify(comparison.callHistory(), comparison.fieldHistory()))
    throw std::logic_error("invalid internal separation certificate");
  if (json) {
    out << "{\"functional\":\"" << c->functional.str()
        << "\",\"call_value\":" << unsigned(c->left_value)
        << ",\"field_value\":" << unsigned(c->right_value)
        << ",\"verified_against_hulls\":true}";
  } else {
    out << "certificate-functional: " << c->functional.str()
        << "\ncertificate-call-value: " << unsigned(c->left_value)
        << "\ncertificate-field-value: " << unsigned(c->right_value)
        << "\ncertificate-verified-against-hulls: true\n";
  }
}
} // namespace
int runInterleavedDyckAffineSPDS(int argc, char **argv) {
  try {
    affine::Options options;
    std::optional<dyck::Vertex> source, target;
    std::optional<std::vector<unsigned>> call_stack, field_stack;
    bool single = false, all_pairs = false, identity = false, pairs = false;
    bool json = false, show_certificate = false, positional = false,
         timings = false, feature_options = false, corridor_rehash = false,
         best_observer = false;
    unsigned label_parity_groups = 0, single_sided_groups = 0;
    std::string path, observer_path, dump_path, queries_path, mode = "joint";
    spds::DemandDirection demand_direction = spds::DemandDirection::Auto;
    std::vector<dyck::Vertex> vertices;
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
      if (!positional && arg == "--all-pairs") {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        all_pairs = true;
      } else if (!positional && arg == "--query") {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        source = number<dyck::Vertex>(argument(i));
        target = number<dyck::Vertex>(argument(i));
        single = true;
      } else if (!positional && (arg == "--source" || arg == "--target")) {
        if (all_pairs || source || target || !queries_path.empty())
          throw std::invalid_argument("multiple query scopes");
        const auto v = number<dyck::Vertex>(argument(i));
        if (arg == "--source")
          source = v;
        else
          target = v;
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
      else if (!positional && arg == "--call-stack")
        call_stack = stack(argument(i));
      else if (!positional && arg == "--field-stack")
        field_stack = stack(argument(i));
      else if (!positional && arg == "--events") {
        options.observer.max_events = number<std::size_t>(argument(i));
        feature_options = true;
      } else if (!positional && arg == "--order-pairs") {
        options.observer.max_order_pairs = number<std::size_t>(argument(i));
        feature_options = true;
      } else if (!positional && arg == "--observer")
        observer_path = argument(i);
      else if (!positional && arg == "--label-parity") {
        label_parity_groups = number<unsigned>(argument(i));
        if (label_parity_groups == 0)
          throw std::invalid_argument("--label-parity must be >= 1");
      } else if (!positional && arg == "--single-sided-parity") {
        single_sided_groups = number<unsigned>(argument(i));
        if (single_sided_groups == 0)
          throw std::invalid_argument("--single-sided-parity must be >= 1");
      } else if (!positional && arg == "--corridor-rehash") {
        corridor_rehash = true;
      } else if (!positional && arg == "--best-observer") {
        best_observer = true;
      } else if (!positional && arg == "--dump-observer")
        dump_path = argument(i);
      else if (!positional && arg == "--mode")
        mode = argument(i);
      else if (!positional && arg == "--identity")
        identity = true;
      else if (!positional && arg == "--certificate")
        show_certificate = true;
      else if (!positional && arg == "--json")
        json = true;
      else if (!positional && arg == "--pairs")
        pairs = true;
      else if (!positional && arg == "--timings")
        timings = true;
      else if (!positional && arg == "--vertex")
        vertices.push_back(number<dyck::Vertex>(argument(i)));
      else if (!positional && arg == "--max-dimension")
        options.max_matrix_dimension = number<std::size_t>(argument(i));
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
    if (mode != "joint" && mode != "independent" && mode != "spds")
      throw std::invalid_argument("invalid --mode");
    const auto comparison_mode = mode == "joint" ? affine::ComparisonMode::Joint
                                 : mode == "independent"
                                     ? affine::ComparisonMode::Independent
                                     : affine::ComparisonMode::Projection;
    if (all_pairs && demand_direction != spds::DemandDirection::Auto)
      throw std::invalid_argument(
          "--direction is only valid for demand queries");
    if (source && !target && demand_direction == spds::DemandDirection::Pre)
      throw std::invalid_argument("--source requires post direction");
    if (target && !source && demand_direction == spds::DemandDirection::Post)
      throw std::invalid_argument("--target requires pre direction");
    if ((call_stack || field_stack || show_certificate) && !single)
      throw std::invalid_argument(
          "explicit stacks and --certificate require --query");
    if (show_certificate && mode != "joint")
      throw std::invalid_argument("--certificate requires --mode joint");
    if ((call_stack || field_stack) &&
        (options.parentheses == spds::StackAcceptance::Any ||
         options.brackets == spds::StackAcceptance::Any))
      throw std::invalid_argument(
          "do not combine explicit stacks with prefix flags");
    const unsigned obs_modes =
        (observer_path.empty() ? 0U : 1U) + (identity ? 1U : 0U) +
        (feature_options ? 1U : 0U) + (label_parity_groups > 0 ? 1U : 0U) +
        (single_sided_groups > 0 ? 1U : 0U) + (best_observer ? 1U : 0U);
    if (obs_modes > 1U)
      throw std::invalid_argument(
          "choose automatic feature budgets, --identity, --label-parity, "
          "--single-sided-parity, --best-observer, or --observer, not a mixture");
    if (corridor_rehash && single_sided_groups == 0 && !best_observer)
      throw std::invalid_argument(
          "--corridor-rehash requires --single-sided-parity");
    if (best_observer) {
      if (corridor_rehash)
        throw std::invalid_argument(
            "do not combine --best-observer with --corridor-rehash");
      const affine::SingleSidedParityOptions best;
      single_sided_groups = best.groups;
      corridor_rehash = best.corridor_active_rehash;
    }
    if (single_sided_groups > 0 &&
        (call_stack || field_stack || show_certificate || !dump_path.empty()))
      throw std::invalid_argument(
          "--single-sided-parity/--best-observer do not support explicit stacks, "
          "--certificate, or --dump-observer");
    if (!queries_path.empty() &&
        (options.parentheses != spds::StackAcceptance::Empty ||
         options.brackets != spds::StackAcceptance::Empty) &&
        demand_direction == spds::DemandDirection::Pre)
      throw std::invalid_argument("prefix flags require post direction");
    auto graph = dyck::Graph::parseDotFile(path);
    for (auto v : vertices)
      graph.addVertex(v);
    if ((source && !graph.containsVertex(*source)) ||
        (target && !graph.containsVertex(*target)))
      throw std::invalid_argument(
          "query vertex is not in graph; use --vertex for isolated vertices");
    const auto observer_started = std::chrono::steady_clock::now();
    options.observer.max_matrix_dimension = options.max_matrix_dimension;
    affine::HistoryObserver observer;
    if (!observer_path.empty()) {
      std::ifstream input(observer_path);
      if (!input)
        throw std::invalid_argument("cannot open observer file");
      observer = affine::HistoryObserver::read(input);
    } else if (label_parity_groups > 0) {
      observer = affine::HistoryObserver::labelParity(graph, label_parity_groups);
    } else if (!identity && single_sided_groups == 0) {
      try {
        observer = affine::HistoryObserver::automatic(graph, options.observer);
      } catch (const std::length_error &error) {
        throw spds::ResourceLimit(error.what());
      }
    }
    const auto observer_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - observer_started)
            .count());
    const bool reverse =
        (target && !source) ||
        (single && demand_direction == spds::DemandDirection::Pre);
    if (reverse && (options.parentheses != spds::StackAcceptance::Empty ||
                    options.brackets != spds::StackAcceptance::Empty))
      throw std::invalid_argument("prefix flags require a forward query");
    affine::Solver solver(options);
    auto analysis = solver.prepare(graph, observer);
    std::ostringstream out;
    if (json)
      out << "{\"engine\":\"affine-spds\",\"semantics\":\"sound-upper-bound\","
             "\"mode\":\""
          << mode << "\",\"scope\":\""
          << (all_pairs               ? "all-pairs"
              : !queries_path.empty() ? "batch"
              : single                ? "pair"
              : source                ? "source"
                                      : "target")
          << "\",\"direction\":\""
          << (!queries_path.empty() &&
                      demand_direction == spds::DemandDirection::Auto
                  ? "auto"
              : reverse ? "pre*"
                        : "post*")
          << "\",";
    else
      out << "engine: affine-spds\nsemantics: sound upper bound\nmode: " << mode
          << "\nscope: "
          << (all_pairs               ? "all-pairs"
              : !queries_path.empty() ? "batch"
              : single                ? "pair"
              : source                ? "source"
                                      : "target")
          << "\ndirection: "
          << (!queries_path.empty() &&
                      demand_direction == spds::DemandDirection::Auto
                  ? "auto"
              : reverse ? "pre*"
                        : "post*")
          << '\n';
    dyck::PairSet candidates;
    if (single_sided_groups > 0) {
      affine::SingleSidedParityOptions ss_opts;
      ss_opts.groups = single_sided_groups;
      ss_opts.corridor_active_rehash = corridor_rehash;
      ss_opts.mode = comparison_mode;
      if (!queries_path.empty()) {
        auto demands = readDemands(queries_path);
        auto result = solver.analyzeSingleSidedParityDemands(
            graph, demands, ss_opts, demand_direction);
        candidates = std::move(result.pairs);
        if (json)
          out << "\"requested_pairs\":" << demands.size() << ',';
        else
          out << "requested-pairs: " << demands.size() << '\n';
        statistics(out, result.statistics, json, timings, observer_us);
      } else if (source || target) {
        if (single) {
          const auto direction = reverse ? spds::DemandDirection::Pre
                                         : spds::DemandDirection::Post;
          auto result = solver.analyzeSingleSidedParityDemands(
              graph, {{*source, *target}}, ss_opts, direction);
          const bool selected = result.pairs.count({*source, *target}) != 0;
          if (selected)
            candidates.insert({*source, *target});
          if (json) {
            out << "\"source\":" << *source << ",\"target\":" << *target
                << ",\"may_reach\":" << boolean(selected) << ',';
          } else {
            out << "query: " << answer(selected) << '\n';
          }
          statistics(out, result.statistics, json, timings, observer_us);
        } else {
          auto result = reverse ? solver.analyzeSingleSidedParityTo(
                                      graph, *target, ss_opts)
                                : solver.analyzeSingleSidedParityFrom(
                                      graph, *source, ss_opts);
          candidates = std::move(result.pairs);
          statistics(out, result.statistics, json, timings, observer_us);
        }
      } else {
        auto result = solver.analyzeSingleSidedParityAll(graph, ss_opts);
        candidates = std::move(result.pairs);
        statistics(out, result.statistics, json, timings, observer_us);
      }
    } else if (!queries_path.empty()) {
      auto demands = readDemands(queries_path);
      auto direction = demand_direction;
      if (direction == spds::DemandDirection::Auto &&
          (options.parentheses != spds::StackAcceptance::Empty ||
           options.brackets != spds::StackAcceptance::Empty))
        direction = spds::DemandDirection::Post;
      auto result =
          analysis.analyzeDemands(demands, comparison_mode, direction);
      candidates = std::move(result.pairs);
      if (json)
        out << "\"requested_pairs\":" << demands.size() << ',';
      else
        out << "requested-pairs: " << demands.size() << '\n';
      statistics(out, result.statistics, json, timings, observer_us);
    } else if (source || target) {
      if (single) {
        const auto direction =
            reverse ? spds::DemandDirection::Pre : spds::DemandDirection::Post;
        auto result =
            (call_stack || field_stack)
                ? analysis.queryPairStacks(
                      *source, *target,
                      call_stack.value_or(std::vector<unsigned>{}),
                      field_stack.value_or(std::vector<unsigned>{}), direction,
                      comparison_mode)
                : analysis.queryPair(*source, *target, direction,
                                     comparison_mode);
        const auto vertex = reverse ? *source : *target;
        auto comparison =
            (call_stack || field_stack)
                ? result.compareStacks(
                      vertex, call_stack.value_or(std::vector<unsigned>{}),
                      field_stack.value_or(std::vector<unsigned>{}))
                : result.compare(vertex);
        const bool selected =
            comparison_mode == affine::ComparisonMode::Joint
                ? comparison.mayReach()
            : comparison_mode == affine::ComparisonMode::Independent
                ? comparison.independentMayReach(observer)
                : comparison.spdsMayReach();
        const char *selected_reason =
            comparison_mode == affine::ComparisonMode::Joint
                ? reason(comparison.verdict())
            : comparison_mode == affine::ComparisonMode::Independent
                ? (selected ? "overlapping-independent-blocks"
                            : "independent-block-separated")
                : (selected ? "projection-accepted" : "projection-rejected");
        if (selected)
          candidates.insert({*source, *target});
        if (json) {
          out << "\"source\":" << *source << ",\"target\":" << *target
              << ",\"may_reach\":" << boolean(selected) << ",\"reason\":\""
              << selected_reason
              << "\",\"call_rank\":" << comparison.callHistory().rank()
              << ",\"field_rank\":" << comparison.fieldHistory().rank();
          if (show_certificate)
            certificate(out, comparison, true);
          out << ',';
        } else {
          out << "query: " << answer(selected)
              << "\nreason: " << selected_reason
              << "\ncall-rank: " << comparison.callHistory().rank()
              << "\nfield-rank: " << comparison.fieldHistory().rank() << '\n';
          if (show_certificate)
            certificate(out, comparison, false);
        }
        statistics(out, result.statistics(), json, timings, observer_us);
      } else {
        auto result = reverse ? analysis.analyzeTo(*target, comparison_mode)
                              : analysis.analyzeFrom(*source, comparison_mode);
        candidates = std::move(result.pairs);
        statistics(out, result.statistics, json, timings, observer_us);
      }
    } else {
      auto result = analysis.analyzeAll(comparison_mode);
      candidates = result.pairs;
      statistics(out, result.statistics, json, timings, observer_us);
    }
    if (json) {
      out << ",\"candidate_pairs\":" << candidates.size();
      if (pairs) {
        out << ",\"pairs\":[";
        bool first = true;
        for (const auto &p : ordered(candidates)) {
          if (!first)
            out << ',';
          first = false;
          out << '[' << p.source << ',' << p.target << ']';
        }
        out << ']';
      }
      out << "}\n";
    } else {
      out << "candidate-pairs: " << candidates.size() << '\n';
      if (pairs)
        for (const auto &p : ordered(candidates))
          out << "pair: " << p.source << ' ' << p.target << '\n';
    }
    if (!dump_path.empty()) {
      for (const auto &input : {path, observer_path})
        if (!input.empty() && std::filesystem::exists(dump_path) &&
            std::filesystem::equivalent(input, dump_path))
          throw std::invalid_argument(
              "observer output must not overwrite an input file");
      std::ofstream file(dump_path);
      if (!file)
        throw std::runtime_error("cannot create observer output");
      observer.write(file);
      file.close();
      if (!file)
        throw std::runtime_error("failed to close observer output");
    }
    // Commit output only after ALL requested saturation/readout/I/O succeeds.
    std::cout << out.str();
    return 0;
  } catch (const spds::ResourceLimit &e) {
    std::cerr << "resource limit: " << e.what() << '\n';
    return 3;
  } catch (const std::bad_alloc &) {
    std::cerr << "resource limit: allocation failed\n";
    return 3;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 2;
  }
}
