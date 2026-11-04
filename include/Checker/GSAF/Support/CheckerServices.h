#pragma once

#include "Analysis/Memory/HeapPointerAnalysis.h"

#include <memory>

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Pass.h>

namespace lotus::gsaf {

inline llvm::Function *calledFunction(llvm::Value *value) {
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(value);
  return call ? call->getCalledFunction() : nullptr;
}

inline llvm::StringRef calledName(llvm::Value *value) {
  auto *function = calledFunction(value);
  return function ? function->getName() : llvm::StringRef{};
}

inline bool isFreeCall(llvm::Instruction *instruction,
                       llvm::TargetLibraryInfoWrapperPass *wrapper) {
  return llvm::isFreeCall(instruction,
                          &wrapper->getTLI(*instruction->getFunction()));
}

inline bool isCallArgument(llvm::Value *value, llvm::Value *operation,
                           size_t index) {
  auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(operation);
  return call && index < call->arg_size() &&
         value == call->getArgOperand(index);
}

std::unique_ptr<HeapPointerAnalysis> heapAnalysis(llvm::Pass *pass);

const char *branchConditionFromTo(llvm::BasicBlock *from, llvm::BasicBlock *to);

} // namespace lotus::gsaf
