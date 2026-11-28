#include "MHPAnalysisTestSupport.h"

#include "Concurrency/MHP/StaticVectorClockMHP.h"

TEST_F(MHPAnalysisTest, DetachResolutionIsIndependentOfBlockStorageOrder) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_detach(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %create

    detach:
      call i32 @pthread_detach(i8* %tid)
      br label %exit

    create:
      %fork = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      br label %detach

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *fork =
      findInstructionByName(*module->getFunction("main"), "fork");
  ASSERT_NE(fork, nullptr);
  const SyncNode *fork_node = mhp.getThreadFlowGraph().getNode(fork, 0, 0);
  ASSERT_NE(fork_node, nullptr);
  ASSERT_NE(fork_node->getForkedThread(), 0u);
  EXPECT_TRUE(mhp.isDetachedThread(fork_node->getForkedThread()));
}

TEST_F(MHPAnalysisTest, IncludedOpenMPTaskRunsInlineWithParentContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)

    define i8* @task_body(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @task_body, i8* (i8*)** %task, align 8
      %task_raw = bitcast i8* (i8*)** %task to i8*
      call i32 @__kmpc_omp_task_begin_if0(i8* null, i32 0, i8* %task_raw)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *task_store =
      &module->getFunction("task_body")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(task_store, after));
  EXPECT_TRUE(mhp.mustBeSequential(task_store, after));
}
TEST_F(MHPAnalysisTest, JoinTargetThroughPhiResolves) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      br i1 %cond, label %left, label %right

    left:
      br label %join

    right:
      br label %join

    join:
      %phi_tid = phi i8* [ %tid, %left ], [ %tid, %right ]
      %joined = call i32 @pthread_join(i8* %phi_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mustBeSequential(worker_inst, post));
}
TEST_F(MHPAnalysisTest, JoinTargetThroughLoadRemainsNonMHP) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      %join_tid = bitcast i8* %tid to i8*
      call i32 @pthread_join(i8* %join_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mustBeSequential(worker_inst, post));
}
TEST_F(MHPAnalysisTest, MultiExitWorkerStillOrdersPostJoinContinuation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %cond = icmp eq i8* %arg, null
      br i1 %cond, label %left, label %right

    left:
      %left_work = add i32 1, 2
      ret i8* null

    right:
      %right_work = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* undef)
      call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(worker_func, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *left_work =
      findInstructionByName(*worker_func, "left_work");
  const Instruction *right_work =
      findInstructionByName(*worker_func, "right_work");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(left_work, nullptr);
  ASSERT_NE(right_work, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(left_work, post));
  EXPECT_FALSE(mhp.mayHappenInParallel(right_work, post));
  EXPECT_TRUE(mhp.mustBeSequential(left_work, post));
  EXPECT_TRUE(mhp.mustBeSequential(right_work, post));
}
TEST_F(MHPAnalysisTest, ConditionalJoinDoesNotOrderPostMergeContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %worker_value = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      br i1 %cond, label %joined, label %merge

    joined:
      call i32 @pthread_join(i8* %tid, i8* null)
      br label %merge

    merge:
      %post = load i32, i32* @shared, align 4
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *worker_inst =
      findInstructionByName(*module->getFunction("worker"), "worker_value");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_inst, post));
  EXPECT_FALSE(hb.mustPrecede(worker_inst, post));
}

TEST_F(MHPAnalysisTest, JoinResolutionIsIndependentOfBasicBlockStorageOrder) {
  const char *source = R"(
    @shared = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    define i8* @worker(i8* %arg) {
    entry:
      store i32 1, i32* @shared
      ret i8* null
    }
    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %create
    join:
      call i32 @pthread_join(i8* %tid, i8* null)
      %after = load i32, i32* @shared
      ret i32 %after
    create:
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      br label %join
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_FALSE(mhp.mayHappenInParallel(worker_store, after));
}

TEST_F(MHPAnalysisTest, RepeatedHelperCallAroundJoinRemainsMHP) {
  const char *source = R"(
    @shared = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define void @helper() {
    entry:
      %h = load i32, i32* @shared
      ret void
    }

    define i8* @worker(i8* %arg) {
    entry:
      %w = load i32, i32* @shared
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      call void @helper()
      call i32 @pthread_join(i8* %tid, i8* null)
      call void @helper()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Instruction *helper =
      findInstructionByName(*module->getFunction("helper"), "h");
  const Instruction *worker =
      findInstructionByName(*module->getFunction("worker"), "w");
  ASSERT_NE(helper, nullptr);
  ASSERT_NE(worker, nullptr);

  MHPAnalysis on_demand(*module);
  on_demand.analyze();
  EXPECT_TRUE(on_demand.mayHappenInParallel(helper, worker));
  EXPECT_EQ(on_demand.getParallelInstructions(helper).count(worker), 1u);

  MHPAnalysis precomputed(*module);
  precomputed.enableMHPPrecomputation(true);
  precomputed.analyze();
  EXPECT_TRUE(precomputed.mayHappenInParallel(helper, worker));
  EXPECT_EQ(precomputed.getParallelInstructions(helper).count(worker), 1u);
}

TEST_F(MHPAnalysisTest, UnresolvedIndirectForkContinuationIsNotPrefork) {
  const char *source = R"(
    @worker_ptr = global i8* (i8*)* @worker
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      %fn = load i8* (i8*)*, i8* (i8*)** @worker_ptr
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* %fn, i8* null)
      %post = add i32 3, 4
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *worker =
      findInstructionByName(*module->getFunction("worker"), "w");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(worker, nullptr);
  ASSERT_NE(post, nullptr);
  EXPECT_TRUE(mhp.mayHappenInParallel(worker, post));
}

TEST_F(MHPAnalysisTest, ConflictingBarrierCountsDoNotCreateMustOrdering) {
  const char *source = R"(
    @bar = global i8 0
    @shared = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_init(i8*, i8*, i32)
    declare i32 @pthread_barrier_wait(i8*)

    define i8* @first(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
      %after = load i32, i32* @shared
      ret i8* null
    }

    define i8* @second(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
      ret i8* null
    }

    define i8* @third(i8* %arg) {
    entry:
      %before = load i32, i32* @shared
      call i32 @pthread_barrier_wait(i8* @bar)
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %t1 = alloca i8
      %t2 = alloca i8
      %t3 = alloca i8
      br i1 %cond, label %two, label %three
    two:
      call i32 @pthread_barrier_init(i8* @bar, i8* null, i32 2)
      br label %start
    three:
      call i32 @pthread_barrier_init(i8* @bar, i8* null, i32 3)
      br label %start
    start:
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @first, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @second, i8* null)
      call i32 @pthread_create(i8* %t3, i8* null, i8* (i8*)* @third, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  const Instruction *after =
      findInstructionByName(*module->getFunction("first"), "after");
  const Instruction *before =
      findInstructionByName(*module->getFunction("third"), "before");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(before, nullptr);
  EXPECT_TRUE(mhp.mayHappenInParallel(after, before));
}
TEST_F(MHPAnalysisTest, AmbiguousJoinDoesNotCreateDefiniteHB) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      %join_tid = select i1 %cond, i8* %tid1, i8* %tid2
      call i32 @pthread_join(i8* %join_tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker1 = module->getFunction("worker1");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker1, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *post = findInstructionByName(*main_func, "post");
  const Instruction *w1 = findInstructionByName(*worker1, "w1");
  const Instruction *w2 = findInstructionByName(*worker2, "w2");
  ASSERT_NE(post, nullptr);
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  EXPECT_FALSE(hb.mustPrecede(w1, post));
  EXPECT_FALSE(hb.mustPrecede(w2, post));
}
TEST_F(MHPAnalysisTest, ForeignJoinHandleDoesNotOrderLocalWorker) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main(i8* %foreign_tid) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %foreign_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_inst, post));
  EXPECT_FALSE(hb.mustPrecede(worker_inst, post));
}
TEST_F(MHPAnalysisTest, ReusedHandleJoinDoesNotOrderLaterCreatePhase) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker2, i8* null)
      %post = add i32 5, 6
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *w1 =
      findInstructionByName(*module->getFunction("worker1"), "w1");
  const Instruction *w2 =
      findInstructionByName(*module->getFunction("worker2"), "w2");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(hb.mustPrecede(w1, post));
  EXPECT_TRUE(mhp.mayHappenInParallel(w2, post));
  EXPECT_FALSE(hb.mustPrecede(w2, post));
}
TEST_F(MHPAnalysisTest, RegionPartitionDoesNotOverlapAcrossBranchMerge) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      %lock_then = call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    else:
      %plain = add i32 1, 2
      br label %merge

    merge:
      %merge_val = phi i32 [ 1, %then ], [ %plain, %else ]
      ret i32 %merge_val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  size_t inst_count = 0;
  for (const BasicBlock &bb : *main_func) {
    for (const Instruction &inst : bb) {
      (void)inst;
      ++inst_count;
    }
  }

  size_t covered = 0;
  for (const auto &region : mhp.getThreadRegionAnalysis().getAllRegions()) {
    if (region->thread_id != 0) {
      continue;
    }
    covered += region->instructions.size();
  }

  EXPECT_EQ(covered, inst_count);
}
TEST_F(MHPAnalysisTest, HelperCalledBeforeAndAfterForkIsNotGloballyPrefork) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i32 @helper() {
    entry:
      %h = add i32 1, 2
      ret i32 %h
    }

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %pre = call i32 @helper()
      %tid = alloca i8
      %fork = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %post = call i32 @helper()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *helper_func = module->getFunction("helper");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(helper_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *helper_inst = findInstructionByName(*helper_func, "h");
  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  ASSERT_NE(helper_inst, nullptr);
  ASSERT_NE(worker_inst, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_EQ(mhp.getThreadFlowGraph().getNodes(helper_inst).size(), 2u);
  EXPECT_TRUE(mhp.mayHappenInParallel(helper_inst, worker_inst));
}
TEST_F(MHPAnalysisTest,
       HelperHiddenFirstForkDoesNotMakeCallerContinuationPrefork) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_helper(i8* %tid)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_store, after));
}
TEST_F(MHPAnalysisTest, ContextSpecificHelperForksKeepDistinctCreateEdges) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %work = add i32 1, 2
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      %fork = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %first = alloca i8
      %second = alloca i8
      call void @spawn_helper(i8* %first)
      call void @spawn_helper(i8* %second)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *fork =
      findInstructionByName(*module->getFunction("spawn_helper"), "fork");
  ASSERT_NE(fork, nullptr);
  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  std::vector<SyncNode *> fork_nodes = tfg.getNodes(fork, 0);
  ASSERT_EQ(fork_nodes.size(), 2u);

  std::unordered_set<ThreadID> child_threads;
  for (SyncNode *fork_node : fork_nodes) {
    EXPECT_NE(fork_node->getCallContextID(), 0u);
    EXPECT_NE(fork_node->getForkedThread(), 0u);
    child_threads.insert(fork_node->getForkedThread());
    size_t create_edges = 0;
    for (SyncNode *succ : fork_node->getSuccessors()) {
      if (tfg.hasEdgeKind(fork_node, succ, EdgeKind::Create)) {
        ++create_edges;
        EXPECT_EQ(succ->getThreadID(), fork_node->getForkedThread());
      }
    }
    EXPECT_EQ(create_edges, 1u);
  }
  EXPECT_EQ(child_threads.size(), 2u);
}
TEST_F(MHPAnalysisTest, HelperHiddenForkStillHonorsJoinOrderingAtCaller) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 9, i32* @shared, align 4
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_helper(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(worker_store, after));
  EXPECT_TRUE(hb.mustPrecede(worker_store, after));
}
TEST_F(MHPAnalysisTest,
       RepeatedHelperForksOnSameHandleDoNotInventJoinOrdering) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 11, i32* @shared, align 4
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_helper(i8* %tid)
      call void @spawn_helper(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_store, after));
  EXPECT_FALSE(hb.mustPrecede(worker_store, after));
}
TEST_F(MHPAnalysisTest, LoopCreateJoinDoesNotAutoSelfParallelizeWorkerBody) {
  const char *source = R"(
    @x = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %a = add i32 1, 2
      %b = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);
  const Instruction *inst_a = findInstructionByName(*worker_func, "a");
  const Instruction *inst_b = findInstructionByName(*worker_func, "b");
  ASSERT_NE(inst_a, nullptr);
  ASSERT_NE(inst_b, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(inst_a, inst_a));
  EXPECT_FALSE(mhp.mayHappenInParallel(inst_a, inst_b));
}
TEST_F(MHPAnalysisTest, OpenMPTargetDataBoundaryOrdersTaskContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end(i8*, i32)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 11, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task1_raw)
      call i32 @__tgt_target_data_end(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task2_raw)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}
TEST_F(MHPAnalysisTest, UnresolvedIndirectCallEnablesConservativeForkFallback) {
  const char *source = R"(
    @hook = external global void ()*

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define void @fork_helper() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %fn = load void ()*, void ()** @hook
      call void %fn()
      %post = add i32 3, 4
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker = module->getFunction("worker");
  ASSERT_NE(worker, nullptr);
  const Instruction *worker_inst = findInstructionByName(*worker, "w");
  ASSERT_NE(worker_inst, nullptr);

  EXPECT_EQ(mhp.getThreadID(worker_inst), std::numeric_limits<ThreadID>::max());
}

TEST_F(MHPAnalysisTest, RecursiveThreadForkTerminatesAndMarksMultiInstance) {
  const char *source = R"(
    @shared = global i32 0, align 4
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @recursive_worker(i8* %arg) {
    entry:
      %w = load i32, i32* @shared, align 4
      %cmp = icmp eq i32 %w, 0
      br i1 %cmp, label %spawn, label %done

    spawn:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @recursive_worker, i8* null)
      br label %done

    done:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @recursive_worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Instruction *wInst =
      findInstructionByName(*module->getFunction("recursive_worker"), "w");
  ASSERT_NE(wInst, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  EXPECT_FALSE(mhp.isSingleThreaded());
  EXPECT_TRUE(mhp.mayHappenInParallel(wInst, wInst));

  StaticVectorClockMHP svc(*module);
  svc.analyze();
  EXPECT_FALSE(svc.isSingleThreaded());
  EXPECT_TRUE(svc.mayHappenInParallel(wInst, wInst));
}

TEST_F(MHPAnalysisTest, SingleThreadedIndirectCallModuleStaysSingleThreaded) {
  const char *source = R"(
    @hook = external global void ()*
    @shared = global i32 0, align 4

    define void @target_a() {
    entry:
      store i32 1, i32* @shared, align 4
      ret void
    }

    define void @target_b() {
    entry:
      store i32 2, i32* @shared, align 4
      ret void
    }

    define i32 @main() {
    entry:
      %fn = load void ()*, void ()** @hook
      call void %fn()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Instruction *storeA =
      &module->getFunction("target_a")->getEntryBlock().front();
  const Instruction *storeB =
      &module->getFunction("target_b")->getEntryBlock().front();

  MHPAnalysis mhp(*module);
  mhp.analyze();
  EXPECT_TRUE(mhp.isSingleThreaded());
  EXPECT_FALSE(mhp.mayHappenInParallel(storeA, storeB));

  StaticVectorClockMHP svc(*module);
  svc.analyze();
  EXPECT_TRUE(svc.isSingleThreaded());
  EXPECT_FALSE(svc.mayHappenInParallel(storeA, storeB));
}

TEST_F(MHPAnalysisTest, IndirectForkResolvesClosedSelectTargetsWithoutConservatism) {
  const char *source = R"(
    @shared = global i32 0, align 4
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @w1(i8* %arg) {
    entry:
      %a = add i32 1, 2
      ret i8* null
    }

    define i8* @w2(i8* %arg) {
    entry:
      %b = add i32 3, 4
      ret i8* null
    }

    define i8* @unrelated(i8* %arg) {
    entry:
      %u = add i32 5, 6
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      %pre = call i8* @unrelated(i8* null)
      %fn = select i1 %cond, i8* (i8*)* @w1, i8* (i8*)* @w2
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn, i8* null)
      %post = add i32 7, 8
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *a = findInstructionByName(*module->getFunction("w1"), "a");
  const Instruction *b = findInstructionByName(*module->getFunction("w2"), "b");
  const Instruction *u =
      findInstructionByName(*module->getFunction("unrelated"), "u");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(u, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_FALSE(mhp.isSingleThreaded());
  EXPECT_EQ(mhp.getThreadID(u), 0u);
  EXPECT_NE(mhp.getThreadID(a), 0u);
  EXPECT_NE(mhp.getThreadID(a), std::numeric_limits<ThreadID>::max());
  EXPECT_NE(mhp.getThreadID(b), 0u);
  EXPECT_NE(mhp.getThreadID(b), std::numeric_limits<ThreadID>::max());
  EXPECT_TRUE(mhp.mayHappenInParallel(a, post));
  EXPECT_TRUE(mhp.mayHappenInParallel(b, post));
  // Pre-fork call to @unrelated stays strictly on thread 0 and must NOT be
  // spuriously marked MHP with post-fork or child threads via open conservatism.
  EXPECT_FALSE(mhp.mayHappenInParallel(u, post));
  EXPECT_FALSE(mhp.mayHappenInParallel(u, a));
  EXPECT_FALSE(mhp.mayHappenInParallel(u, b));
}
