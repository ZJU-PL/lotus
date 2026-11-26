/** @file ProgramVar.h @brief Symbolic program variable identifiers. */
#pragma once

#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "SymbolicExecution/Domain/BigInteger.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace SymbolicExecution {

using lotus::gvfg::GuardedValueFlowNode;

/// Base class for symbolic execution value identities (GVFG nodes or synthetic aux values).
class ProgramValue {
public:
  enum ValKind { VK_GVFG, VK_AUX };

  explicit ProgramValue(ValKind VK) : VK(VK) {}
  virtual ~ProgramValue() = default;

  ValKind getValKind() const { return VK; }
  virtual size_t hash() const { return std::hash<unsigned>()(VK); }
  virtual std::string getID() const = 0;

protected:
  ValKind VK;
};

class GuardedValueFlowNodeValue : public ProgramValue {
public:
  explicit GuardedValueFlowNodeValue(GuardedValueFlowNode *N)
      : ProgramValue(VK_GVFG), N(N) {}

  static bool classof(const ProgramValue *O) {
    return O->getValKind() == VK_GVFG;
  }

  llvm::Value *getValue() const;
  GuardedValueFlowNode *getNode() const { return N; }

  bool operator==(const GuardedValueFlowNodeValue &R) const { return N == R.N; }
  bool operator<(const GuardedValueFlowNodeValue &R) const { return N < R.N; }
  size_t hash() const override;
  std::string getID() const override;

private:
  GuardedValueFlowNode *N;
};

class AuxValue : public ProgramValue {
public:
  AuxValue(llvm::Type *Ty, std::string Name);

  static bool classof(const ProgramValue *O) {
    return O->getValKind() == VK_AUX;
  }

  llvm::Type *getType() const { return Ty; }
  std::string getName() const { return Name; }
  std::string getID() const override { return Name; }

  bool operator==(const AuxValue &R) const {
    return Ty == R.Ty && Name == R.Name;
  }

  bool operator<(const AuxValue &R) const {
    if (Ty == R.Ty) {
      return Name < R.Name;
    }
    return Ty < R.Ty;
  }

  size_t hash() const override;

private:
  llvm::Type *Ty;
  std::string Name;
};

class ProgramValuePtr {
public:
  ProgramValuePtr() : Data(nullptr) {}
  ProgramValuePtr(const GuardedValueFlowNode *V);
  ProgramValuePtr(llvm::Type *Ty, std::string Name);
  ProgramValuePtr(const ProgramValuePtr &R);
  ProgramValuePtr(ProgramValuePtr &&R) noexcept : Data(std::move(R.Data)) {}
  ProgramValuePtr &operator=(ProgramValuePtr R);

  bool isNull() const { return Data == nullptr; }
  bool isVacuous() const { return Data == nullptr; }

  llvm::Value *getLLVMVal() const;
  llvm::Type *getType() const;

  template <typename Ty> const Ty *getAs() const {
    return llvm::cast<Ty>(Data.get());
  }

  template <typename Ty> bool isa() const {
    return Data != nullptr && llvm::isa<Ty>(Data.get());
  }

  bool operator==(const ProgramValuePtr &R) const {
    if (isVacuous() || R.isVacuous()) {
      return Data == R.Data;
    }
    if (isa<GuardedValueFlowNodeValue>() &&
        R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<GuardedValueFlowNodeValue>() ==
             *R.getAs<GuardedValueFlowNodeValue>();
    }
    if (!isa<GuardedValueFlowNodeValue>() &&
        !R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<AuxValue>() == *R.getAs<AuxValue>();
    }
    return false;
  }

  bool operator<(const ProgramValuePtr &R) const {
    if (isVacuous() || R.isVacuous()) {
      return Data < R.Data;
    }
    if (isa<GuardedValueFlowNodeValue>() &&
        R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<GuardedValueFlowNodeValue>() <
             *R.getAs<GuardedValueFlowNodeValue>();
    }
    if (!isa<GuardedValueFlowNodeValue>() &&
        !R.isa<GuardedValueFlowNodeValue>()) {
      return *getAs<AuxValue>() < *R.getAs<AuxValue>();
    }
    return isa<GuardedValueFlowNodeValue>() &&
           !R.isa<GuardedValueFlowNodeValue>();
  }

  const std::unique_ptr<ProgramValue> &getData() const { return Data; }
  std::string getID() const { return Data->getID(); }
  size_t hash() const { return isVacuous() ? 0 : Data->hash(); }

  bool isConstant() const;
  BigInteger getAsConstant() const;
  void dump() const;

private:
  std::unique_ptr<ProgramValue> Data;
};

class Var {
public:
  explicit Var(GuardedValueFlowNode *V) : Data(V) {}
  explicit Var(ProgramValuePtr V) : Data(std::move(V)) {}
  Var(const Var &) = default;
  Var(Var &&) = default;

  Var &operator=(Var R) {
    Data = std::move(R.Data);
    return *this;
  }

  bool operator==(const Var &R) const { return Data == R.Data; }
  bool operator<(const Var &R) const { return Data < R.Data; }

  llvm::Value *getLLVMValue() const { return Data.getLLVMVal(); }
  llvm::Function *getEnclosingFunc() const;
  ProgramValuePtr getValue() const { return Data; }

  bool isConstant() const { return Data.isConstant(); }
  BigInteger getAsConstant() const { return Data.getAsConstant(); }

private:
  ProgramValuePtr Data;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::Var> {
  size_t operator()(const SymbolicExecution::Var &V) const {
    return V.getValue().getData()->hash();
  }
};

template <> struct hash<SymbolicExecution::ProgramValuePtr> {
  size_t operator()(const SymbolicExecution::ProgramValuePtr &V) const {
    return V.hash();
  }
};
} // namespace std
