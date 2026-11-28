#include "ThreadAPITestSupport.h"

TEST_F(ThreadAPITest, SharedLockPredicatesAreConsistentAcrossOverloads) {
  const char *source = R"(
    declare void @_ZNSt12shared_mutex11lock_sharedEv(i8*)
    declare void @_ZNSt12shared_mutex4lockEv(i8*)

    define void @main(i8* %m) {
    entry:
      call void @_ZNSt12shared_mutex11lock_sharedEv(i8* %m)
      call void @_ZNSt12shared_mutex4lockEv(i8* %m)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const auto *shared_call = llvm::dyn_cast<CallBase>(&*it++);
  const auto *exclusive_call = llvm::dyn_cast<CallBase>(&*it++);
  ASSERT_NE(shared_call, nullptr);
  ASSERT_NE(exclusive_call, nullptr);

  EXPECT_EQ(api->isReadLockAcquire(shared_call),
            api->isReadLockAcquire(&*shared_call));
  EXPECT_EQ(api->isWriteLockAcquire(shared_call),
            api->isWriteLockAcquire(&*shared_call));
  EXPECT_EQ(api->isReadLockAcquire(exclusive_call),
            api->isReadLockAcquire(&*exclusive_call));
  EXPECT_EQ(api->isWriteLockAcquire(exclusive_call),
            api->isWriteLockAcquire(&*exclusive_call));
}

TEST_F(ThreadAPITest, LockSemanticDescriptorCapturesModeAndOperation) {
  const char *source = R"(
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)
    declare i32 @pthread_mutex_trylock(i8*)
    declare i32 @pthread_rwlock_unlock(i8*)

    define void @main(i8* %lock) {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* %lock)
      call i32 @pthread_rwlock_wrlock(i8* %lock)
      call i32 @pthread_mutex_trylock(i8* %lock)
      call i32 @pthread_rwlock_unlock(i8* %lock)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *rdlock = &*it++;
  const Instruction *wrlock = &*it++;
  const Instruction *trylock = &*it++;
  const Instruction *unlock = &*it++;

  ThreadAPI::LockSemantics rd = api->describeLockSemantics(rdlock);
  ThreadAPI::LockSemantics wr = api->describeLockSemantics(wrlock);
  ThreadAPI::LockSemantics tr = api->describeLockSemantics(trylock);
  ThreadAPI::LockSemantics un = api->describeLockSemantics(unlock);

  EXPECT_TRUE(rd.is_lock_api);
  EXPECT_TRUE(rd.is_acquire);
  EXPECT_EQ(rd.mode, ThreadAPI::LockMode::Shared);

  EXPECT_TRUE(wr.is_lock_api);
  EXPECT_TRUE(wr.is_acquire);
  EXPECT_EQ(wr.mode, ThreadAPI::LockMode::Exclusive);

  EXPECT_TRUE(tr.is_lock_api);
  EXPECT_TRUE(tr.is_acquire);
  EXPECT_TRUE(tr.is_try);
  EXPECT_EQ(tr.mode, ThreadAPI::LockMode::Exclusive);

  EXPECT_TRUE(un.is_lock_api);
  EXPECT_TRUE(un.is_release);
}

TEST_F(ThreadAPITest, UsesCriticalNameAsAnalysisLockIdentity) {
  const char *source = R"(
    @crit = global [8 x i32] zeroinitializer

    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define void @main() {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *enter = &*it++;
  const Instruction *exit = &*it++;

  EXPECT_EQ(api->getAnalysisLockIdentity(enter),
            module->getNamedGlobal("crit"));
  EXPECT_EQ(api->getAnalysisLockIdentity(exit), module->getNamedGlobal("crit"));
}

TEST_F(ThreadAPITest, WrapperOperationsShareAnalysisLockIdentity) {
  const char *source = R"(
    @lock = global i8 0

    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)
    declare void @fake_unique_locklockEv(i8*)
    declare void @fake_unique_lockunlockEv(i8*)

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @fake_unique_lockunlockEv(i8* %wrapper)
      call void @fake_unique_locklockEv(i8* %wrapper)
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  std::vector<const Instruction *> calls;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      calls.push_back(&inst);
    }
  }
  ASSERT_EQ(calls.size(), 4u);
  const Instruction *ctor = calls[0];
  const Instruction *unlock = calls[1];
  const Instruction *lock = calls[2];
  const Instruction *dtor = calls[3];

  const Value *identity = api->getAnalysisLockIdentity(ctor);
  ASSERT_NE(identity, nullptr);
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(unlock));
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(lock));
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(dtor));
}

TEST_F(ThreadAPITest, ReportsSharedVsExclusiveLockSemantics) {
  const char *source = R"(
    @lock = global i8 0

    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)
    declare i32 @pthread_rwlock_unlock(i8*)

    define void @main() {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      call i32 @pthread_rwlock_unlock(i8* @lock)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  std::vector<const Instruction *> calls;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      calls.push_back(&inst);
    }
  }
  ASSERT_EQ(calls.size(), 3u);

  const auto read_info = api->getLockSemanticInfo(calls[0]);
  const auto write_info = api->getLockSemanticInfo(calls[1]);
  const auto unlock_info = api->getLockSemanticInfo(calls[2]);

  EXPECT_TRUE(read_info.isShared());
  EXPECT_TRUE(write_info.isExclusive());
  EXPECT_TRUE(unlock_info.isRelease());
  EXPECT_EQ(read_info.identity, module->getNamedGlobal("lock"));
  EXPECT_EQ(write_info.identity, module->getNamedGlobal("lock"));
  EXPECT_EQ(unlock_info.identity, module->getNamedGlobal("lock"));
}

TEST_F(ThreadAPITest, SemaphoreLoweringIsExplicitForBinaryAndCountingForms) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)
    declare void @_ZNSt16binary_semaphore7acquireEv(i8*)
    declare void @_ZNSt18counting_semaphore7acquireEv(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @binary_sem_wait(i8* %sem)
      call void @_ZNSt16binary_semaphore7acquireEv(i8* %sem)
      call void @_ZNSt18counting_semaphore7acquireEv(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *binary_sem_wait = module->getFunction("binary_sem_wait");
  const Function *binary_cpp =
      module->getFunction("_ZNSt16binary_semaphore7acquireEv");
  const Function *counting_cpp =
      module->getFunction("_ZNSt18counting_semaphore7acquireEv");
  ASSERT_NE(binary_sem_wait, nullptr);
  ASSERT_NE(binary_cpp, nullptr);
  ASSERT_NE(counting_cpp, nullptr);

  auto binary_info = api->getSemanticLoweringInfo(binary_sem_wait);
  EXPECT_EQ(binary_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      binary_sem_wait, ThreadAPI::SemanticLoweringOwner::LockSet));

  auto binary_cpp_info = api->getSemanticLoweringInfo(binary_cpp);
  EXPECT_EQ(binary_cpp_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      binary_cpp, ThreadAPI::SemanticLoweringOwner::LockSet));

  auto counting_info = api->getSemanticLoweringInfo(counting_cpp);
  EXPECT_EQ(counting_info.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(counting_info.reason, "counting-semaphore-runtime-unmodeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      counting_cpp, ThreadAPI::SemanticLoweringOwner::ExplicitFallback));

  auto generic_info =
      api->getSemanticLoweringInfo(ThreadAPI::TD_SEMAPHORE_ACQUIRE);
  EXPECT_EQ(generic_info.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
}

TEST_F(ThreadAPITest, CppWrapperLockIdentityResolvesUnderlyingMutex) {
  const char *source = R"(
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)

    @lock = global i8 0

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *ctor = &*it++;
  const Instruction *dtor = &*it++;
  const Value *lock = module->getNamedGlobal("lock");
  ASSERT_NE(lock, nullptr);

  EXPECT_EQ(api->getAnalysisLockIdentity(ctor), lock);
  EXPECT_EQ(api->getAnalysisLockIdentity(dtor), lock);
}

TEST_F(ThreadAPITest, TaggedRAIIConstructorsExposeOwnershipSemantics) {
  const char *source = R"(
    declare void @fake_unique_lock_defer_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_try_to_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_adopt_lock_C1E(i8*, i8*, i8*)
    declare void @fake_unique_lock_try_lockEv(i8*)
    declare void @fake_unique_lock_D1Ev(i8*)

    @lock = global i8 0
    @tag = global i8 0

    define void @main() {
    entry:
      %deferred = alloca i8
      %tried = alloca i8
      %adopted = alloca i8
      call void @fake_unique_lock_defer_lock_C1E(i8* %deferred, i8* @lock, i8* @tag)
      call void @fake_unique_lock_D1Ev(i8* %deferred)
      call void @fake_unique_lock_try_to_lock_C1E(i8* %tried, i8* @lock, i8* @tag)
      call void @fake_unique_lock_D1Ev(i8* %tried)
      call void @fake_unique_lock_adopt_lock_C1E(i8* %adopted, i8* @lock, i8* @tag)
      call void @fake_unique_lock_try_lockEv(i8* %deferred)
      call void @fake_unique_lock_D1Ev(i8* %adopted)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  std::vector<const Instruction *> calls;
  for (const Instruction &inst : instructions(*main_func))
    if (isa<CallBase>(&inst))
      calls.push_back(&inst);
  ASSERT_EQ(calls.size(), 7u);

  auto deferred = api->getLockSemanticInfo(calls[0]);
  EXPECT_EQ(deferred.ownership, ThreadAPI::LockOwnershipEffect::Deferred);
  EXPECT_FALSE(api->isTDAcquire(calls[0]));
  EXPECT_TRUE(api->getLockSemanticInfo(calls[1]).conditional);

  auto tried = api->getLockSemanticInfo(calls[2]);
  EXPECT_EQ(tried.ownership, ThreadAPI::LockOwnershipEffect::Try);
  EXPECT_TRUE(tried.is_try);
  EXPECT_TRUE(tried.conditional);
  EXPECT_TRUE(api->getLockSemanticInfo(calls[3]).conditional);

  auto adopted = api->getLockSemanticInfo(calls[4]);
  EXPECT_EQ(adopted.ownership, ThreadAPI::LockOwnershipEffect::Adopt);
  EXPECT_FALSE(api->isTDAcquire(calls[4]));
  EXPECT_TRUE(api->isTryLock(calls[5]));
  EXPECT_TRUE(api->isTDRelease(calls[6]));
}

TEST_F(ThreadAPITest, ScopedLockExposesEveryUnderlyingMutex) {
  const char *source = R"(
    declare void @fake_scoped_lock_C1E(i8*, i8*, i8*)
    declare void @fake_scoped_lock_D1Ev(i8*)
    @lock1 = global i8 0
    @lock2 = global i8 0
    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_scoped_lock_C1E(i8* %wrapper, i8* @lock1, i8* @lock2)
      call void @fake_scoped_lock_D1Ev(i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *ctor = &*it++;
  const Instruction *dtor = &*it;

  for (const Instruction *inst : {ctor, dtor}) {
    auto identities = api->getAnalysisLockIdentities(inst);
    ASSERT_EQ(identities.size(), 2u);
    EXPECT_EQ(identities[0], module->getNamedGlobal("lock1"));
    EXPECT_EQ(identities[1], module->getNamedGlobal("lock2"));
    EXPECT_EQ(api->getAnalysisLockIdentity(inst), nullptr);
  }
}

TEST_F(ThreadAPITest, ClassifiesTryLockModesWithoutBlockingShadowing) {
  const char *source = R"(
    declare i32 @_ZNSt5mutex8try_lockEv(i8*)
    declare i32 @_ZNSt12shared_mutex15try_lock_sharedEv(i8*)
    declare i32 @_ZNSt12shared_mutex8try_lockEv(i8*)
    declare i32 @_ZNSt18shared_timed_mutex19try_lock_shared_forEi(i8*, i32)
    declare i32 @_ZNSt18shared_timed_mutex12try_lock_forEi(i8*, i32)
    declare i32 @_ZNSt20counting_semaphoreILl4EE11try_acquireEv(i8*)
    define void @main(i8* %lock) {
      call i32 @_ZNSt5mutex8try_lockEv(i8* %lock)
      call i32 @_ZNSt12shared_mutex15try_lock_sharedEv(i8* %lock)
      call i32 @_ZNSt12shared_mutex8try_lockEv(i8* %lock)
      call i32 @_ZNSt18shared_timed_mutex19try_lock_shared_forEi(i8* %lock,
                                                                 i32 1)
      call i32 @_ZNSt18shared_timed_mutex12try_lock_forEi(i8* %lock, i32 1)
      call i32 @_ZNSt20counting_semaphoreILl4EE11try_acquireEv(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const std::vector<ThreadAPI::LockMode> expected_modes = {
      ThreadAPI::LockMode::Exclusive, ThreadAPI::LockMode::Shared,
      ThreadAPI::LockMode::Exclusive, ThreadAPI::LockMode::Shared,
      ThreadAPI::LockMode::Exclusive};
  auto it = module->getFunction("main")->front().begin();
  for (ThreadAPI::LockMode expected_mode : expected_modes) {
    const Instruction *call = &*it++;
    const auto semantics = api->describeLockSemantics(call);
    EXPECT_TRUE(semantics.is_acquire);
    EXPECT_TRUE(semantics.is_try);
    EXPECT_EQ(semantics.mode, expected_mode);
  }
  const Instruction *semaphore_try = &*it;
  EXPECT_EQ(api->getType(api->getCallee(semaphore_try)),
            ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE);
  EXPECT_TRUE(api->isTryLock(semaphore_try));
}

TEST_F(ThreadAPITest, UnwrapsOrdinaryConditionVariableWaitMutex) {
  const char *source = R"(
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(
        i8*, i8*)
    @cv = global i8 0
    @mutex = global i8 0
    define void @main() {
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @mutex)
      call void @_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(
          i8* @cv, i8* %wrapper)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  ++it;
  const Instruction *ctor = &*it++;
  const Instruction *wait = &*it;
  EXPECT_EQ(api->getCondMutex(wait), module->getNamedGlobal("mutex"));
  EXPECT_EQ(api->getCondMutex(wait), api->getAnalysisLockIdentity(ctor));
}

TEST_F(ThreadAPITest, WrapperMembersUseUnderlyingMutexAndCorrectMode) {
  const char *source = R"(
    declare void @_ZNSt11unique_lockISt5mutexEC1ERS0_(i8*, i8*)
    declare void @_ZNSt11unique_lockISt5mutexE4lockEv(i8*)
    declare i1 @_ZNSt11unique_lockISt5mutexE8try_lockEv(i8*)
    declare void @_ZNSt11unique_lockISt5mutexE6unlockEv(i8*)
    declare void @_ZNSt11shared_lockISt12shared_mutexEC1ERS0_(i8*, i8*)
    declare void @_ZNSt11shared_lockISt12shared_mutexE4lockEv(i8*)
    declare i1 @_ZNSt11shared_lockISt12shared_mutexE8try_lockEv(i8*)
    declare void @_ZNSt11shared_lockISt12shared_mutexE6unlockEv(i8*)
    @mutex = global i8 0
    @shared = global i8 0
    define void @main() {
      %u = alloca i8
      %s = alloca i8
      call void @_ZNSt11unique_lockISt5mutexEC1ERS0_(i8* %u, i8* @mutex)
      call void @_ZNSt11unique_lockISt5mutexE4lockEv(i8* %u)
      call i1 @_ZNSt11unique_lockISt5mutexE8try_lockEv(i8* %u)
      call void @_ZNSt11unique_lockISt5mutexE6unlockEv(i8* %u)
      call void @_ZNSt11shared_lockISt12shared_mutexEC1ERS0_(i8* %s,
                                                             i8* @shared)
      call void @_ZNSt11shared_lockISt12shared_mutexE4lockEv(i8* %s)
      call i1 @_ZNSt11shared_lockISt12shared_mutexE8try_lockEv(i8* %s)
      call void @_ZNSt11shared_lockISt12shared_mutexE6unlockEv(i8* %s)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  std::vector<const Instruction *> calls;
  for (const Instruction &inst : instructions(*module->getFunction("main")))
    if (isa<CallBase>(inst))
      calls.push_back(&inst);
  ASSERT_EQ(calls.size(), 8u);

  for (unsigned i : {1u, 2u, 3u})
    EXPECT_EQ(api->getAnalysisLockIdentity(calls[i]),
              module->getNamedGlobal("mutex"));
  for (unsigned i : {5u, 6u, 7u})
    EXPECT_EQ(api->getAnalysisLockIdentity(calls[i]),
              module->getNamedGlobal("shared"));
  EXPECT_EQ(api->getType(api->getCallee(calls[1])),
            ThreadAPI::TD_UNIQUE_LOCK_LOCK);
  EXPECT_EQ(api->getType(api->getCallee(calls[5])),
            ThreadAPI::TD_SHARED_RDLOCK);
  EXPECT_EQ(api->describeLockSemantics(calls[5]).mode,
            ThreadAPI::LockMode::Shared);
  EXPECT_EQ(api->getLockSemanticInfo(calls[2]).try_success,
            ThreadAPI::TryLockSuccess::NonZero);
  EXPECT_EQ(api->getLockSemanticInfo(calls[6]).try_success,
            ThreadAPI::TryLockSuccess::NonZero);
}

TEST_F(ThreadAPITest, SharedMutexUnlockModeMatchesTheMemberOperation) {
  const char *source = R"(
    declare void @_ZNSt12shared_mutex6unlockEv(i8*)
    declare void @_ZNSt12shared_mutex13unlock_sharedEv(i8*)
    define void @main(i8* %lock) {
      call void @_ZNSt12shared_mutex6unlockEv(i8* %lock)
      call void @_ZNSt12shared_mutex13unlock_sharedEv(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  EXPECT_EQ(api->describeLockSemantics(&*it++).mode,
            ThreadAPI::LockMode::Exclusive);
  EXPECT_EQ(api->describeLockSemantics(&*it).mode,
            ThreadAPI::LockMode::Shared);
}

TEST_F(ThreadAPITest, WrapperLookupDoesNotUseLaterReconstruction) {
  const char *source = R"(
    declare void @fake_unique_lock_C1E(i8*, i8*)
    declare void @fake_unique_lock_unlockEv(i8*)
    @first = global i8 0
    @second = global i8 0
    define void @main() {
      %wrapper = alloca i8
      call void @fake_unique_lock_C1E(i8* %wrapper, i8* @first)
      call void @fake_unique_lock_unlockEv(i8* %wrapper)
      call void @fake_unique_lock_C1E(i8* %wrapper, i8* @second)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  ++it;
  ++it;
  EXPECT_EQ(api->getAnalysisLockIdentity(&*it),
            module->getNamedGlobal("first"));
}

TEST_F(ThreadAPITest, TryAndConditionalAcquirePolarityIsPerAPI) {
  const char *source = R"(
    declare i32 @pthread_mutex_trylock(i8*)
    declare i1 @_ZNSt5mutex8try_lockEv(i8*)
    declare i32 @omp_test_lock(i8*)
    declare i32 @down_trylock(i8*)
    declare i32 @down_read_trylock(i8*)
    declare i32 @mutex_lock_interruptible(i8*)
    declare i32 @pthread_mutex_timedlock(i8*, i8*)
    define void @main(i8* %lock) {
      call i32 @pthread_mutex_trylock(i8* %lock)
      call i1 @_ZNSt5mutex8try_lockEv(i8* %lock)
      call i32 @omp_test_lock(i8* %lock)
      call i32 @down_trylock(i8* %lock)
      call i32 @down_read_trylock(i8* %lock)
      call i32 @mutex_lock_interruptible(i8* %lock)
      call i32 @pthread_mutex_timedlock(i8* %lock, i8* null)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const std::vector<ThreadAPI::TryLockSuccess> expected = {
      ThreadAPI::TryLockSuccess::Zero, ThreadAPI::TryLockSuccess::NonZero,
      ThreadAPI::TryLockSuccess::NonZero, ThreadAPI::TryLockSuccess::Zero,
      ThreadAPI::TryLockSuccess::NonZero, ThreadAPI::TryLockSuccess::Zero,
      ThreadAPI::TryLockSuccess::Zero};
  auto it = module->getFunction("main")->front().begin();
  for (ThreadAPI::TryLockSuccess success : expected) {
    const Instruction *call = &*it++;
    const auto info = api->getLockSemanticInfo(call);
    EXPECT_TRUE(info.conditional);
    EXPECT_EQ(info.try_success, success);
  }
}

TEST_F(ThreadAPITest, WrappedRwTryLockUsesCanonicalConditionalDescriptor) {
  const char *source = R"(
    declare i32 @__wrap_pthread_rwlock_tryrdlock(i8*)
    define void @main(i8* %lock) {
      call i32 @__wrap_pthread_rwlock_tryrdlock(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *call = &module->getFunction("main")->front().front();
  const auto semantics = api->getLockSemanticInfo(call);
  EXPECT_TRUE(api->isTryLock(call));
  EXPECT_TRUE(semantics.conditional);
  EXPECT_TRUE(semantics.isShared());
  EXPECT_EQ(semantics.try_success, ThreadAPI::TryLockSuccess::Zero);
}

