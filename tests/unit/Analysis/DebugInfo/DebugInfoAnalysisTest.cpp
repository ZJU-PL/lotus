#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "TestUtils/LLVMHelpers.h"
#include "Utils/LLVM/PackedTypeLayout.h"

#include <unordered_map>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

TEST(DebugInfoAnalysisTest, FallsBackToIrNamesWithoutDebugMetadata) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example() {
    entry:
      %value = add i32 1, 2
      ret i32 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  const auto *value = findInstructionByName(function, "value");
  ASSERT_NE(value, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_EQ(analysis.getFunctionName(value), "example");
  EXPECT_EQ(analysis.getVariableName(value), "value");
  EXPECT_EQ(analysis.getTypeName(value), "i32");
  EXPECT_EQ(analysis.getSourceLocation(value), "unknown:0");
}

TEST(DebugInfoAnalysisTest, UsesInstructionNameWhenNoDebugVariableExists) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare i32 @producer()

    define i32 @example() {
    entry:
      %tmp = call i32 @producer()
      ret i32 %tmp
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  const auto *call = findInstructionByName(function, "tmp");
  ASSERT_NE(call, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_EQ(analysis.getVariableName(call), "tmp");
  EXPECT_EQ(analysis.getFunctionName(call), "example");
}

TEST(DebugInfoAnalysisTest, IRExpressionRendererTerminatesOnCyclicPhiAndGep) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i8* @cyclic(i8* %base, i1 %cond) {
    entry:
      br label %loop
    loop:
      %p = phi i8* [ %base, %entry ], [ %next, %loop ]
      %next = getelementptr i8, i8* %p, i64 1
      br i1 %cond, label %loop, label %exit
    exit:
      ret i8* %next
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("cyclic");
  ASSERT_NE(function, nullptr);
  auto *phi = const_cast<llvm::Instruction *>(findInstructionByName(function, "p"));
  auto *next = const_cast<llvm::Instruction *>(findInstructionByName(function, "next"));
  ASSERT_NE(phi, nullptr);
  ASSERT_NE(next, nullptr);

  lotus::PackedTypeLayout layout(module->getDataLayout());
  DebugInfoAnalysis analysis;
  lotus::ir_expression::IRExpressionRenderer renderer(layout, analysis);

  std::unordered_map<llvm::Value *, llvm::Value *> phiChoice{{phi, next}};
  std::string expr = renderer.restore_access_path_expr(next, true, &phiChoice);
  EXPECT_FALSE(expr.empty());
}

} // namespace
