#include "ThreadAPITestSupport.h"
#include "Concurrency/OpenMP/OpenMPModel.h"

TEST_F(ThreadAPITest, MapsOpenMPTaskRuntimeVariants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)
    declare i32 @__kmpc_omp_task_with_deps_51(i8*, i32, i8*, i32, i8*, i32, i8*, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_begin_if0")),
            ThreadAPI::TD_OMP_TASK);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_with_deps_51")),
            ThreadAPI::TD_OMP_TASK_WITH_DEPS);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_complete_if0")),
            ThreadAPI::TD_OMP_TASK_COMPLETE);
  EXPECT_EQ(api->getRuntimeLibrary(
                module->getFunction("__kmpc_omp_task_with_deps_51")),
            ThreadAPI::RuntimeLibrary::OpenMP);
  EXPECT_EQ(
      api->getSemanticTag(module->getFunction("__kmpc_omp_task_with_deps_51")),
      "task-with-deps");
}

TEST_F(ThreadAPITest, DistinguishesOpenMPDoacrossRuntimeVariants) {
  const char *source = R"(
    declare void @__kmpc_doacross_wait(i8*, i32, i64*)
    declare void @__kmpc_doacross_submit(i8*, i32, i64*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_wait")),
            ThreadAPI::TD_OMP_DOACROSS_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_submit")),
            ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
}

TEST_F(ThreadAPITest,
       LongestPrefixRuleWinsForSpecializedOpenMPRuntimeFamilies) {
  const char *source = R"(
    declare void @__kmpc_teams_host(i8*, i32)
    declare void @__kmpc_teams_distribute_nowait_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_static_init_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_dynamic_init_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_guidance_init_4(i8*, i32, i32*)
    declare void @__kmpc_loop_static_4(i8*, i32, i32*)
    declare void @__kmpc_loop_dynamic_4(i8*, i32, i32*)
    declare void @__kmpc_loop_guidance_4(i8*, i32, i32*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  EXPECT_EQ(api->getType(module->getFunction("__kmpc_teams_host")),
            ThreadAPI::TD_OMP_TEAMS_HOST);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_teams_distribute_nowait_4")),
      ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_distribute_static_init_4")),
      ThreadAPI::TD_OMP_DISTRIBUTE_STATIC);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_distribute_dynamic_init_4")),
      ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC);
  EXPECT_EQ(
      api->getType(module->getFunction("__kmpc_distribute_guidance_init_4")),
      ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_static_4")),
            ThreadAPI::TD_OMP_LOOP_STATIC_INIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_dynamic_4")),
            ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_guidance_4")),
            ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT);
}

TEST_F(ThreadAPITest, MapsOpenMPRegionRuntimeVariants) {
  const char *source = R"(
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)
    declare i32 @__kmpc_master(i8*, i32)
    declare void @__kmpc_end_master(i8*, i32)
    declare void @__kmpc_ordered(i8*, i32)
    declare void @__kmpc_end_ordered(i8*, i32)
    declare i32 @__kmpc_reduce(i8*, i32, i32, i64, i8*, void (i8*, i8*)*, [8 x i32]*)
    declare void @__kmpc_for_static_fini(i8*, i32)
    declare void @__kmpc_dispatch_fini_4(i8*, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_single")),
            ThreadAPI::TD_OMP_SINGLE_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_single")),
            ThreadAPI::TD_OMP_SINGLE_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_master")),
            ThreadAPI::TD_OMP_MASTER_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_master")),
            ThreadAPI::TD_OMP_MASTER_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_ordered")),
            ThreadAPI::TD_OMP_ORDERED_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_ordered")),
            ThreadAPI::TD_OMP_ORDERED_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_reduce")),
            ThreadAPI::TD_OMP_REDUCE_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_for_static_fini")),
            ThreadAPI::TD_OMP_FOR_STATIC_FINI);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_dispatch_fini_4")),
            ThreadAPI::TD_OMP_FOR_DISPATCH_FINI);
}

TEST_F(ThreadAPITest, OpenMPBarrierUsesSiteIdentityInsteadOfMetadataOperand) {
  const char *source = R"(
    declare void @__kmpc_barrier(i8*, i32)

    define void @main() {
    entry:
      call void @__kmpc_barrier(i8* null, i32 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *barrier = &main_func->getEntryBlock().front();

  EXPECT_EQ(api->getType(module->getFunction("__kmpc_barrier")),
            ThreadAPI::TD_BAR_WAIT);
  EXPECT_EQ(api->getBarrierVal(barrier), barrier);
}

TEST_F(ThreadAPITest, LongestPrefixRuleWinsForOpenMPDoacross) {
  const char *source = R"(
    declare void @__kmpc_doacross_wait_4(i8*, i32, i64*)
    declare void @__kmpc_doacross_submit_4(i8*, i32, i64*)
    declare void @__kmpc_doacross_init_4(i8*, i32, i64*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_wait_4")),
            ThreadAPI::TD_OMP_DOACROSS_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_submit_4")),
            ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_init_4")),
            ThreadAPI::TD_OMP_DOACROSS_INIT);
}

TEST_F(ThreadAPITest, RecognizesGOMPTaskAndBarrierRuntimeAliases) {
  const char *source = R"(
    declare void @GOMP_barrier()
    declare void @GOMP_taskwait()
    declare void @GOMP_taskgroup_start()
    declare void @GOMP_taskgroup_end()
    declare void @GOMP_task(void ()*, i8*, i8*, i64, i64, i1, i32, i8*, i32)

    define void @worker() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @GOMP_task(void ()* @worker, i8* null, i8* null, i64 0, i64 0,
                           i1 true, i32 0, i8* null, i32 0)
      call void @GOMP_taskwait()
      call void @GOMP_taskgroup_start()
      call void @GOMP_taskgroup_end()
      call void @GOMP_barrier()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_task")),
            ThreadAPI::TD_OMP_TASK);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskwait")),
            ThreadAPI::TD_OMP_TASKWAIT);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskgroup_start")),
            ThreadAPI::TD_OMP_TASKGROUP_START);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskgroup_end")),
            ThreadAPI::TD_OMP_TASKGROUP_END);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_barrier")),
            ThreadAPI::TD_BAR_WAIT);
}

TEST_F(ThreadAPITest, RecognizesCriticalWithHintAsCriticalEntry) {
  const char *source = R"(
    declare void @__kmpc_critical_with_hint(i8*, i32, i8*, i64)

    define void @main(i8* %lock) {
    entry:
      call void @__kmpc_critical_with_hint(i8* null, i32 0, i8* %lock, i64 1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_critical_with_hint")),
            ThreadAPI::TD_ACQUIRE);
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  EXPECT_EQ(api->getAnalysisLockIdentity(call),
            cast<CallBase>(call)->getArgOperand(2));
}

TEST_F(ThreadAPITest, OpenMPForkCarriesAllVariadicCaptures) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*, ...)
    define internal void @outlined(i32* %gtid, i32* %btid, ...) { ret void }
    define void @main(i8* %a, i8* %b) {
      call void (i8*, i32, void (i32*, i32*, ...)*, ...)
        @__kmpc_fork_call(i8* null, i32 2,
                          void (i32*, i32*, ...)* @outlined, i8* %a, i8* %b)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *fork = &module->getFunction("main")->front().front();
  EXPECT_EQ(api->getForkedThread(fork), nullptr);
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("outlined"));
  auto payload = api->getForkPayloadArgs(fork);
  ASSERT_EQ(payload.size(), 2u);
  EXPECT_EQ(payload[0], module->getFunction("main")->getArg(0));
  EXPECT_EQ(payload[1], module->getFunction("main")->getArg(1));
}

TEST_F(ThreadAPITest, OpenMPInitPredicatesAreOperationSpecific) {
  EXPECT_TRUE(OpenMPModel::isTargetInit("__tgt_target"));
  EXPECT_FALSE(OpenMPModel::isTargetInit("__tgt_target_data_begin"));
  EXPECT_TRUE(OpenMPModel::isInteropInit("__kmpc_interop_init"));
  EXPECT_FALSE(OpenMPModel::isInteropInit("__kmpc_interop_fini"));
  EXPECT_TRUE(OpenMPModel::isDoacrossInit("__kmpc_doacross_init"));
  EXPECT_FALSE(OpenMPModel::isDoacrossInit("__kmpc_doacross_wait"));
  EXPECT_FALSE(OpenMPModel::isDoacrossInit("__kmpc_doacross_post"));
}

TEST_F(ThreadAPITest, OpenMPTargetDataPrefixesAndAliasCallsAreRecognized) {
  const char *source = R"(
    declare void @__tgt_target_data_begin_mapper(i8*)
    declare void @__tgt_target_data_end_nowait_mapper(i8*)
    declare void @__tgt_target_data_update_mapper(i8*)
    declare void @__tgt_target_mapper(i8*)
    declare void @__kmpc_fork_call(i8*, i32, i8*, ...)
    @fork_alias = alias void (i8*, i32, i8*, ...),
        void (i8*, i32, i8*, ...)* @__kmpc_fork_call
    define void @main() {
      call void (i8*, i32, i8*, ...) @fork_alias(i8* null, i32 0,
                                                  i8* null)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_begin_mapper")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(
      api->getType(module->getFunction("__tgt_target_data_end_nowait_mapper")),
      ThreadAPI::TD_OMP_TARGET_DATA_END);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_update_mapper")),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_mapper")),
            ThreadAPI::TD_OMP_TARGET);
  const auto *call = dyn_cast<CallBase>(&module->getFunction("main")->front().front());
  ASSERT_NE(call, nullptr);
  EXPECT_TRUE(OpenMPModel::isFork(call));
  EXPECT_TRUE(api->isForkLike(call));
}

TEST_F(ThreadAPITest, OpenMPBookkeepingIsNotBarrierButCancelBarrierIs) {
  const char *source = R"(
    declare void @__kmpc_end_single(i8*, i32)
    declare void @__kmpc_for_static_fini(i8*, i32)
    declare void @__kmpc_end_sections(i8*, i32)
    declare void @__kmpc_dispatch_fini_4(i8*, i32)
    declare i32 @__kmpc_cancel_barrier(i8*, i32)
    define void @main() {
      call void @__kmpc_end_single(i8* null, i32 0)
      call void @__kmpc_for_static_fini(i8* null, i32 0)
      call void @__kmpc_end_sections(i8* null, i32 0)
      call void @__kmpc_dispatch_fini_4(i8* null, i32 0)
      call i32 @__kmpc_cancel_barrier(i8* null, i32 0)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  for (unsigned i = 0; i < 4; ++i)
    EXPECT_FALSE(api->isTDBarWait(&*it++));
  const Instruction *cancel_barrier = &*it;
  EXPECT_EQ(api->getType(api->getLLVMCallSite(cancel_barrier)),
            ThreadAPI::TD_BAR_WAIT);
  EXPECT_TRUE(api->isTDBarWait(cancel_barrier));
  EXPECT_EQ(api->getBarrierVal(cancel_barrier), cancel_barrier);
}

TEST_F(ThreadAPITest, RecognizesParameterizedBarrierArriveMangling) {
  const char *source = R"(
    declare void @_ZNSt7barrierISt18__empty_completionE6arriveEl(i8*, i64)
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZNSt7barrierISt18__empty_completionE6arriveEl")),
            ThreadAPI::TD_BARRIER_ARRIVE);
}

