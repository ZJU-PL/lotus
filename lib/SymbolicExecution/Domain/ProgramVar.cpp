#include "SymbolicExecution/Domain/ProgramVar.h"

#include "SymbolicExecution/Support/HashUtil.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace SymbolicExecution;

std::string GuardedValueFlowNodeValue::getID() const { return ptrToString(N); }

size_t GuardedValueFlowNodeValue::hash() const {
  return std::hash<GuardedValueFlowNode *>()(N);
}

Value *GuardedValueFlowNodeValue::getValue() const {
  if (!N) {
    return nullptr;
  }
  return N->getLLVMValue();
}

AuxValue::AuxValue(Type *Ty, std::string N)
    : ProgramValue(VK_AUX), Ty(Ty), Name(std::move(N)) {}

size_t AuxValue::hash() const {
  return hashCombine({std::hash<Type *>()(Ty), std::hash<std::string>()(Name)});
}

ProgramValuePtr::ProgramValuePtr(const GuardedValueFlowNode *V) {
  if (!V) {
    Data = nullptr;
    return;
  }
  Data = std::unique_ptr<GuardedValueFlowNodeValue>(
      new GuardedValueFlowNodeValue(const_cast<GuardedValueFlowNode *>(V)));
}

ProgramValuePtr::ProgramValuePtr(Type *Ty, std::string Name) {
  Data = std::unique_ptr<AuxValue>(new AuxValue(Ty, std::move(Name)));
}

ProgramValuePtr::ProgramValuePtr(const ProgramValuePtr &R) {
  if (R.isVacuous()) {
    Data = nullptr;
    return;
  }

  const ProgramValue *Ptr = R.Data.get();
  if (!Ptr) {
    Data = nullptr;
    return;
  }

  if (R.isa<GuardedValueFlowNodeValue>()) {
    Data = std::unique_ptr<GuardedValueFlowNodeValue>(
        new GuardedValueFlowNodeValue(*cast<GuardedValueFlowNodeValue>(Ptr)));
  } else {
    Data = std::unique_ptr<AuxValue>(new AuxValue(*cast<AuxValue>(Ptr)));
  }
}

ProgramValuePtr &ProgramValuePtr::operator=(ProgramValuePtr R) {
  std::swap(Data, R.Data);
  return *this;
}

bool ProgramValuePtr::isConstant() const {
  if (!isa<GuardedValueFlowNodeValue>()) {
    return false;
  }

  Value *Val = getLLVMVal();
  if (Val) {
    if (llvm::isa<ConstantInt>(Val) || llvm::isa<ConstantPointerNull>(Val)) {
      return true;
    }
  }
  return false;
}

BigInteger ProgramValuePtr::getAsConstant() const {
  if (isVacuous() || !isa<GuardedValueFlowNodeValue>()) {
    return BigInteger(APInt(64, 0, true));
  }

  Value *Val = getLLVMVal();
  if (!Val) {
    return BigInteger(APInt(64, 0, true));
  }

  if (llvm::isa<ConstantInt>(Val)) {
    return cast<ConstantInt>(Val)->getValue();
  }
  if (llvm::isa<ConstantPointerNull>(Val)) {
    return BigInteger(APInt(64, 0, true));
  }

  llvm::errs() << "Warning: getAsConstant() called on non-constant value\n";
  return BigInteger(APInt(64, 0, true));
}

void ProgramValuePtr::dump() const {
  if (isVacuous()) {
    llvm::errs() << "(vacuous)";
    return;
  }
  const ProgramValue *Val = Data.get();
  if (Val->getValKind() == ProgramValue::VK_GVFG) {
    auto *Node = cast<GuardedValueFlowNodeValue>(Val)->getNode();
    if (Value *LlvmValue = Node ? Node->getLLVMValue() : nullptr)
      llvm::errs() << *LlvmValue << "\n";
    else if (Node)
      llvm::errs() << Node->getDescription() << "\n";
  } else {
    llvm::errs() << cast<AuxValue>(Val)->getName() << "\n";
  }
}

Value *ProgramValuePtr::getLLVMVal() const {
  if (!isa<GuardedValueFlowNodeValue>()) {
    return nullptr;
  }
  return cast<GuardedValueFlowNodeValue>(Data.get())->getValue();
}

Type *ProgramValuePtr::getType() const {
  if (isVacuous()) {
    return nullptr;
  }

  if (isa<GuardedValueFlowNodeValue>()) {
    return cast<GuardedValueFlowNodeValue>(Data.get())->getNode()->getType();
  }
  return cast<AuxValue>(Data.get())->getType();
}

Function *Var::getEnclosingFunc() const {
  if (Data.isVacuous())
    return nullptr;
  if (Value *LlvmVal = Data.getLLVMVal()) {
    if (auto *Inst = dyn_cast<Instruction>(LlvmVal))
      return Inst->getFunction();
    if (auto *Arg = dyn_cast<Argument>(LlvmVal))
      return Arg->getParent();
  }
  if (Data.isa<GuardedValueFlowNodeValue>()) {
    auto *Node = Data.getAs<GuardedValueFlowNodeValue>()->getNode();
    if (Node && Node->getParentBasicBlock())
      return Node->getParentBasicBlock()->getParent();
  }
  return nullptr;
}
