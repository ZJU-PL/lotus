//===-- Verification/Sifa/Domain/OctagonMatrix.h --------------------------===//
//
// Octagon matrix (Ultimate OctagonMatrix-aligned). Miné's octagon domain:
// 2n×2n matrix for constraints ±vi ± vj ≤ c; indices 2i = +vi, 2i+1 = -vi.
// nullopt = +∞ (no constraint). strongClosure, max (join), widenSimple.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace lotus {
namespace sifa {

/// Octagon matrix: 2n×2n, entry (i,j) = c means constraint term(j) - term(i) ≤ c
/// where term(2k) = +v_k and term(2k+1) = -v_k; nullopt = +∞.
class OctagonMatrix {
public:
  explicit OctagonMatrix(std::size_t nVars = 0)
      : n_(nVars), dim_(2 * nVars), data_(dim_ * dim_, std::nullopt) {}

  std::size_t numVars() const { return n_; }
  std::size_t dim() const { return dim_; }

  std::optional<int64_t> get(std::size_t i, std::size_t j) const {
    if (i >= dim_ || j >= dim_)
      return std::nullopt;
    return data_[i * dim_ + j];
  }
  void set(std::size_t i, std::size_t j, int64_t c) {
    if (i < dim_ && j < dim_)
      data_[i * dim_ + j] = c;
  }
  void setMin(std::size_t i, std::size_t j, int64_t c) {
    if (i < dim_ && j < dim_) {
      auto &cell = data_[i * dim_ + j];
      if (!cell || c < *cell)
        cell = c;
    }
  }

  /// Element-wise max (join: looser bound). Same size only.
  /// +∞ is identity top: max(c, +∞) = +∞ (nullopt).
  OctagonMatrix max(const OctagonMatrix &other) const {
    if (dim_ != other.dim_)
      return *this;
    OctagonMatrix out(n_);
    for (std::size_t idx = 0; idx < data_.size(); ++idx) {
      if (data_[idx] && other.data_[idx])
        out.data_[idx] = std::max(*data_[idx], *other.data_[idx]);
    }
    return out;
  }

  /// Widen: if other[i][j] > this[i][j] (or other is unbounded) then
  /// unbounded (nullopt).
  OctagonMatrix widenSimple(const OctagonMatrix &other) const {
    if (dim_ != other.dim_)
      return *this;
    OctagonMatrix out(n_);
    for (std::size_t idx = 0; idx < data_.size(); ++idx) {
      if (data_[idx] && other.data_[idx] && *other.data_[idx] <= *data_[idx])
        out.data_[idx] = *data_[idx];
    }
    return out;
  }

  /// Strong closure (Floyd-Warshall). Entry nullopt = +∞.
  OctagonMatrix strongClosure() const {
    OctagonMatrix out = *this;
    const std::size_t d = dim_;
    for (std::size_t k = 0; k < d; ++k) {
      const std::size_t kRow = k * d;
      for (std::size_t i = 0; i < d; ++i) {
        const std::size_t iRow = i * d;
        const auto ik = out.data_[iRow + k];
        if (!ik)
          continue;
        for (std::size_t j = 0; j < d; ++j) {
          const auto kj = out.data_[kRow + j];
          if (!kj)
            continue;
          int64_t sum;
          if (__builtin_add_overflow(*ik, *kj, &sum))
            continue;
          auto &ij = out.data_[iRow + j];
          if (!ij || sum < *ij)
            ij = sum;
        }
      }
    }
    return out;
  }

  /// True iff any diagonal entry < 0 (inconsistent).
  bool hasNegativeSelfLoop() const {
    for (std::size_t i = 0; i < dim_; ++i) {
      auto c = data_[i * dim_ + i];
      if (c && *c < 0)
        return true;
    }
    return false;
  }

  /// Relax variable \p varIdx in place: set all constraints involving ±v to +∞.
  void relaxVarInPlace(std::size_t varIdx) {
    if (varIdx >= n_)
      return;
    const std::size_t lo = 2 * varIdx, hi = 2 * varIdx + 1;
    for (std::size_t j = 0; j < dim_; ++j) {
      data_[lo * dim_ + j] = std::nullopt;
      data_[hi * dim_ + j] = std::nullopt;
    }
    for (std::size_t i = 0; i < dim_; ++i) {
      data_[i * dim_ + lo] = std::nullopt;
      data_[i * dim_ + hi] = std::nullopt;
    }
  }

  /// Relax variable \p varIdx: return new matrix with all constraints involving
  /// ±v set to +∞ (sound over-approximation for havoc).
  OctagonMatrix relaxVar(std::size_t varIdx) const {
    if (varIdx >= n_)
      return *this;
    OctagonMatrix out = *this;
    out.relaxVarInPlace(varIdx);
    return out;
  }

  /// Rearrange to new variable order. \p copyInstructions[newIdx] = oldIdx (or
  /// dim() for unbounded).
  OctagonMatrix
  rearrange(const std::vector<std::size_t> &copyInstructions) const {
    const std::size_t d = copyInstructions.size();
    if (d % 2 != 0)
      return *this;
    OctagonMatrix out(d / 2);
    for (std::size_t i = 0; i < d; ++i) {
      const std::size_t si = copyInstructions[i];
      if (si >= dim_)
        continue;
      const std::size_t srcRow = si * dim_;
      const std::size_t dstRow = i * d;
      for (std::size_t j = 0; j < d; ++j) {
        const std::size_t sj = copyInstructions[j];
        if (sj < dim_)
          out.data_[dstRow + j] = data_[srcRow + sj];
      }
    }
    return out;
  }

private:
  std::size_t n_ = 0;
  std::size_t dim_ = 0;
  std::vector<std::optional<int64_t>> data_;
};

} // namespace sifa
} // namespace lotus

