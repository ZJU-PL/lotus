#include "Analysis/Summary/Multiplicity/MultiplicityClassifier.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::multiplicity::AllocationMultiplicity;
using lotus::analysis::multiplicity::classifyModuleMultiplicity;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;

TEST(MultiplicityClassifierTest, ClassifiesGlobalsAndLoopFreeStackAsUnique) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @g = global i32 0

    define void @stack_only() {
    entry:
      %slot = alloca i32, align 4
      store i32 1, i32* %slot, align 4
      ret void
    }
  )", "MultiplicityClassifierTest");

  auto result = classifyModuleMultiplicity(*module);
  auto *slot = findInstructionByName(module->getFunction("stack_only"), "slot");
  ASSERT_NE(slot, nullptr);

  EXPECT_EQ(result.allocations.lookup(module->getGlobalVariable("g")),
            AllocationMultiplicity::Unique);
  EXPECT_EQ(result.allocations.lookup(slot), AllocationMultiplicity::Unique);
}

TEST(MultiplicityClassifierTest, MarksLoopBearingStackAllocasAsSummary) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @loop_stack(i32 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %slot = alloca i32, align 4
      %next = add i32 %i, 1
      %done = icmp eq i32 %next, %n
      br i1 %done, label %exit, label %loop
    exit:
      ret void
    }
  )", "MultiplicityClassifierTest");

  auto result = classifyModuleMultiplicity(*module);
  auto *slot = findInstructionByName(module->getFunction("loop_stack"), "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(result.allocations.lookup(slot), AllocationMultiplicity::Summary);
}

TEST(MultiplicityClassifierTest, UsesCallerCountForHeapAllocations) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i8* @malloc(i64)

    define i8* @wrapper() {
    entry:
      %heap = call i8* @malloc(i64 4)
      ret i8* %heap
    }

    define i8* @single_root() {
    entry:
      %res = call i8* @wrapper()
      ret i8* %res
    }

    define i8* @multi_root() {
    entry:
      %res = call i8* @wrapper()
      ret i8* %res
    }
  )", "MultiplicityClassifierTest");

  auto result = classifyModuleMultiplicity(*module);
  auto *heap = findInstructionByName(module->getFunction("wrapper"), "heap");
  ASSERT_NE(heap, nullptr);
  EXPECT_EQ(result.allocations.lookup(heap), AllocationMultiplicity::Summary);
}

TEST(MultiplicityClassifierTest, SingleCallerLoopFreeHeapIsUnique) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i8* @malloc(i64)

    define i8* @wrapper() {
    entry:
      %heap = call i8* @malloc(i64 4)
      ret i8* %heap
    }

    define i8* @main() {
    entry:
      %res = call i8* @wrapper()
      ret i8* %res
    }
  )", "MultiplicityClassifierTest");

  auto result = classifyModuleMultiplicity(*module);
  auto *heap = findInstructionByName(module->getFunction("wrapper"), "heap");
  ASSERT_NE(heap, nullptr);
  EXPECT_EQ(result.allocations.lookup(heap), AllocationMultiplicity::Unique);
}

} // namespace
