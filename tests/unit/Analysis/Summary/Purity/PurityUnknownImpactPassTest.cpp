#include "Analysis/Summary/Purity/PurityUnknownImpactPass.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using lotus::analysis::purity::UnknownCalleeImpactAnalyzer;
using lotus::unittest::parseModuleChecked;

namespace {

TEST(PurityUnknownImpactPassTest, RanksUnknownDeclarationsByCallerImpact) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @unknown_a(i8*)
    declare i32 @unknown_b(i8*)

    define i32 @leaf_a(i8* %p) {
    entry:
      %call = call i32 @unknown_a(i8* %p)
      ret i32 %call
    }

    define i32 @mid_a(i8* %p) {
    entry:
      %call = call i32 @leaf_a(i8* %p)
      ret i32 %call
    }

    define i32 @top_a(i8* %p) {
    entry:
      %call = call i32 @mid_a(i8* %p)
      ret i32 %call
    }

    define i32 @leaf_b(i8* %p) {
    entry:
      %call = call i32 @unknown_b(i8* %p)
      ret i32 %call
    }
  )", "PurityUnknownImpactPassTest");

  UnknownCalleeImpactAnalyzer analyzer;
  const auto impacts = analyzer.rankUnknownCallees(*module);

  ASSERT_EQ(impacts.size(), 2u);

  EXPECT_EQ(impacts[0].symbolName, "unknown_a");
  EXPECT_EQ(impacts[0].directCallerCount, 1u);
  EXPECT_EQ(impacts[0].transitiveCallerCount, 3u);

  EXPECT_EQ(impacts[1].symbolName, "unknown_b");
  EXPECT_EQ(impacts[1].directCallerCount, 1u);
  EXPECT_EQ(impacts[1].transitiveCallerCount, 1u);
}

TEST(PurityUnknownImpactPassTest, IgnoresShadowMemHelperDeclarations) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @shadow.mem.load(i32, i32, i8*)
    declare i32 @unknown_a(i8*)

    define i32 @leaf(i8* %p) {
    entry:
      %mem = call i32 @shadow.mem.load(i32 0, i32 1, i8* null)
      %call = call i32 @unknown_a(i8* %p)
      %sum = add i32 %mem, %call
      ret i32 %sum
    }
  )", "PurityUnknownImpactPassTest");

  UnknownCalleeImpactAnalyzer analyzer;
  const auto impacts = analyzer.rankUnknownCallees(*module);

  ASSERT_EQ(impacts.size(), 1u);
  EXPECT_EQ(impacts[0].symbolName, "unknown_a");
}

} // namespace
