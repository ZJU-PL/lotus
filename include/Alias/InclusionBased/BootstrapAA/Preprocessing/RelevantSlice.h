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

// Algorithm 1 (Kahlon PLDI '08, p. 253): worklist computation of V_P and St_P.
Slice relevant(const Program &program, const ProgramIndex &index,
               const Sets &upper, const SteensgaardHierarchy &hierarchy,
               const ReachabilityInfo &reachability,
               const std::vector<Id> &seeds, bool enabled,
               const std::vector<Id> &seed_objects = {});

} // namespace detail
} // namespace bootstrap
} // namespace lotus
