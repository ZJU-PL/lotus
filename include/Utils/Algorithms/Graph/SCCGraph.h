#pragma once

#include "Utils/ADT/TarjanScc.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lotus {
namespace graph {
using Vertex = uint32_t;
using Adjacency = std::vector<std::vector<Vertex>>;

/// Immutable SCC condensation of a dense-ID directed graph. Component IDs are
/// sinks first: every inter-component edge goes to a smaller ID. Includes
/// isolated vertices; construction uses Lotus's iterative Tarjan
/// implementation.
class SCCGraph {
public:
  explicit SCCGraph(const Adjacency &graph) {
    if (graph.size() > std::numeric_limits<Vertex>::max() - 2)
      throw std::length_error("Too many graph vertices");
    for (const auto &row : graph)
      for (auto target : row)
        if (target >= graph.size())
          throw std::out_of_range("Invalid graph edge");
    auto successors = [&graph](Vertex node) -> const std::vector<Vertex> & {
      return graph[node];
    };
    std::vector<Vertex> order;
    auto count = FindStronglyConnectedComponents<Vertex>(
        static_cast<Vertex>(graph.size()), successors, m_component, order);
    m_members.resize(count);
    m_successors.resize(count);
    m_predecessors.resize(count);
    m_cyclic.assign(count, false);
    for (Vertex v = 0; v < graph.size(); ++v) {
      auto component = m_component[v];
      m_members[component].push_back(v);
      for (auto target : graph[v]) {
        auto next = m_component[target];
        if (next != component)
          m_successors[component].push_back(next);
        else if (target == v)
          m_cyclic[component] = true;
      }
    }
    for (Vertex c = 0; c < count; ++c) {
      auto &row = m_successors[c];
      std::sort(row.begin(), row.end());
      row.erase(std::unique(row.begin(), row.end()), row.end());
      for (auto next : row)
        m_predecessors[next].push_back(c);
      m_cyclic[c] = m_cyclic[c] || m_members[c].size() > 1;
    }
  }
  size_t numVertices() const { return m_component.size(); }
  size_t size() const { return m_members.size(); }
  Vertex componentOf(Vertex vertex) const { return m_component.at(vertex); }
  const std::vector<Vertex> &members(Vertex component) const {
    return m_members.at(component);
  }
  const std::vector<Vertex> &successors(Vertex component) const {
    return m_successors.at(component);
  }
  const std::vector<Vertex> &predecessors(Vertex component) const {
    return m_predecessors.at(component);
  }
  bool cyclic(Vertex component) const { return m_cyclic.at(component); }

private:
  std::vector<Vertex> m_component;
  Adjacency m_members, m_successors, m_predecessors;
  std::vector<bool> m_cyclic;
};
} // namespace graph
} // namespace lotus
