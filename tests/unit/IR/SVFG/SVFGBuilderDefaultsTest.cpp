#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

namespace {

class SVFGBuilderDefaultsTest : public LlvmModuleTest {
protected:
  static std::unique_ptr<SVFG> build(Module *module, ICFG &icfg,
                                     bool pointerOnly) {
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module);

    SVFGBuilderConfig config;
    config.usePointerAnalysis = false;
    config.buildMSSA = true;
    config.pointerOnly = pointerOnly;

    SVFGBuilder builder(config);
    return std::unique_ptr<SVFG>(builder.build(&icfg));
  }
};

} // namespace

TEST_F(SVFGBuilderDefaultsTest, PointerOnlyIsDefault) {
  SVFGBuilderConfig config;
  EXPECT_TRUE(config.pointerOnly);
  EXPECT_TRUE(config.buildMSSA);
  EXPECT_TRUE(config.usePointerAnalysis);
}

TEST_F(SVFGBuilderDefaultsTest, DefaultSkipsNonPointerMemoryAccesses) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 7, i32* %x
      %v = load i32, i32* %x
      %sum = add i32 %v, 1
      ret i32 %sum
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const StoreInst *store = nullptr;
  const BinaryOperator *add = nullptr;
  for (const BasicBlock &block : *mainFn) {
    for (const Instruction &instruction : block) {
      if (const auto *candidate = dyn_cast<StoreInst>(&instruction))
        store = candidate;
      else if (const auto *candidate = dyn_cast<BinaryOperator>(&instruction))
        add = candidate;
    }
  }
  ASSERT_NE(store, nullptr);
  ASSERT_NE(add, nullptr);

  ICFG pointerOnlyIcfg;
  auto pointerOnly = build(module.get(), pointerOnlyIcfg, true);
  ASSERT_NE(pointerOnly, nullptr);

  ICFG fullIcfg;
  auto full = build(module.get(), fullIcfg, false);
  ASSERT_NE(full, nullptr);

  // Pointer-only mode drops non-pointer stores and scalar arithmetic...
  EXPECT_EQ(pointerOnly->getDef(store), nullptr);
  EXPECT_EQ(pointerOnly->getDef(add), nullptr);
  // ...while the full graph keeps them.
  EXPECT_NE(full->getDef(store), nullptr);
  EXPECT_NE(full->getDef(add), nullptr);
  EXPECT_LT(pointerOnly->getNumNodes(), full->getNumNodes());
}
