#include "CFL/InterleavedDyck/StagedBounds/CnfTypes.h"

#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>

namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement {
namespace {

void combine(std::size_t &seed, int value) {
  // Hash arithmetic is unsigned: sparse or negative symbol IDs must not cause
  // the undefined signed shifts of the former 20-bit packing scheme.
  seed ^= std::hash<int>{}(value) + static_cast<std::size_t>(0x9e3779b9U) +
          (seed << 6U) + (seed >> 2U);
}

} // namespace

std::size_t IntPairHasher::operator()(const std::pair<int, int> &p) const {
  std::size_t seed = std::hash<int>{}(p.first);
  combine(seed, p.second);
  return seed;
}

std::size_t
IntTripleHasher::operator()(const std::tuple<int, int, int> &t) const {
  std::size_t seed = std::hash<int>{}(std::get<0>(t));
  combine(seed, std::get<1>(t));
  combine(seed, std::get<2>(t));
  return seed;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement
