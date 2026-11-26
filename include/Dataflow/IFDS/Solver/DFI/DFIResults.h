// Adapted from PhASAR's DFISolverResults (88366fc69).
// Copyright (c) 2026 Fabian Schiebel and others. MIT; see
// third-party/phasar/LICENSE.txt.
#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Solver/DFI/DFIConfig.h"
#include "Utils/Algorithms/Graph/IntervalReachability.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ifds {
namespace dfi {
inline uint64_t node_key(uint32_t instruction, uint32_t fact) {
  return (uint64_t(instruction) << 32) | fact;
}
template <typename Fact> struct ResultsData {
  struct Vertex {
    uint32_t instruction, fact;
  };
  std::vector<const llvm::Instruction *> instructions;
  std::unordered_map<const llvm::Instruction *, uint32_t> instruction_ids;
  std::vector<Fact> facts;
  std::unordered_map<Fact, uint32_t> fact_ids;
  std::vector<Vertex> nodes;
  std::unordered_map<uint64_t, uint32_t> vertex_of;
  std::unordered_map<const llvm::Instruction *, std::vector<uint32_t>> at;
  std::unordered_map<const llvm::Instruction *, std::set<Fact>> exits;
  std::vector<SummaryEdge<Fact>> summaries;
  std::shared_ptr<const lotus::graph::IntervalReachabilityIndex> index;
  DFIIndexDirection direction = DFIIndexDirection::Forward;
};
} // namespace dfi

/// Read-only owning snapshot. The module/context must outlive this view, and
/// must not be mutated while its results are queried. Re-solving the solver
/// does not invalidate an already acquired snapshot over unchanged IR.
template <typename Fact> class DFIResults {
public:
  using Node = typename ExplodedSupergraph<Fact>::Node;
  using FactSet = std::set<Fact>;
  explicit DFIResults(std::shared_ptr<const dfi::ResultsData<Fact>> data)
      : m_data(std::move(data)) {
    if (!m_data)
      throw std::invalid_argument("Missing DFI results");
  }
  FactSet get_facts_at_entry(const llvm::Instruction *inst) const {
    FactSet result;
    auto it = m_data->at.find(inst);
    if (it != m_data->at.end())
      for (auto v : it->second)
        result.insert(m_data->facts[m_data->nodes[v].fact]);
    return result;
  }
  FactSet get_facts_at_exit(const llvm::Instruction *inst) const {
    auto it = m_data->exits.find(inst);
    return it == m_data->exits.end() ? FactSet{} : it->second;
  }
  FactSet get_facts_at_in_llvm_ssa(const llvm::Instruction *inst) const {
    if (!inst)
      return {};
    if (!inst->getType()->isVoidTy()) {
      if (auto *next = inst->getNextNode())
        return get_facts_at_entry(next);
      if (auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(inst))
        return get_facts_at_entry(&invoke->getNormalDest()->front());
    }
    return get_facts_at_exit(inst);
  }
  std::optional<uint32_t> find(const llvm::Instruction *inst,
                               const Fact &fact) const {
    auto instruction_id = m_data->instruction_ids.find(inst);
    auto fact_id = m_data->fact_ids.find(fact);
    if (instruction_id == m_data->instruction_ids.end() ||
        fact_id == m_data->fact_ids.end())
      return std::nullopt;
    auto it = m_data->vertex_of.find(
        dfi::node_key(instruction_id->second, fact_id->second));
    return it == m_data->vertex_of.end() ? std::nullopt
                                         : std::optional<uint32_t>(it->second);
  }
  size_t num_nodes() const { return m_data->nodes.size(); }
  template <typename Callback> void for_each_result(Callback callback) const {
    for (const auto &node : m_data->nodes)
      callback(m_data->instructions[node.instruction],
               m_data->facts[node.fact]);
  }
  void get_summary_edges(std::vector<SummaryEdge<Fact>> &out) const {
    out = m_data->summaries;
  }

private:
  std::shared_ptr<const dfi::ResultsData<Fact>> m_data;
};

/// Reachability over the materialized DFI graph, not branch feasibility. Paths
/// may descend into callees, but do not return above their starting function;
/// explicit unbalanced-return edges are included when that option was enabled.
/// IDs belong to this snapshot. Nodes not reached during solving are absent.
template <typename Fact> class DFIReachability {
public:
  using VertexId = uint32_t;
  explicit DFIReachability(std::shared_ptr<const dfi::ResultsData<Fact>> data)
      : m_data(std::move(data)) {
    if (!m_data || !m_data->index)
      throw std::logic_error("DFI query index was not built");
  }
  std::optional<VertexId> find(const llvm::Instruction *inst,
                               const Fact &fact) const {
    return DFIResults<Fact>(m_data).find(inst, fact);
  }
  bool reaches(VertexId from, VertexId to) const {
    return m_data->direction == DFIIndexDirection::Forward
               ? m_data->index->reaches(from, to)
               : m_data->index->reaches(to, from);
  }
  bool reaches(const llvm::Instruction *from, const Fact &from_fact,
               const llvm::Instruction *to, const Fact &to_fact) const {
    auto a = find(from, from_fact), b = find(to, to_fact);
    return a && b && reaches(*a, *b);
  }
  DFIIndexDirection index_direction() const { return m_data->direction; }
  template <typename Callback>
  void for_each_descendant(VertexId from, Callback callback) const {
    enumerate(from, DFIIndexDirection::Forward, callback);
  }
  template <typename Callback>
  void for_each_ancestor(VertexId to, Callback callback) const {
    enumerate(to, DFIIndexDirection::Backward, callback);
  }
  template <typename Callback>
  void for_each_descendant_fact_at(VertexId from, const llvm::Instruction *at,
                                   Callback callback) const {
    for_each_fact_at(at, [&](VertexId to) {
      if (reaches(from, to))
        callback(m_data->facts[m_data->nodes[to].fact]);
    });
  }
  template <typename Callback>
  void for_each_ancestor_fact_at(VertexId to, const llvm::Instruction *at,
                                 Callback callback) const {
    for_each_fact_at(at, [&](VertexId from) {
      if (reaches(from, to))
        callback(m_data->facts[m_data->nodes[from].fact]);
    });
  }

private:
  template <typename Callback>
  void for_each_fact_at(const llvm::Instruction *at, Callback callback) const {
    auto found = m_data->at.find(at);
    if (found != m_data->at.end())
      for (auto v : found->second)
        callback(v);
  }
  template <typename Callback>
  void enumerate(VertexId v, DFIIndexDirection direction,
                 Callback callback) const {
    if (v >= m_data->nodes.size())
      return;
    auto report = [&](VertexId other) {
      const auto &node = m_data->nodes[other];
      callback(m_data->instructions[node.instruction],
               m_data->facts[node.fact]);
    };
    if (direction == m_data->direction) {
      m_data->index->forEachReachableTarget(v, report);
    } else {
      for (VertexId other = 0; other < m_data->nodes.size(); ++other)
        if (m_data->index->reaches(other, v))
          report(other);
    }
  }
  std::shared_ptr<const dfi::ResultsData<Fact>> m_data;
};
} // namespace ifds
