#include "Concurrency/Core/ConcurrencyContext.h"
#include "Concurrency/MHP/MHPAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace concurrency;

namespace {

class ConcurrencyContextTest : public lotus::unittest::LlvmModuleTest {};

} // namespace

TEST_F(ConcurrencyContextTest, SummarizesModuleFeaturesAndDirectCallGraph) {
  const char *source = R"(
    @mtx = global i8 0
    @counter = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @helper() {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      %v = atomicrmw add i32* @counter, i32 1 seq_cst
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret void
    }

    define i8* @worker(i8* %arg) {
    entry:
      call void @helper()
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call void @helper()
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ConcurrencyContext ctx(*module);
  const ModuleConcurrencySummary &summary = ctx.getSummary();
  EXPECT_TRUE(summary.has_forks);
  EXPECT_TRUE(summary.has_joins);
  EXPECT_TRUE(summary.has_locks);
  EXPECT_TRUE(summary.has_atomics);
  EXPECT_FALSE(summary.has_openmp);
  EXPECT_FALSE(summary.has_mpi);
  EXPECT_FALSE(summary.has_cuda);
  EXPECT_TRUE(summary.hasSharedMemoryConcurrency());
  EXPECT_EQ(summary.fork_sites.size(), 1u);
  EXPECT_EQ(summary.join_sites.size(), 1u);

  const Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);
  auto callersIt = summary.direct_callers.find(helper);
  ASSERT_NE(callersIt, summary.direct_callers.end());
  EXPECT_EQ(callersIt->second.size(), 2u);
}

TEST_F(ConcurrencyContextTest, SharesContextWithMHPAndComputesRegionsLazily) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ConcurrencyContext ctx(*module);
  mhp::MHPAnalysis mhp(ctx);
  mhp.analyze();

  EXPECT_NE(mhp.getAliasAnalysis(), nullptr);
  EXPECT_EQ(mhp.getAliasAnalysis(), ctx.getAliasAnalysisIfPresent());
  EXPECT_FALSE(mhp.getThreadRegionAnalysis().getAllRegions().empty());
}
