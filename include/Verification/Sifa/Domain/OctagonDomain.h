//===-- Verification/Sifa/Domain/OctagonDomain.h
//---------------------------===//
//
// Octagon domain (ported from Ultimate Library-Sifa). Miné's octagon:
// ±vi ± vj ≤ c; OctagonState holds varToIndex + OctagonMatrix; join = max,
// widen = widenSimple, isBottom = strongClosure then hasNegativeSelfLoop.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Domain/IntervalDomain.h"
#include "Verification/Sifa/Domain/OctagonMatrix.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace lotus {
class AliasAnalysisWrapper;
namespace sifa {

/// Octagon state: varToIndex (Value* -> block index), matrix
/// (Ultimate-aligned).
class OctagonState {
public:
  OctagonState() = default;
  explicit OctagonState(bool isBottom) : isBottom_(isBottom) {}

  /// Build state from explicit varToIndex and matrix (for block transfer).
  OctagonState(std::unordered_map<const llvm::Value *, std::size_t> varToIndex,
               OctagonMatrix matrix, bool isBottom = false)
      : isBottom_(isBottom), varToIndex_(std::move(varToIndex)),
        matrix_(std::move(matrix)) {}

  bool isBottom() const { return isBottom_; }
  void setBottom(bool b) { isBottom_ = b; }

  const std::unordered_map<const llvm::Value *, std::size_t> &
  varToIndex() const {
    return varToIndex_;
  }
  const OctagonMatrix &matrix() const { return matrix_; }

  std::optional<Interval> getMemory(const llvm::Value *region) const {
    auto it = memory_.find(region);
    if (it == memory_.end())
      return std::nullopt;
    return it->second;
  }
  void setMemory(const llvm::Value *region, Interval i) {
    memory_[region] = std::move(i);
  }
  const std::unordered_map<const llvm::Value *, Interval> &memory() const {
    return memory_;
  }

  OctagonState join(const OctagonState &other) const {
    if (isBottom_ || hasNegativeSelfLoop())
      return other;
    if (other.isBottom_ || other.hasNegativeSelfLoop())
      return *this;

    std::vector<const llvm::Value *> thisOrdered(varToIndex_.size(), nullptr);
    for (const auto &p : varToIndex_) {
      if (p.second < thisOrdered.size())
        thisOrdered[p.second] = p.first;
    }
    std::vector<const llvm::Value *> otherOrdered(other.varToIndex_.size(),
                                                  nullptr);
    for (const auto &p : other.varToIndex_) {
      if (p.second < otherOrdered.size())
        otherOrdered[p.second] = p.first;
    }

    std::vector<const llvm::Value *> allVars;
    allVars.reserve(thisOrdered.size() + otherOrdered.size());
    for (const llvm::Value *v : thisOrdered) {
      if (v)
        allVars.push_back(v);
    }
    for (const llvm::Value *v : otherOrdered) {
      if (v && !varToIndex_.count(v))
        allVars.push_back(v);
    }

    const std::size_t n = allVars.size();
    std::unordered_map<const llvm::Value *, std::size_t> newVarToIndex;
    newVarToIndex.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      newVarToIndex[allVars[i]] = i;

    const OctagonMatrix &c1 = closedMatrix();
    const OctagonMatrix &c2 = other.closedMatrix();

    std::vector<std::size_t> copy1(2 * n, c1.dim());
    std::vector<std::size_t> copy2(2 * n, c2.dim());
    for (std::size_t i = 0; i < n; ++i) {
      auto it1 = varToIndex_.find(allVars[i]);
      auto it2 = other.varToIndex_.find(allVars[i]);
      if (it1 != varToIndex_.end()) {
        copy1[2 * i] = 2 * it1->second;
        copy1[2 * i + 1] = 2 * it1->second + 1;
      }
      if (it2 != other.varToIndex_.end()) {
        copy2[2 * i] = 2 * it2->second;
        copy2[2 * i + 1] = 2 * it2->second + 1;
      }
    }
    OctagonMatrix m1 = c1.rearrange(copy1);
    OctagonMatrix m2 = c2.rearrange(copy2);

    OctagonState out;
    out.varToIndex_ = std::move(newVarToIndex);
    out.matrix_ = m1.max(m2);
    // Absent memory region denotes top; join only intersects regions present in
    // both states.
    for (const auto &kv : memory_) {
      auto it = other.memory_.find(kv.first);
      if (it == other.memory_.end())
        continue;
      out.memory_[kv.first] = kv.second.join(it->second);
    }
    return out;
  }

  OctagonState widen(const OctagonState &other) const {
    if (isBottom_ || hasNegativeSelfLoop())
      return other;
    if (other.isBottom_ || other.hasNegativeSelfLoop())
      return *this;

    const std::size_t n = varToIndex_.size();
    const OctagonMatrix &otherClosed = other.closedMatrix();
    std::vector<std::size_t> copy2(2 * n, otherClosed.dim());
    for (const auto &kv : varToIndex_) {
      auto it = other.varToIndex_.find(kv.first);
      if (it != other.varToIndex_.end()) {
        copy2[2 * kv.second] = 2 * it->second;
        copy2[2 * kv.second + 1] = 2 * it->second + 1;
      }
    }
    OctagonMatrix m2 = otherClosed.rearrange(copy2);

    OctagonState out;
    out.varToIndex_ = varToIndex_;
    // Keep LHS unclosed per Miné's octagon widening to ensure termination.
    out.matrix_ = matrix_.widenSimple(m2);
    for (const auto &kv : memory_) {
      auto it = other.memory_.find(kv.first);
      if (it == other.memory_.end())
        continue;
      out.memory_[kv.first] = kv.second.widen(it->second);
    }
    return out;
  }

  bool isContainedIn(const OctagonState &b) const {
    if (isBottom_ || hasNegativeSelfLoop())
      return true;
    if (b.isBottom_ || b.hasNegativeSelfLoop())
      return false;
    const OctagonMatrix &aClosed = closedMatrix();
    const OctagonMatrix &bClosed = b.closedMatrix();
    for (const auto &kvB1 : b.varToIndex_) {
      const std::size_t iB = kvB1.second;
      auto itA1 = varToIndex_.find(kvB1.first);
      for (const auto &kvB2 : b.varToIndex_) {
        const std::size_t jB = kvB2.second;
        auto itA2 = varToIndex_.find(kvB2.first);
        for (int si = 0; si < 2; ++si) {
          for (int sj = 0; sj < 2; ++sj) {
            const std::size_t rowB = 2 * iB + si;
            const std::size_t colB = 2 * jB + sj;
            auto cb = bClosed.get(rowB, colB);
            if (!cb)
              continue;
            if (rowB == colB && *cb >= 0)
              continue;
            if (itA1 == varToIndex_.end() || itA2 == varToIndex_.end())
              return false;
            auto ca =
                aClosed.get(2 * itA1->second + si, 2 * itA2->second + sj);
            if (!ca || *ca > *cb)
              return false;
          }
        }
      }
    }
    for (const auto &kv : b.memory_) {
      auto itA = memory_.find(kv.first);
      const Interval ia = (itA == memory_.end()) ? Interval::top() : itA->second;
      if (!IntervalDomain::intervalLeq(ia, kv.second))
        return false;
    }
    return true;
  }

  bool operator==(const OctagonState &o) const {
    const bool b1 = isBottom_ || hasNegativeSelfLoop();
    const bool b2 = o.isBottom_ || o.hasNegativeSelfLoop();
    if (b1 != b2)
      return false;
    if (b1)
      return true;
    return isContainedIn(o) && o.isContainedIn(*this);
  }

  /// Print the state summary to an output stream.
  void print(llvm::raw_ostream &out) const;

  const OctagonMatrix &closedMatrix() const {
    if (!closureCache_) {
      closureCache_ = matrix_.strongClosure();
    }
    return *closureCache_;
  }

  /// True if strong closure has a negative self-loop.
  /// The strong closure is computed lazily and cached to avoid O(n³)
  /// recomputation on every isBottom() call in the hot path of the DAG
  /// interpreter.
  bool hasNegativeSelfLoop() const {
    return closedMatrix().hasNegativeSelfLoop();
  }

  /// Invalidate the cached strong closure (call after mutating matrix_).
  void invalidateClosureCache() { closureCache_ = std::nullopt; }

private:
  static OctagonMatrix expandAndRearrange(const OctagonMatrix &m,
                                          const std::vector<std::size_t> &copy,
                                          std::size_t n) {
    OctagonMatrix out(n);
    const std::size_t d = 2 * n;
    for (std::size_t i = 0; i < d; ++i)
      for (std::size_t j = 0; j < d; ++j) {
        std::size_t si = copy[i], sj = copy[j];
        if (si < m.dim() && sj < m.dim()) {
          auto c = m.get(si, sj);
          if (c)
            out.set(i, j, *c);
        }
      }
    return out;
  }

  bool isBottom_ = false;
  std::unordered_map<const llvm::Value *, std::size_t> varToIndex_;
  OctagonMatrix matrix_;
  std::unordered_map<const llvm::Value *, Interval> memory_;
  /// Lazily computed strong closure cache. Mutable so hasNegativeSelfLoop() is
  /// const.
  mutable std::optional<OctagonMatrix> closureCache_;
};

/// Octagon domain implementing AbstractDomain<Transition, OctagonState>.
/// When BlockTransferPolicy marks a block as block-wise, post(Edge) uses
/// applyBlockWiseHavoc (add all defined values as unconstrained).
class OctagonDomain final : public AbstractDomain<Transition, OctagonState> {
public:
  OctagonDomain() = default;
  explicit OctagonDomain(const BlockTransferPolicy *policy)
      : blockTransferPolicy_(policy) {}
  OctagonDomain(const BlockTransferPolicy *policy,
                lotus::AliasAnalysisWrapper *aliasAnalysis)
      : blockTransferPolicy_(policy), aliasAnalysis_(aliasAnalysis) {}

  void setBlockTransferPolicy(const BlockTransferPolicy *policy) {
    blockTransferPolicy_ = policy;
  }
  const BlockTransferPolicy *getBlockTransferPolicy() const {
    return blockTransferPolicy_;
  }
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    aliasAnalysis_ = aa;
  }
  lotus::AliasAnalysisWrapper *getAliasAnalysis() const {
    return aliasAnalysis_;
  }

  OctagonState top() const override { return OctagonState(false); }
  OctagonState bottom() const override { return OctagonState(true); }
  bool isBottom(const OctagonState &s) const override {
    return s.isBottom() || s.hasNegativeSelfLoop();
  }
  bool leq(const OctagonState &a, const OctagonState &b) const override {
    return a.isContainedIn(b);
  }
  OctagonState join(const OctagonState &a,
                    const OctagonState &b) const override {
    return a.join(b);
  }
  OctagonState widen(const OctagonState &prev,
                     const OctagonState &next) const override {
    return prev.widen(next);
  }
  /// Apply block transfer (copy/constant/affine; non-linear ops havoc).
  /// Implemented in OctagonDomain.cpp.
  OctagonState applyBlockTransfer(llvm::BasicBlock *bb,
                                  const OctagonState &in) const;
  OctagonState applyBlockTransfer(llvm::BasicBlock *bb, const OctagonState &in,
                                  const llvm::Instruction *segmentStart,
                                  const llvm::Instruction *stopBefore) const;
  /// Block-wise fast path: add all values defined in \p bb as unconstrained
  /// (top).
  OctagonState applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                   const OctagonState &in) const;
  OctagonState applyBlockWiseHavoc(llvm::BasicBlock *bb, const OctagonState &in,
                                   const llvm::Instruction *segmentStart,
                                   const llvm::Instruction *stopBefore) const;
  OctagonState post(const Transition &t, const OctagonState &in) const override;
  OctagonState postCall(const Transition &t,
                        const OctagonState &callerState) const override;
  OctagonState postReturn(const Transition &t, const OctagonState &callerState,
                          const OctagonState &calleeSummary) const override;

private:
  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
  lotus::AliasAnalysisWrapper *aliasAnalysis_ = nullptr;
};

} // namespace sifa
} // namespace lotus

