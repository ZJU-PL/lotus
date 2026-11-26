#pragma once

#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"

#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Relevant pointer set V_P and statement slice St_P (Kahlon PLDI '08, Alg. 1).
struct Slice {
  std::vector<bool> values;
  std::vector<bool> objects;
  std::vector<bool> instructions; // St_P membership by site ID
  std::vector<Id> value_list;
  std::vector<Id> object_list;
  std::vector<Id> inst_list; // St_P compact list
  std::vector<bool> relevant_functions;
};

bool touches(const Program &program, const Slice &slice,
             const PointsToSet &targets);

// Reusable per-upper-bound index for Algorithm 1 slicing, avoiding rebuilding
// whole-program store/havoc/memcopy/alloc/call maps on every cluster slice.
struct SliceIndex {
  std::vector<std::vector<Id>> stores_by_comp;
  std::vector<std::vector<Id>> stores_by_obj;
  std::vector<Id> top_stores;
  std::vector<std::vector<Id>> havocs_by_comp;
  std::vector<std::vector<Id>> havocs_by_obj;
  std::vector<Id> top_havocs;
  std::vector<std::vector<Id>> memcopies_by_comp;
  std::vector<std::vector<Id>> memcopies_by_obj;
  std::vector<Id> top_memcopies;
  std::vector<std::vector<Id>> allocs_by_base;
  std::vector<Id> memory_writing_calls;
  std::vector<Id> non_returning_calls;
  std::vector<Id> addr_taken_funcs;
};

SliceIndex buildSliceIndex(const Program &program, const ProgramIndex &index,
                           const Sets &upper,
                           const SteensgaardHierarchy &hierarchy,
                           const ReachabilityInfo &reachability);

// Algorithm 1 (Kahlon PLDI '08, p. 253): worklist computation of V_P and St_P.
Slice relevant(const Program &program, const ProgramIndex &index,
               const Sets &upper, const SteensgaardHierarchy &hierarchy,
               const ReachabilityInfo &reachability,
               const std::vector<Id> &seeds, bool enabled,
               const std::vector<Id> &seed_objects = {},
               const SliceIndex *prebuilt = nullptr);

} // namespace detail
} // namespace bootstrap
} // namespace lotus
