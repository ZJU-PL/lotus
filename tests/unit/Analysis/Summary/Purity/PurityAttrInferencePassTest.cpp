#include "Analysis/Summary/Purity/PurityAttrInferencePass.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using lotus::analysis::purity::PurityAttrInferencePass;
using lotus::unittest::parseModuleChecked;

namespace {

TEST(PurityAttrInferencePassTest, MaterializesSafeFunctionAttributes) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @unknown_ext(i32*)

    define i32 @const_add(i32 %x, i32 %y) {
    entry:
      %sum = add i32 %x, %y
      ret i32 %sum
    }

    define i32 @pure_load(i32* %p) {
    entry:
      %v = load i32, i32* %p, align 4
      ret i32 %v
    }

    define void @impure_store(i32* %p) {
    entry:
      store i32 7, i32* %p, align 4
      ret void
    }

    define i32 @unknown_wrapper(i32* %p) {
    entry:
      %call = call i32 @unknown_ext(i32* %p)
      ret i32 %call
    }
  )", "PurityAttrInferencePassTest");

  PurityAttrInferencePass pass;
  const bool changed = pass.runOnModule(*module);
  EXPECT_TRUE(changed);

  Function *constFn = module->getFunction("const_add");
  Function *pureFn = module->getFunction("pure_load");
  Function *impureFn = module->getFunction("impure_store");
  Function *unknownFn = module->getFunction("unknown_wrapper");

  ASSERT_NE(constFn, nullptr);
  ASSERT_NE(pureFn, nullptr);
  ASSERT_NE(impureFn, nullptr);
  ASSERT_NE(unknownFn, nullptr);

  EXPECT_TRUE(constFn->hasFnAttribute(Attribute::ReadNone));
  EXPECT_FALSE(constFn->hasFnAttribute(Attribute::ReadOnly));

  EXPECT_TRUE(pureFn->hasFnAttribute(Attribute::ReadOnly));
  EXPECT_FALSE(pureFn->hasFnAttribute(Attribute::ReadNone));

  EXPECT_FALSE(impureFn->hasFnAttribute(Attribute::ReadOnly));
  EXPECT_FALSE(impureFn->hasFnAttribute(Attribute::ReadNone));

  EXPECT_FALSE(unknownFn->hasFnAttribute(Attribute::ReadOnly));
  EXPECT_FALSE(unknownFn->hasFnAttribute(Attribute::ReadNone));
}

TEST(PurityAttrInferencePassTest, UpgradesReadonlyToReadNoneForConstFunctions) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @const_add(i32 %x, i32 %y) readonly {
    entry:
      %sum = add i32 %x, %y
      ret i32 %sum
    }
  )", "PurityAttrInferencePassTest");

  Function *constFn = module->getFunction("const_add");
  ASSERT_NE(constFn, nullptr);
  ASSERT_TRUE(constFn->hasFnAttribute(Attribute::ReadOnly));

  PurityAttrInferencePass pass;
  const bool changed = pass.runOnModule(*module);
  EXPECT_TRUE(changed);

  EXPECT_TRUE(constFn->hasFnAttribute(Attribute::ReadNone));
  EXPECT_FALSE(constFn->hasFnAttribute(Attribute::ReadOnly));
}

} // namespace
