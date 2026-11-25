#pragma once

#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"
#include "Alias/InclusionBased/BootstrapAA/Preprocessing/RelevantSlice.h"

#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

struct CoarseResult {
  Sets upper;
  Sets object_upper;
  std::vector<std::vector<Id>> partitions;
  SteensgaardHierarchy hierarchy;
};

// Stage 1 (§2.1): Steensgaard unification partitioning and cycle-collapsed
// points-to hierarchy.
CoarseResult steensgaard(const Program &program,
                         HavocPolicy havoc_policy = HavocPolicy::Noop);

// Optional Stage 1.5 (§4, p. 258): Das (2000) one-level-flow directional
// refinement over a Steensgaard partition's slice (directional inclusion on
// top-level assignments AND single-level memory loads/stores using
// Steensgaard-fixed dereference addresses).
Sets oneLevelFlow(const Program &program, const ProgramIndex &index,
                  const Slice &slice, const Sets &coarse_upper,
                  HavocPolicy havoc_policy = HavocPolicy::Noop);

// Stage 2 (§2.1, Theorem 7): Sparse St_P-restricted Andersen inclusion
// analysis for refining large partitions into overlapping inverse covers.
Sets inclusion(const Program &program, const ProgramIndex &index,
               const Slice &slice,
               HavocPolicy havoc_policy = HavocPolicy::Noop);

} // namespace detail
} // namespace bootstrap
} // namespace lotus
