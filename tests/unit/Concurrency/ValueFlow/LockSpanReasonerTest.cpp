#include "Concurrency/ValueFlow/LockSpanReasoner.h"

#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

namespace {

/// Collects the memory accesses of `function` in program order.
void collectAccesses(Function &function, std::vector<const StoreInst *> &stores,
                     std::vector<const LoadInst *> &loads) {
  for (Instruction &instruction : instructions(&function)) {
    if (auto *store = dyn_cast<StoreInst>(&instruction))
      stores.push_back(store);
    else if (auto *load = dyn_cast<LoadInst>(&instruction))
      loads.push_back(load);
  }
}

class LockSpanReasonerTest : public LlvmModuleTest {};

TEST_F(LockSpanReasonerTest, StoresInOneCriticalSectionShareTheSpan) {
  auto module = parseModule(R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0
    @cell = global i8 0

    define void @writer() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      store i8 1, i8* @cell
      store i8 2, i8* @cell
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  mhp::LockSetAnalysis locks(*module);
  locks.analyze();
  LockSpanReasoner spans(locks);

  std::vector<const StoreInst *> stores;
  std::vector<const LoadInst *> loads;
  collectAccesses(*module->getFunction("writer"), stores, loads);
  ASSERT_EQ(stores.size(), 2u);

  EXPECT_TRUE(spans.inSameSpan(stores[0], stores[1]));
  EXPECT_TRUE(spans.inSameSpan(stores[1], stores[0]));
  EXPECT_TRUE(spans.mustExecuteBefore(stores[0], stores[1]));
  EXPECT_FALSE(spans.mustExecuteBefore(stores[1], stores[0]));
  // Instructions later in the same block are on every path leaving an earlier
  // instruction, so they post-dominate it.
  EXPECT_TRUE(spans.mustExecuteAfter(stores[0], stores[1]));
  EXPECT_FALSE(spans.mustExecuteAfter(stores[1], stores[0]));
}

TEST_F(LockSpanReasonerTest, ReacquiredLockStartsANewSpan) {
  auto module = parseModule(R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0
    @cell = global i8 0

    define void @writer() {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      store i8 1, i8* @cell
      call i32 @pthread_mutex_unlock(i8* @lock)
      call i32 @pthread_mutex_lock(i8* @lock)
      store i8 2, i8* @cell
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  mhp::LockSetAnalysis locks(*module);
  locks.analyze();
  LockSpanReasoner spans(locks);

  std::vector<const StoreInst *> stores;
  std::vector<const LoadInst *> loads;
  collectAccesses(*module->getFunction("writer"), stores, loads);
  ASSERT_EQ(stores.size(), 2u);

  EXPECT_FALSE(spans.inSameSpan(stores[0], stores[1]));
}

TEST_F(LockSpanReasonerTest, LoopBackEdgeSplitsTheSpan) {
  auto module = parseModule(R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0
    @cell = global i8 0

    define void @writer(i1 %again) {
    entry:
      br label %body

    body:
      call i32 @pthread_mutex_lock(i8* @lock)
      store i8 1, i8* @cell
      store i8 2, i8* @cell
      call i32 @pthread_mutex_unlock(i8* @lock)
      br i1 %again, label %body, label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  mhp::LockSetAnalysis locks(*module);
  locks.analyze();
  LockSpanReasoner spans(locks);

  std::vector<const StoreInst *> stores;
  std::vector<const LoadInst *> loads;
  collectAccesses(*module->getFunction("writer"), stores, loads);
  ASSERT_EQ(stores.size(), 2u);

  // The two stores live in the same static critical section, but the loop can
  // release and re-take the lock between two dynamic iterations, so the value
  // written by the first store can escape.
  EXPECT_FALSE(spans.inSameSpan(stores[0], stores[1]));
}

TEST_F(LockSpanReasonerTest, ConditionalOverwriteIsNotUnconditional) {
  auto module = parseModule(R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0
    @cell = global i8 0

    define void @writer(i1 %cond) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      store i8 1, i8* @cell
      br i1 %cond, label %then, label %done

    then:
      store i8 2, i8* @cell
      br label %done

    done:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  mhp::LockSetAnalysis locks(*module);
  locks.analyze();
  LockSpanReasoner spans(locks);

  std::vector<const StoreInst *> stores;
  std::vector<const LoadInst *> loads;
  collectAccesses(*module->getFunction("writer"), stores, loads);
  ASSERT_EQ(stores.size(), 2u);

  EXPECT_TRUE(spans.inSameSpan(stores[0], stores[1]));
  // The overwrite only happens on one path, so it cannot retire the value that
  // the first store published.
  EXPECT_FALSE(spans.mustExecuteAfter(stores[0], stores[1]));
}

TEST_F(LockSpanReasonerTest, ReadLockDoesNotDefineASpan) {
  auto module = parseModule(R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_unlock(i8*)

    @lock = global i8 0
    @cell = global i8 0

    define void @reader() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      %first = load i8, i8* @cell
      %second = load i8, i8* @cell
      call i32 @pthread_rwlock_unlock(i8* @lock)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  mhp::LockSetAnalysis locks(*module);
  locks.analyze();
  LockSpanReasoner spans(locks);

  const Instruction *first =
      findInstructionByName(*module->getFunction("reader"), "first");
  const Instruction *second =
      findInstructionByName(*module->getFunction("reader"), "second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  // Shared-mode holders do not exclude each other, so a read lock cannot
  // justify retiring a value.
  EXPECT_FALSE(spans.inSameSpan(first, second));
}

} // namespace
