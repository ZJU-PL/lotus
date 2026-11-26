/** @file PropertyValue.h @brief Abstract property value representation for symbolic analysis. */
#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace SymbolicExecution {

class PropertyValue;
class PropertyInteger;
class PropertySymExpr;
class Var;
class ProgramValuePtr;
class PropertyValuePtr;

/// Base class for symbolic scalar values (concrete integers and affine expressions).
class PropertyValue {
public:
  enum BinOp { Add, Sub, Mul, UDiv, SDiv };

  static BinOp fromLLVMOp(unsigned OpCode);

  enum PropertyValueKind {
    VK_Integer, // c
    VK_SymExpr  // a1*x1 + ... + an*xn + c
  };

  explicit PropertyValue(PropertyValueKind ValK);
  virtual ~PropertyValue() = default;

  PropertyValuePtr binOp(const PropertyValue *Rhs, BinOp Op) const;
  PropertyValuePtr binOp(const PropertyValuePtr &Rhs, BinOp Op) const;
  PropertyValuePtr binOp(const PropertyValue &Rhs, BinOp Op) const;

  /// Three-way comparison: 0 for false, 1 for true, 2 for unknown.
  unsigned cmp(const PropertyValue *Rhs, unsigned Pred) const;

  PropertyValueKind getKind() const { return ValKind; }

  virtual PropertyValuePtr map(const std::unordered_map<Var, Var> &M,
                               bool Total = true) const;
  virtual PropertyValuePtr mapWithDefault(const std::unordered_map<Var, Var> &M,
                                          Var DefaultV) const;
  virtual PropertyValuePtr
  map(const std::unordered_map<Var, PropertyValuePtr> &M) const;

  virtual size_t hash() const = 0;
  virtual PropertyValue *clone() const = 0;

  bool isEquivalent(const PropertyValue &Rhs) const;

  virtual void dumpDbgString(llvm::raw_ostream &) const {}
  void dump() const;
  std::string getName() const;

  virtual llvm::Function *getEnclosingFunc() const { return nullptr; }

private:
  PropertyValueKind ValKind;
};

/// Shared immutable handle to a PropertyValue with algebraic operators.
class PropertyValuePtr {
public:
  PropertyValuePtr() = default;
  PropertyValuePtr(const std::shared_ptr<PropertyValue> &V) : Data(V) {}
  PropertyValuePtr(const std::shared_ptr<PropertyInteger> &V);
  PropertyValuePtr(const std::shared_ptr<PropertySymExpr> &V);
  PropertyValuePtr(const ProgramValuePtr &V);
  PropertyValuePtr(const PropertyValuePtr &) = default;
  PropertyValuePtr &operator=(const PropertyValuePtr &) = default;

  bool operator==(const PropertyValuePtr &R) const;
  size_t hash() const;

  const PropertyValue *operator->() const { return get(); }
  const PropertyValue &operator*() const { return *get(); }
  operator bool() const { return Data != nullptr; }
  const PropertyValue *get() const { return Data.get(); }

  PropertyValuePtr operator+(const PropertyValuePtr &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Add);
  }
  PropertyValuePtr operator+(const PropertyValue &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Add);
  }
  PropertyValuePtr operator-(const PropertyValuePtr &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Sub);
  }
  PropertyValuePtr operator-(const PropertyValue &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Sub);
  }
  PropertyValuePtr operator-() const;
  PropertyValuePtr operator*(const PropertyValuePtr &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Mul);
  }
  PropertyValuePtr operator*(const PropertyValue &Rhs) const {
    return Data->binOp(Rhs, PropertyValue::Mul);
  }

  bool operator<(int64_t V) const;
  bool operator<=(int64_t V) const;
  bool operator==(int64_t V) const;
  bool operator>(int64_t V) const;

private:
  std::shared_ptr<const PropertyValue> Data = nullptr;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::PropertyValuePtr> {
  size_t operator()(const SymbolicExecution::PropertyValuePtr &V) const {
    return V.hash();
  }
};
} // namespace std
