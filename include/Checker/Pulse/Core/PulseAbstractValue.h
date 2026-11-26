/** @file PulseAbstractValue.h @brief Abstract value representation for Pulse abstract interpretation. */
#pragma once

#include <functional>

#include <llvm/ADT/DenseMapInfo.h>

namespace llvm {
class Value;
} // namespace llvm

namespace pulse {

class AbstractValue {
private:
  const llvm::Value *value_;
  unsigned id_;

public:
  AbstractValue() : value_(nullptr), id_(0) {}
  AbstractValue(const llvm::Value *v, unsigned id) : value_(v), id_(id) {}

  const llvm::Value *getValue() const { return value_; }
  unsigned getId() const { return id_; }

  bool operator==(const AbstractValue &other) const { return id_ == other.id_; }
  bool operator<(const AbstractValue &other) const { return id_ < other.id_; }
};

} // namespace pulse

namespace std {

template <> struct hash<pulse::AbstractValue> {
  size_t operator()(const pulse::AbstractValue &v) const noexcept {
    return std::hash<unsigned>{}(v.getId());
  }
};

} // namespace std

namespace llvm {

template <> struct DenseMapInfo<pulse::AbstractValue> {
  static inline pulse::AbstractValue getEmptyKey() {
    return pulse::AbstractValue(nullptr, ~0u);
  }
  static inline pulse::AbstractValue getTombstoneKey() {
    return pulse::AbstractValue(nullptr, ~0u - 1u);
  }
  static unsigned getHashValue(const pulse::AbstractValue &v) {
    return DenseMapInfo<unsigned>::getHashValue(v.getId());
  }
  static bool isEqual(const pulse::AbstractValue &lhs,
                      const pulse::AbstractValue &rhs) {
    return lhs.getId() == rhs.getId();
  }
};

} // namespace llvm

