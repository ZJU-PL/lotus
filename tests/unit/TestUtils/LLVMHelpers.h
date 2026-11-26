#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace llvm;

namespace lotus {
namespace unittest {

std::unique_ptr<Module> parseModule(LLVMContext &context, const char *source,
                                    StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module> parseModule(LLVMContext &context,
                                    const std::string &source,
                                    StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module>
parseModuleChecked(LLVMContext &context, const char *source,
                   StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module>
parseModuleChecked(LLVMContext &context, const std::string &source,
                   StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module> parseAssembly(LLVMContext &context, const char *source,
                                      StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module> parseAssembly(LLVMContext &context,
                                      const std::string &source,
                                      StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module>
parseAssemblyChecked(LLVMContext &context, const char *source,
                     StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module>
parseAssemblyChecked(LLVMContext &context, const std::string &source,
                     StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module> loadModule(StringRef path, LLVMContext &context,
                                   StringRef diag_owner = "LLVMHelpers");

std::unique_ptr<Module> loadModule(const std::string &path,
                                   LLVMContext &context,
                                   StringRef diag_owner = "LLVMHelpers");

Instruction *findInstructionByName(Function &func, StringRef name);

Instruction *findInstructionByName(Function *func, StringRef name);

const Instruction *findInstructionByName(const Function &func, StringRef name);

const Instruction *findInstructionByName(const Function *func, StringRef name);

Instruction *findInst(Function &func, StringRef name);

Instruction *findInst(Function *func, StringRef name);

std::vector<CallBase *> findCallsTo(Function &func, StringRef callee_name);

std::vector<CallBase *> findCallsTo(Function *func, StringRef callee_name);

std::vector<const CallBase *> findCallsTo(const Function &func,
                                          StringRef callee_name);

std::vector<const CallBase *> findCallsTo(const Function *func,
                                          StringRef callee_name);

CallBase *findCallTo(Function &func, StringRef callee_name);

CallBase *findCallTo(Function *func, StringRef callee_name);

const CallBase *findCallTo(const Function &func, StringRef callee_name);

const CallBase *findCallTo(const Function *func, StringRef callee_name);

std::vector<CallBase *> getIndirectCalls(Function &func);

std::vector<const CallBase *> getIndirectCalls(const Function &func);

CallBase *findIndirectCall(Function &func);

CallBase *findIndirectCall(Function *func);

const CallBase *findIndirectCall(const Function &func);

const CallBase *findIndirectCall(const Function *func);

const BasicBlock *findBasicBlockByName(const Function &func, StringRef name);

BasicBlock *findBasicBlockByName(Function &func, StringRef name);

BasicBlock *findBlock(Function &func, StringRef name);

BasicBlock *findBlock(Function *func, StringRef name);

const BasicBlock *findBlock(const Function &func, StringRef name);

const BasicBlock *findBlock(const Function *func, StringRef name);

PHINode *findPhi(BasicBlock &block, StringRef name);

const PHINode *findPhi(const BasicBlock &block, StringRef name);

PHINode *findPhi(Function &func, StringRef name);

PHINode *findPhi(Function *func, StringRef name);

const PHINode *findPhi(const Function &func, StringRef name);

const PHINode *findPhi(const Function *func, StringRef name);

const Instruction *getFirstInstruction(const Function &func);

Instruction *getFirstInstruction(Function &func);

template <typename InstTy> InstTy *getFirstInstructionAs(Function &func) {
  return dyn_cast_or_null<InstTy>(getFirstInstruction(func));
}

template <typename InstTy>
const InstTy *getFirstInstructionAs(const Function &func) {
  return dyn_cast_or_null<InstTy>(getFirstInstruction(func));
}

Function *findFunctionByName(Module &module, StringRef name);

const Function *findFunctionByName(const Module &module, StringRef name);

Function *getFunctionChecked(Module &module, StringRef name);

const Function *getFunctionChecked(const Module &module, StringRef name);

BasicBlock *getBlockChecked(Function &func, StringRef name);

const BasicBlock *getBlockChecked(const Function &func, StringRef name);

template <typename InstTy>
InstTy *findInstruction(Function &F, StringRef name = "") {
  for (auto &BB : F) {
    for (auto &I : BB) {
      auto *inst = dyn_cast<InstTy>(&I);
      if (inst && (name.empty() || I.getName() == name)) {
        return inst;
      }
    }
  }
  return nullptr;
}

template <typename InstTy>
const InstTy *findInstruction(const Function &F, StringRef name = "") {
  for (const auto &BB : F) {
    for (const auto &I : BB) {
      auto *inst = dyn_cast<InstTy>(&I);
      if (inst && (name.empty() || I.getName() == name)) {
        return inst;
      }
    }
  }
  return nullptr;
}

class LlvmModuleTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source);
  std::unique_ptr<Module> parseModule(const std::string &source);
  bool loadModule(const char *ir);

  std::unique_ptr<Module> module;
};

} // namespace unittest
} // namespace lotus
