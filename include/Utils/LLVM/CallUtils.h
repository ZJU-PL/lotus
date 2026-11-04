#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Statepoint.h"

#include <cstdlib>
#include <vector>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Operator.h>

namespace lotus {
namespace llvm_utils {

/// Recognize the implicit object parameter and C++ member declarations.
inline bool isClassMemberFunction(const llvm::Function &function) {
  if (function.arg_empty())
    return false;
  const auto &first = *function.arg_begin();
  if (first.getName() == "this")
    return true;
  if (!function.isDeclaration() || !first.getType()->isPointerTy())
    return false;
  auto *pointer = llvm::cast<llvm::PointerType>(first.getType());
  if (pointer->isOpaque())
    return false;
  auto *structure =
      llvm::dyn_cast<llvm::StructType>(pointer->getPointerElementType());
  if (!structure || structure->isLiteral() ||
      !structure->getName().startswith("class."))
    return false;
  std::string name = llvm::demangle(function.getName().str());
  size_t parameter = name.find('(');
  return name.substr(0, parameter).find("::") != std::string::npos;
}

inline int virtualCallIndex(const llvm::CallBase *call) {
  if (!call)
    return -1;
  auto *function = llvm::dyn_cast<llvm::LoadInst>(call->getCalledOperand());
  if (!function || !function->getName().startswith("vfn"))
    return -1;
  auto *entry =
      llvm::dyn_cast<llvm::GetElementPtrInst>(function->getPointerOperand());
  if (!entry || entry->getNumIndices() != 1)
    return -1;
  auto *table = llvm::dyn_cast<llvm::LoadInst>(entry->getPointerOperand());
  auto *index = llvm::dyn_cast<llvm::ConstantInt>(entry->getOperand(1));
  if (!table || !table->getName().startswith("vtable") || !index)
    return -1;
  if (!llvm::isa<llvm::BitCastInst>(table->getPointerOperand()))
    return -1;
  return static_cast<int>(index->getSExtValue());
}
inline bool isVirtualCall(const llvm::CallBase *call) {
  return virtualCallIndex(call) >= 0;
}
inline bool isThisPointer(const llvm::Value *value) {
  auto *argument = llvm::dyn_cast_or_null<llvm::Argument>(value);
  return argument && argument->getArgNo() == 0 &&
         isClassMemberFunction(*argument->getParent());
}
inline int getArgumentIndex(const llvm::CallBase *call,
                            const llvm::Value *value) {
  for (unsigned index = 0; index < call->arg_size(); ++index)
    if (call->getArgOperand(index) == value)
      return static_cast<int>(index);
  return -1;
}
inline llvm::Argument *getParameterAt(llvm::Function *function, int index) {
  return function && index >= 0 &&
                 static_cast<unsigned>(index) < function->arg_size()
             ? function->getArg(index)
             : nullptr;
}

inline bool isDestructor(const llvm::Function *function) {
  if (!function)
    return false;
  llvm::ItaniumPartialDemangler demangler;
  if (demangler.partialDemangle(function->getName().str().c_str()) ||
      !demangler.isCtorOrDtor())
    return false;
  char *base = demangler.getFunctionBaseName(nullptr, nullptr);
  bool result = base && base[0] == '~';
  std::free(base);
  return result;
}
inline bool isConstructor(const llvm::Function *function) {
  if (!function)
    return false;
  llvm::ItaniumPartialDemangler demangler;
  return !demangler.partialDemangle(function->getName().str().c_str()) &&
         demangler.isCtorOrDtor() && !isDestructor(function);
}

/// Resolve a syntactically direct call target.
///
/// This follows pointer casts and global aliases, but does not perform
/// call-graph construction or indirect-call resolution.
inline const llvm::Function *getDirectCallee(const llvm::CallBase *Call) {
  if (Call == nullptr) {
    return nullptr;
  }

  if (const auto *Statepoint = llvm::dyn_cast<llvm::GCStatepointInst>(Call)) {
    return Statepoint->getActualCalledFunction();
  }

  const llvm::Value *CalledOperand = Call->getCalledOperand();
  if (CalledOperand == nullptr) {
    return nullptr;
  }

  CalledOperand = CalledOperand->stripPointerCastsAndAliases();
  return llvm::dyn_cast<llvm::Function>(CalledOperand);
}

inline llvm::Function *getDirectCallee(llvm::CallBase *Call) {
  return const_cast<llvm::Function *>(
      getDirectCallee(static_cast<const llvm::CallBase *>(Call)));
}

/// Return the normal continuation instructions of a call-like instruction.
///
/// For invoke, this deliberately excludes the unwind destination. For callbr,
/// every successor is a normal continuation.
inline std::vector<const llvm::Instruction *>
getNormalCallContinuations(const llvm::CallBase *Call) {
  std::vector<const llvm::Instruction *> Continuations;
  if (Call == nullptr) {
    return Continuations;
  }

  if (const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call)) {
    if (!Invoke->getNormalDest()->empty()) {
      Continuations.push_back(&Invoke->getNormalDest()->front());
    }
    return Continuations;
  }

  if (const auto *CallBr = llvm::dyn_cast<llvm::CallBrInst>(Call)) {
    for (unsigned I = 0, E = CallBr->getNumSuccessors(); I < E; ++I) {
      const auto *Successor = CallBr->getSuccessor(I);
      if (!Successor->empty()) {
        Continuations.push_back(&Successor->front());
      }
    }
    return Continuations;
  }

  if (const auto *Next = Call->getNextNode()) {
    Continuations.push_back(Next);
  }
  return Continuations;
}

inline std::vector<llvm::Instruction *>
getNormalCallContinuations(llvm::CallBase *Call) {
  std::vector<llvm::Instruction *> Continuations;
  for (const auto *Instruction :
       getNormalCallContinuations(static_cast<const llvm::CallBase *>(Call))) {
    Continuations.push_back(const_cast<llvm::Instruction *>(Instruction));
  }
  return Continuations;
}

} // namespace llvm_utils
} // namespace lotus
