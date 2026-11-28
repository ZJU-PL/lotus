//===-- Verification/Sifa/Domain/OctagonDomain.cpp ------------------------===//
//
// Instruction-level block transfer for Sifa Octagon domain.
// Applies sound over-approximating transfer: copy/constant/affine assignments
// update octagon constraints; non-linear ops and memory havoc the result.
// When alias analysis is set, Load/Store use region-based memory (IKOS/CLAM
// style).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Domain/OctagonDomain.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Verification/Sifa/RegionMemory.h"

#include <limits>
#include <unordered_map>

using namespace lotus::sifa;

namespace {

std::optional<int64_t> getConstant(const llvm::Value *V);
bool isFunctionLocalValue(const llvm::Function *F, const llvm::Value *v);

struct WorkingOctagon {
  bool isBottom = false;
  std::unordered_map<const llvm::Value *, std::size_t> varToIndex;
  OctagonMatrix matrix;
  std::unordered_map<const llvm::Value *, Interval> memory;

  explicit WorkingOctagon(const OctagonState &s)
      : isBottom(s.isBottom() || s.hasNegativeSelfLoop()),
        varToIndex(s.varToIndex()), matrix(s.matrix()), memory(s.memory()) {}

  OctagonState toState(bool closeAndCheck = true) && {
    if (isBottom)
      return OctagonState(true);
    if (closeAndCheck) {
      OctagonMatrix closed = matrix.strongClosure();
      if (closed.hasNegativeSelfLoop())
        return OctagonState(true);
      OctagonState out(std::move(varToIndex), std::move(closed), false);
      for (const auto &kv : memory)
        out.setMemory(kv.first, kv.second);
      return out;
    }
    OctagonState out(std::move(varToIndex), std::move(matrix), false);
    for (const auto &kv : memory)
      out.setMemory(kv.first, kv.second);
    return out;
  }
};

std::optional<std::size_t> ensureVar(WorkingOctagon &w, const llvm::Value *v) {
  if (!v || llvm::isa<llvm::Constant>(v))
    return std::nullopt;
  auto it = w.varToIndex.find(v);
  if (it != w.varToIndex.end())
    return it->second;
  const std::size_t n = w.varToIndex.size();
  w.varToIndex[v] = n;
  OctagonMatrix next(n + 1);
  const std::size_t oldDim = w.matrix.dim();
  for (std::size_t i = 0; i < oldDim; ++i) {
    for (std::size_t j = 0; j < oldDim; ++j) {
      auto c = w.matrix.get(i, j);
      if (c)
        next.set(i, j, *c);
    }
  }
  w.matrix = std::move(next);
  return n;
}

std::optional<std::size_t> getVarIndex(const WorkingOctagon &w,
                                       const llvm::Value *v) {
  auto it = w.varToIndex.find(v);
  if (it == w.varToIndex.end())
    return std::nullopt;
  return it->second;
}

int64_t floorDiv(int64_t numerator, int64_t denominator) {
  int64_t quotient = numerator / denominator;
  int64_t remainder = numerator % denominator;
  if (remainder != 0 && ((remainder > 0) != (denominator > 0)))
    --quotient;
  return quotient;
}

Interval intervalForClosedVar(const OctagonMatrix &closed, std::size_t idx) {
  std::optional<int64_t> lower;
  std::optional<int64_t> upper;
  if (auto lo = closed.get(2 * idx, 2 * idx + 1))
    lower = -floorDiv(*lo, 2);
  if (auto hi = closed.get(2 * idx + 1, 2 * idx))
    upper = floorDiv(*hi, 2);
  if (lower && upper && *lower > *upper)
    return Interval::bottom();
  return Interval{lower, upper, false};
}

Interval intervalForValue(const OctagonState &s, const llvm::Value *v) {
  if (!v)
    return Interval::top();
  if (const auto constant = getConstant(v))
    return Interval::point(*constant);
  if (s.isBottom())
    return Interval::bottom();
  auto it = s.varToIndex().find(v);
  if (it == s.varToIndex().end())
    return Interval::top();
  const OctagonMatrix &closed = s.closedMatrix();
  if (closed.hasNegativeSelfLoop())
    return Interval::bottom();
  return intervalForClosedVar(closed, it->second);
}

Interval intervalForWorkingValue(const WorkingOctagon &w,
                                 const llvm::Value *v) {
  if (!v)
    return Interval::top();
  if (const auto constant = getConstant(v))
    return Interval::point(*constant);
  if (w.isBottom)
    return Interval::bottom();
  auto idx = getVarIndex(w, v);
  if (!idx)
    return Interval::top();
  const OctagonMatrix closed = w.matrix.strongClosure();
  if (closed.hasNegativeSelfLoop())
    return Interval::bottom();
  return intervalForClosedVar(closed, *idx);
}

/// Difference constraint v_lhs - v_rhs <= bound in DBM form:
///   (+v_lhs) - (+v_rhs) <= bound  ==> m[2*rhs, 2*lhs]     <= bound
///   (-v_rhs) - (-v_lhs) <= bound  ==> m[2*lhs+1, 2*rhs+1] <= bound
void setDifferenceMin(OctagonMatrix &m, std::size_t lhsIdx,
                      std::size_t rhsIdx, int64_t bound) {
  m.setMin(2 * rhsIdx, 2 * lhsIdx, bound);
  m.setMin(2 * lhsIdx + 1, 2 * rhsIdx + 1, bound);
}

void havocWorkingVar(WorkingOctagon &w, const llvm::Value *v) {
  if (!v)
    return;
  auto it = w.varToIndex.find(v);
  if (it != w.varToIndex.end())
    w.matrix.relaxVarInPlace(it->second);
}

void assignWorkingConstant(WorkingOctagon &w, const llvm::Value *res,
                           int64_t c) {
  auto ri = ensureVar(w, res);
  if (!ri)
    return;
  std::size_t r = *ri;
  w.matrix.relaxVarInPlace(r);
  int64_t twoC, negTwoC;
  if (__builtin_mul_overflow(c, int64_t{2}, &twoC) ||
      __builtin_sub_overflow(int64_t{0}, twoC, &negTwoC))
    return;
  w.matrix.set(2 * r + 1, 2 * r, twoC);
  w.matrix.set(2 * r, 2 * r + 1, negTwoC);
}

void assignWorkingCopy(WorkingOctagon &w, const llvm::Value *res,
                       const llvm::Value *src) {
  if (!res || llvm::isa<llvm::Constant>(res))
    return;
  if (res == src)
    return;
  if (auto c = getConstant(src)) {
    assignWorkingConstant(w, res, *c);
    return;
  }
  auto ri = ensureVar(w, res);
  if (!ri)
    return;
  w.matrix.relaxVarInPlace(*ri);
  if (!src)
    return;
  auto si = ensureVar(w, src);
  if (!si)
    return;
  setDifferenceMin(w.matrix, *ri, *si, 0);
  setDifferenceMin(w.matrix, *si, *ri, 0);
}

void assignWorkingAffine(WorkingOctagon &w, const llvm::Value *res,
                         const llvm::Value *src, int64_t k) {
  int64_t negK;
  if (__builtin_sub_overflow(int64_t{0}, k, &negK)) {
    havocWorkingVar(w, res);
    return;
  }
  if (res == src) {
    if (k != 0)
      havocWorkingVar(w, res);
    return;
  }
  if (auto c = getConstant(src)) {
    int64_t sum;
    if (__builtin_add_overflow(*c, k, &sum))
      havocWorkingVar(w, res);
    else
      assignWorkingConstant(w, res, sum);
    return;
  }
  auto ri = ensureVar(w, res);
  if (!ri)
    return;
  w.matrix.relaxVarInPlace(*ri);
  auto si = ensureVar(w, src);
  if (!si)
    return;
  setDifferenceMin(w.matrix, *ri, *si, k);
  setDifferenceMin(w.matrix, *si, *ri, negK);
}

void assignWorkingInterval(WorkingOctagon &w, const llvm::Value *res,
                           const Interval &interval) {
  if (interval.isBottom()) {
    w.isBottom = true;
    return;
  }
  if (interval.isTop()) {
    havocWorkingVar(w, res);
    return;
  }
  if (interval.isPoint() && interval.lo) {
    assignWorkingConstant(w, res, *interval.lo);
    return;
  }
  auto ri = ensureVar(w, res);
  if (!ri)
    return;
  std::size_t r = *ri;
  w.matrix.relaxVarInPlace(r);
  if (interval.hi) {
    int64_t twoHi;
    if (!__builtin_mul_overflow(*interval.hi, int64_t{2}, &twoHi))
      w.matrix.set(2 * r + 1, 2 * r, twoHi);
  }
  if (interval.lo) {
    int64_t twoLo, negTwoLo;
    if (!__builtin_mul_overflow(*interval.lo, int64_t{2}, &twoLo) &&
        !__builtin_sub_overflow(int64_t{0}, twoLo, &negTwoLo))
      w.matrix.set(2 * r, 2 * r + 1, negTwoLo);
  }
}

void tightenWorkingInterval(WorkingOctagon &w, const llvm::Value *value,
                            const Interval &interval) {
  if (!value || llvm::isa<llvm::Constant>(value) || w.isBottom)
    return;
  if (interval.isBottom()) {
    w.isBottom = true;
    return;
  }
  if (interval.isTop())
    return;
  auto ri = ensureVar(w, value);
  if (!ri)
    return;
  std::size_t r = *ri;
  if (interval.hi) {
    int64_t twoHi;
    if (!__builtin_mul_overflow(*interval.hi, int64_t{2}, &twoHi))
      w.matrix.setMin(2 * r + 1, 2 * r, twoHi);
  }
  if (interval.lo) {
    int64_t twoLo, negTwoLo;
    if (!__builtin_mul_overflow(*interval.lo, int64_t{2}, &twoLo) &&
        !__builtin_sub_overflow(int64_t{0}, twoLo, &negTwoLo))
      w.matrix.setMin(2 * r, 2 * r + 1, negTwoLo);
  }
}

/// Get constant from \p V if ConstantInt (fits in 64 bits), else None.
std::optional<int64_t> getConstant(const llvm::Value *V) {
  const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
  if (!C || C->getBitWidth() > 64)
    return std::nullopt;
  return C->getSExtValue();
}

bool isFunctionLocalValue(const llvm::Function *F, const llvm::Value *v) {
  if (!F || !v)
    return false;
  if (const auto *I = llvm::dyn_cast<llvm::Instruction>(v))
    return I->getFunction() == F;
  if (const auto *A = llvm::dyn_cast<llvm::Argument>(v))
    return A->getParent() == F;
  return false;
}

llvm::BasicBlock::const_iterator
segmentBegin(const llvm::BasicBlock *bb,
             const llvm::Instruction *segmentStart) {
  if (segmentStart) {
    return segmentStart->getIterator();
  }
  auto it = bb->begin();
  while (it != bb->end() && llvm::isa<llvm::PHINode>(&*it)) {
    ++it;
  }
  return it;
}

const llvm::Value *incomingValueForPredecessor(const llvm::PHINode &phi,
                                               const llvm::BasicBlock *pred) {
  if (!pred)
    return nullptr;
  const int index =
      phi.getBasicBlockIndex(const_cast<llvm::BasicBlock *>(pred));
  if (index < 0)
    return nullptr;
  return phi.getIncomingValue(static_cast<unsigned>(index));
}

OctagonState applyIncomingPhis(const Transition &t, OctagonState out) {
  if (out.isBottom() || out.hasNegativeSelfLoop())
    return OctagonState(true);
  if (!t.source || !t.target || !t.landsAtBlockEntry())
    return out;

  struct PhiUpdate {
    const llvm::PHINode *phi;
    const llvm::Value *incoming;
    Interval interval;
    bool incomingTracked;
  };
  std::vector<PhiUpdate> updates;
  for (const llvm::Instruction &I : *t.target) {
    const auto *phi = llvm::dyn_cast<llvm::PHINode>(&I);
    if (!phi)
      break;
    const llvm::Value *incoming = incomingValueForPredecessor(*phi, t.source);
    const Interval incomingInterval = intervalForValue(out, incoming);
    if (incomingInterval.isBottom())
      return OctagonState(true);
    updates.push_back(PhiUpdate{
        phi, incoming, incomingInterval,
        incoming && !llvm::isa<llvm::Constant>(incoming) &&
            out.varToIndex().count(incoming) > 0});
  }
  if (updates.empty())
    return out;

  WorkingOctagon w(out);
  for (const auto &upd : updates) {
    if (!upd.interval.isTop()) {
      assignWorkingInterval(w, upd.phi, upd.interval);
    } else if (upd.incomingTracked) {
      assignWorkingCopy(w, upd.phi, upd.incoming);
    } else {
      havocWorkingVar(w, upd.phi);
    }
    if (w.isBottom)
      return OctagonState(true);
  }
  return std::move(w).toState(/*closeAndCheck=*/true);
}

bool evaluatePredicate(llvm::CmpInst::Predicate predicate, int64_t lhs,
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

OctagonState addDifferenceConstraint(const OctagonState &s,
                                     const llvm::Value *lhs,
                                     const llvm::Value *rhs, int64_t bound) {
  WorkingOctagon w(s);
  auto lhsIndex = ensureVar(w, lhs);
  auto rhsIndex = ensureVar(w, rhs);
  if (!lhsIndex || !rhsIndex)
    return s;
  setDifferenceMin(w.matrix, *lhsIndex, *rhsIndex, bound);
  return std::move(w).toState(/*closeAndCheck=*/true);
}

OctagonState constrainToInterval(OctagonState state, const llvm::Value *value,
                                 const Interval &interval) {
  if (!value || llvm::isa<llvm::Constant>(value))
    return state;
  WorkingOctagon w(state);
  tightenWorkingInterval(w, value, interval);
  return std::move(w).toState(/*closeAndCheck=*/true);
}

OctagonState refineForPredicate(OctagonState out, const llvm::ICmpInst &cmp,
                                bool truthy) {
  const llvm::Value *lhsValue = cmp.getOperand(0);
  const llvm::Value *rhsValue = cmp.getOperand(1);
  const Interval lhs = intervalForValue(out, lhsValue);
  const Interval rhs = intervalForValue(out, rhsValue);
  if (lhs.isBottom() || rhs.isBottom())
    return OctagonState(true);

  llvm::CmpInst::Predicate predicate = cmp.getPredicate();
  if (!truthy)
    predicate = cmp.getInversePredicate();
  if (llvm::CmpInst::isUnsigned(predicate) &&
      ((!lhs.lo.has_value() || *lhs.lo < 0) ||
       (!rhs.lo.has_value() || *rhs.lo < 0))) {
    return out;
  }

  if (lhs.isPoint() && rhs.isPoint()) {
    return evaluatePredicate(predicate, *lhs.lo, *rhs.lo) ? out
                                                          : OctagonState(true);
  }

  switch (predicate) {
  case llvm::CmpInst::ICMP_EQ:
    if (const auto c = getConstant(lhsValue))
      return constrainToInterval(std::move(out), rhsValue, Interval::point(*c));
    if (const auto c = getConstant(rhsValue))
      return constrainToInterval(std::move(out), lhsValue, Interval::point(*c));
    {
      WorkingOctagon w(out);
      auto l = ensureVar(w, lhsValue);
      auto r = ensureVar(w, rhsValue);
      if (l && r) {
        setDifferenceMin(w.matrix, *l, *r, 0);
        setDifferenceMin(w.matrix, *r, *l, 0);
      }
      return std::move(w).toState(/*closeAndCheck=*/true);
    }
  case llvm::CmpInst::ICMP_NE:
    if (lhs.isPoint() && rhs.isPoint() && lhs.lo == rhs.lo)
      return OctagonState(true);
    return out;
  case llvm::CmpInst::ICMP_SLT:
  case llvm::CmpInst::ICMP_ULT:
    if (const auto c = getConstant(rhsValue)) {
      if (*c == std::numeric_limits<int64_t>::min())
        return OctagonState(true);
      return constrainToInterval(std::move(out), lhsValue,
                                 Interval{std::nullopt, *c - 1, false});
    }
    if (const auto c = getConstant(lhsValue)) {
      if (*c == std::numeric_limits<int64_t>::max())
        return OctagonState(true);
      return constrainToInterval(std::move(out), rhsValue,
                                 Interval{*c + 1, std::nullopt, false});
    }
    return addDifferenceConstraint(std::move(out), lhsValue, rhsValue, -1);
  case llvm::CmpInst::ICMP_SLE:
  case llvm::CmpInst::ICMP_ULE:
    if (const auto c = getConstant(rhsValue))
      return constrainToInterval(std::move(out), lhsValue,
                                 Interval{std::nullopt, *c, false});
    if (const auto c = getConstant(lhsValue))
      return constrainToInterval(std::move(out), rhsValue,
                                 Interval{*c, std::nullopt, false});
    return addDifferenceConstraint(std::move(out), lhsValue, rhsValue, 0);
  case llvm::CmpInst::ICMP_SGT:
  case llvm::CmpInst::ICMP_UGT:
    if (const auto c = getConstant(rhsValue)) {
      if (*c == std::numeric_limits<int64_t>::max())
        return OctagonState(true);
      return constrainToInterval(std::move(out), lhsValue,
                                 Interval{*c + 1, std::nullopt, false});
    }
    if (const auto c = getConstant(lhsValue)) {
      if (*c == std::numeric_limits<int64_t>::min())
        return OctagonState(true);
      return constrainToInterval(std::move(out), rhsValue,
                                 Interval{std::nullopt, *c - 1, false});
    }
    return addDifferenceConstraint(std::move(out), rhsValue, lhsValue, -1);
  case llvm::CmpInst::ICMP_SGE:
  case llvm::CmpInst::ICMP_UGE:
    if (const auto c = getConstant(rhsValue))
      return constrainToInterval(std::move(out), lhsValue,
                                 Interval{*c, std::nullopt, false});
    if (const auto c = getConstant(lhsValue))
      return constrainToInterval(std::move(out), rhsValue,
                                 Interval{std::nullopt, *c, false});
    return addDifferenceConstraint(std::move(out), rhsValue, lhsValue, 0);
  default:
    return out;
  }
}

OctagonState refineForCondition(OctagonState out, const llvm::Value *condition,
                                bool truthy) {
  if (out.isBottom() || !condition)
    return out;
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(condition))
    return (!constant->isZero() == truthy) ? out : OctagonState(true);
  if (const auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(condition))
    return refineForPredicate(std::move(out), *cmp, truthy);
  return constrainToInterval(std::move(out), condition,
                             Interval::point(truthy ? 1 : 0));
}

OctagonState refineForSwitch(const Transition &t, OctagonState out,
                             const llvm::SwitchInst &switchInst) {
  const Interval condition = intervalForValue(out, switchInst.getCondition());
  const llvm::BasicBlock *target = t.target;
  if (!target)
    return out;

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
    if (condition.isPoint() && condition.lo.has_value() &&
        *condition.lo != *matchedCase)
      return OctagonState(true);
    return constrainToInterval(std::move(out), switchInst.getCondition(),
                               Interval::point(*matchedCase));
  }

  if (switchInst.getDefaultDest() != target)
    return out;
  if (condition.isPoint() && condition.lo.has_value()) {
    for (const auto &caseHandle : switchInst.cases()) {
      if (caseHandle.getCaseValue()->getBitWidth() > 64)
        continue;
      if (*condition.lo == caseHandle.getCaseValue()->getSExtValue())
        return OctagonState(true);
    }
  }
  return out;
}

OctagonState refineForTakenEdge(const Transition &t, OctagonState out) {
  if (out.isBottom() || !t.source)
    return out;
  const llvm::Instruction *terminator = t.source->getTerminator();
  if (const auto *branch = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
    if (!branch->isConditional())
      return out;
    if (branch->getSuccessor(0) == t.target)
      return refineForCondition(std::move(out), branch->getCondition(), true);
    if (branch->getSuccessor(1) == t.target)
      return refineForCondition(std::move(out), branch->getCondition(), false);
    return OctagonState(true);
  }
  if (const auto *switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator))
    return refineForSwitch(t, std::move(out), *switchInst);
  return out;
}

OctagonState projectCallState(const Transition &t,
                              const OctagonState &callerState) {
  if (callerState.isBottom() || callerState.hasNegativeSelfLoop() ||
      !t.call || !t.callee)
    return callerState.isBottom() || callerState.hasNegativeSelfLoop()
               ? OctagonState(true)
               : callerState;
  WorkingOctagon projected{OctagonState(false)};
  const llvm::Function *caller = t.source ? t.source->getParent() : nullptr;
  for (const auto &kv : callerState.varToIndex()) {
    if (!isFunctionLocalValue(caller, kv.first))
      assignWorkingInterval(projected, kv.first,
                            intervalForValue(callerState, kv.first));
  }
  for (const auto &kv : callerState.memory())
    projected.memory[kv.first] = kv.second;
  unsigned actualIndex = 0;
  for (const llvm::Argument &formal : t.callee->args()) {
    if (actualIndex >= t.call->arg_size())
      break;
    assignWorkingInterval(
        projected, &formal,
        intervalForValue(callerState, t.call->getArgOperand(actualIndex)));
    ++actualIndex;
  }
  return std::move(projected).toState(/*closeAndCheck=*/true);
}

Interval returnedInterval(const llvm::Function &callee,
                          const OctagonState &calleeSummary) {
  Interval result = Interval::bottom();
  bool sawReturn = false;
  for (const llvm::BasicBlock &bb : callee) {
    const auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
    if (!ret || ret->getNumOperands() == 0)
      continue;
    const Interval value =
        intervalForValue(calleeSummary, ret->getReturnValue());
    result = sawReturn ? result.join(value) : value;
    sawReturn = true;
  }
  return sawReturn ? result : Interval::top();
}

OctagonState mergeReturnState(const Transition &t,
                              const OctagonState &callerState,
                              const OctagonState &calleeSummary) {
  if (callerState.isBottom() || callerState.hasNegativeSelfLoop() ||
      calleeSummary.isBottom() || calleeSummary.hasNegativeSelfLoop())
    return OctagonState(true);
  WorkingOctagon out(callerState);
  const llvm::Function *callee = t.callee;
  for (const auto &kv : calleeSummary.varToIndex()) {
    if (!isFunctionLocalValue(callee, kv.first))
      assignWorkingInterval(out, kv.first,
                            intervalForValue(calleeSummary, kv.first));
  }
  for (const auto &kv : calleeSummary.memory())
    out.memory[kv.first] = kv.second;
  if (t.call && !t.call->getType()->isVoidTy())
    assignWorkingInterval(out, t.call,
                          returnedInterval(*callee, calleeSummary));
  return std::move(out).toState(/*closeAndCheck=*/true);
}

} // namespace

OctagonState OctagonDomain::applyBlockTransfer(llvm::BasicBlock *bb,
                                               const OctagonState &in) const {
  return applyBlockTransfer(bb, in, nullptr, nullptr);
}

OctagonState
OctagonDomain::applyBlockTransfer(llvm::BasicBlock *bb, const OctagonState &in,
                                  const llvm::Instruction *segmentStart,
                                  const llvm::Instruction *stopBefore) const {
  if (in.isBottom() || in.hasNegativeSelfLoop())
    return OctagonState(true);
  WorkingOctagon out(in);

  lotus::AliasAnalysisWrapper *AA = getAliasAnalysis();
  std::vector<const llvm::Value *> regions;
  std::vector<const llvm::Value *> resolved;
  if (AA)
    regions = getRegionsForFunction(*bb->getParent());

  for (llvm::BasicBlock::const_iterator it = segmentBegin(bb, segmentStart),
                                        end = bb->end();
       it != end; ++it) {
    const llvm::Instruction &I = *it;
    if (&I == stopBefore || I.isTerminator())
      break;
    if (I.getType()->isVoidTy()) {
      if (AA && llvm::isa<llvm::StoreInst>(&I)) {
        auto *SI = llvm::cast<llvm::StoreInst>(&I);
        const llvm::Value *ptr = SI->getPointerOperand();
        Interval val = Interval::top();
        resolvePointerToRegions(AA, ptr, regions, resolved);
        for (const llvm::Value *r : resolved) {
          auto curIt = out.memory.find(r);
          out.memory[r] =
              curIt != out.memory.end() ? curIt->second.join(val) : val;
        }
      }
      continue;
    }
    if (!I.getType()->isIntegerTy() && !I.getType()->isPointerTy())
      continue;

    const llvm::Value *res = &I;
    if (AA && llvm::isa<llvm::AllocaInst>(&I)) {
      havocWorkingVar(out, res);
      out.memory[&I] = Interval::top();
      continue;
    }
    if (AA && llvm::isa<llvm::LoadInst>(&I) && I.getType()->isIntegerTy()) {
      const llvm::Value *ptr =
          llvm::cast<llvm::LoadInst>(&I)->getPointerOperand();
      resolvePointerToRegions(AA, ptr, regions, resolved);
      Interval loadVal = Interval::bottom();
      for (const llvm::Value *r : resolved) {
        auto curIt = out.memory.find(r);
        Interval ir = curIt != out.memory.end() ? curIt->second : Interval::top();
        loadVal = loadVal.isBottom() ? ir : loadVal.join(ir);
      }
      if (loadVal.isBottom())
        loadVal = Interval::top();
      assignWorkingInterval(out, res, loadVal);
      if (out.isBottom)
        return OctagonState(true);
      continue;
    }
    if (AA && llvm::isa<llvm::GetElementPtrInst>(&I)) {
      havocWorkingVar(out, res);
      continue;
    }

    switch (I.getOpcode()) {
    case llvm::Instruction::PHI:
    case llvm::Instruction::Select:
    case llvm::Instruction::ICmp:
      havocWorkingVar(out, res);
      break;
    case llvm::Instruction::Add: {
      const unsigned bw = I.getType()->getIntegerBitWidth();
      auto c0 = getConstant(I.getOperand(0));
      auto c1 = getConstant(I.getOperand(1));
      if (c0 && c1) {
        int64_t sum;
        if (__builtin_add_overflow(*c0, *c1, &sum)) {
          havocWorkingVar(out, res);
        } else {
          if (bw < 64 && bw >= 1) {
            uint64_t mask = (uint64_t{1} << bw) - 1;
            uint64_t u = static_cast<uint64_t>(sum) & mask;
            if (u & (uint64_t{1} << (bw - 1)))
              u |= ~mask;
            sum = static_cast<int64_t>(u);
          }
          assignWorkingConstant(out, res, sum);
        }
      } else if (bw >= 32 || I.hasNoSignedWrap()) {
        if (bw == 1) {
          havocWorkingVar(out, res);
        } else if (c1 && *c1 == 0) {
          assignWorkingCopy(out, res, I.getOperand(0));
        } else if (c1) {
          assignWorkingAffine(out, res, I.getOperand(0), *c1);
        } else if (c0 && *c0 == 0) {
          assignWorkingCopy(out, res, I.getOperand(1));
        } else if (c0) {
          assignWorkingAffine(out, res, I.getOperand(1), *c0);
        } else {
          havocWorkingVar(out, res);
        }
      } else {
        havocWorkingVar(out, res);
      }
      break;
    }
    case llvm::Instruction::Sub: {
      const unsigned bw = I.getType()->getIntegerBitWidth();
      auto c0 = getConstant(I.getOperand(0));
      auto c1 = getConstant(I.getOperand(1));
      if (c0 && c1) {
        int64_t diff;
        if (__builtin_sub_overflow(*c0, *c1, &diff)) {
          havocWorkingVar(out, res);
        } else {
          if (bw < 64 && bw >= 1) {
            uint64_t mask = (uint64_t{1} << bw) - 1;
            uint64_t u = static_cast<uint64_t>(diff) & mask;
            if (u & (uint64_t{1} << (bw - 1)))
              u |= ~mask;
            diff = static_cast<int64_t>(u);
          }
          assignWorkingConstant(out, res, diff);
        }
      } else if (bw >= 32 || I.hasNoSignedWrap()) {
        if (bw == 1) {
          havocWorkingVar(out, res);
        } else if (c1 && *c1 == 0) {
          assignWorkingCopy(out, res, I.getOperand(0));
        } else if (c1) {
          int64_t negC1;
          if (__builtin_sub_overflow(int64_t{0}, *c1, &negC1))
            havocWorkingVar(out, res);
          else
            assignWorkingAffine(out, res, I.getOperand(0), negC1);
        } else {
          havocWorkingVar(out, res);
        }
      } else {
        havocWorkingVar(out, res);
      }
      break;
    }
    case llvm::Instruction::Trunc: {
      const llvm::Value *op0 = I.getOperand(0);
      const unsigned w = I.getType()->getIntegerBitWidth();
      if (w == 0 || w > 64) {
        havocWorkingVar(out, res);
        break;
      }
      if (auto c = getConstant(op0)) {
        if (w < 64) {
          uint64_t mask = (uint64_t{1} << w) - 1;
          assignWorkingConstant(
              out, res, static_cast<int64_t>(static_cast<uint64_t>(*c) & mask));
        } else {
          assignWorkingConstant(out, res, *c);
        }
        break;
      }
      if (w < 64) {
        const int64_t maxVal = static_cast<int64_t>((uint64_t{1} << w) - 1);
        const Interval iv = intervalForWorkingValue(out, op0);
        if (iv.isBottom())
          return OctagonState(true);
        if (iv.lo && iv.hi && *iv.lo >= 0 && *iv.hi <= maxVal) {
          assignWorkingCopy(out, res, op0);
        } else {
          assignWorkingInterval(out, res, Interval{0, maxVal, false});
        }
      } else {
        assignWorkingCopy(out, res, op0);
      }
      break;
    }
    case llvm::Instruction::ZExt: {
      const llvm::Value *op0 = I.getOperand(0);
      if (!op0->getType()->isIntegerTy() ||
          op0->getType()->getIntegerBitWidth() > 64 ||
          I.getType()->getIntegerBitWidth() > 64) {
        havocWorkingVar(out, res);
        break;
      }
      const unsigned wIn = op0->getType()->getIntegerBitWidth();
      if (auto c = getConstant(op0)) {
        if (wIn < 64) {
          uint64_t mask = (uint64_t{1} << wIn) - 1;
          assignWorkingConstant(
              out, res, static_cast<int64_t>(static_cast<uint64_t>(*c) & mask));
        } else {
          assignWorkingConstant(out, res, *c);
        }
        break;
      }
      if (wIn < 64) {
        const int64_t maxIn = static_cast<int64_t>((uint64_t{1} << wIn) - 1);
        const Interval iv = intervalForWorkingValue(out, op0);
        if (iv.isBottom())
          return OctagonState(true);
        if (iv.lo && *iv.lo >= 0 && (!iv.hi || *iv.hi <= maxIn)) {
          assignWorkingCopy(out, res, op0);
        } else {
          assignWorkingInterval(out, res, Interval{0, maxIn, false});
        }
      } else {
        havocWorkingVar(out, res);
      }
      break;
    }
    case llvm::Instruction::SExt:
    case llvm::Instruction::BitCast: {
      const llvm::Value *op0 = I.getOperand(0);
      if (auto c = getConstant(op0))
        assignWorkingConstant(out, res, *c);
      else if (op0->getType()->isIntegerTy() && I.getType()->isIntegerTy() &&
               op0->getType()->getIntegerBitWidth() <= 64 &&
               I.getType()->getIntegerBitWidth() <= 64)
        assignWorkingCopy(out, res, op0);
      else
        havocWorkingVar(out, res);
      break;
    }
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::IntToPtr:
    default:
      havocWorkingVar(out, res);
      break;
    }
  }
  return std::move(out).toState(/*closeAndCheck=*/true);
}

OctagonState OctagonDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                                const OctagonState &in) const {
  return applyBlockWiseHavoc(bb, in, nullptr, nullptr);
}

OctagonState
OctagonDomain::applyBlockWiseHavoc(llvm::BasicBlock *bb, const OctagonState &in,
                                   const llvm::Instruction *segmentStart,
                                   const llvm::Instruction *stopBefore) const {
  if (in.isBottom() || in.hasNegativeSelfLoop())
    return OctagonState(true);
  WorkingOctagon out(in);
  for (llvm::BasicBlock::const_iterator it = segmentBegin(bb, segmentStart),
                                        end = bb->end();
       it != end; ++it) {
    const llvm::Instruction &I = *it;
    if (&I == stopBefore || I.isTerminator())
      break;
    if (I.getType()->isVoidTy())
      continue;
    if (I.getType()->isIntegerTy() || I.getType()->isPointerTy())
      havocWorkingVar(out, &I);
  }
  return std::move(out).toState(/*closeAndCheck=*/false);
}

OctagonState OctagonDomain::post(const Transition &t,
                                 const OctagonState &in) const {
  if (in.isBottom() || in.hasNegativeSelfLoop())
    return OctagonState(true);
  if (t.kind == TransitionKind::Marker)
    return in;
  if (t.kind == TransitionKind::EnterCall)
    return in;
  if (t.kind == TransitionKind::ReturnSummary)
    return in;
  if (t.kind != TransitionKind::Edge || !t.source)
    return in;
  OctagonState out =
      blockTransferPolicy_ && blockTransferPolicy_->useBlockWise(t.source)
          ? applyBlockWiseHavoc(t.source, in, t.segmentStart, t.stopBefore)
          : applyBlockTransfer(t.source, in, t.segmentStart, t.stopBefore);
  out = refineForTakenEdge(t, std::move(out));
  return applyIncomingPhis(t, std::move(out));
}

OctagonState OctagonDomain::postCall(const Transition &t,
                                     const OctagonState &callerState) const {
  return projectCallState(t, callerState);
}

OctagonState
OctagonDomain::postReturn(const Transition &t, const OctagonState &callerState,
                          const OctagonState &calleeSummary) const {
  return mergeReturnState(t, callerState, calleeSummary);
}

void OctagonState::print(llvm::raw_ostream &out) const {
  if (isBottom_) {
    out << "  (bottom)\n";
    return;
  }
  out << "  variables: " << varToIndex_.size();
  if (!memory_.empty())
    out << ", memory regions: " << memory_.size();
  out << "\n";
}
