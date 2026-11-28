#include "ThreadAPITestSupport.h"

TEST_F(ThreadAPITest, DescribesMPIBarrierUsingStructuredConfig) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  ThreadAPI::APIDescription desc =
      api->describe(module->getFunction("MPI_Ibarrier"));
  EXPECT_EQ(desc.type, ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(desc.library, ThreadAPI::RuntimeLibrary::MPI);
  EXPECT_EQ(desc.semantic_tag, "ibarrier");
  EXPECT_TRUE(desc.from_config);
}

TEST_F(ThreadAPITest, ReportsExplicitSemanticLoweringStatus) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  auto async = api->getSemanticLoweringInfo(ThreadAPI::TD_ASYNC);
  auto future_get = api->getSemanticLoweringInfo(ThreadAPI::TD_FUTURE_GET);
  auto omp_atomic =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_ATOMIC_START);
  auto task_complete =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_TASK_COMPLETE);
  auto doacross_submit =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
  auto atomic_wait = api->getSemanticLoweringInfo(ThreadAPI::TD_ATOMIC_WAIT);

  EXPECT_EQ(async.kind, ThreadAPI::SemanticLoweringKind::Deferred);
  EXPECT_STREQ(async.reason, "async-launch-policy-witness");
  EXPECT_NE(async.owners, 0u);
  EXPECT_EQ(future_get.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(future_get.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_FUTURE_GET, ThreadAPI::SemanticLoweringOwner::HB));
  EXPECT_EQ(omp_atomic.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(omp_atomic.reason, "openmp-atomic-runtime-unmodeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_ATOMIC_START,
      ThreadAPI::SemanticLoweringOwner::ExplicitFallback));
  EXPECT_EQ(task_complete.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(task_complete.reason, "modeled");
  EXPECT_TRUE(
      api->hasSemanticLoweringOwner(ThreadAPI::TD_OMP_TASK_COMPLETE,
                                    ThreadAPI::SemanticLoweringOwner::OpenMP));
  EXPECT_EQ(doacross_submit.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(doacross_submit.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_DOACROSS_SUBMIT, ThreadAPI::SemanticLoweringOwner::HB));
  EXPECT_EQ(atomic_wait.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(atomic_wait.reason, "cpp-atomic-wait-runtime-unmodeled");
}

TEST_F(ThreadAPITest, SpecialSemanticLoweringStatesStayExplicitlyEnumerated) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  std::set<ThreadAPI::TD_TYPE> expected_non_modeled = {
      ThreadAPI::TD_DUMMY,
      ThreadAPI::TD_ASYNC,
      ThreadAPI::TD_CUDA_MULTI_DEVICE_LAUNCH,
      ThreadAPI::TD_OMP_ATOMIC_START,
      ThreadAPI::TD_OMP_ATOMIC_END,
      ThreadAPI::TD_OMP_CANCEL,
      ThreadAPI::TD_OMP_TARGET_DATA_UPDATE,
      ThreadAPI::TD_OMP_TEAMS,
      ThreadAPI::TD_OMP_TEAMS_HOST,
      ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE,
      ThreadAPI::TD_OMP_DISTRIBUTE,
      ThreadAPI::TD_OMP_DISTRIBUTE_STATIC,
      ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC,
      ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE,
      ThreadAPI::TD_OMP_LOOP_STATIC_INIT,
      ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT,
      ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT,
      ThreadAPI::TD_OMP_AFFINITY,
      ThreadAPI::TD_OMP_SCOPE_START,
      ThreadAPI::TD_OMP_SCOPE_END,
      ThreadAPI::TD_OMP_TASKLOOP_SIMD,
      ThreadAPI::TD_OMP_TASKLOOP_FINI,
      ThreadAPI::TD_OMP_INTEROP_INIT,
      ThreadAPI::TD_OMP_INTEROP_FINI,
      ThreadAPI::TD_SEMAPHORE_ACQUIRE,
      ThreadAPI::TD_SEMAPHORE_RELEASE,
      ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE,
      ThreadAPI::TD_ATOMIC_WAIT,
      ThreadAPI::TD_ATOMIC_NOTIFY_ONE,
      ThreadAPI::TD_ATOMIC_NOTIFY_ALL,
      ThreadAPI::TD_JTHREAD_DTOR,
      ThreadAPI::TD_MPI_SESSION_GET_INFO,
      ThreadAPI::TD_MPI_SESSION_GET_NUM_ERRCODES,
      ThreadAPI::TD_MPI_SESSION_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_SESSION_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_ERRHANDLER_CREATE,
      ThreadAPI::TD_MPI_ERRHANDLER_FREE,
      ThreadAPI::TD_MPI_COMM_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_COMM_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_COMM_CALL_ERRHANDLER,
      ThreadAPI::TD_MPI_WIN_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_WIN_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_FILE_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_FILE_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_ERROR_CLASS,
      ThreadAPI::TD_MPI_ERROR_STRING,
      ThreadAPI::TD_MPI_INFO_CREATE,
      ThreadAPI::TD_MPI_INFO_DUP,
      ThreadAPI::TD_MPI_INFO_FREE,
      ThreadAPI::TD_MPI_INFO_GET,
      ThreadAPI::TD_MPI_INFO_GET_VALUELEN,
      ThreadAPI::TD_MPI_INFO_GET_NKEYS,
      ThreadAPI::TD_MPI_INFO_GET_NTHKEY,
      ThreadAPI::TD_MPI_INFO_GET_KEYVAL,
      ThreadAPI::TD_MPI_INFO_SET,
      ThreadAPI::TD_MPI_INFO_DELETE,
      ThreadAPI::TD_MPI_INFO_C2F,
      ThreadAPI::TD_MPI_INFO_CREATE_ENV,
      ThreadAPI::TD_MPI_INFO_FREE_ENV,
      ThreadAPI::TD_MPI_GET_COUNT,
      ThreadAPI::TD_MPI_GET_ELEMENTS,
      ThreadAPI::TD_MPI_GET_ELEMENTS_X,
      ThreadAPI::TD_MPI_STATUS_SIZE,
      ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS,
      ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS_X,
      ThreadAPI::TD_KERNEL_ATOMIC_READ,
      ThreadAPI::TD_KERNEL_ATOMIC_WRITE,
      ThreadAPI::TD_KERNEL_ATOMIC_RMW,
  };

  for (int raw = static_cast<int>(ThreadAPI::TD_DUMMY);
       raw <= static_cast<int>(ThreadAPI::TD_KERNEL_MEMORY_BARRIER); ++raw) {
    ThreadAPI::TD_TYPE type = static_cast<ThreadAPI::TD_TYPE>(raw);
    const char *name = ThreadAPI::tdTypeToString(type);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(name[0], '\0');

    ThreadAPI::SemanticLoweringInfo info = api->getSemanticLoweringInfo(type);
    ASSERT_NE(info.reason, nullptr);
    EXPECT_NE(info.reason[0], '\0') << name;

    if (expected_non_modeled.count(type) != 0) {
      EXPECT_NE(info.kind, ThreadAPI::SemanticLoweringKind::Modeled) << name;
      if (type != ThreadAPI::TD_ASYNC &&
          info.kind != ThreadAPI::SemanticLoweringKind::Deferred) {
        EXPECT_NE(info.owners &
                      ThreadAPI::semanticLoweringOwnerMask(
                          ThreadAPI::SemanticLoweringOwner::ExplicitFallback),
                  0u)
            << name;
      }
    } else {
      EXPECT_EQ(info.kind, ThreadAPI::SemanticLoweringKind::Modeled) << name;
      EXPECT_NE(info.owners, 0u) << name;
    }
  }
}

TEST_F(ThreadAPITest, ModeledConcurrencyFunctionsExposeConcreteLoweringOwners) {
  const char *source = R"(
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)

    @crit = global [8 x i32] zeroinitializer

    define void @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *critical = module->getFunction("__kmpc_critical");
  const Function *idup = module->getFunction("MPI_Comm_idup");
  ASSERT_NE(critical, nullptr);
  ASSERT_NE(idup, nullptr);

  ThreadAPI::SemanticLoweringInfo critical_info =
      api->getSemanticLoweringInfo(critical);
  EXPECT_EQ(critical_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      critical, ThreadAPI::SemanticLoweringOwner::OpenMP));
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      critical, ThreadAPI::SemanticLoweringOwner::LockSet));

  ThreadAPI::SemanticLoweringInfo idup_info =
      api->getSemanticLoweringInfo(idup);
  EXPECT_EQ(idup_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      idup, ThreadAPI::SemanticLoweringOwner::MPI));
}

TEST_F(ThreadAPITest, MPIConfiguredAPIsHaveConsistentLoweringLibraries) {
  const char *source = R"(
    declare i32 @MPI_Session_get_info(i8*, i8*)
    declare i32 @MPI_Type_get_extent(i32, i64*, i64*)
    declare i32 @MPI_Cart_create(i8*, i32, i32*, i32*, i32, i8*)

    define i32 @main(i8* %session, i8* %info, i64* %lb, i64* %extent,
                     i8* %comm, i32* %dims, i32* %periods, i8* %newcomm) {
    entry:
      call i32 @MPI_Session_get_info(i8* %session, i8* %info)
      call i32 @MPI_Type_get_extent(i32 0, i64* %lb, i64* %extent)
      call i32 @MPI_Cart_create(i8* %comm, i32 1, i32* %dims, i32* %periods,
                                i32 0, i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *session = module->getFunction("MPI_Session_get_info");
  const Function *extent = module->getFunction("MPI_Type_get_extent");
  const Function *cart = module->getFunction("MPI_Cart_create");
  ASSERT_NE(session, nullptr);
  ASSERT_NE(extent, nullptr);
  ASSERT_NE(cart, nullptr);

  EXPECT_NE(api->getType(session), ThreadAPI::TD_DUMMY);
  EXPECT_NE(api->getType(extent), ThreadAPI::TD_DUMMY);
  EXPECT_NE(api->getType(cart), ThreadAPI::TD_DUMMY);

  auto session_type_info = api->getSemanticLoweringInfo(api->getType(session));
  auto session_func_info = api->getSemanticLoweringInfo(session);
  EXPECT_EQ(session_type_info.kind, session_func_info.kind);
  EXPECT_EQ(session_type_info.owners, session_func_info.owners);

  auto extent_type_info = api->getSemanticLoweringInfo(api->getType(extent));
  auto extent_func_info = api->getSemanticLoweringInfo(extent);
  EXPECT_EQ(extent_type_info.kind, extent_func_info.kind);
  EXPECT_EQ(extent_type_info.owners, extent_func_info.owners);

  auto cart_type_info = api->getSemanticLoweringInfo(api->getType(cart));
  auto cart_func_info = api->getSemanticLoweringInfo(cart);
  EXPECT_EQ(cart_type_info.kind, cart_func_info.kind);
  EXPECT_EQ(cart_type_info.owners, cart_func_info.owners);
}

TEST_F(ThreadAPITest, NormalizesWrappedAndOpenMPIForwarderNames) {
  const char *source = R"(
    declare i32 @__wrap_MPI_Barrier(i8*)
    declare i32 @__wrap_PMPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @ompi_mpi_allreduce(i8*, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @__wrap_MPI_Barrier(i8* %comm)
      call i32 @__wrap_PMPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @ompi_mpi_allreduce(i8* null, i8* null, i32 1, i32 0, i32 0,
                                   i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__wrap_MPI_Barrier")),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("__wrap_PMPI_Bcast")),
            ThreadAPI::TD_MPI_BCAST);
  EXPECT_EQ(api->getType(module->getFunction("ompi_mpi_allreduce")),
            ThreadAPI::TD_MPI_ALLREDUCE);
  EXPECT_EQ(api->getRuntimeLibrary(module->getFunction("ompi_mpi_allreduce")),
            ThreadAPI::RuntimeLibrary::MPI);
}

TEST_F(ThreadAPITest, JoinAndHareAccessorsAreBoundsChecked) {
  const char *source = R"(
    declare void @_ZNSt6thread4joinEv(i8*)
    declare void @_ZNSt7jthread4joinEv(i8*)
    declare void @hare_parallel_for(i8*)
    define void @main(i8* %handle) {
      call void @_ZNSt6thread4joinEv(i8* %handle)
      call void @_ZNSt7jthread4joinEv(i8* %handle)
      call void @hare_parallel_for(i8* %handle)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *join = &*it++;
  const Instruction *jjoin = &*it++;
  const Instruction *hare = &*it++;
  EXPECT_NE(api->getJoinedThread(join), nullptr);
  EXPECT_EQ(api->getRetParmAtJoinedSite(join), nullptr);
  EXPECT_NE(api->getJoinedThread(jjoin), nullptr);
  EXPECT_EQ(api->getRetParmAtJoinedSite(jjoin), nullptr);
  EXPECT_EQ(api->getTaskFuncAtHareParForSite(hare), nullptr);
  EXPECT_EQ(api->getTaskDataAtHareParForSite(hare), nullptr);
}

TEST_F(ThreadAPITest, ResolvesAliasesAndConfiguredLinkerWrappers) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    @pthread_mutex_lock_alias = alias i32 (i8*),
        i32 (i8*)* @pthread_mutex_lock
    declare i32 @__wrap_pthread_mutex_lock(i8*)
    define void @main(i8* %mutex) {
      call i32 @pthread_mutex_lock_alias(i8* %mutex)
      call i32 @__wrap_pthread_mutex_lock(i8* %mutex)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *alias_call = &*it++;
  const Instruction *wrapper_call = &*it;
  EXPECT_EQ(api->getCallee(alias_call), module->getFunction("pthread_mutex_lock"));
  EXPECT_EQ(api->getType(api->getCallee(alias_call)), ThreadAPI::TD_ACQUIRE);
  EXPECT_EQ(api->getType(api->getCallee(wrapper_call)), ThreadAPI::TD_ACQUIRE);
  EXPECT_EQ(api->getAnalysisLockIdentity(alias_call),
            module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getAnalysisLockIdentity(wrapper_call),
            module->getFunction("main")->getArg(0));
}

TEST_F(ThreadAPITest, RejectsSynchronizationAndCUDANearMissNames) {
  const char *source = R"(
    declare void @debug_mutex_lockEv(i8*)
    declare i32 @my_atomic_helper(i8*)
    declare void @copy_memcpy_stats(i8*)
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("debug_mutex_lockEv")),
            ThreadAPI::TD_DUMMY);
  EXPECT_EQ(api->getType(module->getFunction("my_atomic_helper")),
            ThreadAPI::TD_DUMMY);
  EXPECT_EQ(api->getType(module->getFunction("copy_memcpy_stats")),
            ThreadAPI::TD_DUMMY);
}

TEST_F(ThreadAPITest, PublicAliasSpellingPrecedesPrivateImplementation) {
  const char *source = R"(
    define i32 @__libc_internal_lock(i8* %lock) { ret i32 0 }
    @pthread_mutex_lock = alias i32 (i8*),
        i32 (i8*)* @__libc_internal_lock
    define void @main(i8* %lock) {
      call i32 @pthread_mutex_lock(i8* %lock)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const auto *call = dyn_cast<CallBase>(
      &module->getFunction("main")->front().front());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(api->getCallee(call), module->getFunction("__libc_internal_lock"));
  EXPECT_EQ(api->getType(call), ThreadAPI::TD_ACQUIRE);
  EXPECT_TRUE(api->isTDAcquire(call));
  EXPECT_EQ(api->getAnalysisLockIdentity(call),
            module->getFunction("main")->getArg(0));
}

TEST_F(ThreadAPITest, PublicForkAndJoinAliasesKeepConfiguredLayouts) {
  const char *source = R"(
    define i32 @private_create(i8** %thread, i8* %attr,
                               i8* (i8*)* %start, i8* %arg) {
      ret i32 0
    }
    define i32 @private_join(i8* %thread, i8** %result) { ret i32 0 }
    @pthread_create = alias i32 (i8**, i8*, i8* (i8*)*, i8*),
        i32 (i8**, i8*, i8* (i8*)*, i8*)* @private_create
    @pthread_join = alias i32 (i8*, i8**),
        i32 (i8*, i8**)* @private_join
    define i8* @worker(i8* %arg) { ret i8* %arg }
    define void @main(i8** %thread, i8* %arg, i8** %result) {
      call i32 @pthread_create(i8** %thread, i8* null,
                               i8* (i8*)* @worker, i8* %arg)
      %handle = load i8*, i8** %thread
      call i32 @pthread_join(i8* %handle, i8** %result)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *fork = &*it++;
  ++it;
  const Instruction *join = &*it;
  EXPECT_TRUE(api->isForkLike(fork));
  EXPECT_EQ(api->getForkedThread(fork),
            module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("worker"));
  EXPECT_EQ(api->getActualParmAtForkSite(fork),
            module->getFunction("main")->getArg(1));
  EXPECT_TRUE(api->isJoinLike(join));
  EXPECT_EQ(api->getRetParmAtJoinedSite(join),
            module->getFunction("main")->getArg(2));
}

