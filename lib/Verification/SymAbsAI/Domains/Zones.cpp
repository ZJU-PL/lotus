// TODO: by LLM; to be checked. 
// For example, what is the precise definition of "zone domain over bit-vector semantics"
#include "Verification/SymAbsAI/Domains/Zones.h"

#include "Verification/SymAbsAI/Core/Foundation/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Foundation/ParamStrategy.h"
#include "Verification/SymAbsAI/Core/Integration/FunctionContext.h"
// #include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

#include <z3++.h>

using namespace symabs_ai;
using namespace symabs_ai::domains;
using std::unique_ptr;

static const int64_t INF = std::numeric_limits<int64_t>::max();
static const int64_t NINF = std::numeric_limits<int64_t>::min();

namespace {
uint64_t bitMask(unsigned bw) {
  assert(bw > 0 && bw <= 64);
  return bw == 64 ? ~uint64_t(0) : ((uint64_t(1) << bw) - 1);
}

int64_t signedFromBitPattern(uint64_t value) {
  int64_t result;
  static_assert(sizeof(result) == sizeof(value), "unexpected int64_t size");
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

int64_t signedResidue(uint64_t value, unsigned bw) {
  value &= bitMask(bw);

  if (bw < 64) {
    uint64_t sign_bit = uint64_t(1) << (bw - 1);
    if (value & sign_bit)
      value |= ~bitMask(bw);
  }

  return signedFromBitPattern(value);
}
} // namespace

bool Zone::isJoinableWith(const AbstractValue &other) const {
  if (auto *o = dynamic_cast<const Zone *>(&other))
    return o->Left_ == Left_ && o->Right_ == Right_;
  return false;
}

bool Zone::tighten(int64_t new_lower, int64_t new_upper) {
  if (isBottom())
    return false;
  if (isTop()) {
    Top_ = false;
    Lower_ = new_lower;
    Upper_ = new_upper;
    checkConsistency();
    return !isBottom();
  }

  int64_t old_lower = Lower_;
  int64_t old_upper = Upper_;

  Lower_ = std::max(Lower_, new_lower);
  Upper_ = std::min(Upper_, new_upper);
  checkConsistency();

  return (Lower_ != old_lower || Upper_ != old_upper) && !isBottom();
}

bool Zone::joinWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Zone &>(av_other);
  assert(isJoinableWith(other));

  if (isBottom()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Upper_ = other.Upper_;
    Lower_ = other.Lower_;
    return !other.isBottom();
  }
  if (other.isBottom())
    return false;

  if (isTop())
    return false;
  if (other.isTop()) {
    bool changed = !isTop();
    havoc();
    return changed;
  }

  // Join: widen bounds to include both
  int64_t new_lower = std::min(Lower_, other.Lower_);
  int64_t new_upper = std::max(Upper_, other.Upper_);

  bool changed = (new_lower != Lower_ || new_upper != Upper_);
  Lower_ = new_lower;
  Upper_ = new_upper;

  return changed;
}

bool Zone::meetWith(const AbstractValue &av_other) {
  auto &other = dynamic_cast<const Zone &>(av_other);
  assert(isJoinableWith(other));

  if (isTop()) {
    Top_ = other.Top_;
    Bottom_ = other.Bottom_;
    Upper_ = other.Upper_;
    Lower_ = other.Lower_;
    return !other.isTop();
  }
  if (other.isTop())
    return false;

  if (other.isBottom()) {
    bool changed = !isBottom();
    resetToBottom();
    return changed;
  }

  // Meet: narrow bounds to intersection
  int64_t new_lower = std::max(Lower_, other.Lower_);
  int64_t new_upper = std::min(Upper_, other.Upper_);

  bool changed = (new_lower != Lower_ || new_upper != Upper_);
  Lower_ = new_lower;
  Upper_ = new_upper;
  checkConsistency();

  return changed;
}

bool Zone::updateWith(const ConcreteState &cstate) {
  unsigned bw = Fctx_.sortForType(Left_->getType()).bv_size();
  uint64_t left = (uint64_t)cstate[Left_];
  uint64_t right = (uint64_t)cstate[Right_];
  int64_t diff = signedResidue(left - right, bw);

  if (isBottom()) {
    Bottom_ = false;
    Top_ = false;
    Lower_ = diff;
    Upper_ = diff;
    PrevLower_ = Lower_;
    PrevUpper_ = Upper_;
    return true;
  }

  if (isTop()) {
    Top_ = false;
    Lower_ = diff;
    Upper_ = diff;
    PrevLower_ = Lower_;
    PrevUpper_ = Upper_;
    return true;
  }

  PrevLower_ = Lower_;
  PrevUpper_ = Upper_;

  // Widen bounds if needed to include new concrete value
  int64_t old_lower = Lower_;
  int64_t old_upper = Upper_;

  if (diff < Lower_)
    Lower_ = diff;
  if (diff > Upper_)
    Upper_ = diff;

  return (Lower_ != old_lower || Upper_ != old_upper);
}

void Zone::widen() {
  if (isBottom() || isTop()) {
    PrevLower_ = Lower_;
    PrevUpper_ = Upper_;
    return;
  }
  if (Lower_ < PrevLower_)
    Lower_ = NINF;
  if (Upper_ > PrevUpper_)
    Upper_ = INF;
  if (Lower_ == NINF && Upper_ == INF)
    Top_ = true;
  PrevLower_ = Lower_;
  PrevUpper_ = Upper_;
}

z3::expr Zone::toFormula(const ValueMapping &vmap, z3::context &ctx) const {
  if (isTop())
    return ctx.bool_val(true);
  if (isBottom())
    return ctx.bool_val(false);

  unsigned bw = Fctx_.sortForType(Left_->getType()).bv_size();
  z3::expr diff = vmap[Left_] - vmap[Right_];
  z3::expr result = ctx.bool_val(true);

  if (Upper_ != INF) {
    z3::expr ub = ctx.bv_val((uint64_t)Upper_, bw);
    result = result && z3::sle(diff, ub);
  }

  if (Lower_ != NINF) {
    z3::expr lb = ctx.bv_val((uint64_t)Lower_, bw);
    result = result && z3::sge(diff, lb);
  }

  return result;
}

void Zone::prettyPrint(PrettyPrinter &out) const {
  if (isTop()) {
    out << pp::top;
    return;
  }
  if (isBottom()) {
    out << pp::bottom;
    return;
  }

  if (Lower_ == NINF && Upper_ == INF) {
    out << pp::top;
  } else if (Lower_ == Upper_) {
    out << Left_ << " - " << Right_ << " = " << Lower_;
  } else {
    out << Lower_ << " <= " << Left_ << " - " << Right_ << " <= " << Upper_;
  }
}

namespace {
// Wrapper to match alt_ffunc_0 signature
std::unique_ptr<AbstractValue> ZoneFactory(const FunctionContext &fctx,
                                           llvm::BasicBlock *bb, bool after) {
  return params::ForNonPointerPairs<Zone>(fctx, bb, after, false);
}

DomainConstructor::Register _zone("Zones", "difference-bound zone domain (DBM)",
                                  ZoneFactory);
} // namespace
