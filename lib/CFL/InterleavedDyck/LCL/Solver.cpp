#include "CFL/InterleavedDyck/LCL/Solver.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::lcl {
namespace {

// q1, q2, (i, [i, q1(i, and q1[i in Table 2, respectively. A Seek state
// searches for a matching opener; a Deferred state has crossed the other
// alphabet during that search. Q and the tape symbol Z are independent.
enum class ControlKind {
  Scan,
  Accept,
  SeekParenthesis,
  SeekBracket,
  DeferredParenthesis,
  DeferredBracket,
};

struct Control {
  ControlKind kind = ControlKind::Scan;
  unsigned id = 0;

  bool operator==(const Control &other) const {
    return kind == other.kind && id == other.id;
  }
};

// Only Neutral (#), OpenParenthesis, and OpenBracket occur as tape symbols.
using Tape = Label;

struct Summary {
  Control q;
  Tape z;

  bool operator==(const Summary &other) const {
    return q == other.q && z == other.z;
  }
};

std::size_t combineHash(std::size_t seed, std::size_t value) {
  return seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
}

struct ControlHash {
  std::size_t operator()(const Control &q) const {
    return combineHash(std::hash<unsigned>{}(static_cast<unsigned>(q.kind)),
                       std::hash<unsigned>{}(q.id));
  }
};

struct TapeHash {
  std::size_t operator()(const Tape &z) const {
    return combineHash(std::hash<unsigned>{}(static_cast<unsigned>(z.kind)),
                       std::hash<unsigned>{}(z.id));
  }
};

struct SummaryHash {
  std::size_t operator()(const Summary &summary) const {
    return combineHash(ControlHash{}(summary.q), TapeHash{}(summary.z));
  }
};

bool isSweepEnd(Control q) {
  return q.kind == ControlKind::Scan || q.kind == ControlKind::Accept;
}

// I-rules (1)-(4). Neutral labels are epsilon, NOT the tape marker #.
Summary initialSummary(Label label) {
  switch (label.kind) {
  case LabelKind::OpenParenthesis: // (1)
  case LabelKind::OpenBracket:     // (3)
    return {{ControlKind::Scan, 0}, label};
  case LabelKind::CloseParenthesis: // (2)
    return {{ControlKind::SeekParenthesis, label.id}, Label::neutral()};
  case LabelKind::CloseBracket: // (4)
    return {{ControlKind::SeekBracket, label.id}, Label::neutral()};
  case LabelKind::Neutral:
    break;
  }
  throw std::logic_error("LCL: nonterminal label reached trellis initialization");
}

// delta(Z_left, q_right): the don't-care fields of the two trellis children
// must not be unified. All thirteen transition schemes in Table 2 appear here.
std::optional<Summary> findRule(Tape z, Control q) {
  const bool blank = z.kind == LabelKind::Neutral;
  const bool paren = z.kind == LabelKind::OpenParenthesis;
  const bool bracket = z.kind == LabelKind::OpenBracket;
  switch (q.kind) {
  case ControlKind::Scan: // (5): q1 scans every work-tape symbol.
    return Summary{q, z};
  case ControlKind::Accept:
    if (blank) { // (6)
      return Summary{q, z};
    }
    return Summary{{ControlKind::Scan, 0}, z}; // (7)
  case ControlKind::SeekParenthesis:
    if (blank) { // (8)
      return Summary{q, z};
    }
    if (bracket) { // (9)
      return Summary{{ControlKind::DeferredParenthesis, q.id}, z};
    }
    if (paren && z.id == q.id) { // (10)
      return Summary{{ControlKind::Accept, 0}, Label::neutral()};
    }
    break; // A different type of parenthesis cannot be matched or skipped.
  case ControlKind::DeferredParenthesis:
    if (blank || bracket) { // (11)
      return Summary{q, z};
    }
    if (paren && z.id == q.id) { // (12)
      return Summary{{ControlKind::Scan, 0}, Label::neutral()};
    }
    break;
  case ControlKind::SeekBracket:
    if (blank) { // (13)
      return Summary{q, z};
    }
    if (paren) { // (14)
      return Summary{{ControlKind::DeferredBracket, q.id}, z};
    }
    if (bracket && z.id == q.id) { // (15)
      return Summary{{ControlKind::Accept, 0}, Label::neutral()};
    }
    break;
  case ControlKind::DeferredBracket:
    if (blank || paren) { // (16)
      return Summary{q, z};
    }
    if (bracket && z.id == q.id) { // (17)
      return Summary{{ControlKind::Scan, 0}, Label::neutral()};
    }
    break;
  }
  return std::nullopt;
}

using Mask = std::uint8_t;
constexpr Mask OPEN_P = 1U;
constexpr Mask CLOSE_P = 2U;
constexpr Mask OPEN_B = 4U;
constexpr Mask CLOSE_B = 8U;
constexpr Mask ALL = OPEN_P | CLOSE_P | OPEN_B | CLOSE_B;

Mask boundaryMask(LabelKind kind) {
  switch (kind) {
  case LabelKind::OpenParenthesis:
    return OPEN_P;
  case LabelKind::CloseParenthesis:
    return CLOSE_P;
  case LabelKind::OpenBracket:
    return OPEN_B;
  case LabelKind::CloseBracket:
    return CLOSE_B;
  case LabelKind::Neutral:
    break;
  }
  throw std::logic_error("LCL: epsilon edge reached saturation");
}

// Section 5.3, outgoing-edge feasibility: an opener requires q1; a close
// parenthesis excludes [i/q1[i; a close bracket excludes (i/q1(i.
Mask feasibleOutgoing(Control q) {
  switch (q.kind) {
  case ControlKind::Scan:
    return ALL;
  case ControlKind::Accept:
    return CLOSE_P | CLOSE_B;
  case ControlKind::SeekParenthesis:
  case ControlKind::DeferredParenthesis:
    return CLOSE_P;
  case ControlKind::SeekBracket:
  case ControlKind::DeferredBracket:
    return CLOSE_B;
  }
  return 0;
}

// Section 5.3, incoming-edge feasibility: a closer requires #; an opener
// excludes an outstanding work-tape symbol belonging to the other alphabet.
Mask feasibleIncoming(Tape z) {
  switch (z.kind) {
  case LabelKind::Neutral:
    return ALL;
  case LabelKind::OpenParenthesis:
    return OPEN_P;
  case LabelKind::OpenBracket:
    return OPEN_B;
  default:
    return 0;
  }
}

void validate(const Graph &graph, const Options &options) {
  switch (options.algorithm) {
  case Algorithm::Baseline:
  case Algorithm::Refined:
    break;
  default:
    throw std::invalid_argument("LCL: invalid algorithm");
  }
  for (const Edge &edge : graph.edges()) {
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis:
    case LabelKind::CloseParenthesis:
    case LabelKind::OpenBracket:
    case LabelKind::CloseBracket:
    case LabelKind::Neutral:
      break;
    default:
      throw std::invalid_argument("LCL: invalid interleaved-Dyck label kind");
    }
  }
}

// Exact epsilon elimination: E' = epsilon* ; E_terminal ; epsilon*.
// Keep epsilon-only pairs separately; they do not denote length-one trellis
// summaries. The fast path avoids all-pairs closure on epsilon-free inputs.
Graph normalize(const Graph &input, PairSet &epsilon_pairs,
                const Options &options) {
  const bool has_epsilon =
      std::any_of(input.edges().begin(), input.edges().end(), [](const Edge &e) {
        return e.label.kind == LabelKind::Neutral;
      });
  if (!has_epsilon) {
    for (Vertex v : input.vertices()) {
      epsilon_pairs.insert({v, v});
    }
    if (options.max_normalized_edges != 0 &&
        input.edges().size() > options.max_normalized_edges) {
      throw std::length_error("LCL: normalized-edge limit exceeded");
    }
    return input;
  }

  const auto &vertices = input.vertices();
  const std::size_t n = vertices.size();
  std::unordered_map<Vertex, std::size_t> indices;
  indices.reserve(n);
  Graph output;
  for (std::size_t i = 0; i < n; ++i) {
    indices.emplace(vertices[i], i);
    output.addVertex(vertices[i]);
  }
  std::vector<std::vector<std::size_t>> epsilon_out(n), successors(n),
      predecessors(n);
  for (const Edge &edge : input.edges()) {
    if (edge.label.kind == LabelKind::Neutral) {
      epsilon_out[indices.at(edge.source)].push_back(indices.at(edge.target));
    }
  }
  std::vector<bool> visited(n);
  for (std::size_t start = 0; start < n; ++start) {
    std::fill(visited.begin(), visited.end(), false);
    auto &reached = successors[start];
    reached.push_back(start);
    visited[start] = true;
    for (std::size_t cursor = 0; cursor < reached.size(); ++cursor) {
      const std::size_t v = reached[cursor];
      predecessors[v].push_back(start);
      epsilon_pairs.insert({vertices[start], vertices[v]});
      for (std::size_t next : epsilon_out[v]) {
        if (!visited[next]) {
          visited[next] = true;
          reached.push_back(next);
        }
      }
    }
  }
  for (const Edge &edge : input.edges()) {
    if (edge.label.kind == LabelKind::Neutral) {
      continue;
    }
    for (std::size_t from : predecessors[indices.at(edge.source)]) {
      for (std::size_t to : successors[indices.at(edge.target)]) {
        if (output.addEdge(vertices[from], vertices[to], edge.label) &&
            options.max_normalized_edges != 0 &&
            output.edges().size() > options.max_normalized_edges) {
          throw std::length_error("LCL: normalized-edge limit exceeded");
        }
      }
    }
  }
  return output;
}

struct LeftTerm {
  Mask white = 0; // all L-terms: L union L_b in Algorithm 2
  Mask gray = 0;  // non-spurious L-terms: L in Algorithm 2
};

struct Cell {
  // false: white only; true: also has a non-spurious gray derivation.
  // Gray validity belongs to a complete (q,Z), not merely to q.
  std::unordered_map<Summary, bool, SummaryHash> summaries;
  std::unordered_map<Tape, LeftTerm, TapeHash> left;
  std::unordered_map<Control, Mask, ControlHash> right;
};

struct WorkItem {
  Pair pair;
  Summary summary;
  bool gray;
};

class Saturation {
public:
  Saturation(const Graph &graph, const Options &options, Statistics &statistics)
      : graph_(graph), options_(options), statistics_(statistics),
        refined_(options.algorithm == Algorithm::Refined),
        filter_(refined_ && options.enable_feasibility) {
    for (const Edge &edge : graph_.edges()) {
      outgoing_[edge.source].push_back(&edge);
      incoming_[edge.target].push_back(&edge);
    }
  }

  void run(PairSet &upper_bound, PairSet &summary_pairs) {
    // Algorithm 1/2 initialization. Record in S immediately, not only on pop.
    for (const Edge &edge : graph_.edges()) {
      const Summary summary = initialSummary(edge.label);
      const Pair pair{edge.source, edge.target};
      addSummary(pair, cells_[pair], summary,
                 refined_ && isSweepEnd(summary.q));
    }
    while (!worklist_.empty()) {
      const WorkItem item = worklist_.front();
      worklist_.pop_front();
      ++statistics_.worklist_pops;
      const auto out = outgoing_.find(item.pair.target);
      if (out != outgoing_.end()) {
        for (const Edge *edge : out->second) {
          addLeft({item.pair.source, edge->target}, item.summary.z,
                  boundaryMask(edge->label.kind), item.gray);
        }
      }
      const auto in = incoming_.find(item.pair.source);
      if (in != incoming_.end()) {
        for (const Edge *edge : in->second) {
          addRight({edge->source, item.pair.target}, item.summary.q,
                   boundaryMask(edge->label.kind));
        }
      }
    }
    const Summary accepting{{ControlKind::Accept, 0}, Label::neutral()};
    for (const auto &entry : cells_) {
      if (!entry.second.summaries.empty()) {
        summary_pairs.insert(entry.first);
      }
      const auto found = entry.second.summaries.find(accepting);
      if (found != entry.second.summaries.end() &&
          (!refined_ || found->second)) {
        upper_bound.insert(entry.first);
      }
    }
  }

private:
  void addSummary(Pair pair, Cell &cell, Summary summary, bool gray) {
    const auto inserted = cell.summaries.emplace(summary, gray);
    if (inserted.second) {
      if (options_.max_summaries != 0 &&
          statistics_.summaries >= options_.max_summaries) {
        throw std::length_error("LCL: summary limit exceeded");
      }
      ++statistics_.summaries;
    } else {
      if (!gray || inserted.first->second) {
        return;
      }
      inserted.first->second = true;
      ++statistics_.summary_upgrades;
    }
    if (gray) {
      ++statistics_.gray_summaries;
    }
    // Re-enqueue an existing white summary when it gains a gray derivation.
    // Otherwise its dependent L_b terms would never be upgraded to L.
    worklist_.push_back({pair, summary, gray});
    statistics_.peak_worklist =
        std::max(statistics_.peak_worklist, worklist_.size());
  }

  void join(Pair pair, Cell &cell, Tape z, LeftTerm left, Control q, Mask right) {
    ++statistics_.rule_lookups;
    const auto summary = findRule(z, q);
    if (!summary) {
      return;
    }
    const Mask out_allowed = filter_ ? feasibleOutgoing(summary->q) : ALL;
    const Mask in_allowed = filter_ ? feasibleIncoming(summary->z) : ALL;
    if ((left.white & out_allowed) == 0 || (right & in_allowed) == 0) {
      return;
    }
    const bool gray = refined_ && isSweepEnd(summary->q) &&
                      (left.gray & out_allowed) != 0;
    addSummary(pair, cell, *summary, gray);
  }

  void addLeft(Pair pair, Tape z, Mask boundary, bool gray) {
    Cell &cell = cells_[pair];
    LeftTerm &left = cell.left[z];
    const Mask new_white = static_cast<Mask>(left.white | boundary);
    const Mask new_gray = gray ? static_cast<Mask>(left.gray | boundary)
                               : left.gray;
    if (new_white == left.white && new_gray == left.gray) {
      return;
    }
    left = {new_white, new_gray};
    ++statistics_.left_term_updates;
    // join() only mutates summaries and the worklist, not these term maps.
    for (const auto &right : cell.right) {
      join(pair, cell, z, left, right.first, right.second);
    }
  }

  void addRight(Pair pair, Control q, Mask boundary) {
    Cell &cell = cells_[pair];
    Mask &right = cell.right[q];
    const Mask new_right = static_cast<Mask>(right | boundary);
    if (new_right == right) {
      return;
    }
    right = new_right;
    ++statistics_.right_term_updates;
    for (const auto &left : cell.left) {
      join(pair, cell, left.first, left.second, q, right);
    }
  }

  const Graph &graph_;
  const Options &options_;
  Statistics &statistics_;
  const bool refined_;
  const bool filter_;
  std::unordered_map<Vertex, std::vector<const Edge *>> outgoing_;
  std::unordered_map<Vertex, std::vector<const Edge *>> incoming_;
  std::unordered_map<Pair, Cell, PairHash> cells_;
  std::deque<WorkItem> worklist_;
};

} // namespace

Result Solver::analyze(const Graph &graph, const Options &options) const {
  validate(graph, options);
  Result result;
  result.statistics.input_vertices = graph.vertices().size();
  result.statistics.input_edges = graph.edges().size();
  const Graph normalized = normalize(graph, result.upper_bound, options);
  result.summary_pairs = result.upper_bound; // epsilon-only fragments have summaries too.
  result.statistics.normalized_edges = normalized.edges().size();
  result.statistics.epsilon_pairs = result.upper_bound.size();
  Saturation(normalized, options, result.statistics)
      .run(result.upper_bound, result.summary_pairs);
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::lcl
