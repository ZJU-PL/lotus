#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Hybrid/Solver.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace hybrid = dyck::hybrid;

namespace {

template <typename Int>
Int parseInteger(std::string_view text, std::string_view opt) {
  Int value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer for " + std::string(opt) +
                                ": " + std::string(text));
  return value;
}

std::vector<dyck::Pair> readQueryFile(const std::string &path) {
  std::ifstream file;
  std::istream *in = &std::cin;
  if (path != "-") {
    file.open(path);
    if (!file)
      throw std::invalid_argument("cannot open queries file: " + path);
    in = &file;
  }
  std::vector<dyck::Pair> pairs;
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(*in, line)) {
    ++lineno;
    const auto hash = line.find('#');
    if (hash != std::string::npos)
      line.resize(hash);
    std::istringstream row(line);
    dyck::Pair p;
    std::string extra;
    if (!(row >> p.source)) {
      row.clear();
      row >> std::ws;
      if (row.eof())
        continue;
      throw std::invalid_argument("invalid query on line " +
                                  std::to_string(lineno));
    }
    if (!(row >> p.target) || (row >> extra))
      throw std::invalid_argument("invalid query on line " +
                                  std::to_string(lineno));
    pairs.push_back(p);
  }
  return pairs;
}

std::vector<dyck::Pair> sortedPairs(const dyck::PairSet &set) {
  std::vector<dyck::Pair> out(set.begin(), set.end());
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source, a.target) < std::tie(b.source, b.target);
  });
  return out;
}

void usage(std::ostream &out) {
  out << "Usage: lotus-cfl-interleaved-dyck hybrid [options] <graph.dot>\n\n"
         "Dual-Core Sandwich demand synthesis pipeline:\n"
         "  Core 1: DyckReachability Boolean SPDS U1 (Stage 1) + Union-Dyck LB L1 (Stage 2a)\n"
         "  Core 2: ParitySemiring AffineSPDS with best observer (G=32, active-rehash) on U1 \\ L1\n\n"
         "Query scope (at least one required):\n"
         "  --all-pairs                 analyze all non-reflexive pairs\n"
         "  --query S T                 demand pair (S, T) (repeatable)\n"
         "  --queries FILE              demand 'S T' pairs from FILE ('-' stdin)\n"
         "  --source V                  demand all pairs from V (repeatable)\n"
         "  --target V                  demand all pairs to V (repeatable)\n\n"
         "Graph & pipeline options:\n"
         "  --bidirect                  add complement reverse edges to input graph\n"
         "  --parity-groups N           blocks per single-sided projection (default 32 -> 64-dim)\n"
         "  --no-corridor-rehash        use fixed (id mod N) instead of active rehash\n"
         "  --feedback-rounds N         dead-edge feedback rounds after Stage 3 (default 1)\n"
         "  --no-lower                  disable Stage 2a Union-Dyck lower bound\n"
         "  --no-affine                 disable Stage 3 single-sided AffineSPDS\n"
         "  --affine-max-edges N        max edges for Stage 3 Affine (default 25000, 0=unlimited)\n"
         "  --pairs, --print-result     print final upper-bound pairs\n"
         "  --print-lower               print certified lower-bound pairs\n"
         "  --json                      emit machine-readable JSON\n"
         "  --timings                   print stage timings\n"
         "  -h, --help                  show this help\n";
}

} // namespace

int runInterleavedDyckHybrid(int argc, char **argv) {
  try {
    hybrid::Options options;
    hybrid::Demands demands;
    bool all_pairs = false, bidirect = false, print_pairs = false,
         print_lower = false, json = false, timings = false, positional = false;
    std::string input;

    auto nextArg = [&](int &i, std::string_view opt) -> std::string_view {
      if (++i >= argc)
        throw std::invalid_argument("missing value for " + std::string(opt));
      return argv[i];
    };

    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (!positional && arg == "--") {
        positional = true;
        continue;
      }
      if (!positional && (arg == "-h" || arg == "--help")) {
        usage(std::cout);
        return 0;
      }
      if (!positional && arg == "--all-pairs") {
        all_pairs = true;
      } else if (!positional && arg == "--query") {
        const auto s = parseInteger<dyck::Vertex>(nextArg(i, arg), arg);
        const auto t = parseInteger<dyck::Vertex>(nextArg(i, arg), arg);
        demands.pairs.push_back({s, t});
      } else if (!positional && arg == "--queries") {
        auto loaded = readQueryFile(std::string(nextArg(i, arg)));
        demands.pairs.insert(demands.pairs.end(), loaded.begin(), loaded.end());
      } else if (!positional && arg == "--source") {
        demands.sources.push_back(
            parseInteger<dyck::Vertex>(nextArg(i, arg), arg));
      } else if (!positional && arg == "--target") {
        demands.targets.push_back(
            parseInteger<dyck::Vertex>(nextArg(i, arg), arg));
      } else if (!positional && arg == "--bidirect") {
        bidirect = true;
      } else if (!positional && arg == "--parity-groups") {
        options.parity_groups = parseInteger<unsigned>(nextArg(i, arg), arg);
      } else if (!positional && arg == "--no-corridor-rehash") {
        options.corridor_active_rehash = false;
      } else if (!positional && arg == "--feedback-rounds") {
        options.affine_feedback_rounds =
            parseInteger<unsigned>(nextArg(i, arg), arg);
      } else if (!positional && arg == "--no-lower") {
        options.enable_lower = false;
      } else if (!positional && arg == "--no-affine") {
        options.enable_affine = false;
      } else if (!positional && arg == "--affine-max-edges") {
        options.affine_max_edges =
            parseInteger<std::size_t>(nextArg(i, arg), arg);
      } else if (!positional && (arg == "--pairs" || arg == "--print-result")) {
        print_pairs = true;
      } else if (!positional && arg == "--print-lower") {
        print_lower = true;
      } else if (!positional && arg == "--json") {
        json = true;
      } else if (!positional && arg == "--timings") {
        timings = true;
      } else if (!positional && !arg.empty() && arg.front() == '-') {
        throw std::invalid_argument("unknown option: " + std::string(arg));
      } else {
        if (!input.empty())
          throw std::invalid_argument("multiple graph files provided");
        input = std::string(arg);
      }
    }

    if (input.empty())
      throw std::invalid_argument("missing <graph.dot>");
    if (all_pairs && !demands.empty())
      throw std::invalid_argument("cannot combine --all-pairs with demand flags");
    if (!all_pairs && demands.empty())
      throw std::invalid_argument(
          "missing query scope; pass --all-pairs, --query, --queries, "
          "--source, or --target");

    auto graph = dyck::Graph::parseDotFile(input);
    if (bidirect)
      graph.makeBidirected();
    const hybrid::Solver solver;
    const auto res = all_pairs ? solver.analyze(graph, options)
                               : solver.analyzeDemands(graph, demands, options);
    const auto &st = res.statistics;

    if (json) {
      std::cout << "{\"engine\":\"hybrid\""
                << ",\"matrix_dimension\":" << st.matrix_dimension
                << ",\"max_affine_rank\":" << st.max_affine_rank
                << ",\"requested_pairs\":" << demands.pairs.size()
                << ",\"stage1_spds\":" << res.stage1_spds.size()
                << ",\"stage2_lower\":" << st.stage2_lower_count
                << ",\"certified_lower\":" << res.certified_lower.size()
                << ",\"stage3_affine\":" << res.stage3_affine.size()
                << ",\"candidate_pairs\":" << res.final_upper.size()
                << ",\"unresolved_pairs\":" << res.unresolvedCount()
                << ",\"affine_executed\":"
                << (st.affine_executed ? "true" : "false")
                << ",\"short_circuited_after\":" << st.short_circuited_after
                << ",\"timings_us\":{\"slice\":" << st.timings.slice_us
                << ",\"stage1_spds\":" << st.timings.stage1_spds_us
                << ",\"stage2_lower\":" << st.timings.stage2_lower_us
                << ",\"stage3_affine\":" << st.timings.stage3_affine_us
                << ",\"total\":" << st.timings.total_us << "}";
      if (print_pairs) {
        std::cout << ",\"pairs\":[";
        bool first = true;
        for (const auto &p : sortedPairs(res.final_upper)) {
          if (!first)
            std::cout << ',';
          first = false;
          std::cout << '[' << p.source << ',' << p.target << ']';
        }
        std::cout << ']';
      }
      if (print_lower) {
        std::cout << ",\"lower_pairs\":[";
        bool first = true;
        for (const auto &p : sortedPairs(res.certified_lower)) {
          if (!first)
            std::cout << ',';
          first = false;
          std::cout << '[' << p.source << ',' << p.target << ']';
        }
        std::cout << ']';
      }
      std::cout << "}\n";
    } else {
      std::cout << "engine: hybrid\n"
                << "matrix-dimension: " << st.matrix_dimension << '\n'
                << "slice-vertices: " << st.slice_vertices << '\n'
                << "slice-edges: " << st.slice_edges << '\n'
                << "stage1-spds: " << res.stage1_spds.size() << '\n'
                << "certified-lower: " << res.certified_lower.size() << '\n'
                << "stage3-affine: " << res.stage3_affine.size()
                << " (executed=" << (st.affine_executed ? "yes" : "no")
                << ", max-rank=" << st.max_affine_rank << ")\n"
                << "candidate-pairs: " << res.final_upper.size() << '\n'
                << "unresolved-pairs: " << res.unresolvedCount() << '\n'
                << "total-us: " << st.timings.total_us << '\n';
      if (timings) {
        std::cout << "slice-us: " << st.timings.slice_us
                  << "\nstage1-spds-us: " << st.timings.stage1_spds_us
                  << "\nstage2-lower-us: " << st.timings.stage2_lower_us
                  << "\nstage3-affine-us: " << st.timings.stage3_affine_us
                  << '\n';
      }
      if (print_lower) {
        for (const auto &p : sortedPairs(res.certified_lower))
          std::cout << "lower: " << p.source << ' ' << p.target << '\n';
      }
      if (print_pairs) {
        for (const auto &p : sortedPairs(res.final_upper))
          std::cout << "pair: " << p.source << ' ' << p.target << '\n';
      }
    }
    return 0;
  } catch (const dyck::spds::ResourceLimit &e) {
    std::cerr << "resource-limit: " << e.what() << '\n';
    return 3;
  } catch (const std::bad_alloc &) {
    std::cerr << "resource-limit: allocation failed\n";
    return 3;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 2;
  }
}
