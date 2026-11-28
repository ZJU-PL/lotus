#pragma once

#include "Concurrency/API/ThreadAPI.h"

#include <cstdint>
#include <set>
#include <unordered_set>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace mhp::detail {

bool isNonBinarySemaphoreOp(const ThreadAPI *thread_api,
                            const llvm::Instruction *inst);

void collectDefinedFunctionTargets(
    const llvm::Value *called, std::set<llvm::Function *> &callees,
    std::unordered_set<const llvm::Value *> &visited);

bool getConstantOffsetPointerInfo(const llvm::Value *ptr,
                                  const llvm::Module *module,
                                  const llvm::Value *&base, int64_t &offset,
                                  uint64_t &size);

bool areDisjointConstantOffsetPointers(const llvm::Value *lhs,
                                       const llvm::Value *rhs,
                                       const llvm::Module *module);

bool instructionPrecedesOrEquals(const llvm::Instruction *lhs,
                                 const llvm::Instruction *rhs,
                                 const llvm::Function *func);

} // namespace mhp::detail
