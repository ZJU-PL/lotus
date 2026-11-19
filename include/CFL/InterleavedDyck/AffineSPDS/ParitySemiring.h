#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Statistics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lotus::cfl::interleaved_dyck::affine {

namespace detail {

template <class Word> constexpr Word fullParityMask(std::size_t bits) noexcept {
  static_assert(std::is_unsigned_v<Word>,
                "ParitySpace requires an unsigned integer Word");
  if (bits == 0)
    return Word{0};
  if (bits >= sizeof(Word) * 8U)
    return static_cast<Word>(~Word{0});
  return static_cast<Word>((std::uint64_t{1} << bits) - 1U);
}

template <class Word> inline std::size_t lowestSetBit(Word x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::size_t>(
      __builtin_ctzll(static_cast<unsigned long long>(x)));
#else
  std::size_t idx = 0;
  while ((x & Word{1}) == 0) {
    x >>= 1U;
    ++idx;
  }
  return idx;
#endif
}

template <class Word> inline std::size_t popcountWord(Word x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::size_t>(
      __builtin_popcountll(static_cast<unsigned long long>(x)));
#else
  std::size_t count = 0;
  while (x != 0) {
    x = static_cast<Word>(x & static_cast<Word>(x - 1U));
    ++count;
  }
  return count;
#endif
}

} // namespace detail

/// Exact affine subspace `offset + span(basis) \subseteq (GF(2)^B, XOR)` for
/// parity-toggle block-diagonal observers `M(x) = I + sum_b x_b E_{2b,2b+1}`.
///
/// Because `N(x) * N(y) = 0` for all `x, y`, matrix product is exactly `XOR`
/// and `extend((a + U), (b + V)) = (a ^ b) + span(U U V)` with no quadratic
/// cross-terms. RREF invariants:
///   - `basis[p]` has pivot bit `p` iff bit `p` is set in `pivot_mask`.
///   - For every active pivot `p`: `lowestSetBit(basis[p]) == p` and
///     `(basis[p] & pivot_mask) == (Word{1} << p)`.
///   - `(offset & pivot_mask) == 0`.
template <class Word> struct ParitySpace {
  static_assert(std::is_unsigned_v<Word>,
                "ParitySpace requires an unsigned integer Word");
  static constexpr std::size_t MAX_BITS = sizeof(Word) * 8U;

  Word offset = 0;
  Word pivot_mask = 0;
  bool empty = true;
  std::array<Word, MAX_BITS> basis{};

  static constexpr ParitySpace zero() noexcept { return ParitySpace{}; }
  static constexpr ParitySpace one() noexcept { return point(Word{0}); }
  static constexpr ParitySpace point(Word mask) noexcept {
    ParitySpace s;
    s.offset = mask;
    s.pivot_mask = 0;
    s.empty = false;
    return s;
  }

  constexpr bool isIdentity() const noexcept {
    return !empty && offset == 0 && pivot_mask == 0;
  }
  std::size_t rank() const noexcept {
    return empty ? 0U : detail::popcountWord(pivot_mask);
  }
  constexpr bool containsZero() const noexcept { return !empty && offset == 0; }

  Word reduce(Word v, AlgebraStatistics *stats = nullptr) const noexcept {
    if (stats)
      ++stats->basis_reductions;
    Word active = static_cast<Word>(v & pivot_mask);
    while (active != 0) {
      const std::size_t p = detail::lowestSetBit(active);
      v = static_cast<Word>(v ^ basis[p]);
      active = static_cast<Word>(active & static_cast<Word>(active - 1U));
    }
    return v;
  }

  Word basisOr() const noexcept {
    Word mask = 0;
    Word active = pivot_mask;
    while (active != 0) {
      const std::size_t p = detail::lowestSetBit(active);
      mask = static_cast<Word>(mask | basis[p]);
      active = static_cast<Word>(active & static_cast<Word>(active - 1U));
    }
    return mask;
  }

  bool insertDirection(Word v, Word full_mask,
                       AlgebraStatistics *stats = nullptr) noexcept {
    if (pivot_mask == full_mask)
      return false;
    v = reduce(v, stats);
    if (v == 0)
      return false;
    if (stats)
      ++stats->basis_insertions;
    const std::size_t new_p = detail::lowestSetBit(v);
    Word existing = pivot_mask;
    while (existing != 0) {
      const std::size_t q = detail::lowestSetBit(existing);
      if (((basis[q] >> new_p) & 1U) != 0U)
        basis[q] = static_cast<Word>(basis[q] ^ v);
      existing =
          static_cast<Word>(existing & static_cast<Word>(existing - 1U));
    }
    basis[new_p] = v;
    pivot_mask = static_cast<Word>(pivot_mask | static_cast<Word>(Word{1} << new_p));
    if (((offset >> new_p) & 1U) != 0U)
      offset = static_cast<Word>(offset ^ v);
    return true;
  }

  bool combineWith(const ParitySpace &other, Word full_mask,
                   AlgebraStatistics *stats = nullptr) noexcept {
    if (this == &other || other.empty)
      return false;
    if (empty) {
      *this = other;
      return true;
    }
    if (pivot_mask == full_mask || *this == other)
      return false;
    bool changed = insertDirection(static_cast<Word>(offset ^ other.offset),
                                   full_mask, stats);
    Word active = other.pivot_mask;
    while (active != 0 && pivot_mask != full_mask) {
      const std::size_t p = detail::lowestSetBit(active);
      if (((pivot_mask >> p) & 1U) == 0U || basis[p] != other.basis[p])
        changed = insertDirection(other.basis[p], full_mask, stats) || changed;
      active = static_cast<Word>(active & static_cast<Word>(active - 1U));
    }
    return changed;
  }

  bool extendAndCombine(const ParitySpace &left, const ParitySpace &right,
                        Word full_mask,
                        AlgebraStatistics *stats = nullptr) noexcept {
    if (left.empty || right.empty || pivot_mask == full_mask)
      return false;
    if (left.isIdentity())
      return combineWith(right, full_mask, stats);
    if (right.isIdentity())
      return combineWith(left, full_mask, stats);
    if (this == &left || this == &right) {
      const ParitySpace left_copy = left;
      const ParitySpace right_copy = right;
      return extendAndCombine(left_copy, right_copy, full_mask, stats);
    }
    if (stats)
      ++stats->matrix_products;
    const Word cand_offset = static_cast<Word>(left.offset ^ right.offset);
    bool changed = false;
    if (empty) {
      offset = cand_offset;
      pivot_mask = 0;
      empty = false;
      changed = true;
    } else {
      changed =
          insertDirection(static_cast<Word>(offset ^ cand_offset), full_mask,
                          stats);
    }
    Word l_active = left.pivot_mask;
    while (l_active != 0 && pivot_mask != full_mask) {
      const std::size_t p = detail::lowestSetBit(l_active);
      if (((pivot_mask >> p) & 1U) == 0U || basis[p] != left.basis[p])
        changed =
            insertDirection(left.basis[p], full_mask, stats) || changed;
      l_active =
          static_cast<Word>(l_active & static_cast<Word>(l_active - 1U));
    }
    Word r_active = right.pivot_mask;
    while (r_active != 0 && pivot_mask != full_mask) {
      const std::size_t p = detail::lowestSetBit(r_active);
      if (((pivot_mask >> p) & 1U) == 0U || basis[p] != right.basis[p])
        changed =
            insertDirection(right.basis[p], full_mask, stats) || changed;
      r_active =
          static_cast<Word>(r_active & static_cast<Word>(r_active - 1U));
    }
    return changed;
  }

  bool intersects(const ParitySpace &other, Word full_mask,
                  AlgebraStatistics *stats = nullptr) const noexcept {
    if (stats)
      ++stats->intersection_tests;
    if (empty || other.empty)
      return false;
    if (offset == other.offset || pivot_mask == full_mask ||
        other.pivot_mask == full_mask) {
      if (stats)
        ++stats->intersection_fast_paths;
      return true;
    }
    const Word diff = static_cast<Word>(offset ^ other.offset);
    if (reduce(diff, stats) == 0 || other.reduce(diff, stats) == 0)
      return true;
    ParitySpace sum = *this;
    sum.offset = 0;
    Word active = other.pivot_mask;
    while (active != 0 && sum.pivot_mask != full_mask) {
      const std::size_t p = detail::lowestSetBit(active);
      if (((sum.pivot_mask >> p) & 1U) == 0U ||
          sum.basis[p] != other.basis[p]) {
        sum.insertDirection(other.basis[p], full_mask, stats);
      }
      active = static_cast<Word>(active & static_cast<Word>(active - 1U));
    }
    return sum.reduce(diff, stats) == 0;
  }

  bool independentIntersects(const ParitySpace &other, Word full_mask,
                             AlgebraStatistics *stats = nullptr) const noexcept {
    if (stats)
      ++stats->intersection_tests;
    if (empty || other.empty)
      return false;
    const Word free_bits = static_cast<Word>(basisOr() | other.basisOr());
    const Word constrained =
        static_cast<Word>(static_cast<Word>(~free_bits) & full_mask);
    return (static_cast<Word>(offset ^ other.offset) & constrained) == 0;
  }

  bool operator==(const ParitySpace &other) const noexcept {
    if (empty != other.empty)
      return false;
    if (empty)
      return true;
    if (offset != other.offset || pivot_mask != other.pivot_mask)
      return false;
    Word active = pivot_mask;
    while (active != 0) {
      const std::size_t p = detail::lowestSetBit(active);
      if (basis[p] != other.basis[p])
        return false;
      active = static_cast<Word>(active & static_cast<Word>(active - 1U));
    }
    return true;
  }
  bool operator!=(const ParitySpace &other) const noexcept {
    return !(*this == other);
  }
};

/// Finite-height commutative GF(2)-affine semiring isomorphic to `B`-block
/// parity-toggle matrix observers (`B <= sizeof(Word) * 8`).
template <class Word> class ParitySemiring {
public:
  using Weight = ParitySpace<Word>;

  explicit ParitySemiring(std::size_t bits = 1,
                          AlgebraStatistics *stats = nullptr)
      : bits_(bits), full_mask_(detail::fullParityMask<Word>(bits)),
        stats_(stats) {}

  std::size_t bits() const noexcept { return bits_; }
  Word fullMask() const noexcept { return full_mask_; }

  ParitySemiring withStatistics(AlgebraStatistics *stats) const noexcept {
    return ParitySemiring(bits_, stats);
  }

  constexpr Weight zero() const noexcept { return Weight::zero(); }
  constexpr Weight one() const noexcept { return Weight::one(); }

  Weight toggle(std::size_t bit) const noexcept {
    return Weight::point(static_cast<Word>(Word{1} << bit));
  }
  Weight point(Word mask) const noexcept {
    return Weight::point(static_cast<Word>(mask & full_mask_));
  }

  Weight combine(const Weight &left, const Weight &right) const noexcept {
    Weight result = left;
    result.combineWith(right, full_mask_, stats_);
    return result;
  }
  bool combineWith(Weight &left, const Weight &right) const noexcept {
    return left.combineWith(right, full_mask_, stats_);
  }

  Weight extend(const Weight &left, const Weight &right) const noexcept {
    Weight result = zero();
    result.extendAndCombine(left, right, full_mask_, stats_);
    return result;
  }
  bool extendAndCombine(Weight &target, const Weight &left,
                        const Weight &right) const noexcept {
    return target.extendAndCombine(left, right, full_mask_, stats_);
  }

  bool containsZero(const Weight &w) const noexcept {
    if (stats_) {
      ++stats_->intersection_tests;
      if (w.containsZero()) {
        ++stats_->intersection_fast_paths;
        return true;
      }
      return false;
    }
    return w.containsZero();
  }

  bool intersects(const Weight &left, const Weight &right) const noexcept {
    return left.intersects(right, full_mask_, stats_);
  }
  bool independentIntersects(const Weight &left,
                             const Weight &right) const noexcept {
    return left.independentIntersects(right, full_mask_, stats_);
  }

private:
  std::size_t bits_;
  Word full_mask_;
  AlgebraStatistics *stats_ = nullptr;
};

} // namespace lotus::cfl::interleaved_dyck::affine