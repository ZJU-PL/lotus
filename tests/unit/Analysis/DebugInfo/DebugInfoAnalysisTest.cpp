#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"
#include "TestUtils/LLVMHelpers.h"
#include "Utils/LLVM/PackedTypeLayout.h"

#include <unordered_map>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
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

TEST(DebugInfoAnalysisTest, HandlesDetachedConstantExprInstructionSafely) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    @buf = global [4 x i8] zeroinitializer

    define i8* @example() {
    entry:
      ret i8* getelementptr inbounds ([4 x i8], [4 x i8]* @buf, i64 0, i64 1)
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *ret = llvm::dyn_cast<llvm::ReturnInst>(
      function->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);
  auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(ret->getReturnValue());
  ASSERT_NE(ce, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_FALSE(analysis.getIRString(ce).empty());
  EXPECT_EQ(analysis.getSourceFile(ce), "");
  EXPECT_FALSE(analysis.getVariableName(ce).empty());

  llvm::Instruction *detached = ce->getAsInstruction();
  ASSERT_NE(detached, nullptr);
  EXPECT_EQ(detached->getParent(), nullptr);
  EXPECT_FALSE(analysis.getIRString(detached).empty());
  EXPECT_EQ(analysis.getFunctionName(detached), "unknown_function");
  EXPECT_EQ(analysis.getSourceFile(detached), "");
  EXPECT_FALSE(analysis.getVariableName(detached).empty());
  detached->deleteValue();
}

TEST(DebugInfoAnalysisTest, CachesIRStringsAcrossFunctionsSafely) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @first(i32 %a) {
    entry:
      %x = add i32 %a, 1
      ret i32 %x
    }

    define i32 @second(i32 %b) {
    entry:
      %y = mul i32 %b, 3
      ret i32 %y
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *x = findInstructionByName(module->getFunction("first"), "x");
  const auto *y = findInstructionByName(module->getFunction("second"), "y");
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  DebugInfoAnalysis analysis;
  std::string xFirst = analysis.getIRString(x);
  std::string yFirst = analysis.getIRString(y);
  EXPECT_FALSE(xFirst.empty());
  EXPECT_FALSE(yFirst.empty());
  EXPECT_NE(xFirst, yFirst);
  EXPECT_EQ(analysis.getIRString(x), xFirst);
  EXPECT_EQ(analysis.getIRString(y), yFirst);
}

TEST(DebugInfoAnalysisTest,
     IRExpressionRendererHandlesVariableStructArrayGepOffset) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    %struct.S = type { i32, [8 x i64] }

    define i64* @index_field(%struct.S* %s, i64 %i) {
    entry:
      %elem = getelementptr inbounds %struct.S, %struct.S* %s, i64 0, i32 1, i64 %i
      ret i64* %elem
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("index_field");
  ASSERT_NE(function, nullptr);
  auto *elem =
      const_cast<llvm::Instruction *>(findInstructionByName(function, "elem"));
  ASSERT_NE(elem, nullptr);

  lotus::PackedTypeLayout layout(module->getDataLayout());
  DebugInfoAnalysis analysis;
  lotus::ir_expression::IRExpressionRenderer renderer(layout, analysis);

  auto *gep = llvm::cast<llvm::GEPOperator>(elem);
  auto offs = renderer.get_gep_offset(gep, nullptr);
  EXPECT_EQ(offs.first, 0);
  EXPECT_GE(offs.second, 0);
  EXPECT_FALSE(renderer.restore_access_path_expr(elem, true, nullptr).empty());
}

} // namespace
