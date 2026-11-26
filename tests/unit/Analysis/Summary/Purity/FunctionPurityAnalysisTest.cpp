#include "Analysis/Summary/Purity/DeclarationSummaryProvider.h"
#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;
using lotus::analysis::purity::ExternalPuritySummaryProvider;
using lotus::analysis::purity::FunctionPurityAnalysis;
using lotus::analysis::purity::FunctionPurityAnalysisOptions;
using lotus::analysis::purity::FunctionEffectSummary;
using lotus::analysis::purity::MemorySSAMode;
using lotus::analysis::purity::PurityKind;
using lotus::analysis::purity::SummaryConfidence;
using lotus::analysis::purity::SummarySource;
using lotus::unittest::parseModuleChecked;

namespace {

TEST(FunctionPurityAnalysisTest,
     ClassifiesConstPureUnknownAndImpureCandidates) {
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
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("const_add")),
            PurityKind::Const);
  EXPECT_EQ(analysis.getPurity(module->getFunction("pure_load")),
            PurityKind::Pure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("impure_store")),
            PurityKind::Impure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("unknown_wrapper")),
            PurityKind::Unknown);

  EXPECT_TRUE(analysis.isConst(module->getFunction("const_add")));
  EXPECT_TRUE(analysis.isPure(module->getFunction("pure_load")));
  EXPECT_TRUE(analysis.isAtMostPure(module->getFunction("const_add")));
  EXPECT_TRUE(analysis.isAtMostPure(module->getFunction("pure_load")));
  EXPECT_FALSE(analysis.isKnown(module->getFunction("unknown_wrapper")));
}

TEST(FunctionPurityAnalysisTest, PropagatesConstAndPureInterprocedurally) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @leaf_const(i32 %x) {
    entry:
      %r = mul i32 %x, 3
      ret i32 %r
    }

    define i32 @wrapper_const(i32 %x) {
    entry:
      %call = call i32 @leaf_const(i32 %x)
      %inc = add i32 %call, 1
      ret i32 %inc
    }

    define i32 @leaf_pure(i32* %p) {
    entry:
      %v = load i32, i32* %p, align 4
      ret i32 %v
    }

    define i32 @wrapper_pure(i32* %p) {
    entry:
      %call = call i32 @leaf_pure(i32* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("leaf_const")),
            PurityKind::Const);
  EXPECT_EQ(analysis.getPurity(module->getFunction("wrapper_const")),
            PurityKind::Const);
  EXPECT_EQ(analysis.getPurity(module->getFunction("leaf_pure")),
            PurityKind::Pure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("wrapper_pure")),
            PurityKind::Pure);
}

TEST(FunctionPurityAnalysisTest,
     HandlesRecursionAndLibraryDeclarationsConservatively) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @reader_ext(i8*) readonly
    declare i32 @unknown(i32*)

    define i32 @recursive(i32 %n) {
    entry:
      %cond = icmp eq i32 %n, 0
      br i1 %cond, label %base, label %step

    base:
      ret i32 1

    step:
      %dec = sub i32 %n, 1
      %call = call i32 @recursive(i32 %dec)
      %sum = add i32 %call, %n
      ret i32 %sum
    }

    define i32 @reader_ext_wrapper(i8* %p) {
    entry:
      %call = call i32 @reader_ext(i8* %p)
      ret i32 %call
    }

    define i32 @unknown_wrapper(i32* %p) {
    entry:
      %call = call i32 @unknown(i32* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("recursive")),
            PurityKind::Const);
  EXPECT_EQ(analysis.getPurity(module->getFunction("reader_ext_wrapper")),
            PurityKind::Pure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("unknown_wrapper")),
            PurityKind::Unknown);
}

TEST(FunctionPurityAnalysisTest, TreatsIndirectCallsAsUnknown) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @indirect(i32 (i32)* %fp, i32 %x) {
    entry:
      %call = call i32 %fp(i32 %x)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("indirect")),
            PurityKind::Unknown);
}

TEST(FunctionPurityAnalysisTest, TreatsShadowMemHelpersAsInternalBookkeeping) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @shadow.mem.load(i32, i32, i8*)

    define i32 @shadow_user() {
    entry:
      %mem = call i32 @shadow.mem.load(i32 0, i32 1, i8* null)
      ret i32 %mem
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("shadow_user")),
            PurityKind::Const);
}

TEST(FunctionPurityAnalysisTest,
     UsesMemorySSASummaryToIgnoreNonEscapingLocalStores) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @shadow.mem.init(i32, i8*)

    define void @local_store_only() {
    entry:
      %slot = alloca i32, align 4
      %mem = call i32 @shadow.mem.init(i32 0, i8* null)
      store i32 7, i32* %slot, align 4
      ret void
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  auto *function = module->getFunction("local_store_only");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(analysis.getPurity(function), PurityKind::Const);
  EXPECT_TRUE(analysis.getEffects(function).fromMemorySSA);
  EXPECT_TRUE(analysis.hasMemorySSASummaries());
}

TEST(FunctionPurityAnalysisTest,
     DisablingMemorySSAFallsBackToConservativeStoreHandling) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @shadow.mem.init(i32, i8*)

    define void @local_store_only() {
    entry:
      %slot = alloca i32, align 4
      %mem = call i32 @shadow.mem.init(i32 0, i8* null)
      store i32 7, i32* %slot, align 4
      ret void
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module, MemorySSAMode::Disabled);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("local_store_only")),
            PurityKind::Impure);
  EXPECT_FALSE(analysis.hasMemorySSASummaries());
}

TEST(FunctionPurityAnalysisTest,
     UsesMemorySSACallsiteSummaryForExternalWrites) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @shadow.mem.arg.mod(i32, i32, i32, i8*)
    declare void @lib_writer(i8*)

    @glob = global i8 0

    define void @writer_wrapper(i8* %p) {
    entry:
      %mem = call i32 @shadow.mem.arg.mod(i32 7, i32 11, i32 0, i8* @glob)
      call void @lib_writer(i8* %p)
      ret void
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("writer_wrapper")),
            PurityKind::Impure);
}

TEST(FunctionPurityAnalysisTest,
     UsesMemorySSACallsiteSummaryToKeepUnknownReadsConservative) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @shadow.mem.arg.ref(i32, i32, i32, i8*)
    declare i32 @lib_reader(i8*)

    @glob = global i8 0

    define i32 @reader_wrapper(i8* %p) {
    entry:
      %mem = call i32 @shadow.mem.arg.ref(i32 7, i32 11, i32 0, i8* @glob)
      %call = call i32 @lib_reader(i8* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  auto *wrapper = module->getFunction("reader_wrapper");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_EQ(analysis.getPurity(wrapper), PurityKind::Unknown);
  EXPECT_TRUE(analysis.getEffects(wrapper).fromMemorySSA);
}

TEST(FunctionPurityAnalysisTest, RejectsVolatileAndInlineAsmEffects) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @volatile_reader(i32* %p) {
    entry:
      %v = load volatile i32, i32* %p, align 4
      ret i32 %v
    }

    define void @asm_user() {
    entry:
      call void asm sideeffect "", ""()
      ret void
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("volatile_reader")),
            PurityKind::Impure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("asm_user")),
            PurityKind::Impure);
}

TEST(FunctionPurityAnalysisTest,
     ProviderChainPrefersLocalAttributesOverExternalSummaries) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @reader_ext(i8*) readonly

    define i32 @reader_wrapper(i8* %p) {
    entry:
      %call = call i32 @reader_ext(i8* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  auto externalProvider = std::make_shared<ExternalPuritySummaryProvider>();
  FunctionEffectSummary impureSummary;
  impureSummary.writesReachableMemory = true;
  externalProvider->setSummary("reader_ext", impureSummary);

  FunctionPurityAnalysisOptions options;
  options.externalSummaryProviders.push_back(externalProvider);

  FunctionPurityAnalysis analysis(*module, options);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("reader_wrapper")),
            PurityKind::Pure);
}

TEST(FunctionPurityAnalysisTest,
     ExternalSummariesPropagateConfidenceAndDependencies) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @lib_reader(i8*)

    define i32 @reader_wrapper(i8* %p) {
    entry:
      %call = call i32 @lib_reader(i8* %p)
      ret i32 %call
    }

    define i32 @reader_top(i8* %p) {
    entry:
      %call = call i32 @reader_wrapper(i8* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  auto externalProvider = std::make_shared<ExternalPuritySummaryProvider>();
  FunctionEffectSummary pureSummary;
  pureSummary.readsReachableMemory = true;
  externalProvider->setSummary("lib_reader", pureSummary);

  FunctionPurityAnalysisOptions options;
  options.externalSummaryProviders.push_back(externalProvider);

  FunctionPurityAnalysis analysis(*module, options);
  analysis.run();

  auto *wrapper = module->getFunction("reader_wrapper");
  auto *top = module->getFunction("reader_top");
  ASSERT_NE(wrapper, nullptr);
  ASSERT_NE(top, nullptr);

  EXPECT_EQ(analysis.getPurity(wrapper), PurityKind::Pure);
  EXPECT_EQ(analysis.getPurity(top), PurityKind::Pure);

  const auto topEffects = analysis.getEffects(top);
  EXPECT_EQ(topEffects.source, SummarySource::Propagated);
  EXPECT_EQ(topEffects.confidence, SummaryConfidence::Medium);
  ASSERT_EQ(topEffects.dependsOn.size(), 1u);
  EXPECT_EQ(topEffects.dependsOn.front(), "lib_reader");
}

} // namespace
