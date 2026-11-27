#pragma once

#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cstddef>
#include <utility>

#include <llvm/ADT/DenseMapInfo.h>

namespace context {
class Context;
} // namespace context

namespace llvm {
class Value;
} // namespace llvm

namespace tpa::taint {

class TaintValue {
private:
  const context::Context *context;
  const llvm::Value *value;

  using PairType = std::pair<const context::Context *, const llvm::Value *>;

public:
  TaintValue(const context::Context *c, const llvm::Value *v)
      : context(c), value(v) {}
  TaintValue(const PairType &p) : context(p.first), value(p.second) {}

  const context::Context *getContext() const { return context; }
  const llvm::Value *getValue() const { return value; }

  bool operator==(const TaintValue &other) const {
    return (context == other.context) && (value == other.value);
  }
  bool operator!=(const TaintValue &other) const { return !(*this == other); }
  bool operator<(const TaintValue &rhs) const {
    if (context < rhs.context)
      return true;
    else if (rhs.context < context)
      return false;
    else
      return value < rhs.value;
  }
  bool operator>(const TaintValue &rhs) const { return rhs < *this; }
  bool operator<=(const TaintValue &rhs) const { return !(rhs < *this); }
  bool operator>=(const TaintValue &rhs) const { return !(*this < rhs); }

  operator PairType() const { return std::make_pair(context, value); }
};

} // namespace tpa::taint

namespace std {
template <> struct hash<tpa::taint::TaintValue> {
  size_t operator()(const tpa::taint::TaintValue &tv) const {
    return util::hashPair(tv.getContext(), tv.getValue());
  }
};
} // namespace std

namespace llvm {
template <> struct DenseMapInfo<tpa::taint::TaintValue> {
  using CtxInfo = DenseMapInfo<const context::Context *>;
  using ValInfo = DenseMapInfo<const llvm::Value *>;

  static tpa::taint::TaintValue getEmptyKey() {
    return tpa::taint::TaintValue(CtxInfo::getEmptyKey(),
                                  ValInfo::getEmptyKey());
  }
  static tpa::taint::TaintValue getTombstoneKey() {
    return tpa::taint::TaintValue(CtxInfo::getTombstoneKey(),
                                  ValInfo::getTombstoneKey());
  }
  static unsigned getHashValue(const tpa::taint::TaintValue &tv) {
    return static_cast<unsigned>(
        util::hashPair(tv.getContext(), tv.getValue()));
  }
  static bool isEqual(const tpa::taint::TaintValue &lhs,
                      const tpa::taint::TaintValue &rhs) {
    return lhs == rhs;
  }
};
} // namespace llvm
