#pragma once

#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

using Sets = std::vector<PointsToSet>;

struct LimitReached {};

inline Id nextId(std::size_t size, const char *kind) {
  if (size >= INVALID)
    throw std::length_error(std::string("BootstrapAA ") + kind +
                            " ID space exhausted");
  return static_cast<Id>(size);
}

inline bool isMemory(const Object &object) {
  return object.kind == ObjectKind::Global ||
         object.kind == ObjectKind::Stack || object.kind == ObjectKind::Heap;
}

template <typename Fn> void forInstructions(const Program &program, Fn fn) {
  for (Id f = 0; f < program.functions.size(); ++f)
    for (const Block &block : program.functions[f].blocks)
      for (const Instruction &instruction : block.instructions)
        fn(f, instruction);
}

inline PointsToSet valueOf(const Program &program, const Sets &values,
                           Id value) {
  if (value == INVALID)
    return PointsToSet::top();
  return program.values[value].constant ? program.values[value].initial
                                        : values[value];
}

// Compute Kosaraju strongly connected components of a directed graph on [0..n).
void computeKosarajuSCCs(std::size_t n,
                         const std::vector<std::vector<Id>> &successors,
                         const std::vector<std::vector<Id>> &predecessors,
                         std::vector<Id> &component, Id &component_count);

struct Targets {
  std::set<Id> functions;
  bool opaque = false;
};

Targets callees(const Program &program, const Instruction &instruction,
                const PointsToSet &target);

struct InstLocation {
  Id function = INVALID;
  Id block = INVALID;
  std::size_t index = 0;
  const Instruction *inst = nullptr;
};

// Precomputed program indices for O(|St_P|) slicing, sparse Andersen,
// backward CFG traversal, and interprocedural summary lookup.
struct ProgramIndex {
  std::vector<InstLocation> by_site; // indexed by Instruction::id
  std::vector<std::vector<Id>> defining_sites; // value -> sites defining value
  std::vector<std::vector<std::pair<Id, std::pair<Id, std::size_t>>>>
      phi_defs; // value -> (func, (pred_block, edge_idx))
  std::vector<Id> store_sites;
  std::vector<Id> memcopy_sites;
  std::vector<Id> havoc_sites;
  std::vector<Id> call_sites;
  std::vector<Id> allocate_sites;
  std::vector<std::vector<Id>> return_sites; // func -> Return instruction IDs
  // Reverse CFG per function: pred_edges[f][block] = list of (pred_block,
  // edge_idx).
  std::vector<std::vector<std::vector<std::pair<Id, std::size_t>>>> pred_edges;
  // Direct/coarse callers per function: incoming_calls[f] = list of call site
  // IDs that may invoke f (populated once coarse upper bounds are known).
  std::vector<std::vector<Id>> incoming_calls;
  // Loop-membership per CFG block (for sound contradiction checks & ROBDD
  // quantification).
  std::vector<std::vector<bool>> in_loop_block; // [func][block]
  // Immediate dominator per reachable CFG block (INVALID if unreachable).
  std::vector<std::vector<Id>> idom; // [func][block]
  // True if `obj` is a singleton cell with no unaligned sub-object interior
  // pointers (safe for exact-address equality in mustAlias and `!=` guards).
  std::vector<bool> exact_address_singleton;
  // True if `obj` may be addressed at an unaligned/interior byte offset
  // (independent of `singleton`, used to keep MemCopy alignment sound).
  std::vector<bool> has_unaligned_interior;

  explicit ProgramIndex(const Program &program);
  void populateCallers(const Program &program, const Sets &upper,
                       const SteensgaardHierarchy &hierarchy);
  bool blockDominates(Id func, Id dom_block, Id block) const;
};

struct CallGraphInfo {
  std::vector<std::vector<Id>> successors;
  std::vector<std::vector<Id>> predecessors;
  std::vector<Id> function_scc;
  std::vector<std::size_t> function_rank;
  std::vector<std::vector<Id>> scc_members;
  // Reverse topological order of SCCs (leaves / callees first, entry last).
  std::vector<Id> reverse_topo_sccs;
  std::vector<bool> scc_recursive;
  std::size_t scc_count = 0;
  std::size_t recursive_scc_count = 0;
};

CallGraphInfo buildCallGraph(const Program &program, const Sets &upper);

struct ReachabilityInfo {
  std::vector<bool> may_return;         // indexed by function
  std::vector<bool> reachable_site;     // indexed by site ID (within function)
  std::vector<bool> reachable_function; // from program.entry
};

ReachabilityInfo computeReachability(const Program &program,
                                     const ProgramIndex &index,
                                     const Sets &upper);

} // namespace detail
} // namespace bootstrap
} // namespace lotus
