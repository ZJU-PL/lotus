/**
 * @file KINTCheckerFixture.h
 * @brief Shared fixture and helpers for KINT checker tests.
 *
 * Each KINT*Test.cpp translation unit includes this header and defines a
 * group of TEST_F(KINTCheckerTest, ...) cases. Helpers that used to live in
 * an anonymous namespace in KINTCheckerTest.cpp are compiled once in
 * KINTCheckerFixture.cpp. PassBuilder is kept out of this shared header.
 */

#pragma once

#include "Checker/KINT/BugDetection.h"
#include "Checker/KINT/KINTTaintAnalysis.h"
#include "Checker/KINT/Options.h"
#include "Checker/KINT/SmtMemory.h"
#include "TestUtils/LLVMHelpers.h"

#include <memory>
#include <optional>

#include <gtest/gtest.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <z3++.h>

namespace kint_test {

uint64_t getNumeralU64(const z3::expr &expr);
z3::expr bvValFromAPInt(z3::context &ctx, const llvm::APInt &value);

} // namespace kint_test

class KINTCheckerTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;

  std::unique_ptr<llvm::Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source, "KINTCheckerTest");
  }

  void runPass(llvm::Module &module);

  static bool hasKintErrorMetadata(const llvm::Function &F,
                                   llvm::Instruction::BinaryOps opcode) {
    for (const llvm::Instruction &I : llvm::instructions(F)) {
      const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(&I);
      if (!bin || bin->getOpcode() != opcode)
        continue;
      if (bin->getMetadata("mkint.err"))
        return true;
    }
    return false;
  }
};
