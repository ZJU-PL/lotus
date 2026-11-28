#include "ThreadAPITestSupport.h"

TEST_F(ThreadAPITest, ParsesExtendedTypeNames) {
  EXPECT_EQ(ThreadAPI::stringToType("TD_CANCEL"), ThreadAPI::TD_CANCEL);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_BARRIER"),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(ThreadAPI::stringToType("TD_SHARED_LOCK_DTOR"),
            ThreadAPI::TD_SHARED_LOCK_DTOR);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_TASKWAIT_DEPS"),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_SINGLE_END"),
            ThreadAPI::TD_OMP_SINGLE_END);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_ORDERED_START"),
            ThreadAPI::TD_OMP_ORDERED_START);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_TARGET_DATA_UPDATE"),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_PERSISTENT_SEND_INIT"),
            ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_PERSISTENT_RECV_INIT"),
            ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_REQUEST_START"),
            ThreadAPI::TD_MPI_REQUEST_START);
}
TEST_F(ThreadAPITest, PthreadCancelIsNotClassifiedAsJoin) {
  const char *source = R"(
    declare i32 @pthread_cancel(i8*)

    define i32 @main(i8* %tid) {
    entry:
      %cancel = call i32 @pthread_cancel(i8* %tid)
      ret i32 %cancel
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *cancel_func = module->getFunction("pthread_cancel");
  ASSERT_NE(cancel_func, nullptr);
  EXPECT_EQ(api->getType(cancel_func), ThreadAPI::TD_CANCEL);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *cancel_call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDJoin(cancel_call));
}

TEST_F(ThreadAPITest, PthreadRecognitionHonorsConcurrencyConfig) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8**)
    declare i32 @pthread_mutex_lock(i8*)
    declare void @__kmpc_barrier(i8*, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  concurrency::ConcurrencyConfig none(
      concurrency::ThreadingModelOptions::None);
  api->setConfig(none);
  EXPECT_EQ(api->getType(module->getFunction("pthread_create")),
            ThreadAPI::TD_DUMMY);
  EXPECT_EQ(api->getType(module->getFunction("pthread_join")),
            ThreadAPI::TD_DUMMY);
  EXPECT_EQ(api->getType(module->getFunction("pthread_mutex_lock")),
            ThreadAPI::TD_DUMMY);

  concurrency::ConcurrencyConfig pthread_only(
      concurrency::ThreadingModelOptions::EnablePthread);
  api->setConfig(pthread_only);
  EXPECT_EQ(api->getType(module->getFunction("pthread_create")),
            ThreadAPI::TD_FORK);
  EXPECT_EQ(api->getType(module->getFunction("pthread_join")),
            ThreadAPI::TD_JOIN);
  EXPECT_EQ(api->getType(module->getFunction("pthread_mutex_lock")),
            ThreadAPI::TD_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_barrier")),
            ThreadAPI::TD_DUMMY);

  ThreadAPI::resetThreadAPI();
}
TEST_F(ThreadAPITest, DistinguishesBlockingAndNonBlockingMPICollectives) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %bar = call i32 @MPI_Barrier(i8* %comm)
      %ibar = call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      %bcast = call i32 @MPI_Bcast(i8* null, i32 0, i32 0, i32 0, i8* %comm)
      %ibcast = call i32 @MPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *bar = &*it++;
  const Instruction *ibar = &*it++;
  const Instruction *bcast = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_TRUE(api->isBlockingMPIBarrier(bar));
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isBlockingMPICollective(bcast));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}
TEST_F(ThreadAPITest, NormalizesPMPIAliasesForMPIClassification) {
  const char *source = R"(
    declare i32 @PMPI_Ibarrier(i8*, i8*)
    declare i32 @PMPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %ibar = call i32 @PMPI_Ibarrier(i8* %comm, i8* %req)
      %ibcast = call i32 @PMPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *ibar = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_EQ(api->getType(module->getFunction("PMPI_Ibarrier")),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("PMPI_Ibcast")),
            ThreadAPI::TD_MPI_BCAST);
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}
TEST_F(ThreadAPITest, MatchesSpecificOpenMPTargetDataBeforeGenericTarget) {
  const char *source = R"(
    declare void @__tgt_target_data_begin(i64, i8*)
    declare void @__tgt_target_data_end(i64, i8*)

    define void @main() {
    entry:
      call void @__tgt_target_data_begin(i64 0, i8* null)
      call void @__tgt_target_data_end(i64 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_begin")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_end")),
            ThreadAPI::TD_OMP_TARGET_DATA_END);
}
TEST_F(ThreadAPITest, ClassifiesCUDAOperations) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaDeviceSynchronize()
    declare void @llvm.nvvm.barrier0()
    declare void @llvm.nvvm.bar.warp.sync(i32)
    declare void @llvm.nvvm.membar.gl()
    declare i32 @atomicAdd(i32*, i32)

    define void @kernel(i32* %ptr) {
    entry:
      call void @llvm.nvvm.barrier0()
      call void @llvm.nvvm.bar.warp.sync(i32 -1)
      call void @llvm.nvvm.membar.gl()
      %old = call i32 @atomicAdd(i32* %ptr, i32 1)
      ret void
    }

    define i32 @main(i32* %ptr) {
    entry:
      call void @__set_CUDAConfig(i32 2, i32 32)
      call void @kernel(i32* %ptr)
      %sync = call i32 @cudaDeviceSynchronize()
      ret i32 %sync
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  EXPECT_EQ(api->getType(module->getFunction("__set_CUDAConfig")),
            ThreadAPI::TD_CUDA_KERNEL_LAUNCH);
  EXPECT_EQ(api->getType(module->getFunction("cudaDeviceSynchronize")),
            ThreadAPI::TD_CUDA_DEVICE_SYNC);
  EXPECT_EQ(api->getType(module->getFunction("llvm.nvvm.barrier0")),
            ThreadAPI::TD_CUDA_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("llvm.nvvm.bar.warp.sync")),
            ThreadAPI::TD_CUDA_WARP_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("llvm.nvvm.membar.gl")),
            ThreadAPI::TD_CUDA_MEMORY_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("atomicAdd")),
            ThreadAPI::TD_CUDA_ATOMIC);
}
TEST_F(ThreadAPITest, ClassifiesExtendedCUDAHostRuntimeOperations) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque

    declare i32 @cudaStreamCreate(%stream_t**)
    declare i32 @cudaStreamSynchronize(%stream_t*)
    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaEventSynchronize(%event_t*)
    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, %stream_t*)
    declare i32 @cudaFree(i8*)
    declare i32 @cudaMemAdvise(i8*, i64, i32, i32)

    define i32 @main(%stream_t** %s, %event_t* %e, i8* %dst, i8* %src) {
    entry:
      %stream = load %stream_t*, %stream_t** %s
      %c0 = call i32 @cudaStreamCreate(%stream_t** %s)
      %c1 = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 64, i32 1, %stream_t* %stream)
      %c2 = call i32 @cudaEventRecord(%event_t* %e, %stream_t* %stream)
      %c3 = call i32 @cudaStreamSynchronize(%stream_t* %stream)
      %c4 = call i32 @cudaEventSynchronize(%event_t* %e)
      %c5 = call i32 @cudaFree(i8* %dst)
      %c6 = call i32 @cudaMemAdvise(i8* %dst, i64 64, i32 0, i32 0)
      ret i32 %c6
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  EXPECT_EQ(api->getType(module->getFunction("cudaStreamCreate")),
            ThreadAPI::TD_CUDA_STREAM);
  EXPECT_EQ(api->getType(module->getFunction("cudaStreamSynchronize")),
            ThreadAPI::TD_CUDA_STREAM);
  EXPECT_EQ(api->getType(module->getFunction("cudaEventRecord")),
            ThreadAPI::TD_CUDA_EVENT);
  EXPECT_EQ(api->getType(module->getFunction("cudaEventSynchronize")),
            ThreadAPI::TD_CUDA_EVENT);
  EXPECT_EQ(api->getType(module->getFunction("cudaMemcpyAsync")),
            ThreadAPI::TD_CUDA_MEMCPY);
  EXPECT_EQ(api->getType(module->getFunction("cudaFree")),
            ThreadAPI::TD_CUDA_FREE);
  EXPECT_EQ(api->getType(module->getFunction("cudaMemAdvise")),
            ThreadAPI::TD_CUDA_UNIFIED_MEMORY);
}
TEST_F(ThreadAPITest,
       ClassifiesCUDAStreamWaitEventAndUnifiedMemoryAttachVariants) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque

    declare i32 @cudaStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i32 @cudaStreamAttachMemAsync(%stream_t*, i8*, i64, i32)
    declare i32 @cudaHostAlloc(i8**, i64, i32)

    define i32 @main(%stream_t* %stream, %event_t* %event, i8** %host_slot,
                     i8* %ptr) {
    entry:
      %wait = call i32 @cudaStreamWaitEvent(%stream_t* %stream,
                                            %event_t* %event, i32 0)
      %attach = call i32 @cudaStreamAttachMemAsync(%stream_t* %stream,
                                                   i8* %ptr, i64 128, i32 1)
      %host = call i32 @cudaHostAlloc(i8** %host_slot, i64 256, i32 0)
      %sum = add i32 %wait, %attach
      %sum2 = add i32 %sum, %host
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  EXPECT_EQ(api->getType(module->getFunction("cudaStreamWaitEvent")),
            ThreadAPI::TD_CUDA_STREAM);
  EXPECT_EQ(api->getType(module->getFunction("cudaStreamAttachMemAsync")),
            ThreadAPI::TD_CUDA_UNIFIED_MEMORY);
  EXPECT_EQ(api->getType(module->getFunction("cudaHostAlloc")),
            ThreadAPI::TD_CUDA_UNIFIED_MEMORY);
}
TEST_F(ThreadAPITest, CUDAStreamAndUnifiedMemoryLoweringStayExplicitlyModeled) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  auto stream_info = api->getSemanticLoweringInfo(ThreadAPI::TD_CUDA_STREAM);
  auto event_info = api->getSemanticLoweringInfo(ThreadAPI::TD_CUDA_EVENT);
  auto unified_info =
      api->getSemanticLoweringInfo(ThreadAPI::TD_CUDA_UNIFIED_MEMORY);

  EXPECT_EQ(stream_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_EQ(event_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_EQ(unified_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_EQ(stream_info.reason, "modeled");
  EXPECT_EQ(event_info.reason, "modeled");
  EXPECT_EQ(unified_info.reason, "modeled");
}
TEST_F(ThreadAPITest, ResolvesKernelFunctionFromCUDAConfigLaunchPair) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)

    define void @kernel(i32* %ptr) {
    entry:
      ret void
    }

    define void @main(i32* %ptr) {
    entry:
      call void @__set_CUDAConfig(i32 2, i32 32)
      call void @kernel(i32* %ptr)
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
  const Instruction *launch = &*it++;
  const Instruction *kernel_call = &*it++;

  EXPECT_TRUE(api->isTDFork(launch));
  EXPECT_EQ(api->getForkedFun(launch), module->getFunction("kernel"));
  auto payloads = api->getForkPayloadArgs(launch);
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(payloads[0], main_func->getArg(0));
  EXPECT_TRUE(api->isCUDAKernelCallImmediatelyAfterLaunch(kernel_call));
}
TEST_F(ThreadAPITest, PreservesMangledCppAsyncNamesDuringClassification) {
  const char *source = R"(
    declare void @_ZNSt5async12launch_asyncEv(i32)

    define void @main() {
    entry:
      call void @_ZNSt5async12launch_asyncEv(i32 1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *async_func =
      module->getFunction("_ZNSt5async12launch_asyncEv");
  ASSERT_NE(async_func, nullptr);
  EXPECT_EQ(api->getType(async_func), ThreadAPI::TD_ASYNC);
}
TEST_F(ThreadAPITest, AsyncWithUnknownPolicyStillLooksForkLike) {
  const char *source = R"(
    declare void @_ZNSt5async12launch_asyncEiPFvPvES1_(i32, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @main(i32 %policy) {
    entry:
      %payload = alloca i8, align 1
      call void @_ZNSt5async12launch_asyncEiPFvPvES1_(
          i32 %policy, i8* (i8*)* @worker, i8* %payload)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call =
      main_func->getEntryBlock().getTerminator()->getPrevNode();
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(api->isTDFork(call));
  EXPECT_EQ(api->getForkedFun(call)->stripPointerCasts(),
            module->getFunction("worker"));
  auto payloads = api->getForkPayloadArgs(call);
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(payloads[0], &main_func->getEntryBlock().front());
}
TEST_F(ThreadAPITest, FunctorStyleThreadLaunchStillReturnsPayloadArgs) {
  const char *source = R"(
    declare void @_ZNSt6threadC1ER8FunctorPi(i8*, i8*, i32*)

    define void @main() {
    entry:
      %thread_obj = alloca i8, align 1
      %functor = alloca i8, align 1
      %payload = alloca i32, align 4
      call void @_ZNSt6threadC1ER8FunctorPi(i8* %thread_obj, i8* %functor,
                                            i32* %payload)
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
  const Instruction *thread_obj = &*it++;
  const Instruction *functor = &*it++;
  const Instruction *payload = &*it++;
  const Instruction *call = &*it;
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(api->isTDFork(call));
  EXPECT_EQ(api->getForkedFun(call), nullptr);
  auto payloads = api->getForkPayloadArgs(call);
  ASSERT_EQ(payloads.size(), 2u);
  EXPECT_EQ(payloads[0], functor);
  EXPECT_EQ(payloads[1], payload);
  EXPECT_NE(thread_obj, nullptr);
}
TEST_F(ThreadAPITest, RecognizesExtendedOpenMPTargetDataVariantsAndHelpers) {
  const char *source = R"(
    declare void @__tgt_target_data_update(i64, i8*)
    declare void @__tgt_target_enter_data(i64, i8*)
    declare void @__tgt_target_exit_data(i64, i8*)

    define void @main() {
    entry:
      call void @__tgt_target_data_update(i64 0, i8* null)
      call void @__tgt_target_enter_data(i64 0, i8* null)
      call void @__tgt_target_exit_data(i64 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_update")),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_enter_data")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_exit_data")),
            ThreadAPI::TD_OMP_TARGET_DATA_END);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *update = &*it++;
  const Instruction *enter = &*it++;
  const Instruction *exit = &*it++;

  EXPECT_TRUE(api->isOMPTargetOp(update));
  EXPECT_TRUE(api->isOMPTargetDataOp(update));
  EXPECT_TRUE(api->isOMPTargetDataOp(enter));
  EXPECT_TRUE(api->isOMPTargetDataOp(exit));
}
TEST_F(ThreadAPITest, RecognizesOpenMPLockLifecycleAndTryLockRoutines) {
  const char *source = R"(
    declare void @omp_init_lock(i8*)
    declare i32 @omp_test_lock(i8*)
    declare void @omp_destroy_lock(i8*)
    declare void @omp_init_nest_lock(i8*)
    declare i32 @omp_test_nest_lock(i8*)
    declare void @omp_destroy_nest_lock(i8*)

    define void @main(i8* %lock, i8* %nest) {
    entry:
      call void @omp_init_lock(i8* %lock)
      call i32 @omp_test_lock(i8* %lock)
      call void @omp_destroy_lock(i8* %lock)
      call void @omp_init_nest_lock(i8* %nest)
      call i32 @omp_test_nest_lock(i8* %nest)
      call void @omp_destroy_nest_lock(i8* %nest)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("omp_init_lock")),
            ThreadAPI::TD_MUTEX_INI);
  EXPECT_EQ(api->getType(module->getFunction("omp_test_lock")),
            ThreadAPI::TD_TRY_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("omp_destroy_lock")),
            ThreadAPI::TD_MUTEX_DESTROY);
  EXPECT_EQ(api->getType(module->getFunction("omp_init_nest_lock")),
            ThreadAPI::TD_MUTEX_INI);
  EXPECT_EQ(api->getType(module->getFunction("omp_test_nest_lock")),
            ThreadAPI::TD_TRY_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("omp_destroy_nest_lock")),
            ThreadAPI::TD_MUTEX_DESTROY);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *test_lock = &*it++;
  ++it;
  ++it;
  const Instruction *test_nest_lock = &*it;
  EXPECT_TRUE(api->isTryLock(test_lock));
  EXPECT_TRUE(api->isTryLock(test_nest_lock));
}
TEST_F(ThreadAPITest, DefaultSemaphoresAreNotLockExclusionOps) {
  const char *source = R"(
    declare i32 @sem_wait(i8*)
    declare i32 @sem_post(i8*)
    declare void @fake_counting_semaphore_acquireEv(i8*)
    declare void @fake_counting_semaphore_releaseEv(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @sem_wait(i8* %sem)
      call i32 @sem_post(i8* %sem)
      call void @fake_counting_semaphore_acquireEv(i8* %sem)
      call void @fake_counting_semaphore_releaseEv(i8* %sem)
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
  const Instruction *sem_wait_call = &*it++;
  const Instruction *sem_post_call = &*it++;
  const Instruction *cpp_acquire = &*it++;
  const Instruction *cpp_release = &*it++;

  EXPECT_TRUE(api->isSemaphoreOp(sem_wait_call));
  EXPECT_TRUE(api->isSemaphoreOp(sem_post_call));
  EXPECT_TRUE(api->isSemaphoreOp(cpp_acquire));
  EXPECT_TRUE(api->isSemaphoreOp(cpp_release));

  EXPECT_FALSE(api->isBinarySemaphoreOp(sem_wait_call));
  EXPECT_FALSE(api->isBinarySemaphoreOp(cpp_acquire));
  EXPECT_FALSE(api->isTDAcquire(sem_wait_call));
  EXPECT_FALSE(api->isTDRelease(sem_post_call));
  EXPECT_FALSE(api->isTDAcquire(cpp_acquire));
  EXPECT_FALSE(api->isTDRelease(cpp_release));
}
TEST_F(ThreadAPITest, ConfigTaggedBinarySemaphoresRemainExclusionCapable) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)
    declare i32 @binary_sem_post(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @binary_sem_wait(i8* %sem)
      call i32 @binary_sem_post(i8* %sem)
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
  const Instruction *binary_wait = &*it++;
  const Instruction *binary_post = &*it++;

  EXPECT_TRUE(api->isSemaphoreOp(binary_wait));
  EXPECT_TRUE(api->isSemaphoreOp(binary_post));
  EXPECT_TRUE(api->isBinarySemaphoreOp(binary_wait));
  EXPECT_TRUE(api->isBinarySemaphoreOp(binary_post));
  EXPECT_TRUE(api->isTDAcquire(binary_wait));
  EXPECT_TRUE(api->isTDRelease(binary_post));
  EXPECT_FALSE(api->isTDRelease(binary_wait));
  EXPECT_FALSE(api->isTDAcquire(binary_post));
}
TEST_F(ThreadAPITest, RecognizesAdditionalMPICommunicatorManagementAPIs) {
  const char *source = R"(
    declare i32 @MPI_Intercomm_create(i8*, i32, i8*, i32, i32, i8**)
    declare i32 @MPI_Intercomm_merge(i8*, i32, i8**)
    declare i32 @MPI_Comm_disconnect(i8**)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("MPI_Intercomm_create")),
            ThreadAPI::TD_MPI_COMM_CREATE);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Intercomm_merge")),
            ThreadAPI::TD_MPI_COMM_CREATE);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Comm_disconnect")),
            ThreadAPI::TD_MPI_COMM_FREE);
}
TEST_F(ThreadAPITest, RecognizesPersistentMPIRequestLifecycleHelpers) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)

    define i32 @main(i8* %comm, i8* %req1, i8* %req2) {
    entry:
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Recv_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req2)
      call i32 @MPI_Start(i8* %req1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("MPI_Send_init")),
            ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Recv_init")),
            ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Start")),
            ThreadAPI::TD_MPI_REQUEST_START);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *send_init = &*it++;
  const Instruction *recv_init = &*it++;
  const Instruction *start = &*it++;

  EXPECT_TRUE(api->isMPIRequestManagement(send_init));
  EXPECT_TRUE(api->isMPIRequestManagement(recv_init));
  EXPECT_TRUE(api->isMPIRequestManagement(start));
  EXPECT_TRUE(api->isPersistentMPIRequestInit(send_init));
  EXPECT_TRUE(api->isPersistentMPIRequestInit(recv_init));
  EXPECT_TRUE(api->isPersistentMPIRequestStart(start));
}
TEST_F(ThreadAPITest, RecognizesJthreadAndTreatsItAsForkLike) {
  const char *source = R"(
    declare void @_ZNSt7jthreadC1EPFvPvES0_(i8*, i8* (i8*)*, i8*)
    declare void @_ZNSt7jthread4joinEv(i8*)
    declare void @_ZNSt7jthread6detachEv(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @main() {
    entry:
      %thr = alloca i8
      call void @_ZNSt7jthreadC1EPFvPvES0_(i8* %thr, i8* (i8*)* @worker, i8* null)
      call void @_ZNSt7jthread4joinEv(i8* %thr)
      call void @_ZNSt7jthread6detachEv(i8* %thr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *fork = nullptr;
  const Instruction *join = nullptr;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      if (!fork) {
        fork = &inst;
      } else if (!join) {
        join = &inst;
        break;
      }
    }
  }
  ASSERT_NE(fork, nullptr);
  ASSERT_NE(join, nullptr);

  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthreadC1EPFvPvES0_")),
            ThreadAPI::TD_JTHREAD_FORK);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthread4joinEv")),
            ThreadAPI::TD_JTHREAD_JOIN);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthread6detachEv")),
            ThreadAPI::TD_DETACH);
  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_TRUE(api->isTDJoin(join));
}
TEST_F(ThreadAPITest, StdThreadMoveConstructorIsNotFork) {
  const char *source = R"(
    declare void @_ZNSt6threadC1EOS_(i8*, i8*)

    define void @main() {
    entry:
      %dst = alloca i8
      %src = alloca i8
      call void @_ZNSt6threadC1EOS_(i8* %dst, i8* %src)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *move_ctor = module->getFunction("_ZNSt6threadC1EOS_");
  ASSERT_NE(move_ctor, nullptr);
  EXPECT_EQ(api->getType(move_ctor), ThreadAPI::TD_DUMMY);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDFork(call));
}
TEST_F(ThreadAPITest, RecognizesLibcxxJoinDetachManglings) {
  const char *source = R"(
    declare void @_ZNSt3__16thread4joinEv(i8*)
    declare void @_ZNSt3__16thread6detachEv(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt3__16thread4joinEv")),
            ThreadAPI::TD_JOIN);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt3__16thread6detachEv")),
            ThreadAPI::TD_DETACH);
}
TEST_F(ThreadAPITest, UnwrapsConditionVariableAnyWaitMutexFromUniqueLock) {
  const char *source = R"(
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @_ZNSt22condition_variable_any4waitERSt11unique_lockISt5mutexE(i8*, i8*)

    @cv = global i8 0
    @lock = global i8 0

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @_ZNSt22condition_variable_any4waitERSt11unique_lockISt5mutexE(i8* @cv, i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Function *wait_func = module->getFunction(
      "_ZNSt22condition_variable_any4waitERSt11unique_lockISt5mutexE");
  ASSERT_NE(wait_func, nullptr);
  EXPECT_EQ(api->getType(wait_func), ThreadAPI::TD_COND_WAIT);
  auto it = main_func->getEntryBlock().begin();
  ++it;
  ++it;
  const Instruction *wait = &*it;
  ASSERT_TRUE(api->isTDCondWait(wait));
  EXPECT_EQ(api->getCondVal(wait), module->getNamedGlobal("cv"));
  EXPECT_EQ(api->getCondMutex(wait), module->getNamedGlobal("lock"));
}
TEST_F(ThreadAPITest, StdJthreadMoveConstructorIsNotFork) {
  const char *source = R"(
    declare void @_ZNSt7jthreadC1EOS_(i8*, i8*)

    define void @main() {
    entry:
      %dst = alloca i8
      %src = alloca i8
      call void @_ZNSt7jthreadC1EOS_(i8* %dst, i8* %src)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *move_ctor = module->getFunction("_ZNSt7jthreadC1EOS_");
  ASSERT_NE(move_ctor, nullptr);
  EXPECT_EQ(api->getType(move_ctor), ThreadAPI::TD_DUMMY);
}
TEST_F(ThreadAPITest, RecognizesGNUOpenMPParallelForkAndBarrierVariants) {
  const char *source = R"(
    declare void @GOMP_parallel(void ()*, i8*, i32, i32)
    declare void @GOMP_parallel_end()

    define void @worker() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @GOMP_parallel(void ()* @worker, i8* null, i32 1, i32 0)
      call void @GOMP_parallel_end()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_parallel")),
            ThreadAPI::TD_FORK);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_parallel_end")),
            ThreadAPI::TD_BAR_WAIT);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *fork = &*it++;
  const Instruction *end = &*it++;

  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_EQ(api->getForkedThread(fork), nullptr);
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("worker"));
  EXPECT_TRUE(api->isTDBarWait(end));
}
TEST_F(ThreadAPITest, RecognizesGNUOpenMPTaskloopPrefixVariants) {
  const char *source = R"(
    declare void @GOMP_taskloop(i8*)
    declare void @GOMP_taskloop_ull(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskloop")),
            ThreadAPI::TD_OMP_TASKLOOP);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskloop_ull")),
            ThreadAPI::TD_OMP_TASKLOOP);
}
TEST_F(ThreadAPITest, MapsOpenMPTaskwaitWithDepsVariants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_taskwait(i8*, i32)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait_deps_51(i8*, i32, i32, i8*, i32, i8*, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_taskwait")),
            ThreadAPI::TD_OMP_TASKWAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_wait_deps")),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_taskwait_deps_51")),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
}
TEST_F(ThreadAPITest, ClassifiesSharedTimedMutexReleases) {
  const char *source = R"(
    declare void @_ZNSt18shared_timed_mutex13unlock_sharedEv(i8*)
    declare void @_ZNSt18shared_timed_mutex6unlockEv(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZNSt18shared_timed_mutex13unlock_sharedEv")),
            ThreadAPI::TD_SHARED_UNLOCK);
  EXPECT_EQ(
      api->getType(module->getFunction("_ZNSt18shared_timed_mutex6unlockEv")),
      ThreadAPI::TD_SHARED_UNLOCK);
}
TEST_F(ThreadAPITest, ExtractsOutlinedOpenMPForkTarget) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*)

    define internal void @.omp_outlined.(i32* %gtid, i32* %btid, ...) {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @__kmpc_fork_call(i8* null, i32 0,
                                  void (i32*, i32*, ...)* @.omp_outlined.)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *fork = &main_func->getEntryBlock().front();
  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction(".omp_outlined."));
}

TEST_F(ThreadAPITest, HandlesIndirectCallInConditionalLockAndWrapperDtorQueries) {
  const char *source = R"(
    define i32 @caller(i32 (i8*)* %fn, i8* %arg) {
    entry:
      %res = call i32 %fn(i8* %arg)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const auto *cb = dyn_cast<CallBase>(&caller->getEntryBlock().front());
  ASSERT_NE(cb, nullptr);
  EXPECT_FALSE(api->isConditionalLockAcquire(cb));
  EXPECT_FALSE(api->cppWrapperDestructorReleases(cb));
}
