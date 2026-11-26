#pragma once

#include <cstdint>

namespace tpa::taint {

enum LatticeCompareResult : std::uint8_t {
  Equal,
  LessThan,
  GreaterThan,
  Incomparable,
};

// Lattice is a generic trait template that should be specialize by classed that
// wants to be properly handled by the dataflow engine

// The default specialization produces an error
template <typename LatticeType> struct Lattice {
  // Members to include:
  // static LatticeCompareResult compare(const LatticeType& lhs, const
  // LatticeType& rhs); static LatticeType merge(const LatticeType& lhs, const
  // LatticeType& rhs);

  // If anyone tries to use this class without having an appropriate
  // specialization, make an error.  If you get this error, it's because you
  // need to include the appropriate specialization of Lattice<> for your
  // lattice, or you need to define it for a new lattice type.
  using UnknownLatticeType = typename LatticeType::UnknownLatticeTypeError;
};

} // namespace tpa::taint
