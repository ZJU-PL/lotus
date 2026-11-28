#include "Concurrency/ConcurrencyFacade.h"
#include "Concurrency/Core/ConcurrencyContext.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/MHP/MHPAnalysis.h"
#include "Concurrency/Thread/Sharing/EscapeAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
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

TEST_F(ConcurrencyContextTest, SyncFreeLibcCallPreservesMustLockSet) {
  const char *source = R"(
    @mtx = global i8 0
    @shared = global i32 0, align 4
    @fmt = private constant [4 x i8] c"%d\0A\00"

    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    declare i32 @printf(i8*, ...)
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @critical_with_libc() {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      %p = call i8* @malloc(i64 16)
      %fmtptr = getelementptr [4 x i8], [4 x i8]* @fmt, i64 0, i64 0
      call i32 (i8*, ...) @printf(i8* %fmtptr, i32 42)
      call void @free(i8* %p)
      store i32 7, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ConcurrencyContext ctx(*module);
  mhp::LockSetAnalysis lsa(ctx);
  lsa.analyze();

  const Instruction *storeInst = nullptr;
  for (const Instruction &I :
       instructions(*module->getFunction("critical_with_libc"))) {
    if (isa<StoreInst>(&I)) {
      storeInst = &I;
      break;
    }
  }
  ASSERT_NE(storeInst, nullptr);
  EXPECT_TRUE(lsa.mustHoldLock(storeInst, module->getNamedGlobal("mtx")));
}

TEST_F(ConcurrencyContextTest, AccessPathStructFieldLockCanonicalizesAcrossGEPs) {
  const char *source = R"(
    %Node = type { i32, i8 }
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @update(%Node* %node) {
    entry:
      %mtx1 = getelementptr inbounds %Node, %Node* %node, i32 0, i32 1
      call i32 @pthread_mutex_lock(i8* %mtx1)
      %valptr = getelementptr inbounds %Node, %Node* %node, i32 0, i32 0
      store i32 99, i32* %valptr, align 4
      %mtx2 = getelementptr inbounds %Node, %Node* %node, i32 0, i32 1
      call i32 @pthread_mutex_unlock(i8* %mtx2)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ConcurrencyContext ctx(*module);
  mhp::LockSetAnalysis lsa(ctx);
  lsa.analyze();

  const Function *fn = module->getFunction("update");
  const Instruction *storeInst = nullptr;
  const Instruction *retInst = nullptr;
  const Value *mtx1 = nullptr;
  const Value *mtx2 = nullptr;
  for (const Instruction &I : instructions(*fn)) {
    if (I.getName() == "mtx1")
      mtx1 = &I;
    if (I.getName() == "mtx2")
      mtx2 = &I;
    if (isa<StoreInst>(&I))
      storeInst = &I;
    if (isa<ReturnInst>(&I))
      retInst = &I;
  }
  ASSERT_NE(mtx1, nullptr);
  ASSERT_NE(mtx2, nullptr);
  ASSERT_NE(storeInst, nullptr);
  ASSERT_NE(retInst, nullptr);

  EXPECT_TRUE(lsa.locksMustMatch(mtx1, mtx2));
  EXPECT_TRUE(lsa.mustHoldLock(storeInst, mtx2));
  // Because mtx2 matches mtx1, the unlock releases it cleanly before ret.
  EXPECT_TRUE(lsa.getMayLockSetAt(retInst).empty());
}

TEST_F(ConcurrencyContextTest, NonCapturingLibcDoesNotCauseFalseStackEscape) {
  const char *source = R"(
    @fmt = private constant [4 x i8] c"%s\0A\00"
    declare i32 @printf(i8*, ...)
    declare i64 @strlen(i8*)

    define i64 @local_only() {
    entry:
      %buf = alloca [16 x i8], align 1
      %ptr = getelementptr [16 x i8], [16 x i8]* %buf, i64 0, i64 0
      store i8 65, i8* %ptr, align 1
      %fmtptr = getelementptr [4 x i8], [4 x i8]* @fmt, i64 0, i64 0
      call i32 (i8*, ...) @printf(i8* %fmtptr, i8* %ptr)
      %len = call i64 @strlen(i8* %ptr)
      ret i64 %len
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  const Value *buf = nullptr;
  for (const Instruction &I :
       instructions(*module->getFunction("local_only"))) {
    if (I.getName() == "buf") {
      buf = &I;
      break;
    }
  }
  ASSERT_NE(buf, nullptr);
  EXPECT_FALSE(escape.isEscaped(buf));
}

TEST_F(ConcurrencyContextTest, ResolvesIndirectCalleesSafely) {
  const char *source = R"(
    @ext_hook = external global void ()*
    define void @target_a() { ret void }
    define void @target_b() { ret void }

    define void @caller(i1 %cond) {
    entry:
      %fp = select i1 %cond, void ()* @target_a, void ()* @target_b
      call void %fp()
      %ext = load void ()*, void ()** @ext_hook
      call void %ext()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ConcurrencyContext ctx(*module);
  const CallBase *knownCall = nullptr;
  const CallBase *extCall = nullptr;
  for (const Instruction &I : instructions(*module->getFunction("caller"))) {
    if (const auto *cb = dyn_cast<CallBase>(&I)) {
      if (!knownCall)
        knownCall = cb;
      else
        extCall = cb;
    }
  }
  ASSERT_NE(knownCall, nullptr);
  ASSERT_NE(extCall, nullptr);

  const auto &rKnown = ctx.resolveCallees(knownCall);
  EXPECT_EQ(rKnown.targets.size(), 2u);
  EXPECT_FALSE(rKnown.has_unresolved);

  const auto &rExt = ctx.resolveCallees(extCall);
  EXPECT_TRUE(rExt.has_unresolved);
}

TEST_F(ConcurrencyContextTest, FacadeSummarizesLinuxKernelModule) {
  const char *source = R"(
    @k_lock = global i8 0
    declare void @_raw_spin_lock(i8*)
    declare void @_raw_spin_unlock(i8*)

    define void @kernel_fn() {
    entry:
      call void @_raw_spin_lock(i8* @k_lock)
      call void @_raw_spin_lock(i8* @k_lock)
      call void @_raw_spin_unlock(i8* @k_lock)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = ConcurrencyFacade::analyzeLinuxKernel(*module);
  EXPECT_GT(summary.operation_count, 0u);
  EXPECT_GT(summary.double_lock_count, 0u);
}
