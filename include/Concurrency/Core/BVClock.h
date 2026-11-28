/**
 * @file BVClock.h
 * @brief Bit-vector clock implementation for concurrency analysis
 *
 * A BVClock is a causal-history set where each clock element is a single bit.
 * Each logical event must therefore have a unique dimension; setting the same
 * dimension twice does not represent two distinct events.
 *
 * @author rainoftime
 */

#pragma once

#include <cstddef>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Support/raw_ostream.h>

class FBVClock;

/// @brief Bit-vector clock for tracking causal relationships
///
/// A causal-history set where each one-shot event dimension is a single bit.
/// Used for efficient partial order comparisons in concurrent program analysis.
class BVClock {
public:
  /// @brief Default constructor - creates a vector clock initialized to 0
  BVClock() = default;
  BVClock(const BVClock &vc) = default;
  BVClock(BVClock &&vc) = default;
  BVClock &operator=(const BVClock &vc) = default;
  BVClock &operator=(BVClock &&vc) = default;
  BVClock &operator=(const FBVClock &vc);
  /// @brief Vector clock union (happens-before composition)
  /// @param vc The vector clock to union with
  /// @return A new vector clock where each clock element d takes the value
  ///         max((*this)[d], vc[d]) for all d
  BVClock operator+(const BVClock &vc) const;
  /// @brief Assign-union operator
  /// @param vc The vector clock to union with
  /// @return Reference to this vector clock after union
  BVClock &operator+=(const BVClock &vc) {
    if (vec.size() < vc.vec.size()) {
      vec.resize(vc.vec.size(), false);
    }
    for (std::size_t i = 0; i < vc.vec.size(); ++i) {
      vec[i] = vec[i] || vc.vec[i];
    }
    return *this;
  }
  /// @brief Assign-union with rvalue reference
  BVClock &operator+=(BVClock &&vc);
  /// @brief Assign-union with FBVClock
  BVClock &operator+=(const FBVClock &vc);
  /// @brief Access the value of clock element d
  /// @param d The clock element index
  /// @return The value of clock element d, or false if d is out of range
  bool operator[](int d) const {
    validateIndex(d);
    if (static_cast<std::size_t>(d) < vec.size())
      return vec[static_cast<std::size_t>(d)];
    return false;
  }
  /// @brief Set clock element d to true
  /// @param d The clock element index
  void set(int d) {
    validateIndex(d);
    const std::size_t index = static_cast<std::size_t>(d);
    if (index >= vec.size()) {
      vec.resize(index + 1, false);
    }
    vec[index] = true;
  }
  /// @brief Clear all clock elements
  void clear() { vec.clear(); }

  //@{
  /// @name Partial Order Comparisons
  ///
  /// A vector clock u is considered strictly less than a vector clock
  /// v iff for all d in DOM, it holds that u[d] <= v[d], and there is
  /// at least one d such that u[d] < v[d].
  /// @param vc The vector clock to compare with
  /// @return true if the comparison holds
  bool lt(const BVClock &vc) const;
  bool leq(const BVClock &vc) const;
  bool gt(const BVClock &vc) const;
  bool geq(const BVClock &vc) const;
  //@}

  //@{
  /// @name Total Order Comparisons
  ///
  /// These operators use a canonical lexicographic ordering suitable for
  /// ordered containers. Use lt/leq/gt/geq for the causal partial order.
  bool operator==(const BVClock &vc) const;
  bool operator!=(const BVClock &vc) const;
  bool operator<(const BVClock &vc) const;
  bool operator<=(const BVClock &vc) const;
  bool operator>(const BVClock &vc) const;
  bool operator>=(const BVClock &vc) const;
  //@}

  /// @brief Convert the vector clock to a string representation
  /// @return String representation of the vector clock
  std::string to_string() const;

private:
  static void validateIndex(int d);
  std::size_t effectiveSize() const;

  std::vector<bool> vec; ///< Internal bit vector storage
};

inline std::ostream &operator<<(std::ostream &os, const BVClock &vc) {
  return os << vc.to_string();
}

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const BVClock &vc) {
  return os << vc.to_string();
}

