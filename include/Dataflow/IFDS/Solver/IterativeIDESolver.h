/*
 * Compact, iterative IDE solver facade.
 *
 * Lotus's IDESolver already implements the tabulation semantics and the
 * summary-edge algorithm used by all of the existing analyses.  This class
 * supplies the migration layer for clients that need the representation used
 * by PhASAR's IterativeIDESolver: instructions and facts are interned into
 * dense IDs and path edges are retained as four integers rather than as a
 * collection of pointer-rich keys.  The compact table is populated while the
 * ordinary solver runs, so result queries and all Problem callbacks remain
 * source-compatible with IDESolver.
 *
 * The ID table is deliberately optional.  It is enabled by default because
 * it is cheap to build and is useful for large analyses; call
 * release_compact_storage() when only the normal IDESolver query API is
 * needed.  Setting retain_compact_edges(false) keeps the interners and
 * statistics but releases the edge vector after solve().
 */

#pragma once

#include "Dataflow/IFDS/Solver/IDESolver.h"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifds {

/// Statistics for the compact path-edge representation.
struct IterativeIDESolverStatistics {
  size_t path_edges = 0;
  size_t unique_instructions = 0;
  size_t unique_facts = 0;
  size_t edge_bytes = 0;
  size_t instruction_bytes = 0;
  size_t fact_bytes = 0;

  size_t estimated_memory_bytes() const {
    return edge_bytes + instruction_bytes + fact_bytes;
  }
};

/// A path edge in the compact representation.  IDs are local to one solver
/// run and are stable until clear_compact_storage() or solve() is called.
struct IterativeIDECompactPathEdge {
  uint32_t start_instruction = 0;
  uint32_t start_fact = 0;
  uint32_t target_instruction = 0;
  uint32_t target_fact = 0;

  bool operator==(const IterativeIDECompactPathEdge &other) const {
    return start_instruction == other.start_instruction &&
           start_fact == other.start_fact &&
           target_instruction == other.target_instruction &&
           target_fact == other.target_fact;
  }
};

template <typename Problem>
class IterativeIDESolver final : public IDESolver<Problem> {
public:
  using Base = IDESolver<Problem>;
  using Fact = typename Base::Fact;
  using PathEdgeType = typename Base::PathEdgeType;

  explicit IterativeIDESolver(Problem &problem) : Base(problem) {}

  /// Enable or disable the compact table.  Disabling it does not change IDE
  /// semantics; it only avoids interning and storing the optional edge index.
  void set_compact_ids(bool enable) { m_compact_ids = enable; }
  bool compact_ids_enabled() const { return m_compact_ids; }

  /// Keep compact edges after solve().  Setting this to false is useful for
  /// checker clients that only need values and statistics.
  void set_retain_compact_edges(bool retain) { m_retain_edges = retain; }
  bool retain_compact_edges() const { return m_retain_edges; }

  /// Retain tabulation caches after solve().  The default releases them once
  /// values and compact edges have been materialized.
  void set_retain_solver_storage(bool retain) { m_retain_solver_storage = retain; }
  bool retain_solver_storage() const { return m_retain_solver_storage; }

  void solve(const llvm::Module &module) {
    clear_compact_storage();
    Base::solve(module);
    if (m_compact_ids && !m_retain_edges) {
      m_edges.clear();
      m_edge_set.clear();
    }
    if (!m_retain_solver_storage)
      Base::release_transient_storage();
  }

  void clear_compact_storage() {
    std::vector<IterativeIDECompactPathEdge>().swap(m_edges);
    std::unordered_set<IterativeIDECompactPathEdge, CompactEdgeHash>().swap(
        m_edge_set);
    std::unordered_map<const llvm::Instruction *, uint32_t>().swap(
        m_instruction_ids);
    std::vector<const llvm::Instruction *>().swap(m_instructions);
    std::unordered_map<Fact, uint32_t>().swap(m_fact_ids);
    std::vector<Fact>().swap(m_facts);
  }

  /// Explicitly release the compact table while retaining the normal solver
  /// results.  This is the memory-reclamation point used by long-running
  /// whole-program analyses.
  void release_compact_storage() { clear_compact_storage(); }

  const std::vector<IterativeIDECompactPathEdge> &get_compact_path_edges()
      const {
    return m_edges;
  }

  const std::vector<const llvm::Instruction *> &get_compact_instructions()
      const {
    return m_instructions;
  }

  const std::vector<Fact> &get_compact_facts() const { return m_facts; }

  /// Returns a stable ID for a previously observed instruction/fact, or the
  /// sentinel UINT32_MAX if the object is not in the current run.
  uint32_t instruction_id(const llvm::Instruction *instruction) const {
    auto it = m_instruction_ids.find(instruction);
    return it == m_instruction_ids.end() ? invalid_id() : it->second;
  }
  uint32_t fact_id(const Fact &fact) const {
    auto it = m_fact_ids.find(fact);
    return it == m_fact_ids.end() ? invalid_id() : it->second;
  }
  static constexpr uint32_t invalid_id() {
    return std::numeric_limits<uint32_t>::max();
  }

  IterativeIDESolverStatistics get_iterative_statistics() const {
    IterativeIDESolverStatistics result;
    result.path_edges = m_edges.size();
    result.unique_instructions = m_instructions.size();
    result.unique_facts = m_facts.size();
    result.edge_bytes = m_edges.capacity() * sizeof(IterativeIDECompactPathEdge);
    result.instruction_bytes =
        m_instructions.capacity() * sizeof(const llvm::Instruction *);
    result.fact_bytes = m_facts.capacity() * sizeof(Fact);
    return result;
  }

protected:
  bool wants_path_edge_callbacks() const override { return m_compact_ids; }

  void on_path_edge_added(const PathEdgeType &edge) override {
    if (!m_compact_ids)
      return;
    const auto *start = edge.start_node;
    const auto *target = edge.target_node;
    const uint32_t start_instruction = intern_instruction(start);
    const uint32_t target_instruction = intern_instruction(target);
    const uint32_t start_fact = intern_fact(edge.start_fact);
    const uint32_t target_fact = intern_fact(edge.target_fact);
    IterativeIDECompactPathEdge compact{start_instruction, start_fact,
                                        target_instruction, target_fact};
    if (m_edge_set.insert(compact).second)
      m_edges.push_back(compact);
  }

private:
  struct CompactEdgeHash {
    size_t operator()(const IterativeIDECompactPathEdge &edge) const {
      size_t h = 14695981039346656037ULL;
      auto mix = [&h](uint32_t value) {
        h ^= value;
        h *= 1099511628211ULL;
      };
      mix(edge.start_instruction);
      mix(edge.start_fact);
      mix(edge.target_instruction);
      mix(edge.target_fact);
      return h;
    }
  };

  uint32_t intern_instruction(const llvm::Instruction *instruction) {
    auto [it, inserted] = m_instruction_ids.emplace(
        instruction, static_cast<uint32_t>(m_instructions.size()));
    if (inserted)
      m_instructions.push_back(instruction);
    return it->second;
  }

  uint32_t intern_fact(const Fact &fact) {
    auto [it, inserted] = m_fact_ids.emplace(
        fact, static_cast<uint32_t>(m_facts.size()));
    if (inserted)
      m_facts.push_back(fact);
    return it->second;
  }

  bool m_compact_ids = true;
  bool m_retain_edges = true;
  bool m_retain_solver_storage = false;
  std::unordered_map<const llvm::Instruction *, uint32_t> m_instruction_ids;
  std::vector<const llvm::Instruction *> m_instructions;
  std::unordered_map<Fact, uint32_t> m_fact_ids;
  std::vector<Fact> m_facts;
  std::vector<IterativeIDECompactPathEdge> m_edges;
  std::unordered_set<IterativeIDECompactPathEdge, CompactEdgeHash> m_edge_set;
};

} // namespace ifds
