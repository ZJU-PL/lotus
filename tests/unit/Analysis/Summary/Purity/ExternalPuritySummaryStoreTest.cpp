#include "Analysis/Summary/Purity/ExternalPuritySummaryStore.h"
#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <unistd.h>

using namespace llvm;
using lotus::analysis::purity::ExternalPuritySummaryRecord;
using lotus::analysis::purity::ExternalPuritySummaryStore;
using lotus::analysis::purity::ExternalSummaryState;
using lotus::analysis::purity::FunctionEffectSummary;
using lotus::analysis::purity::FunctionPurityAnalysis;
using lotus::analysis::purity::FunctionPurityAnalysisOptions;
using lotus::analysis::purity::PurityKind;
using lotus::analysis::purity::SummaryConfidence;
using lotus::unittest::parseModuleChecked;

namespace {

std::string makeTemporaryJsonPath() {
  SmallString<128> path;
  int fd = -1;
  std::error_code ec = sys::fs::createTemporaryFile("lotus-purity", "json", fd,
                                                    path);
  EXPECT_FALSE(ec);
  if (fd >= 0) {
    ::close(fd);
  }
  return path.str().str();
}

TEST(ExternalPuritySummaryStoreTest, RoundTripsAndFiltersByState) {
  ExternalPuritySummaryStore store;

  ExternalPuritySummaryRecord validated;
  validated.functionName = "lib_reader";
  validated.state = ExternalSummaryState::Validated;
  validated.summary.readsReachableMemory = true;
  validated.summary.confidence = SummaryConfidence::Medium;
  store.upsert(validated);

  ExternalPuritySummaryRecord suggested;
  suggested.functionName = "lib_writer";
  suggested.state = ExternalSummaryState::Suggested;
  suggested.summary.writesReachableMemory = true;
  store.upsert(suggested);

  std::string path = makeTemporaryJsonPath();
  std::string error;
  ASSERT_TRUE(store.saveToFile(path, error)) << error;

  ExternalPuritySummaryStore loaded;
  ASSERT_TRUE(loaded.loadFromFile(path, error)) << error;
  ASSERT_TRUE(loaded.contains("lib_reader"));
  ASSERT_TRUE(loaded.contains("lib_writer"));

  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @lib_reader(i8*)
    declare void @lib_writer(i8*)

    define i32 @reader_wrapper(i8* %p) {
    entry:
      %call = call i32 @lib_reader(i8* %p)
      ret i32 %call
    }

    define void @writer_wrapper(i8* %p) {
    entry:
      call void @lib_writer(i8* %p)
      ret void
    }
  )", "ExternalPuritySummaryStoreTest");

  FunctionPurityAnalysisOptions validatedOnly;
  validatedOnly.externalSummaryProviders.push_back(
      loaded.createProvider(false, true));
  FunctionPurityAnalysis validatedAnalysis(*module, validatedOnly);
  validatedAnalysis.run();

  EXPECT_EQ(validatedAnalysis.getPurity(module->getFunction("reader_wrapper")),
            PurityKind::Pure);
  EXPECT_EQ(validatedAnalysis.getPurity(module->getFunction("writer_wrapper")),
            PurityKind::Unknown);

  FunctionPurityAnalysisOptions withSuggested;
  withSuggested.externalSummaryProviders.push_back(
      loaded.createProvider(true, true));
  FunctionPurityAnalysis suggestedAnalysis(*module, withSuggested);
  suggestedAnalysis.run();

  EXPECT_EQ(suggestedAnalysis.getPurity(module->getFunction("writer_wrapper")),
            PurityKind::Impure);
}

} // namespace
