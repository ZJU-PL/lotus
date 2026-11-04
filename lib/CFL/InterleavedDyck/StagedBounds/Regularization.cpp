#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace lotus::cfl::interleaved_dyck::staged_bounds::detail {

Graph automatonProduct(const Graph &graph, BenchmarkKind benchmark,
                       std::size_t &state_count, std::size_t &accept_state) {
  Graph product;
  if (benchmark == BenchmarkKind::ValueFlow) {
    state_count = 6;
    accept_state = 2;
    for (const Edge &edge : graph.edges()) {
      const auto add = [&](std::size_t from, std::size_t to, Label label) {
        product.addEdge(productVertex(edge.source, state_count, from),
                        productVertex(edge.target, state_count, to), label);
      };
      if (edge.label.kind == LabelKind::OpenBracket) {
        add(0, 3, Label::neutral());
        add(1, 3, Label::neutral());
        add(2, 3, Label::neutral());
        add(3, 4, Label::neutral());
        add(4, 4, Label::neutral());
        add(5, 4, Label::neutral());
      } else if (edge.label.kind == LabelKind::CloseBracket) {
        add(3, 2, Label::neutral());
        add(4, 5, Label::neutral());
        add(5, 5, Label::neutral());
      } else {
        add(1, 1, edge.label);
        add(2, 1, edge.label);
        add(3, 3, edge.label);
        add(4, 4, edge.label);
        add(5, 4, edge.label);
      }
    }
    return product;
  }

  const auto brackets = labelIds(graph, Alphabet::Bracket);
  state_count = brackets.size() + 2;
  accept_state = 0;
  std::unordered_map<unsigned, std::size_t> bracket_state;
  for (std::size_t index = 0; index < brackets.size(); ++index) {
    bracket_state[brackets[index]] = index + 1;
  }
  // Only q0 and the sink are accepting. A typed state qi can be entered
  // only through ob_i from q0, and can leave only through cb_i (to q0) or
  // another opening bracket (to the sink). Trim each qi layer before building
  // it: retain vertices reachable from its entries and able to reach an exit
  // through non-bracket edges. This ignores parenthesis balance, so it is a
  // conservative graph trim that preserves every accepted Dyck witness.
  const auto &vertices = graph.vertices();
  const auto &edges = graph.edges();
  std::unordered_map<Vertex, std::size_t> dense;
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    dense.emplace(vertices[i], i);
  }
  std::vector<std::pair<std::size_t, std::size_t>> endpoints;
  endpoints.reserve(edges.size());
  std::vector<std::vector<std::size_t>> outgoing(vertices.size());
  std::vector<std::vector<std::size_t>> reverse_plain(vertices.size());
  std::vector<std::vector<std::size_t>> openings(brackets.size());
  std::vector<std::vector<std::size_t>> closings(brackets.size());
  std::vector<bool> has_opening(vertices.size(), false);
  const auto add = [&](std::size_t edge_index, std::size_t from, std::size_t to,
                       Label label) {
    const Edge &edge = edges[edge_index];
    product.addEdge(productVertex(edge.source, state_count, from),
                    productVertex(edge.target, state_count, to), label);
  };
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const Edge &edge = edges[index];
    const auto source = dense.at(edge.source);
    const auto target = dense.at(edge.target);
    endpoints.emplace_back(source, target);
    outgoing[source].push_back(index);
    if (edge.label.kind == LabelKind::OpenBracket) {
      openings[bracket_state.at(edge.label.id) - 1].push_back(index);
      has_opening[source] = true;
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      closings[bracket_state.at(edge.label.id) - 1].push_back(index);
    } else {
      reverse_plain[target].push_back(source);
      // Every q0 vertex is a possible query source and accepting endpoint.
      add(index, 0, 0, edge.label);
    }
  }

  // Reuse O(|V|) scratch storage across types; do not allocate |V| x |states|.
  std::vector<std::size_t> forward(vertices.size(), 0);
  std::vector<std::size_t> viable(vertices.size(), 0);
  std::vector<std::size_t> forward_queue, viable_queue, sink_queue;
  std::vector<bool> sink_reached(vertices.size(), false);
  const std::size_t sink = state_count - 1;
  for (std::size_t type = 0; type < brackets.size(); ++type) {
    const std::size_t state = type + 1; // Also a nonzero traversal stamp.
    forward_queue.clear();
    viable_queue.clear();
    const auto reach = [&](std::size_t vertex) {
      if (forward[vertex] != state) {
        forward[vertex] = state;
        forward_queue.push_back(vertex);
      }
    };
    for (std::size_t index : openings[type]) {
      reach(endpoints[index].second);
    }
    for (std::size_t head = 0; head < forward_queue.size(); ++head) {
      for (std::size_t index : outgoing[forward_queue[head]]) {
        if (!isBracket(edges[index].label.kind)) {
          reach(endpoints[index].second);
        }
      }
    }
    const auto keep = [&](std::size_t vertex) {
      if (forward[vertex] == state && viable[vertex] != state) {
        viable[vertex] = state;
        viable_queue.push_back(vertex);
      }
    };
    for (std::size_t vertex : forward_queue) {
      if (has_opening[vertex]) {
        keep(vertex);
      }
    }
    for (std::size_t index : closings[type]) {
      keep(endpoints[index].first);
    }
    for (std::size_t head = 0; head < viable_queue.size(); ++head) {
      for (std::size_t predecessor : reverse_plain[viable_queue[head]]) {
        keep(predecessor);
      }
    }

    for (std::size_t index : openings[type]) {
      if (viable[endpoints[index].second] == state) {
        add(index, 0, state, Label::neutral());
      }
    }
    for (std::size_t vertex : viable_queue) {
      for (std::size_t index : outgoing[vertex]) {
        const Label label = edges[index].label;
        const auto target = endpoints[index].second;
        if (label.kind == LabelKind::OpenBracket) {
          add(index, state, sink, Label::neutral());
          if (!sink_reached[target]) {
            sink_reached[target] = true;
            sink_queue.push_back(target);
          }
        } else if (label.kind == LabelKind::CloseBracket) {
          if (label.id == brackets[type]) {
            add(index, state, 0, Label::neutral());
          }
        } else if (viable[target] == state) {
          add(index, state, state, label);
        }
      }
    }
  }
  // The sink is accepting and loops on every label; only its forward-reachable
  // part matters. In particular, graphs with no nested opening brackets need
  // no sink copy at all.
  for (std::size_t head = 0; head < sink_queue.size(); ++head) {
    for (std::size_t index : outgoing[sink_queue[head]]) {
      const auto target = endpoints[index].second;
      if (!sink_reached[target]) {
        sink_reached[target] = true;
        sink_queue.push_back(target);
      }
      const Label label = edges[index].label;
      add(index, sink, sink, isBracket(label.kind) ? Label::neutral() : label);
    }
  }
  return product;
}

PairSet regularization(const Graph &graph, BenchmarkKind benchmark) {
  std::size_t states = 0;
  std::size_t accept_state = 0;
  const Graph product =
      automatonProduct(graph, benchmark, states, accept_state);
  return runClassicProjectedMapped(
      product, Alphabet::Parenthesis,
      [states, accept_state](const Pair &pair) -> std::optional<Pair> {
        const auto source_remainder = pair.source % static_cast<Vertex>(states);
        auto target_remainder = pair.target % static_cast<Vertex>(states);
        if (source_remainder < 0) {
          return std::nullopt;
        }
        if (target_remainder < 0) {
          target_remainder += static_cast<Vertex>(states);
        }
        if (source_remainder == 0 &&
            (target_remainder == static_cast<Vertex>(accept_state) ||
             target_remainder == static_cast<Vertex>(states - 1))) {
          const Pair mapped{pair.source / static_cast<Vertex>(states),
                            pair.target / static_cast<Vertex>(states)};
          if (mapped.source != mapped.target) {
            return mapped;
          }
        }
        return std::nullopt;
      });
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::detail
