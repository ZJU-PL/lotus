#include "ThreadAPITestSupport.h"

TEST_F(ThreadAPITest, PreservesLinuxTryModesAndRecognizesAtomics) {
  const char *source = R"(
    declare i32 @pthread_rwlock_tryrdlock(i8*)
    declare i32 @pthread_rwlock_trywrlock(i8*)
    declare i32 @down_trylock(i8*)
    declare i32 @down_read_trylock(i8*)
    declare i32 @down_write_trylock(i8*)
    declare i32 @atomic_read(i8*)
    declare void @atomic_set(i8*, i32)
    declare i32 @atomic_cmpxchg(i8*, i32, i32)
    declare i32 @test_and_set_bit(i32, i8*)
    define void @main(i8* %lock) {
      call i32 @pthread_rwlock_tryrdlock(i8* %lock)
      call i32 @pthread_rwlock_trywrlock(i8* %lock)
      call i32 @down_trylock(i8* %lock)
      call i32 @down_read_trylock(i8* %lock)
      call i32 @down_write_trylock(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *pthread_read_try = &*it++;
  const Instruction *pthread_write_try = &*it++;
  const Instruction *exclusive_try = &*it++;
  const Instruction *read_try = &*it++;
  const Instruction *write_try = &*it;
  EXPECT_TRUE(api->isTryLock(pthread_read_try));
  EXPECT_EQ(api->describeLockSemantics(pthread_read_try).mode,
            ThreadAPI::LockMode::Shared);
  EXPECT_TRUE(api->getLockSemanticInfo(pthread_read_try).conditional);
  EXPECT_TRUE(api->isTryLock(pthread_write_try));
  EXPECT_EQ(api->describeLockSemantics(pthread_write_try).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_TRUE(api->getLockSemanticInfo(pthread_write_try).conditional);
  EXPECT_TRUE(api->isTryLock(exclusive_try));
  EXPECT_EQ(api->describeLockSemantics(exclusive_try).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_TRUE(api->isTryLock(read_try));
  EXPECT_EQ(api->describeLockSemantics(read_try).mode,
            ThreadAPI::LockMode::Shared);
  EXPECT_TRUE(api->isTryLock(write_try));
  EXPECT_EQ(api->describeLockSemantics(write_try).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_EQ(api->getType(module->getFunction("atomic_read")),
            ThreadAPI::TD_KERNEL_ATOMIC_READ);
  EXPECT_EQ(api->getType(module->getFunction("atomic_set")),
            ThreadAPI::TD_KERNEL_ATOMIC_WRITE);
  EXPECT_EQ(api->getType(module->getFunction("atomic_cmpxchg")),
            ThreadAPI::TD_KERNEL_ATOMIC_RMW);
  EXPECT_EQ(api->getType(module->getFunction("test_and_set_bit")),
            ThreadAPI::TD_KERNEL_ATOMIC_RMW);
}

TEST_F(ThreadAPITest, KernelThreadLayoutsCanUseCallResults) {
  const char *source = R"(
    declare i8* @kthread_run(i8* (i8*)*, i8*, i8*)
    declare i32 @kthread_stop(i8*)
    define i8* @worker(i8* %arg) { ret i8* %arg }
    define void @main(i8* %data, i8* %name) {
      %task = call i8* @kthread_run(i8* (i8*)* @worker, i8* %data,
                                    i8* %name)
      %result = call i32 @kthread_stop(i8* %task)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *fork = &*it++;
  const Instruction *join = &*it;

  EXPECT_EQ(api->getForkedThread(fork), fork);
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("worker"));
  EXPECT_EQ(api->getActualParmAtForkSite(fork),
            module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getJoinedThread(join), fork);
  EXPECT_EQ(api->getRetParmAtJoinedSite(join), join);
}

TEST_F(ThreadAPITest, KernelBitOperationsSeparateReadFromRmw) {
  const char *source = R"(
    declare i1 @test_bit(i64, i8*)
    declare void @set_bit(i64, i8*)
    declare void @clear_bit(i64, i8*)
    declare void @change_bit(i64, i8*)
    declare i1 @test_and_set_bit(i64, i8*)
    declare i1 @test_and_clear_bit(i64, i8*)
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("test_bit")),
            ThreadAPI::TD_KERNEL_ATOMIC_READ);
  for (const char *name : {"set_bit", "clear_bit", "change_bit",
                           "test_and_set_bit", "test_and_clear_bit"})
    EXPECT_EQ(api->getType(module->getFunction(name)),
              ThreadAPI::TD_KERNEL_ATOMIC_RMW);
}

TEST_F(ThreadAPITest, ExpandedKthreadCreateWakeSequenceLowersAsFork) {
  const char *source = R"(
    declare i8* @kthread_create_on_node(i8* (i8*)*, i8*, i32, i8*, ...)
    declare i32 @wake_up_process(i8*)
    define i8* @worker(i8* %data) { ret i8* %data }
    define void @main(i8* %data, i8* %name) {
      %task = call i8* (i8* (i8*)*, i8*, i32, i8*, ...)
          @kthread_create_on_node(i8* (i8*)* @worker, i8* %data, i32 -1,
                                  i8* %name)
      call i32 @wake_up_process(i8* %task)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *create = &*it++;
  const Instruction *wake = &*it;
  EXPECT_FALSE(api->isForkLike(create));
  EXPECT_TRUE(api->isForkLike(wake));
  EXPECT_EQ(api->getForkedThread(wake), create);
  EXPECT_EQ(api->getForkedFun(wake), module->getFunction("worker"));
  EXPECT_EQ(api->getActualParmAtForkSite(wake),
            module->getFunction("main")->getArg(0));
  auto payload = api->getForkPayloadArgs(wake);
  ASSERT_EQ(payload.size(), 1u);
  EXPECT_EQ(payload.front(), module->getFunction("main")->getArg(0));
}

TEST_F(ThreadAPITest, UnrelatedWakeUpProcessIsNotInventedAsFork) {
  const char *source = R"(
    declare i32 @wake_up_process(i8*)
    define void @main(i8* %existing_task) {
      call i32 @wake_up_process(i8* %existing_task)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *wake = &module->getFunction("main")->front().front();
  EXPECT_FALSE(api->isForkLike(wake));
  EXPECT_EQ(api->getForkedFun(wake), nullptr);
}

