#include "ConcurrencyCheckerTestSupport.h"

#include "Checker/Framework/BugReportMgr.h"
#include "Utils/Formats/cJSON.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

namespace {

size_t countReportsForBugTypeName(const std::string &typeName) {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  int id = mgr.find_bug_type(typeName);
  if (id < 0)
    return 0;
  const auto *reports = mgr.get_reports_for_type(id);
  return reports ? reports->size() : 0;
}

} // namespace

TEST_F(ConcurrencyCheckerTest,
       SparseFlowSensitiveRefinementEliminatesStrongUpdateFalsePositiveRace) {
  const char *source = R"(
    @slot = global i32* null
    @x = global i32 0, align 4
    @y = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8**)

    define i8* @worker1(i8* %arg) {
    entry:
      %p = load i32*, i32** @slot, align 8
      store i32 1, i32* %p, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      store i32 2, i32* @x, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      store i32* @x, i32** @slot, align 8
      store i32* @y, i32** @slot, align 8
      %t1 = alloca i8, align 1
      %t2 = alloca i8, align 1
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @worker2, i8* null)
      call i32 @pthread_join(i8* %t1, i8** null)
      call i32 @pthread_join(i8* %t2, i8** null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  // Baseline (flow-insensitive): @slot points to {@x, @y}, so worker1's store
  // via *@slot appears to race with worker2's store to @x.
  concurrency::ConcurrencyChecker baseline(*module);
  baseline.enableDeadlockCheck(false);
  baseline.enableAtomicityCheck(false);
  baseline.enableCondVarCheck(false);
  baseline.enableLockMismatchCheck(false);
  baseline.enableOpenMPCheck(false);
  baseline.enableMPICheck(false);
  baseline.enableCUDACheck(false);
  baseline.enableLinuxKernelCheck(false);
  baseline.enableStarvationCheck(false);
  baseline.runAnalyses();
  baseline.checkDataRaces();
  EXPECT_GT(baseline.getStatistics().dataRacesFound, 0u);

  // With sparse flow-sensitive refinement: strong update overwrites @slot to
  // @y before fork, so *@slot and @x are provably disjoint.
  concurrency::ConcurrencyChecker refined(*module);
  refined.enableSparseFlowSensitiveRefinement(true);
  refined.enableDeadlockCheck(false);
  refined.enableAtomicityCheck(false);
  refined.enableCondVarCheck(false);
  refined.enableLockMismatchCheck(false);
  refined.enableOpenMPCheck(false);
  refined.enableMPICheck(false);
  refined.enableCUDACheck(false);
  refined.enableLinuxKernelCheck(false);
  refined.enableStarvationCheck(false);
  refined.runAnalyses();
  refined.checkDataRaces();
  EXPECT_EQ(refined.getStatistics().dataRacesFound, 0u);

  // Also verify sparse flow-sensitive + MSLI still detects a true data race
  // (no unsound suppression).
  const char *trueRaceSource = R"(
    @slot = global i32* null
    @x = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8**)

    define i8* @worker1(i8* %arg) {
    entry:
      %p = load i32*, i32** @slot, align 8
      store i32 1, i32* %p, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      store i32 2, i32* @x, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      store i32* @x, i32** @slot, align 8
      %t1 = alloca i8, align 1
      %t2 = alloca i8, align 1
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @worker2, i8* null)
      call i32 @pthread_join(i8* %t1, i8** null)
      call i32 @pthread_join(i8* %t2, i8** null)
      ret i32 0
    }
  )";
  auto trueRaceModule = parseModule(trueRaceSource);
  ASSERT_NE(trueRaceModule, nullptr);
  concurrency::ConcurrencyChecker refinedTrue(*trueRaceModule);
  refinedTrue.enableSparseFlowSensitiveRefinement(true);
  refinedTrue.enableMultiStageSlicing(true);
  refinedTrue.enableDeadlockCheck(false);
  refinedTrue.enableAtomicityCheck(false);
  refinedTrue.enableCondVarCheck(false);
  refinedTrue.enableLockMismatchCheck(false);
  refinedTrue.enableOpenMPCheck(false);
  refinedTrue.enableMPICheck(false);
  refinedTrue.enableCUDACheck(false);
  refinedTrue.enableLinuxKernelCheck(false);
  refinedTrue.enableStarvationCheck(false);
  refinedTrue.runAnalyses();
  refinedTrue.checkDataRaces();
  EXPECT_GT(refinedTrue.getStatistics().dataRacesFound, 0u);
}

TEST_F(ConcurrencyCheckerTest,
       DispatchesAllOpenMPAndMPIBugTypesIntoBugReportMgr) {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  mgr.clear_all_reports();

  const char *ompSource = R"(
    declare void @__kmpc_critical(i8*, i32, i8*)
    declare void @__kmpc_end_critical(i8*, i32, i8*)
    declare void @__kmpc_barrier(i8*, i32)

    define void @bad_omp() {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, i8* null)
      call void @__kmpc_barrier(i8* null, i32 0)
      call void @__kmpc_end_critical(i8* null, i32 0, i8* null)
      ret void
    }
  )";
  auto ompModule = parseModule(ompSource);
  ASSERT_NE(ompModule, nullptr);

  concurrency::ConcurrencyChecker ompChecker(*ompModule);
  ompChecker.enableDataRaceCheck(false);
  ompChecker.enableDeadlockCheck(false);
  ompChecker.enableAtomicityCheck(false);
  ompChecker.enableCondVarCheck(false);
  ompChecker.enableLockMismatchCheck(false);
  ompChecker.enableMPICheck(false);
  ompChecker.enableCUDACheck(false);
  ompChecker.enableLinuxKernelCheck(false);
  ompChecker.enableStarvationCheck(false);
  ompChecker.enableOpenMPCheck(true);
  ompChecker.runAnalyses();
  ompChecker.checkOpenMPBugs();
  EXPECT_GT(ompChecker.getStatistics().openMPBugsFound, 0u);
  EXPECT_GT(countReportsForBugTypeName("OpenMP Barrier in Critical"), 0u);

  const char *mpiSource = R"(
    declare i32 @MPI_Init(i32*, i8***)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define void @bad_mpi(i8* %buf, i8* %comm) {
    entry:
      call i32 @MPI_Init(i32* null, i8*** null)
      call i32 @MPI_Send(i8* %buf, i32 1, i32 0, i32 1, i32 -5, i8* %comm)
      ret void
    }
  )";
  auto mpiModule = parseModule(mpiSource);
  ASSERT_NE(mpiModule, nullptr);

  concurrency::ConcurrencyChecker mpiChecker(*mpiModule);
  mpiChecker.enableDataRaceCheck(false);
  mpiChecker.enableDeadlockCheck(false);
  mpiChecker.enableAtomicityCheck(false);
  mpiChecker.enableCondVarCheck(false);
  mpiChecker.enableLockMismatchCheck(false);
  mpiChecker.enableOpenMPCheck(false);
  mpiChecker.enableCUDACheck(false);
  mpiChecker.enableLinuxKernelCheck(false);
  mpiChecker.enableStarvationCheck(false);
  mpiChecker.enableMPICheck(true);
  mpiChecker.runAnalyses();
  mpiChecker.checkMPIBugs();
  EXPECT_GT(mpiChecker.getStatistics().mpiBugsFound, 0u);
  EXPECT_GT(countReportsForBugTypeName("MPI Invalid Tag"), 0u);
  EXPECT_GT(countReportsForBugTypeName("MPI Missing Finalize"), 0u);
  // Null-instruction module-level steps must still populate at least one step.
  int missingFinId = mgr.find_bug_type("MPI Missing Finalize");
  ASSERT_GE(missingFinId, 0);
  const auto *missingFinReports = mgr.get_reports_for_type(missingFinId);
  ASSERT_NE(missingFinReports, nullptr);
  ASSERT_FALSE(missingFinReports->empty());
  EXPECT_FALSE(missingFinReports->front()->get_steps().empty());
}

TEST_F(ConcurrencyCheckerTest, LinuxKernelCheckerRunsThroughConcurrencyChecker) {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  mgr.clear_all_reports();

  const char *source = R"(
    @spin = global i8 0
    declare void @_raw_spin_lock(i8*)
    declare void @_raw_spin_unlock(i8*)

    define void @driver_entry() {
    entry:
      call void @_raw_spin_lock(i8* @spin)
      call void @_raw_spin_lock(i8* @spin)
      call void @_raw_spin_unlock(i8* @spin)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableDataRaceCheck(false);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableOpenMPCheck(false);
  checker.enableMPICheck(false);
  checker.enableCUDACheck(false);
  checker.enableStarvationCheck(false);
  checker.enableLinuxKernelCheck(true);
  checker.runAnalyses();
  checker.checkLinuxKernelBugs();

  EXPECT_GT(checker.getStatistics().linuxKernelBugsFound, 0u);
  EXPECT_GT(countReportsForBugTypeName("Linux Kernel Double Lock"), 0u);
}

TEST_F(ConcurrencyCheckerTest,
       DataRaceCheckerHandlesMemIntrinsicDistinctFieldsAndAnonStruct) {
  const char *source = R"(
    %struct.Pair = type { i32, i32 }
    %struct.__anon_data = type { i32 }
    @pair = global %struct.Pair zeroinitializer, align 4
    @anon_obj = global %struct.__anon_data zeroinitializer, align 4
    @buf_dst = global [8 x i8] zeroinitializer, align 1
    @buf_src = global [8 x i8] zeroinitializer, align 1
    @g_ptr1 = global i32* null, align 8
    @g_ptr2 = global i32* null, align 8

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8* noalias writeonly, i8* noalias readonly, i64, i1 immarg)

    define i8* @worker1(i8* %arg) {
    entry:
      ; Distinct struct field 0
      %f0 = getelementptr inbounds %struct.Pair, %struct.Pair* @pair, i32 0, i32 0
      store i32 1, i32* %f0, align 4
      ; Distinct global pointer 1
      store i32* null, i32** @g_ptr1, align 8
      ; Anonymous struct write (must NOT be ignored)
      %a1 = getelementptr inbounds %struct.__anon_data, %struct.__anon_data* @anon_obj, i32 0, i32 0
      store i32 10, i32* %a1, align 4
      ; memcpy write to buf_dst reading buf_src
      %d = getelementptr [8 x i8], [8 x i8]* @buf_dst, i64 0, i64 0
      %s = getelementptr [8 x i8], [8 x i8]* @buf_src, i64 0, i64 0
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %d, i8* %s, i64 8, i1 false)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      ; Distinct struct field 1 (must NOT race with field 0)
      %f1 = getelementptr inbounds %struct.Pair, %struct.Pair* @pair, i32 0, i32 1
      store i32 2, i32* %f1, align 4
      ; Distinct global pointer 2 (must NOT race with g_ptr1)
      store i32* null, i32** @g_ptr2, align 8
      ; Anonymous struct conflicting write
      %a2 = getelementptr inbounds %struct.__anon_data, %struct.__anon_data* @anon_obj, i32 0, i32 0
      store i32 20, i32* %a2, align 4
      ; Conflicting write to buf_src (races with worker1's memcpy read!)
      %s2 = getelementptr [8 x i8], [8 x i8]* @buf_src, i64 0, i64 0
      store i8 7, i8* %s2, align 1
      ret i8* null
    }

    define i32 @main() {
    entry:
      %t1 = alloca i8, align 1
      %t2 = alloca i8, align 1
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();
  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  auto findStoreTo = [&](const char *fnName, const char *globalName) -> const Instruction * {
    for (const Instruction &I : instructions(*module->getFunction(fnName))) {
      if (const auto *st = dyn_cast<StoreInst>(&I)) {
        const Value *base = llvm::getUnderlyingObject(st->getPointerOperand());
        if (base && base->getName() == globalName)
          return st;
      }
    }
    return nullptr;
  };
  const Instruction *memcpyInst = nullptr;
  for (const Instruction &I : instructions(*module->getFunction("worker1"))) {
    if (isa<MemCpyInst>(&I))
      memcpyInst = &I;
  }

  const Instruction *pairW1 = findStoreTo("worker1", "pair");
  const Instruction *pairW2 = findStoreTo("worker2", "pair");
  const Instruction *gptrW1 = findStoreTo("worker1", "g_ptr1");
  const Instruction *gptrW2 = findStoreTo("worker2", "g_ptr2");
  const Instruction *anonW1 = findStoreTo("worker1", "anon_obj");
  const Instruction *anonW2 = findStoreTo("worker2", "anon_obj");
  const Instruction *bufSrcW2 = findStoreTo("worker2", "buf_src");

  ASSERT_NE(pairW1, nullptr);
  ASSERT_NE(pairW2, nullptr);
  ASSERT_NE(gptrW1, nullptr);
  ASSERT_NE(gptrW2, nullptr);
  ASSERT_NE(anonW1, nullptr);
  ASSERT_NE(anonW2, nullptr);
  ASSERT_NE(memcpyInst, nullptr);
  ASSERT_NE(bufSrcW2, nullptr);

  EXPECT_FALSE(checker.wouldReportDataRace(pairW1, pairW2));
  EXPECT_FALSE(checker.wouldReportDataRace(gptrW1, gptrW2));
  EXPECT_TRUE(checker.wouldReportDataRace(anonW1, anonW2));
  EXPECT_TRUE(checker.wouldReportDataRace(memcpyInst, bufSrcW2));

  // Verify reportBug forwards DataRaceInfo and keeps sibling trace levels at 0.
  BugReportMgr &mgr = BugReportMgr::get_instance();
  mgr.clear_all_reports();
  concurrency::ConcurrencyChecker driver(*module);
  driver.enableDeadlockCheck(false);
  driver.enableAtomicityCheck(false);
  driver.enableCondVarCheck(false);
  driver.enableLockMismatchCheck(false);
  driver.enableOpenMPCheck(false);
  driver.enableMPICheck(false);
  driver.enableCUDACheck(false);
  driver.enableLinuxKernelCheck(false);
  driver.enableStarvationCheck(false);
  driver.runAnalyses();
  driver.checkDataRaces();
  EXPECT_GE(driver.getStatistics().dataRacesFound, 2u);
  int raceId = mgr.find_bug_type("Data Race");
  ASSERT_GE(raceId, 0);
  const auto *raceReports = mgr.get_reports_for_type(raceId);
  ASSERT_NE(raceReports, nullptr);
  ASSERT_FALSE(raceReports->empty());
  const BugReport *first = raceReports->front();
  ASSERT_NE(first->get_extras(), nullptr);
  EXPECT_TRUE(first->get_extras()->metadata.count("access_path_1") > 0);
  EXPECT_TRUE(first->get_extras()->metadata.count("description") > 0);
}

TEST_F(ConcurrencyCheckerTest,
       DeadlockCheckerFiltersSequentialGateLockTryLockAndSPMDBarrier) {
  // 1. Sequential single-threaded AB then BA must NOT be reported as deadlock.
  {
    const char *seqSource = R"(
      @a = global i8 0
      @b = global i8 0
      declare i32 @pthread_mutex_lock(i8*)
      declare i32 @pthread_mutex_unlock(i8*)

      define i32 @main() {
      entry:
        call i32 @pthread_mutex_lock(i8* @a)
        call i32 @pthread_mutex_lock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @a)
        call i32 @pthread_mutex_lock(i8* @b)
        call i32 @pthread_mutex_lock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @b)
        ret i32 0
      }
    )";
    auto mod = parseModule(seqSource);
    ASSERT_NE(mod, nullptr);
    concurrency::ConcurrencyChecker checker(*mod);
    checker.enableDataRaceCheck(false);
    checker.enableAtomicityCheck(false);
    checker.enableCondVarCheck(false);
    checker.enableLockMismatchCheck(false);
    checker.enableOpenMPCheck(false);
    checker.enableMPICheck(false);
    checker.enableCUDACheck(false);
    checker.enableLinuxKernelCheck(false);
    checker.enableStarvationCheck(false);
    checker.runAnalyses();
    checker.checkDeadlocks();
    EXPECT_EQ(checker.getStatistics().deadlocksFound, 0u);
  }

  // 2. Concurrent AB-BA protected by a common outer gate lock G must NOT report
  // deadlock.
  {
    const char *gateSource = R"(
      @gate = global i8 0
      @a = global i8 0
      @b = global i8 0
      declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
      declare i32 @pthread_mutex_lock(i8*)
      declare i32 @pthread_mutex_unlock(i8*)

      define i8* @w1(i8* %arg) {
      entry:
        call i32 @pthread_mutex_lock(i8* @gate)
        call i32 @pthread_mutex_lock(i8* @a)
        call i32 @pthread_mutex_lock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @gate)
        ret i8* null
      }

      define i8* @w2(i8* %arg) {
      entry:
        call i32 @pthread_mutex_lock(i8* @gate)
        call i32 @pthread_mutex_lock(i8* @b)
        call i32 @pthread_mutex_lock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @gate)
        ret i8* null
      }

      define i32 @main() {
      entry:
        %t1 = alloca i8
        %t2 = alloca i8
        call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @w1, i8* null)
        call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @w2, i8* null)
        ret i32 0
      }
    )";
    auto mod = parseModule(gateSource);
    ASSERT_NE(mod, nullptr);
    concurrency::ConcurrencyChecker checker(*mod);
    checker.enableDataRaceCheck(false);
    checker.enableAtomicityCheck(false);
    checker.enableCondVarCheck(false);
    checker.enableLockMismatchCheck(false);
    checker.enableOpenMPCheck(false);
    checker.enableMPICheck(false);
    checker.enableCUDACheck(false);
    checker.enableLinuxKernelCheck(false);
    checker.enableStarvationCheck(false);
    checker.runAnalyses();
    checker.checkDeadlocks();
    EXPECT_EQ(checker.getStatistics().deadlocksFound, 0u);
  }

  // 3. Real 3-thread lock cycle A -> B -> C -> A MUST be detected.
  {
    const char *cycle3Source = R"(
      @a = global i8 0
      @b = global i8 0
      @c = global i8 0
      declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
      declare i32 @pthread_mutex_lock(i8*)
      declare i32 @pthread_mutex_unlock(i8*)

      define i8* @w1(i8* %arg) {
        call i32 @pthread_mutex_lock(i8* @a)
        call i32 @pthread_mutex_lock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @b)
        call i32 @pthread_mutex_unlock(i8* @a)
        ret i8* null
      }
      define i8* @w2(i8* %arg) {
        call i32 @pthread_mutex_lock(i8* @b)
        call i32 @pthread_mutex_lock(i8* @c)
        call i32 @pthread_mutex_unlock(i8* @c)
        call i32 @pthread_mutex_unlock(i8* @b)
        ret i8* null
      }
      define i8* @w3(i8* %arg) {
        call i32 @pthread_mutex_lock(i8* @c)
        call i32 @pthread_mutex_lock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @a)
        call i32 @pthread_mutex_unlock(i8* @c)
        ret i8* null
      }
      define i32 @main() {
        %t1 = alloca i8
        %t2 = alloca i8
        %t3 = alloca i8
        call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @w1, i8* null)
        call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @w2, i8* null)
        call i32 @pthread_create(i8* %t3, i8* null, i8* (i8*)* @w3, i8* null)
        ret i32 0
      }
    )";
    auto mod = parseModule(cycle3Source);
    ASSERT_NE(mod, nullptr);
    concurrency::ConcurrencyChecker checker(*mod);
    checker.enableDataRaceCheck(false);
    checker.enableAtomicityCheck(false);
    checker.enableCondVarCheck(false);
    checker.enableLockMismatchCheck(false);
    checker.enableOpenMPCheck(false);
    checker.enableMPICheck(false);
    checker.enableCUDACheck(false);
    checker.enableLinuxKernelCheck(false);
    checker.enableStarvationCheck(false);
    checker.runAnalyses();
    checker.checkDeadlocks();
    EXPECT_EQ(checker.getStatistics().deadlocksFound, 1u);
  }

  // 4. Single barrier call site inside a loop-spawned multi-instance worker
  // must NOT be falsely flagged as barrier divergence.
  {
    const char *spmdSource = R"(
      @bar = global i8 0
      declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
      declare i32 @pthread_barrier_wait(i8*)

      define i8* @worker(i8* %arg) {
      entry:
        call i32 @pthread_barrier_wait(i8* @bar)
        ret i8* null
      }

      define i32 @main() {
      entry:
        %t = alloca i8
        br label %loop
      loop:
        %i = phi i32 [ 0, %entry ], [ %next, %loop ]
        call i32 @pthread_create(i8* %t, i8* null, i8* (i8*)* @worker, i8* null)
        %next = add i32 %i, 1
        %cond = icmp slt i32 %next, 4
        br i1 %cond, label %loop, label %done
      done:
        ret i32 0
      }
    )";
    auto mod = parseModule(spmdSource);
    ASSERT_NE(mod, nullptr);
    concurrency::ConcurrencyChecker checker(*mod);
    checker.enableDataRaceCheck(false);
    checker.enableAtomicityCheck(false);
    checker.enableCondVarCheck(false);
    checker.enableLockMismatchCheck(false);
    checker.enableOpenMPCheck(false);
    checker.enableMPICheck(false);
    checker.enableCUDACheck(false);
    checker.enableLinuxKernelCheck(false);
    checker.enableStarvationCheck(false);
    checker.runAnalyses();
    checker.checkDeadlocks();
    EXPECT_EQ(checker.getStatistics().deadlocksFound, 0u);
  }
}

TEST_F(ConcurrencyCheckerTest,
       LockMismatchCheckerDetectsEarlyReturnLeakAndRemovesFragileBypasses) {
  const char *source = R"(
    @m = global i8 0
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    ; Merged return block where error branch forgets to unlock.
    define i32 @early_return_leak(i1 %err) {
    entry:
      call i32 @pthread_mutex_lock(i8* @m)
      br i1 %err, label %ret_bb, label %unlock_bb
    unlock_bb:
      call i32 @pthread_mutex_unlock(i8* @m)
      br label %ret_bb
    ret_bb:
      %r = phi i32 [ -1, %entry ], [ 0, %unlock_bb ]
      ret i32 %r
    }

    ; Unlock BEFORE lock in a function named "acquire_something" (must NOT be
    ; bypassed by name or equal acq/rel count!).
    define void @acquire_bad_order() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @m)
      call i32 @pthread_mutex_lock(i8* @m)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::LockSetAnalysis lsa(*module);
  lsa.analyze();
  concurrency::LockMismatchChecker checker(*module, &lsa,
                                           ThreadAPI::getThreadAPI());
  auto reports = checker.checkLockMisuse();

  bool foundEarlyLeak = false;
  bool foundUnlockWithoutLock = false;
  for (const auto &r : reports) {
    if (r.description.find("early-exit") != std::string::npos ||
        r.description.find("Lock leak") != std::string::npos) {
      foundEarlyLeak = true;
    }
    if (r.description.find("Unlock called without holding") !=
        std::string::npos) {
      foundUnlockWithoutLock = true;
    }
  }
  EXPECT_TRUE(foundEarlyLeak);
  EXPECT_TRUE(foundUnlockWithoutLock);
}

TEST_F(ConcurrencyCheckerTest,
       ConditionVariableCheckerDetectsSpuriousWakeupMultiMutexAndExtraLock) {
  const char *source = R"(
    @cv = global i8 0
    @m1 = global i8 0
    @m2 = global i8 0
    @extra = global i8 0

    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)

    define void @wait_outside_loop_with_extra_lock() {
    entry:
      call i32 @pthread_mutex_lock(i8* @extra)
      call i32 @pthread_mutex_lock(i8* @m1)
      call i32 @pthread_cond_wait(i8* @cv, i8* @m1)
      call i32 @pthread_mutex_unlock(i8* @m1)
      call i32 @pthread_mutex_unlock(i8* @extra)
      ret void
    }

    define void @wait_with_second_mutex(i1 %flag) {
    entry:
      call i32 @pthread_mutex_lock(i8* @m2)
      br label %loop
    loop:
      call i32 @pthread_cond_wait(i8* @cv, i8* @m2)
      br i1 %flag, label %loop, label %done
    done:
      call i32 @pthread_mutex_unlock(i8* @m2)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyContext ctx(*module);
  mhp::LockSetAnalysis lsa(ctx);
  lsa.analyze();
  concurrency::ConditionVariableChecker checker(*module,
                                                ThreadAPI::getThreadAPI(), &lsa);
  checker.setContext(&ctx);
  auto reports = checker.checkConditionVariables();

  bool foundSpurious = false;
  bool foundMultiMutex = false;
  bool foundExtraLock = false;
  for (const auto &r : reports) {
    if (r.description.find("spurious wakeups") != std::string::npos)
      foundSpurious = true;
    if (r.description.find("multiple distinct mutexes") != std::string::npos)
      foundMultiMutex = true;
    if (r.description.find("another unrelated mutex") != std::string::npos)
      foundExtraLock = true;
  }
  EXPECT_TRUE(foundSpurious);
  EXPECT_TRUE(foundMultiMutex);
  EXPECT_TRUE(foundExtraLock);
}

TEST_F(ConcurrencyCheckerTest,
       AtomicityCheckerDetectsSplitCriticalSectionAndSkipsContinuousLock) {
  const char *splitSource = R"(
    @mtx = global i8 0
    @balance = global i32 0, align 4
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define i8* @updater_split(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      %v = load i32, i32* @balance, align 4
      call i32 @pthread_mutex_unlock(i8* @mtx)
      %v2 = add i32 %v, 1
      call i32 @pthread_mutex_lock(i8* @mtx)
      store i32 %v2, i32* @balance, align 4
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret i8* null
    }

    define i8* @interleaver(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      store i32 100, i32* @balance, align 4
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret i8* null
    }

    define i32 @main() {
      %t1 = alloca i8
      %t2 = alloca i8
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @updater_split, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @interleaver, i8* null)
      ret i32 0
    }
  )";
  auto splitMod = parseModule(splitSource);
  ASSERT_NE(splitMod, nullptr);

  concurrency::ConcurrencyChecker splitChecker(*splitMod);
  splitChecker.enableDeadlockCheck(false);
  splitChecker.enableCondVarCheck(false);
  splitChecker.enableLockMismatchCheck(false);
  splitChecker.enableOpenMPCheck(false);
  splitChecker.enableMPICheck(false);
  splitChecker.enableCUDACheck(false);
  splitChecker.enableLinuxKernelCheck(false);
  splitChecker.enableStarvationCheck(false);
  splitChecker.runAnalyses();
  splitChecker.checkDataRaces();
  splitChecker.checkAtomicityViolations();
  // Individually locked -> 0 data races, but split CS -> >= 1 atomicity bug!
  EXPECT_EQ(splitChecker.getStatistics().dataRacesFound, 0u);
  EXPECT_GT(splitChecker.getStatistics().atomicityViolationsFound, 0u);

  // Continuous single critical section: 0 atomicity violations!
  const char *atomicSource = R"(
    @mtx = global i8 0
    @balance = global i32 0, align 4
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define i8* @updater_atomic(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      %v = load i32, i32* @balance, align 4
      %v2 = add i32 %v, 1
      store i32 %v2, i32* @balance, align 4
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret i8* null
    }

    define i8* @interleaver(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      store i32 100, i32* @balance, align 4
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret i8* null
    }

    define i32 @main() {
      %t1 = alloca i8
      %t2 = alloca i8
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @updater_atomic, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @interleaver, i8* null)
      ret i32 0
    }
  )";
  auto atomicMod = parseModule(atomicSource);
  ASSERT_NE(atomicMod, nullptr);

  concurrency::ConcurrencyChecker atomicChecker(*atomicMod);
  atomicChecker.enableDataRaceCheck(false);
  atomicChecker.enableDeadlockCheck(false);
  atomicChecker.enableCondVarCheck(false);
  atomicChecker.enableLockMismatchCheck(false);
  atomicChecker.enableOpenMPCheck(false);
  atomicChecker.enableMPICheck(false);
  atomicChecker.enableCUDACheck(false);
  atomicChecker.enableLinuxKernelCheck(false);
  atomicChecker.enableStarvationCheck(false);
  atomicChecker.runAnalyses();
  atomicChecker.checkAtomicityViolations();
  EXPECT_EQ(atomicChecker.getStatistics().atomicityViolationsFound, 0u);
}

TEST_F(ConcurrencyCheckerTest,
       OpenMPCheckerCFGAwareRegionsAndCriticalImplicitFlush) {
  // Balanced if/else taskgroup + critical section without explicit flush:
  // ZERO taskgroup mismatches and ZERO bogus missing-flush warnings!
  const char *cleanSource = R"(
    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)
    declare void @__kmpc_critical(i8*, i32, i8*)
    declare void @__kmpc_end_critical(i8*, i32, i8*)

    define void @clean_omp(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      br label %merge
    else:
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      br label %merge
    merge:
      call void @__kmpc_critical(i8* null, i32 0, i8* null)
      call void @__kmpc_end_critical(i8* null, i32 0, i8* null)
      ret void
    }
  )";
  auto cleanMod = parseModule(cleanSource);
  ASSERT_NE(cleanMod, nullptr);
  concurrency::OpenMPChecker cleanChecker(*cleanMod, nullptr,
                                          ThreadAPI::getThreadAPI());
  auto cleanReports = cleanChecker.checkOpenMPBugs();
  for (const auto &r : cleanReports) {
    EXPECT_NE(r.bugType,
              concurrency::ConcurrencyBugType::OPENMP_TASKGROUP_MISMATCH);
    EXPECT_NE(r.bugType, concurrency::ConcurrencyBugType::OPENMP_MISSING_FLUSH);
  }

  // Non-atomic spin-wait loop inside OpenMP outlined function MUST trigger
  // OPENMP_MISSING_FLUSH.
  const char *spinSource = R"(
    @ready = global i32 0, align 4
    define void @.omp_outlined.() {
    entry:
      br label %spin
    spin:
      %v = load i32, i32* @ready, align 4
      %done = icmp eq i32 %v, 0
      br i1 %done, label %spin, label %exit
    exit:
      ret void
    }
  )";
  auto spinMod = parseModule(spinSource);
  ASSERT_NE(spinMod, nullptr);
  concurrency::OpenMPChecker spinChecker(*spinMod, nullptr,
                                         ThreadAPI::getThreadAPI());
  bool foundSpinFlush = false;
  for (const auto &r : spinChecker.checkOpenMPBugs()) {
    if (r.bugType == concurrency::ConcurrencyBugType::OPENMP_MISSING_FLUSH)
      foundSpinFlush = true;
  }
  EXPECT_TRUE(foundSpinFlush);
}

TEST_F(ConcurrencyCheckerTest,
       AnalysisDumperProducesValidJSONWithThreadGroupedMHPAndExtendedSummaries) {
  const char *source = R"(
    @shared = global i32 0, align 4
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @w1(i8* %arg) {
      store i32 1, i32* @shared, align 4
      ret i8* null
    }
    define i8* @w2(i8* %arg) {
      store i32 2, i32* @shared, align 4
      ret i8* null
    }
    define i32 @main() {
      %t1 = alloca i8
      %t2 = alloca i8
      call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @w1, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @w2, i8* null)
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableSparseFlowSensitiveRefinement(true);
  checker.runAnalyses();

  std::string jsonStr;
  raw_string_ostream os(jsonStr);
  checker.dumpAnalysisResults(os, true);
  os.flush();

  cJSON *root = cJSON_Parse(jsonStr.c_str());
  ASSERT_NE(root, nullptr) << "Invalid JSON output:\n" << jsonStr;
  cJSON *mhpPairs = cJSON_GetObjectItem(root, "mhp_pairs");
  ASSERT_NE(mhpPairs, nullptr);
  EXPECT_GT(cJSON_GetArraySize(mhpPairs), 0);
  cJSON *stats = cJSON_GetObjectItem(root, "statistics");
  ASSERT_NE(stats, nullptr);
  EXPECT_NE(cJSON_GetObjectItem(stats, "sparse_refinement"), nullptr);
  cJSON_Delete(root);
}

TEST_F(ConcurrencyCheckerTest,
       StarvationCheckerResolvesCppGlobalAliasDestructorAndConditionalWrappers) {
  // 1. Clean: C++ GlobalAlias destructor releases lock before callback, and
  // conditional lock wrapper acquires/releases conditionally before callback.
  const char *cleanSource = R"(
    @mtx = global i8 0
    @cb = external global void ()*
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @_ZN9MutexLockD1Ev = unnamed_addr alias void (i8*), void (i8*)* @_ZN9MutexLockD2Ev

    define void @_ZN9MutexLockD2Ev(i8* %m) {
    entry:
      call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }

    define void @maybe_lock(i1 %cond, i8* %m) {
    entry:
      br i1 %cond, label %do_lock, label %done
    do_lock:
      call i32 @pthread_mutex_lock(i8* %m)
      br label %done
    done:
      ret void
    }

    define void @maybe_unlock(i1 %cond, i8* %m) {
    entry:
      br i1 %cond, label %do_unlock, label %done
    do_unlock:
      call i32 @pthread_mutex_unlock(i8* %m)
      br label %done
    done:
      ret void
    }

    define void @clean_caller(i1 %cond) {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      call void @_ZN9MutexLockD1Ev(i8* @mtx)
      call void @maybe_lock(i1 %cond, i8* @mtx)
      call void @maybe_unlock(i1 %cond, i8* @mtx)
      %fn = load void ()*, void ()** @cb
      call void %fn()
      ret void
    }
  )";
  auto cleanMod = parseModule(cleanSource);
  ASSERT_NE(cleanMod, nullptr);
  mhp::LockSetAnalysis cleanLsa(*cleanMod);
  cleanLsa.analyze();
  concurrency::StarvationChecker cleanChecker(*cleanMod, cleanLsa);
  EXPECT_TRUE(cleanChecker.checkStarvation().empty());

  // 2. Buggy: lock unconditionally held across an indirect callback MUST report
  // starvation/lock-inversion risk.
  const char *buggySource = R"(
    @mtx = global i8 0
    @cb = external global void ()*
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @holds_lock_across_callback() {
    entry:
      call i32 @pthread_mutex_lock(i8* @mtx)
      %fn = load void ()*, void ()** @cb
      call void %fn()
      call i32 @pthread_mutex_unlock(i8* @mtx)
      ret void
    }
  )";
  auto buggyMod = parseModule(buggySource);
  ASSERT_NE(buggyMod, nullptr);
  mhp::LockSetAnalysis buggyLsa(*buggyMod);
  buggyLsa.analyze();
  concurrency::StarvationChecker buggyChecker(*buggyMod, buggyLsa);
  EXPECT_FALSE(buggyChecker.checkStarvation().empty());
}

TEST_F(ConcurrencyCheckerTest,
       LockMismatchCheckerHandlesVTableTryLockSretAndTransitiveUnlockWrappers) {
  // 1. Clean wrappers:
  //   - transitive 2-level unlock helper (caller holds lock -> mid -> leaf)
  //   - -O0 parameter-spill alloca conditional acquire wrapper
  //   - C++17 sret RAII lock guard factory
  //   - try_lock conditional acquire
  const char *cleanSource = R"(
    %struct.Guard = type { i8* }
    @g_mtx = global i8 0
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_trylock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @leaf_unlock(i8* %m) {
    entry:
      call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }

    define void @mid_unlock(i8* %m) {
    entry:
      call void @leaf_unlock(i8* %m)
      ret void
    }

    define void @caller_uses_transitive_unlock() {
    entry:
      call i32 @pthread_mutex_lock(i8* @g_mtx)
      call void @mid_unlock(i8* @g_mtx)
      ret void
    }

    define void @o0_conditional_acquire(i8* %m, i1 %cond) {
    entry:
      %m.addr = alloca i8*, align 8
      store i8* %m, i8** %m.addr, align 8
      br i1 %cond, label %do_lock, label %done
    do_lock:
      %mp = load i8*, i8** %m.addr, align 8
      call i32 @pthread_mutex_lock(i8* %mp)
      br label %done
    done:
      ret void
    }

    define void @make_guard(%struct.Guard* sret(%struct.Guard) %agg, i8* %m) {
    entry:
      %f = getelementptr inbounds %struct.Guard, %struct.Guard* %agg, i32 0, i32 0
      store i8* %m, i8** %f, align 8
      call i32 @pthread_mutex_lock(i8* %m)
      ret void
    }

    define i32 @try_acquire_wrapper(i8* %m) {
    entry:
      %rc = call i32 @pthread_mutex_trylock(i8* %m)
      ret i32 %rc
    }
  )";
  auto cleanMod = parseModule(cleanSource);
  ASSERT_NE(cleanMod, nullptr);
  mhp::LockSetAnalysis cleanLsa(*cleanMod);
  cleanLsa.analyze();
  concurrency::LockMismatchChecker cleanChecker(*cleanMod, &cleanLsa,
                                                ThreadAPI::getThreadAPI());
  EXPECT_TRUE(cleanChecker.checkLockMisuse().empty());

  // 2. Real double unlock MUST still be detected.
  const char *doubleUnlockSource = R"(
    @m = global i8 0
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @double_unlock_bug() {
    entry:
      call i32 @pthread_mutex_lock(i8* @m)
      call i32 @pthread_mutex_unlock(i8* @m)
      call i32 @pthread_mutex_unlock(i8* @m)
      ret void
    }
  )";
  auto buggyMod = parseModule(doubleUnlockSource);
  ASSERT_NE(buggyMod, nullptr);
  mhp::LockSetAnalysis buggyLsa(*buggyMod);
  buggyLsa.analyze();
  concurrency::LockMismatchChecker buggyChecker(*buggyMod, &buggyLsa,
                                                ThreadAPI::getThreadAPI());
  EXPECT_FALSE(buggyChecker.checkLockMisuse().empty());
}

TEST_F(ConcurrencyCheckerTest,
       PrecisionRecallMatrixCoversSingleThreadJoinedHandoffSVCAndMPIWindow) {
  // 1. Benign single-threaded program with indirect calls + RMW on globals:
  //    MUST report 0 races, 0 atomicity violations, 0 deadlocks.
  {
    const char *stSource = R"(
      @hook = external global void ()*
      @counter = global i32 0, align 4

      define void @helper_rmw() {
      entry:
        %v = load i32, i32* @counter, align 4
        %v2 = add i32 %v, 1
        store i32 %v2, i32* @counter, align 4
        ret void
      }

      define i32 @main() {
      entry:
        %fn = load void ()*, void ()** @hook
        call void %fn()
        %x = load i32, i32* @counter, align 4
        %x2 = add i32 %x, 2
        store i32 %x2, i32* @counter, align 4
        ret i32 %x2
      }
    )";
    auto mod = parseModule(stSource);
    ASSERT_NE(mod, nullptr);
    concurrency::ConcurrencyChecker checker(*mod);
    checker.runAnalyses();
    checker.runChecks();
    EXPECT_EQ(checker.getStatistics().dataRacesFound, 0u);
    EXPECT_EQ(checker.getStatistics().atomicityViolationsFound, 0u);
    EXPECT_EQ(checker.getStatistics().deadlocksFound, 0u);
  }

  // 2. Benign pthread_create + pthread_join handoff checked under BOTH Region
  //    and StaticVectorClock MHP backends: MUST report 0 data races and 0
  //    atomicity violations.
  {
    const char *handoffSource = R"(
      @payload = global i32 0, align 4
      declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
      declare i32 @pthread_join(i8*, i8**)

      define i8* @producer(i8* %arg) {
      entry:
        store i32 42, i32* @payload, align 4
        ret i8* null
      }

      define i32 @main() {
      entry:
        %t = alloca i8
        store i32 1, i32* @payload, align 4
        call i32 @pthread_create(i8* %t, i8* null, i8* (i8*)* @producer, i8* null)
        call i32 @pthread_join(i8* %t, i8** null)
        %v = load i32, i32* @payload, align 4
        ret i32 %v
      }
    )";
    for (auto backend :
         {concurrency::ConcurrencyChecker::MHPBackendKind::Region,
          concurrency::ConcurrencyChecker::MHPBackendKind::StaticVectorClock}) {
      auto mod = parseModule(handoffSource);
      ASSERT_NE(mod, nullptr);
      concurrency::ConcurrencyChecker checker(*mod);
      checker.setMHPBackend(backend);
      checker.runAnalyses();
      checker.runChecks();
      EXPECT_EQ(checker.getStatistics().dataRacesFound, 0u);
      EXPECT_EQ(checker.getStatistics().atomicityViolationsFound, 0u);
    }
  }

  // 3. Buggy concurrent unprotected RMW checked under BOTH Region and
  //    StaticVectorClock MHP backends: MUST detect both data race AND atomicity
  //    violation.
  {
    const char *rmwBugSource = R"(
      @shared = global i32 0, align 4
      declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

      define i8* @inc_worker(i8* %arg) {
      entry:
        %v = load i32, i32* @shared, align 4
        %next = add i32 %v, 1
        store i32 %next, i32* @shared, align 4
        ret i8* null
      }

      define i8* @clobber_worker(i8* %arg) {
      entry:
        store i32 99, i32* @shared, align 4
        ret i8* null
      }

      define i32 @main() {
      entry:
        %t1 = alloca i8
        %t2 = alloca i8
        call i32 @pthread_create(i8* %t1, i8* null, i8* (i8*)* @inc_worker, i8* null)
        call i32 @pthread_create(i8* %t2, i8* null, i8* (i8*)* @clobber_worker, i8* null)
        ret i32 0
      }
    )";
    for (auto backend :
         {concurrency::ConcurrencyChecker::MHPBackendKind::Region,
          concurrency::ConcurrencyChecker::MHPBackendKind::StaticVectorClock}) {
      auto mod = parseModule(rmwBugSource);
      ASSERT_NE(mod, nullptr);
      concurrency::ConcurrencyChecker checker(*mod);
      checker.setMHPBackend(backend);
      checker.runAnalyses();
      checker.runChecks();
      EXPECT_GT(checker.getStatistics().dataRacesFound, 0u);
      EXPECT_GT(checker.getStatistics().atomicityViolationsFound, 0u);
    }
  }

  // 4. Buggy MPI window leak MUST report with non-empty diagnostic steps.
  {
    BugReportMgr &mgr = BugReportMgr::get_instance();
    mgr.clear_all_reports();
    const char *mpiWinSource = R"(
      declare i32 @MPI_Init(i32*, i8***)
      declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*, i8*)
      declare i32 @MPI_Finalize()

      define void @leak_win(i8* %base, i8* %comm, i8* %win) {
      entry:
        call i32 @MPI_Init(i32* null, i8*** null)
        call i32 @MPI_Win_create(i8* %base, i64 64, i32 4, i8* null, i8* %comm, i8* %win)
        call i32 @MPI_Finalize()
        ret void
      }
    )";
    auto mod = parseModule(mpiWinSource);
    ASSERT_NE(mod, nullptr);
    concurrency::ConcurrencyChecker checker(*mod);
    checker.enableDataRaceCheck(false);
    checker.enableDeadlockCheck(false);
    checker.enableAtomicityCheck(false);
    checker.enableCondVarCheck(false);
    checker.enableLockMismatchCheck(false);
    checker.enableOpenMPCheck(false);
    checker.enableCUDACheck(false);
    checker.enableLinuxKernelCheck(false);
    checker.enableStarvationCheck(false);
    checker.enableMPICheck(true);
    checker.runAnalyses();
    checker.checkMPIBugs();
    int winLeakId = mgr.find_bug_type("MPI Window Leak");
    ASSERT_GE(winLeakId, 0);
    const auto *winReports = mgr.get_reports_for_type(winLeakId);
    ASSERT_NE(winReports, nullptr);
    ASSERT_FALSE(winReports->empty());
    EXPECT_FALSE(winReports->front()->get_steps().empty());
  }
}
