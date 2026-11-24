#include "Analysis/Summary/ParameterSummary/ParameterEffectSummary.h"
#include "Analysis/Summary/ParameterSummary/ResourceTable.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::parametersummary::computeParameterEffectSummaries;
using lotus::analysis::parametersummary::ResourceRole;
using lotus::analysis::parametersummary::ResourceTable;
using lotus::unittest::parseModuleChecked;

TEST(ParameterEffectSummaryTest, ModelsExternalRolesFromResourceKnowledge) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @free(i8*)
    declare i8* @malloc(i64)
    declare i64 @strlen(i8*)
  )", "ParameterEffectSummaryTest");

  auto summaries = computeParameterEffectSummaries(*module);
  auto *free_fn = module->getFunction("free");
  auto *malloc_fn = module->getFunction("malloc");
  auto *strlen_fn = module->getFunction("strlen");
  ASSERT_NE(free_fn, nullptr);
  ASSERT_NE(malloc_fn, nullptr);
  ASSERT_NE(strlen_fn, nullptr);

  EXPECT_TRUE(summaries.lookup(free_fn).paramFreed.lookup(0));
  EXPECT_TRUE(summaries.lookup(malloc_fn).returnIsAllocated);
  EXPECT_TRUE(summaries.lookup(strlen_fn).paramDereferenced.lookup(0));
}

TEST(ParameterEffectSummaryTest, DetectsDirectLoadsAndStoresOnParameters) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @touch(i32* %p, i32 %v) {
    entry:
      %x = load i32, i32* %p, align 4
      store i32 %v, i32* %p, align 4
      ret void
    }
  )", "ParameterEffectSummaryTest");

  auto summaries = computeParameterEffectSummaries(*module);
  auto *touch = module->getFunction("touch");
  ASSERT_NE(touch, nullptr);
  EXPECT_TRUE(summaries.lookup(touch).paramDereferenced.lookup(0));
  EXPECT_FALSE(summaries.lookup(touch).paramDereferenced.lookup(1));
}

TEST(ParameterEffectSummaryTest, ComposesTransitivelyOnAcyclicCallGraphs) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @free(i8*)
    declare i8* @malloc(i64)

    define void @wrapper(i8* %p) {
    entry:
      call void @free(i8* %p)
      ret void
    }

    define i8* @alloc_wrapper() {
    entry:
      %mem = call i8* @malloc(i64 4)
      ret i8* %mem
    }
  )", "ParameterEffectSummaryTest");

  auto summaries = computeParameterEffectSummaries(*module);
  auto *wrapper = module->getFunction("wrapper");
  auto *alloc_wrapper = module->getFunction("alloc_wrapper");
  ASSERT_NE(wrapper, nullptr);
  ASSERT_NE(alloc_wrapper, nullptr);

  EXPECT_TRUE(summaries.lookup(wrapper).paramFreed.lookup(0));
  EXPECT_TRUE(summaries.lookup(alloc_wrapper).returnIsAllocated);
}

TEST(ParameterEffectSummaryTest, AcceptsExplicitResourceTableInput) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @pool_free(i8*)

    define void @wrapper(i8* %p) {
    entry:
      call void @pool_free(i8* %p)
      ret void
    }
  )", "ParameterEffectSummaryTest");

  ResourceTable table = ResourceTable::empty();
  table.add("pool_free", ResourceRole::Deallocator);

  auto summaries = computeParameterEffectSummaries(*module, table);
  auto *wrapper = module->getFunction("wrapper");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_TRUE(summaries.lookup(wrapper).paramFreed.lookup(0));
}

TEST(ParameterEffectSummaryTest, DisablesTransitiveCompositionWhenCyclesExist) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @free(i8*)

    define void @a(i8* %p) {
    entry:
      call void @b(i8* %p)
      ret void
    }

    define void @b(i8* %p) {
    entry:
      %cond = icmp eq i8* %p, null
      br i1 %cond, label %free_path, label %recurse
    free_path:
      call void @free(i8* %p)
      ret void
    recurse:
      call void @a(i8* %p)
      ret void
    }
  )", "ParameterEffectSummaryTest");

  auto summaries = computeParameterEffectSummaries(*module);
  auto *a = module->getFunction("a");
  auto *b = module->getFunction("b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  EXPECT_FALSE(summaries.lookup(a).paramFreed.lookup(0));
  EXPECT_FALSE(summaries.lookup(b).paramFreed.lookup(0));
}

TEST(ParameterEffectSummaryTest, MirrorsSinglePassModuleOrderComposition) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @free(i8*)

    define void @caller(i8* %p) {
    entry:
      call void @callee(i8* %p)
      ret void
    }

    define void @callee(i8* %p) {
    entry:
      call void @free(i8* %p)
      ret void
    }
  )", "ParameterEffectSummaryTest");

  auto summaries = computeParameterEffectSummaries(*module);
  auto *caller = module->getFunction("caller");
  auto *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  EXPECT_FALSE(summaries.lookup(caller).paramFreed.lookup(0));
  EXPECT_TRUE(summaries.lookup(callee).paramFreed.lookup(0));
}

} // namespace
