#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

namespace {
class AliasWrapperBuildTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::Value *first = nullptr;
  llvm::Value *second = nullptr;

  void SetUp() override {
    llvm::SMDiagnostic error;
    module = llvm::parseAssemblyString(R"(
      define void @main() {
        %a = alloca i32
        %b = alloca i32
        ret void
      }
    )", error, context);
    ASSERT_NE(module, nullptr);
    auto &block = module->getFunction("main")->getEntryBlock();
    first = &block.front();
    auto next = block.begin();
    ++next;
    second = &*next;
  }
};

TEST_F(AliasWrapperBuildTest, CoreBackendsRemainAvailable) {
  for (auto implementation : {lotus::AAConfig::Implementation::SparrowAA,
                              lotus::AAConfig::Implementation::DyckAA,
                              lotus::AAConfig::Implementation::Combined}) {
    lotus::AAConfig config;
    config.impl = implementation;
    lotus::AliasAnalysisWrapper wrapper(*module, config);
    ASSERT_TRUE(wrapper.isInitialized());
    EXPECT_EQ(wrapper.query(first, first), llvm::AliasResult::MustAlias);
    EXPECT_EQ(wrapper.query(first, second), llvm::AliasResult::NoAlias);
  }
}

TEST_F(AliasWrapperBuildTest, DisabledBackendsFailConservatively) {
  std::vector<lotus::AAConfig::Implementation> disabled;
#if !LOTUS_AA_WRAPPER_ENABLE_DDA
  disabled.push_back(lotus::AAConfig::Implementation::DDA);
#endif
#if !LOTUS_AA_WRAPPER_ENABLE_TPA
  disabled.push_back(lotus::AAConfig::Implementation::TPA);
#endif
#if !LOTUS_AA_WRAPPER_ENABLE_CCLYZER
  disabled.push_back(lotus::AAConfig::Implementation::CclyzerAA);
#endif
  if (disabled.empty())
    GTEST_SKIP() << "All optional wrapper backends are enabled";
  for (auto implementation : disabled) {
    lotus::AAConfig config;
    config.impl = implementation;
    lotus::AliasAnalysisWrapper wrapper(*module, config);
    EXPECT_FALSE(wrapper.isInitialized());
    // Even identical pointers must not bypass the initialization failure.
    EXPECT_EQ(wrapper.query(first, first), llvm::AliasResult::MayAlias);
    EXPECT_EQ(wrapper.query(first, second), llvm::AliasResult::MayAlias);
    std::vector<const llvm::Value *> points_to;
    EXPECT_FALSE(wrapper.getPointsToSet(first, points_to));
    EXPECT_TRUE(wrapper.mayNull(first));
  }
}

TEST_F(AliasWrapperBuildTest, EnabledOptionalBackendsInitialize) {
  std::vector<lotus::AAConfig::Implementation> enabled;
#if LOTUS_AA_WRAPPER_ENABLE_DDA
  enabled.push_back(lotus::AAConfig::Implementation::DDA);
#endif
#if LOTUS_AA_WRAPPER_ENABLE_TPA
  enabled.push_back(lotus::AAConfig::Implementation::TPA);
#endif
  if (enabled.empty())
    GTEST_SKIP() << "All optional in-process wrapper backends are disabled";
  for (auto implementation : enabled) {
    // TPA normalizes the module, so give each backend its own input.
    llvm::SMDiagnostic error;
    auto input = llvm::parseAssemblyString(R"(
      define void @main() {
        %a = alloca i32
        %b = alloca i32
        ret void
      }
    )", error, context);
    ASSERT_NE(input, nullptr);
    auto *pointer = &input->getFunction("main")->getEntryBlock().front();
    lotus::AAConfig config;
    config.impl = implementation;
    lotus::AliasAnalysisWrapper wrapper(*input, config);
    ASSERT_TRUE(wrapper.isInitialized());
    // Obtain the pointer after initialization in case normalization replaced it.
    pointer = &input->getFunction("main")->getEntryBlock().front();
    EXPECT_EQ(wrapper.query(pointer, pointer), llvm::AliasResult::MustAlias);
  }
}
} // namespace
