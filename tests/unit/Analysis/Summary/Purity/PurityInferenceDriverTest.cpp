#include "Analysis/Summary/Purity/ExternalPuritySummaryStore.h"
#include "Analysis/Summary/Purity/PurityInferenceDriver.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using lotus::analysis::purity::ExternalPuritySummaryRecord;
using lotus::analysis::purity::ExternalPuritySummaryStore;
using lotus::analysis::purity::ExternalSummaryState;
using lotus::analysis::purity::PurityInferenceDriver;
using lotus::analysis::purity::PurityInferenceDriverOptions;
using lotus::analysis::purity::PurityKind;
using lotus::unittest::parseModuleChecked;

namespace {

TEST(PurityInferenceDriverTest, InvalidatesDependenciesAndUpdatesReport) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @lib_reader(i8*)
    declare i32 @unknown_ext(i8*)

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

    define i32 @unknown_wrapper(i8* %p) {
    entry:
      %call = call i32 @unknown_ext(i8* %p)
      ret i32 %call
    }
  )", "PurityInferenceDriverTest");

  ExternalPuritySummaryStore store;
  ExternalPuritySummaryRecord validated;
  validated.functionName = "lib_reader";
  validated.state = ExternalSummaryState::Validated;
  validated.summary.readsReachableMemory = true;
  store.upsert(validated);

  PurityInferenceDriverOptions options;
  options.invalidatedSummaries.push_back("lib_reader");

  PurityInferenceDriver driver(options);
  const auto report = driver.run(*module, store);

  const auto *record = store.find("lib_reader");
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->state, ExternalSummaryState::Rejected);

  EXPECT_EQ(report.invalidatedFunctions.size(), 2u);
  EXPECT_EQ(report.invalidatedFunctions[0], "reader_top");
  EXPECT_EQ(report.invalidatedFunctions[1], "reader_wrapper");

  auto it = llvm::find_if(report.functions, [](const auto &function) {
    return function.functionName == "reader_top";
  });
  ASSERT_NE(it, report.functions.end());
  EXPECT_EQ(it->purity, PurityKind::Unknown);

  auto unknownIt = llvm::find_if(report.unknownSummaries, [](const auto &entry) {
    return entry.impact.symbolName == "lib_reader";
  });
  ASSERT_NE(unknownIt, report.unknownSummaries.end());
  EXPECT_TRUE(unknownIt->hasStoredSummary);
  EXPECT_EQ(unknownIt->storedState, ExternalSummaryState::Rejected);
}

TEST(PurityInferenceDriverTest, AppliesAttributesWhenRequested) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @lib_reader(i8*)

    define i32 @reader_wrapper(i8* %p) {
    entry:
      %call = call i32 @lib_reader(i8* %p)
      ret i32 %call
    }
  )", "PurityInferenceDriverTest");

  ExternalPuritySummaryStore store;
  ExternalPuritySummaryRecord validated;
  validated.functionName = "lib_reader";
  validated.state = ExternalSummaryState::Validated;
  validated.summary.readsReachableMemory = true;
  store.upsert(validated);

  PurityInferenceDriverOptions options;
  options.applyAttributes = true;

  PurityInferenceDriver driver(options);
  const auto report = driver.run(*module, store);

  EXPECT_TRUE(report.attributesApplied);
  auto *wrapper = module->getFunction("reader_wrapper");
  ASSERT_NE(wrapper, nullptr);
  EXPECT_TRUE(wrapper->hasFnAttribute(Attribute::ReadOnly));
}

} // namespace
