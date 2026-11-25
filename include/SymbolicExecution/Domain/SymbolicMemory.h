/** @file SymbolicMemory.h @brief Symbolic access paths and abstract memory objects. */
#pragma once

#include "SymbolicExecution/Domain/BigInteger.h"
#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Support/HashUtil.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

namespace SymbolicExecution {

class PTItem;

/// Symbolic access path: base object plus symbolic or concrete offset.
class AccessPath {
  friend class PTItem;

public:
  AccessPath(const AccessPath &) = default;
  AccessPath(ProgramValuePtr Parent, PropertyValuePtr Offset)
      : Parent(std::move(Parent)), Offset(std::move(Offset)) {}
  AccessPath() : AccessPath(ProgramValuePtr(), PropertyValuePtr()) {}
  AccessPath &operator=(const AccessPath &) = default;

  ProgramValuePtr getParent() const { return Parent; }
  const PropertyValuePtr &getOffset() const { return Offset; }

  bool operator==(const AccessPath &R) const {
    return Parent == R.Parent && Offset == R.Offset;
  }

  bool operator<(const AccessPath &R) const {
    if (!(Parent == R.Parent))
      return Parent < R.Parent;
    return Offset.get() < R.Offset.get();
  }

  bool isEmpty() const {
    if (Parent.isNull()) {
      assert(!Offset);
      return true;
    }
    return false;
  }

  size_t hash() const { return hashCombine({Parent.hash(), Offset.hash()}); }

  std::string getID() const {
    if (isEmpty()) {
      return "0";
    }
    return Parent.getID() + "_" + std::to_string(Offset->hash());
  }

private:
  ProgramValuePtr Parent;
  PropertyValuePtr Offset;
};

/// Abstract memory object reachable during symbolic execution.
class PTItem {
public:
  enum MemObjKind { MK_CONCRETE, MK_SYMBOLIC, MK_PLACEHOLDER };

  PTItem(ProgramValuePtr AllocSite, MemObjKind K, int64_t Off = 0,
         const PropertyValuePtr &Sz = PropertyValuePtr());
  PTItem(AccessPath AP, MemObjKind K) : AP(std::move(AP)), Size(), K(K) {}

  bool operator==(const PTItem &R) const {
    return AP == R.AP && Size == R.Size && K == R.K;
  }

  size_t hash() const {
    return hashCombine({AP.hash(), Size.hash(), static_cast<size_t>(K)});
  }

  bool isSymbolic() const;
  bool isConcrete() const;
  bool isPlaceHolder() const;
  bool isOffsetSymbolic() const;
  BigInteger getConstOffset() const;

  ProgramValuePtr getAllocSite() const { return AP.getParent(); }
  PropertyValuePtr getOffset() const { return AP.getOffset(); }
  PropertyValuePtr getSize() const { return Size; }
  AccessPath getAP() const { return AP; }

  PTItem offsetBy(const PropertyValuePtr &Off) const;
  PTItem offsetBy(int64_t Off) const;
  void changeOffsetBy(const PropertyValuePtr &Off);
  std::string getID() const;

private:
  AccessPath AP;
  PropertyValuePtr Size;
  MemObjKind K;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::AccessPath> {
  size_t operator()(const SymbolicExecution::AccessPath &V) const;
};

template <> struct hash<SymbolicExecution::PTItem> {
  size_t operator()(const SymbolicExecution::PTItem &V) const;
};
} // namespace std
