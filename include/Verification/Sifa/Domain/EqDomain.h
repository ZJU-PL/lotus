//===-- Verification/Sifa/Domain/EqDomain.h -------------------------------===//
//
// Equality domain (ported from Ultimate Library-Sifa).
//
// Ultimate's EqDomain is StateBasedDomain<EqState> with EqConstraint<EqNode>.
// Lotus uses union-find over LLVM Value* for equality classes; join merges
// classes from both states; post(Edge) applies block transfer (copy/phi/select
// equality).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/RegionMemory.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus {
class AliasAnalysisWrapper;
namespace sifa {

/// Equality state: union-find over Value* (Ultimate EqState wraps
/// EqConstraint).
class EqState {
public:
  EqState() = default;
  explicit EqState(bool isBottom) : isBottom_(isBottom) {}

  bool isBottom() const { return isBottom_; }
  void setBottom(bool b) { isBottom_ = b; }

  /// Representative of \p v (path compression). If \p v not seen, returns v.
  const llvm::Value *find(const llvm::Value *v) const {
    if (!v)
      return v;
    auto it = parent_.find(v);
    if (it == parent_.end())
      return v;
    if (it->second == v)
      return v;
    const llvm::Value *root = find(it->second);
    parent_[v] = root;
    return root;
  }

  /// Union-find link: make \p a and \p b equivalent.
  void unite(const llvm::Value *a, const llvm::Value *b) {
    if (!a || !b)
      return;
    const llvm::Value *ra = find(a);
    const llvm::Value *rb = find(b);
    if (ra == rb)
      return;
    ensure(ra);
    ensure(rb);
    parent_[ra] = rb;
  }

  /// Ensure \p v is in the map (self-loop).
  void ensure(const llvm::Value *v) {
    if (v && parent_.find(v) == parent_.end())
      parent_[v] = v;
  }

  /// Remove \p v from its equivalence class (making {v} a singleton) while
  /// preserving equivalences among all other members and memory regions.
  void isolate(const llvm::Value *v) {
    if (!v)
      return;
    const llvm::Value *oldRoot = find(v);
    for (auto &kv : parent_) {
      find(kv.first);
    }
    const llvm::Value *newRoot = nullptr;
    for (const auto &kv : parent_) {
      if (kv.first != v && kv.second == oldRoot) {
        newRoot = kv.first;
        break;
      }
    }
    if (oldRoot == v) {
      if (newRoot) {
        parent_[newRoot] = newRoot;
        for (auto &kv : parent_) {
          if (kv.first != v && kv.second == v)
            kv.second = newRoot;
        }
        for (auto &kv : memory_) {
          if (kv.second == v)
            kv.second = newRoot;
        }
      } else {
        for (auto it = memory_.begin(); it != memory_.end();) {
          if (it->second == v)
            it = memory_.erase(it);
          else
            ++it;
        }
      }
    } else if (newRoot) {
      for (auto &kv : memory_) {
        if (kv.second == v)
          kv.second = oldRoot;
      }
    }
    parent_[v] = v;
  }

  /// All keys (values that have been seen).
  std::unordered_set<const llvm::Value *> keys() const {
    std::unordered_set<const llvm::Value *> k;
    for (const auto &p : parent_)
      k.insert(p.first);
    return k;
  }

  /// Region memory: content of region is equivalent to representative.
  const llvm::Value *getMemory(const llvm::Value *region) const {
    auto it = memory_.find(region);
    if (it == memory_.end())
      return nullptr;
    return find(it->second);
  }
  void setMemory(const llvm::Value *region, const llvm::Value *rep) {
    if (rep)
      memory_[region] = find(rep);
    else
      memory_.erase(region);
  }
  const std::unordered_map<const llvm::Value *, const llvm::Value *> &
  memory() const {
    return memory_;
  }

  EqState join(const EqState &other) const {
    if (isBottom_)
      return other;
    if (other.isBottom_)
      return *this;
    EqState out;
    struct PairHash {
      std::size_t operator()(
          const std::pair<const llvm::Value *, const llvm::Value *> &p) const {
        return std::hash<const llvm::Value *>()(p.first) ^
               (std::hash<const llvm::Value *>()(p.second) << 1);
      }
    };
    std::unordered_map<std::pair<const llvm::Value *, const llvm::Value *>,
                       const llvm::Value *, PairHash>
        repByPair;
    for (const auto &kv : parent_) {
      const llvm::Value *v = kv.first;
      if (!v)
        continue;
      auto key = std::make_pair(find(v), other.find(v));
      auto it = repByPair.find(key);
      if (it == repByPair.end()) {
        repByPair.emplace(key, v);
        out.ensure(v);
      } else {
        out.ensure(v);
        out.unite(v, it->second);
      }
    }
    for (const auto &kv : memory_) {
      auto itO = other.memory_.find(kv.first);
      if (itO == other.memory_.end())
        continue;
      auto key = std::make_pair(find(kv.second), other.find(itO->second));
      auto repIt = repByPair.find(key);
      if (repIt != repByPair.end()) {
        out.setMemory(kv.first, repIt->second);
      } else if (key.first == key.second && key.first) {
        out.ensure(key.first);
        out.setMemory(kv.first, key.first);
      }
    }
    return out;
  }

  EqState widen(const EqState &other) const { return join(other); }

  bool isContainedIn(const EqState &b) const {
    if (isBottom_)
      return true;
    if (b.isBottom_)
      return false;
    for (const auto &kv : b.parent_) {
      const llvm::Value *v = kv.first;
      const llvm::Value *rb = b.find(v);
      if (rb != v && find(v) != find(rb))
        return false;
    }
    for (const auto &kv : b.memory_) {
      const llvm::Value *ma = getMemory(kv.first);
      if (!ma)
        return false;
      const llvm::Value *mb = b.find(kv.second);
      if (find(ma) != find(mb))
        return false;
    }
    return true;
  }

  bool operator==(const EqState &o) const {
    if (isBottom_ != o.isBottom_)
      return false;
    if (isBottom_)
      return true;
    return isContainedIn(o) && o.isContainedIn(*this);
  }

private:
  bool isBottom_ = false;
  mutable std::unordered_map<const llvm::Value *, const llvm::Value *>
      parent_; // path compression in find()
  std::unordered_map<const llvm::Value *, const llvm::Value *>
      memory_; // region -> representative
};

/// Equality domain implementing AbstractDomain<Transition, EqState>.
/// When BlockTransferPolicy marks a block as block-wise, post(Edge) uses
/// applyBlockWiseHavoc (ensure all defined values, no new equalities).
class EqDomain final : public AbstractDomain<Transition, EqState> {
public:
  EqDomain() = default;
  explicit EqDomain(const BlockTransferPolicy *policy)
      : blockTransferPolicy_(policy) {}
  EqDomain(const BlockTransferPolicy *policy,
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

  EqState top() const override { return EqState(false); }
  EqState bottom() const override { return EqState(true); }
  bool isBottom(const EqState &s) const override { return s.isBottom(); }
  bool leq(const EqState &a, const EqState &b) const override {
    return a.isContainedIn(b);
  }
  EqState join(const EqState &a, const EqState &b) const override {
    return a.join(b);
  }
  EqState widen(const EqState &prev, const EqState &next) const override {
    return prev.widen(next);
  }
  EqState applyBlockWiseHavoc(llvm::BasicBlock *bb, const EqState &in) const {
    if (in.isBottom())
      return in;
    EqState out = in;
    for (llvm::Instruction &I : *bb) {
      if (I.isTerminator())
        break;
      if (I.getType()->isVoidTy())
        continue;
      out.ensure(&I);
    }
    return out;
  }
  EqState post(const Transition &t, const EqState &in) const override {
    if (in.isBottom())
      return in;
    if (t.kind != TransitionKind::Edge || !t.source)
      return in;
    auto begin =
        t.segmentStart ? t.segmentStart->getIterator() : t.source->begin();
    while (begin != t.source->end() && llvm::isa<llvm::PHINode>(*begin))
      ++begin;
    if (blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source)) {
      EqState out = in;
      for (auto it = begin; it != t.source->end(); ++it) {
        const llvm::Instruction &I = *it;
        if (&I == t.stopBefore || I.isTerminator())
          break;
        if (I.getType()->isVoidTy())
          continue;
        out.ensure(&I);
      }
      out = refineForTakenEdge(t, std::move(out));
      return applyIncomingPhis(t, std::move(out));
    }
    EqState out = in;
    lotus::AliasAnalysisWrapper *AA = getAliasAnalysis();
    std::vector<const llvm::Value *> regions;
    std::vector<const llvm::Value *> resolved;
    if (AA)
      regions = getRegionsForFunction(*t.source->getParent());
    for (auto it = begin; it != t.source->end(); ++it) {
      const llvm::Instruction &I = *it;
      if (&I == t.stopBefore || I.isTerminator())
        break;
      if (I.getType()->isVoidTy()) {
        if (AA && llvm::isa<llvm::StoreInst>(&I)) {
          auto *SI = llvm::cast<llvm::StoreInst>(&I);
          const llvm::Value *ptr = SI->getPointerOperand();
          const llvm::Value *val = SI->getValueOperand();
          resolvePointerToRegions(AA, ptr, regions, resolved);
          for (const llvm::Value *r : resolved)
            out.setMemory(r, val);
        }
        continue;
      }
      if (I.getType()->isIntegerTy() || I.getType()->isPointerTy()) {
        if (AA && llvm::isa<llvm::AllocaInst>(&I)) {
          out.isolate(&I);
          // Region exists; content unknown until first store (no setMemory).
        } else if (AA && llvm::isa<llvm::LoadInst>(&I) &&
                   I.getType()->isIntegerTy()) {
          const llvm::Value *ptr =
              llvm::cast<llvm::LoadInst>(&I)->getPointerOperand();
          resolvePointerToRegions(AA, ptr, regions, resolved);
          std::vector<const llvm::Value *> reps;
          for (const llvm::Value *r : resolved) {
            if (const llvm::Value *rep = out.getMemory(r))
              reps.push_back(rep);
          }
          out.isolate(&I);
          for (const llvm::Value *rep : reps)
            out.unite(&I, rep);
        } else if (AA && llvm::isa<llvm::GetElementPtrInst>(&I)) {
          out.isolate(&I);
        } else if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
          const llvm::Value *src = Cast->getOperand(0);
          out.isolate(&I);
          out.unite(&I, src);
        } else if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(&I)) {
          const llvm::Value *rt = out.find(Sel->getTrueValue());
          const llvm::Value *rf = out.find(Sel->getFalseValue());
          out.isolate(&I);
          if (rt && rt == rf)
            out.unite(&I, rt);
        } else {
          out.isolate(&I);
        }
      }
    }
    out = refineForTakenEdge(t, std::move(out));
    return applyIncomingPhis(t, std::move(out));
  }
  EqState postCall(const Transition &t,
                   const EqState &callerState) const override {
    if (callerState.isBottom() || !t.call || !t.callee)
      return callerState;
    EqState projected;
    const llvm::Function *caller = t.source ? t.source->getParent() : nullptr;
    for (const llvm::Value *value : callerState.keys()) {
      if (!isFunctionLocalValue(caller, value))
        copyEquality(projected, callerState, value);
    }
    for (const auto &kv : callerState.memory()) {
      projected.ensure(kv.second);
      projected.setMemory(kv.first, kv.second);
    }
    unsigned actualIndex = 0;
    for (const llvm::Argument &formal : t.callee->args()) {
      if (actualIndex >= t.call->arg_size())
        break;
      projected.ensure(&formal);
      const llvm::Value *actual = t.call->getArgOperand(actualIndex);
      projected.ensure(callerState.find(actual));
      projected.unite(&formal, callerState.find(actual));
      ++actualIndex;
    }
    return projected;
  }
  EqState postReturn(const Transition &t, const EqState &callerState,
                     const EqState &calleeSummary) const override {
    if (callerState.isBottom() || calleeSummary.isBottom())
      return bottom();
    EqState out = callerState;
    const llvm::Function *callee = t.callee;
    for (const llvm::Value *value : calleeSummary.keys()) {
      if (!isFunctionLocalValue(callee, value))
        copyEquality(out, calleeSummary, value);
    }
    for (const auto &kv : calleeSummary.memory()) {
      out.ensure(kv.second);
      out.setMemory(kv.first, kv.second);
    }
    if (t.call && !t.call->getType()->isVoidTy()) {
      out.ensure(t.call);
      if (const llvm::Value *rep =
              returnedRepresentative(*callee, calleeSummary))
        out.unite(t.call, rep);
    }
    return out;
  }

private:
  static bool isFunctionLocalValue(const llvm::Function *F,
                                   const llvm::Value *V) {
    if (!F || !V)
      return false;
    if (const auto *I = llvm::dyn_cast<llvm::Instruction>(V))
      return I->getFunction() == F;
    if (const auto *A = llvm::dyn_cast<llvm::Argument>(V))
      return A->getParent() == F;
    return false;
  }

  static void copyEquality(EqState &out, const EqState &in,
                           const llvm::Value *value) {
    if (!value)
      return;
    const llvm::Value *rep = in.find(value);
    out.ensure(value);
    out.ensure(rep);
    out.unite(value, rep);
  }

  static EqState bottomState() { return EqState(true); }

  static const llvm::ConstantInt *knownConstant(const EqState &state,
                                                const llvm::Value *value) {
    if (!value)
      return nullptr;
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
      return constant;
    return llvm::dyn_cast<llvm::ConstantInt>(state.find(value));
  }

  static bool evaluatePredicate(llvm::CmpInst::Predicate predicate, int64_t lhs,
                                int64_t rhs) {
    switch (predicate) {
    case llvm::CmpInst::ICMP_EQ:
      return lhs == rhs;
    case llvm::CmpInst::ICMP_NE:
      return lhs != rhs;
    case llvm::CmpInst::ICMP_SLT:
      return lhs < rhs;
    case llvm::CmpInst::ICMP_SLE:
      return lhs <= rhs;
    case llvm::CmpInst::ICMP_SGT:
      return lhs > rhs;
    case llvm::CmpInst::ICMP_SGE:
      return lhs >= rhs;
    case llvm::CmpInst::ICMP_ULT:
      return static_cast<uint64_t>(lhs) < static_cast<uint64_t>(rhs);
    case llvm::CmpInst::ICMP_ULE:
      return static_cast<uint64_t>(lhs) <= static_cast<uint64_t>(rhs);
    case llvm::CmpInst::ICMP_UGT:
      return static_cast<uint64_t>(lhs) > static_cast<uint64_t>(rhs);
    case llvm::CmpInst::ICMP_UGE:
      return static_cast<uint64_t>(lhs) >= static_cast<uint64_t>(rhs);
    default:
      return false;
    }
  }

  static EqState refineForPredicate(EqState out, const llvm::ICmpInst &cmp,
                                    bool truthy) {
    const llvm::Value *lhsValue = cmp.getOperand(0);
    const llvm::Value *rhsValue = cmp.getOperand(1);
    const auto *lhsConst = knownConstant(out, lhsValue);
    const auto *rhsConst = knownConstant(out, rhsValue);
    llvm::CmpInst::Predicate predicate = cmp.getPredicate();
    if (!truthy)
      predicate = cmp.getInversePredicate();

    if (lhsConst && rhsConst) {
      if (lhsConst->getBitWidth() > 64 || rhsConst->getBitWidth() > 64)
        return out;
      return evaluatePredicate(predicate, lhsConst->getSExtValue(),
                               rhsConst->getSExtValue())
                 ? out
                 : bottomState();
    }

    switch (predicate) {
    case llvm::CmpInst::ICMP_EQ:
      out.ensure(lhsValue);
      out.ensure(rhsValue);
      out.unite(lhsValue, rhsValue);
      return out;
    case llvm::CmpInst::ICMP_NE:
      if (out.find(lhsValue) == out.find(rhsValue))
        return bottomState();
      return out;
    default:
      return out;
    }
  }

  static EqState refineForCondition(EqState out, const llvm::Value *condition,
                                    bool truthy) {
    if (out.isBottom() || !condition)
      return out;
    if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition))
      return (!constant->isZero() == truthy) ? out : bottomState();
    if (const auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(condition))
      return refineForPredicate(std::move(out), *cmp, truthy);

    const llvm::ConstantInt *known = knownConstant(out, condition);
    if (known)
      return (!known->isZero() == truthy) ? out : bottomState();
    out.ensure(condition);
    out.unite(condition,
              truthy ? llvm::ConstantInt::getTrue(condition->getContext())
                     : llvm::ConstantInt::getFalse(condition->getContext()));
    return out;
  }

  static EqState refineForSwitch(const Transition &t, EqState out,
                                 const llvm::SwitchInst &switchInst) {
    const llvm::BasicBlock *target = t.target;
    if (!target)
      return out;
    const llvm::ConstantInt *known =
        knownConstant(out, switchInst.getCondition());

    std::optional<int64_t> matchedCase;
    bool targetIsCase = false;
    for (const auto &caseHandle : switchInst.cases()) {
      if (caseHandle.getCaseSuccessor() != target)
        continue;
      targetIsCase = true;
      if (caseHandle.getCaseValue()->getBitWidth() > 64)
        return out;
      matchedCase = caseHandle.getCaseValue()->getSExtValue();
      break;
    }

    if (targetIsCase) {
      if (!matchedCase.has_value())
        return out;
      if (known && known->getSExtValue() != *matchedCase)
        return bottomState();
      out.ensure(switchInst.getCondition());
      out.unite(switchInst.getCondition(),
                llvm::ConstantInt::get(switchInst.getCondition()->getType(),
                                       *matchedCase, true));
      return out;
    }

    if (switchInst.getDefaultDest() != target)
      return out;
    if (known && known->getBitWidth() <= 64) {
      for (const auto &caseHandle : switchInst.cases()) {
        if (caseHandle.getCaseValue()->getBitWidth() > 64)
          continue;
        if (known->getSExtValue() == caseHandle.getCaseValue()->getSExtValue())
          return bottomState();
      }
    }
    return out;
  }

  static EqState refineForTakenEdge(const Transition &t, EqState out) {
    if (out.isBottom() || !t.source)
      return out;
    const llvm::Instruction *terminator = t.source->getTerminator();
    if (const auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
      if (!branch->isConditional())
        return out;
      if (branch->getSuccessor(0) == t.target)
        return refineForCondition(std::move(out), branch->getCondition(), true);
      if (branch->getSuccessor(1) == t.target)
        return refineForCondition(std::move(out), branch->getCondition(),
                                  false);
      return bottomState();
    }
    if (const auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator))
      return refineForSwitch(t, std::move(out), *switchInst);
    return out;
  }

  static const llvm::Value *returnedRepresentative(const llvm::Function &callee,
                                                   const EqState &summary) {
    const llvm::Value *result = nullptr;
    bool sawReturn = false;
    for (const llvm::BasicBlock &bb : callee) {
      const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (!ret || ret->getNumOperands() == 0)
        continue;
      const llvm::Value *rep = summary.find(ret->getReturnValue());
      if (!sawReturn) {
        result = rep;
        sawReturn = true;
        continue;
      }
      if (result != rep)
        return nullptr;
    }
    return sawReturn ? result : nullptr;
  }

  static const llvm::Value *
  incomingValueForPredecessor(const llvm::PHINode &phi,
                              const llvm::BasicBlock *pred) {
    if (!pred)
      return nullptr;
    const int index =
        phi.getBasicBlockIndex(const_cast<llvm::BasicBlock *>(pred));
    if (index < 0)
      return nullptr;
    return phi.getIncomingValue(static_cast<unsigned>(index));
  }

  static EqState applyIncomingPhis(const Transition &t, EqState out) {
    if (out.isBottom())
      return out;
    if (!t.source || !t.target || !t.landsAtBlockEntry())
      return out;
    std::vector<std::pair<const llvm::PHINode *, const llvm::Value *>> phis;
    std::unordered_set<const llvm::Value *> targetPhiSet;
    for (const llvm::Instruction &I : *t.target) {
      const auto *phi = llvm::dyn_cast<llvm::PHINode>(&I);
      if (!phi)
        break;
      phis.emplace_back(phi, incomingValueForPredecessor(*phi, t.source));
      targetPhiSet.insert(phi);
    }
    if (phis.empty())
      return out;
    std::vector<std::pair<const llvm::PHINode *, const llvm::Value *>>
        assignments;
    assignments.reserve(phis.size());
    std::unordered_map<const llvm::Value *, const llvm::Value *> nonPhiRep;
    for (const auto &entry : phis) {
      if (!entry.second) {
        assignments.emplace_back(entry.first, nullptr);
        continue;
      }
      const llvm::Value *oldRoot = out.find(entry.second);
      assignments.emplace_back(entry.first, oldRoot);
      if (!targetPhiSet.count(oldRoot)) {
        nonPhiRep.emplace(oldRoot, oldRoot);
      }
    }
    for (const llvm::Value *k : out.keys()) {
      if (targetPhiSet.count(k))
        continue;
      const llvm::Value *r = out.find(k);
      if (!nonPhiRep.count(r))
        nonPhiRep.emplace(r, k);
    }
    for (const auto &entry : phis)
      out.isolate(entry.first);
    for (const auto &kv : assignments) {
      out.ensure(kv.first);
      if (!kv.second)
        continue;
      auto repIt = nonPhiRep.find(kv.second);
      if (repIt != nonPhiRep.end()) {
        out.ensure(repIt->second);
        out.unite(kv.first, repIt->second);
      } else {
        nonPhiRep.emplace(kv.second, kv.first);
      }
    }
    return out;
  }

  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
  lotus::AliasAnalysisWrapper *aliasAnalysis_ = nullptr;
};

} // namespace sifa
} // namespace lotus

