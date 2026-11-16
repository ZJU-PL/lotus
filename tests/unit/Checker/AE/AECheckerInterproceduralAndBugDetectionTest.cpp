#include "AECheckerTestSupport.h"

TEST_F(AECheckerTest,
       InternalIndirectCallWithRecursiveTargetPreservesRecursiveSummary) {
  const char *source = R"(
    declare i1 @unknown()

    define i32 @helper(i32 %n) {
    entry:
      ret i32 0
    }

    define i32 @demo(i32 %a) {
    entry:
      %cmp = icmp sge i32 %a, 5
      br i1 %cmp, label %base, label %recur

    recur:
      %inc = add i32 %a, 1
      %res = call i32 @demo(i32 %inc)
      ret i32 %res

    base:
      ret i32 %a
    }

    define i32 @dispatch(i32 %n) {
    entry:
      %cond = call i1 @unknown()
      %fp = select i1 %cond, i32 (i32)* @helper, i32 (i32)* @demo
      %res = call i32 %fp(i32 %n)
      ret i32 %res
    }

    define i32 @main() {
    entry:
      %res = call i32 @dispatch(i32 0)
      ret i32 %res
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false, false,
                          AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);

  IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
  // dispatch() may call helper(), which returns 0; demo() contributes the
  // recursive [5, +inf] summary to the same join.
  EXPECT_EQ(ret.lb().getIntNumeral(), 0) << ret.toString();
  EXPECT_TRUE(ret.ub().is_infinity()) << ret.toString();
}
TEST_F(AECheckerTest, DefaultWidenDelayMatchesExplicitThree) {
  const char *source = R"(
    define i32 @count_to_three() {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, 3
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      br label %loop

    exit:
      ret i32 %i
    }

    define i32 @main() {
    entry:
      %res = call i32 @count_to_three()
      ret i32 %res
    }
  )";

  auto defaultModule = parseModule(source);
  ASSERT_NE(defaultModule, nullptr);
  AEResult defaultResult =
      runAE(defaultModule.get(), true, false,
            AbstractInterpretation::WIDEN_NARROW, std::nullopt);
  IntervalValue defaultRet =
      getFunctionReturnInterval(defaultModule.get(), "main");

  auto explicitModule = parseModule(source);
  ASSERT_NE(explicitModule, nullptr);
  AEResult explicitResult = runAE(explicitModule.get(), true, false,
                                  AbstractInterpretation::WIDEN_NARROW, 3u);
  IntervalValue explicitRet =
      getFunctionReturnInterval(explicitModule.get(), "main");

  EXPECT_EQ(defaultResult.overflow_bugs, explicitResult.overflow_bugs);
  EXPECT_EQ(defaultResult.null_bugs, explicitResult.null_bugs);
  EXPECT_TRUE(defaultRet.equals(explicitRet));
}
TEST_F(AECheckerTest, ParitySensitiveRegressionHarness) {
  struct ParityCase {
    const char *name;
    const char *source;
    size_t expectedOverflow;
    size_t expectedNull;
    std::optional<IntervalValue> expectedMainReturn;
    AbstractInterpretation::HandleRecur recursionMode;
    std::optional<unsigned> widenDelay;
  };

  const std::vector<ParityCase> cases = {
      ParityCase{"recursive_top_summary",
                 R"(
         define i32 @demo(i32 %a) {
         entry:
           %cmp = icmp sge i32 %a, 6
           br i1 %cmp, label %base, label %recur

         recur:
           %inc = add i32 %a, 1
           %res = call i32 @demo(i32 %inc)
           ret i32 %res

         base:
           ret i32 %a
         }

         define i32 @main() {
         entry:
           %res = call i32 @demo(i32 0)
           ret i32 %res
         }
       )",
                 0u, 0u, IntervalValue::top(), AbstractInterpretation::TOP, 3u},
      ParityCase{"indirect_external_memcpy_no_null_false_positive",
                 R"(
         declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

         define void @test_copy() {
           %dst = alloca [4 x i8], align 1
           %src = alloca [8 x i8], align 1
           %dst_ptr = bitcast [4 x i8]* %dst to i8*
           %src_ptr = bitcast [8 x i8]* %src to i8*
           %fp = alloca void (i8*, i8*, i64, i1)*, align 8
           store void (i8*, i8*, i64, i1)* @llvm.memcpy.p0i8.p0i8.i64,
                 void (i8*, i8*, i64, i1)** %fp
           %f = load void (i8*, i8*, i64, i1)*, void (i8*, i8*, i64, i1)** %fp
           call void %f(i8* %dst_ptr, i8* %src_ptr, i64 8, i1 false)
           ret void
         }
       )",
                 1u, 0u, std::nullopt, AbstractInterpretation::WIDEN_NARROW,
                 3u},
  };

  for (const ParityCase &testCase : cases) {
    SCOPED_TRACE(testCase.name);
    auto module = parseModule(testCase.source);
    ASSERT_NE(module, nullptr);

    AEResult result = runAE(module.get(), true, false, testCase.recursionMode,
                            testCase.widenDelay);
    EXPECT_EQ(result.overflow_bugs, testCase.expectedOverflow);
    EXPECT_EQ(result.null_bugs, testCase.expectedNull);
    if (testCase.expectedMainReturn.has_value()) {
      IntervalValue ret = getFunctionReturnInterval(module.get(), "main");
      EXPECT_TRUE(ret.equals(*testCase.expectedMainReturn));
    }
  }
}
TEST_F(AECheckerTest, MemoryLeakDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @test_leak() {
      %p = call i8* @malloc(i64 32)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, true);
  EXPECT_GT(result.mem_leak_bugs, 0u);
}
TEST_F(AECheckerTest, MemoryLeakEscapeViaOutParameterIsNotReported) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @publish(i8** %out) {
    entry:
      %p = call i8* @malloc(i64 32)
      store i8* %p, i8** %out
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i8*, align 8
      call void @publish(i8** %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, true);
  EXPECT_EQ(result.mem_leak_bugs, 0u);
}
TEST_F(AECheckerTest, MemoryLeakOnLastReferenceOverwrite) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @overwrite_last_ref() {
    entry:
      %slot = alloca i8*, align 8
      %p = call i8* @malloc(i64 8)
      store i8* %p, i8** %slot
      store i8* null, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), true, true);
  EXPECT_GT(result.mem_leak_bugs, 0u);
}
TEST_F(AECheckerTest, DivZeroDetection) {
  const char *source = R"(
    define i32 @main(i32 %d) {
    entry:
      %q = sdiv i32 42, %d
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result =
      runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
            true, false);
  EXPECT_GT(result.divzero_bugs, 0u);
}
TEST_F(AECheckerTest, IntegerOverflowDetection) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %q = add i32 2147483647, 1
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result =
      runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
            false, true);
  EXPECT_GT(result.int_overflow_bugs, 0u);
}
TEST_F(AECheckerTest, MainRootedAnalysisSkipsUnreachableHelpers) {
  const char *source = R"(
    define void @helper() {
    entry:
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %val = load i32, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get(), false);
  EXPECT_EQ(result.null_bugs, 0u);
}
TEST_F(AECheckerTest, AEBugReportsAreClearedBetweenRuns) {
  const char *leakSource = R"(
    declare i8* @malloc(i64)

    define void @test_leak() {
    entry:
      %p = call i8* @malloc(i64 32)
      ret void
    }
  )";

  auto leakModule = parseModule(leakSource);
  ASSERT_NE(leakModule, nullptr);

  AEResult leakResult = runAE(leakModule.get(), true, true);
  EXPECT_GT(leakResult.mem_leak_bugs, 0u);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int leakType = mgr.find_bug_type("AE Memory Leak");
  ASSERT_GE(leakType, 0);
  const auto *leakReports = mgr.get_reports_for_type(leakType);
  ASSERT_NE(leakReports, nullptr);
  EXPECT_EQ(leakReports->size(), 1u);

  const char *divSource = R"(
    define i32 @test_divzero(i32 %d) {
    entry:
      %q = sdiv i32 42, %d
      ret i32 %q
    }
  )";

  auto divModule = parseModule(divSource);
  ASSERT_NE(divModule, nullptr);

  AEResult divResult =
      runAE(divModule.get(), true, false, AbstractInterpretation::WIDEN_NARROW,
            3u, true, false);
  EXPECT_GT(divResult.divzero_bugs, 0u);

  leakReports = mgr.get_reports_for_type(leakType);
  EXPECT_TRUE(leakReports == nullptr || leakReports->empty());
}
TEST_F(AECheckerTest, QuietModePreservesReportedFindings) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @test_leak() {
    entry:
      %p = call i8* @malloc(i64 32)
      ret void
    }
  )";

  auto normalModule = parseModule(source);
  auto quietModule = parseModule(source);
  ASSERT_NE(normalModule, nullptr);
  ASSERT_NE(quietModule, nullptr);

  setenv("LOTUS_AE_QUIET", "0", 1);
  runAE(normalModule.get(), true, true);
  setenv("LOTUS_AE_QUIET", "1", 1);
  BugReportMgr &mgr = BugReportMgr::get_instance();
  int leakType = mgr.find_bug_type("AE Memory Leak");
  ASSERT_GE(leakType, 0);
  const auto *normalReports = mgr.get_reports_for_type(leakType);
  const size_t normalCount = normalReports ? normalReports->size() : 0;

  runAE(quietModule.get(), true, true);
  const auto *quietReports = mgr.get_reports_for_type(leakType);
  const size_t quietCount = quietReports ? quietReports->size() : 0;

  EXPECT_GT(normalCount, 0u);
  EXPECT_EQ(quietCount, normalCount);
}
TEST_F(AECheckerTest, EnabledChecksAutoInstallLibraryDetectors) {
  const char *source = R"(
    define void @test_auto_install(i32 %idx) {
    entry:
      %arr = alloca [4 x i32], align 4
      %slot = alloca i32*, align 8
      store i32* null, i32** %slot
      %p = load i32*, i32** %slot
      %idx64 = sext i32 %idx to i64
      %bad = getelementptr inbounds [4 x i32], [4 x i32]* %arr, i64 0, i64 %idx64
      store i32 1, i32* %bad
      %v = load i32, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      call void @test_auto_install(i32 6)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  ae.reset();
  ae.setStrictCheckpoint(false);
  ae.setAnalyzeAllFunctions(true);
  ae.setEnableBufOverflowCheck(true);
  ae.setEnableNullDerefCheck(true);
  ae.runOnModule(module.get());

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int overflowType = mgr.find_bug_type("AE Buffer Overflow");
  ASSERT_GE(overflowType, 0);
  const auto *overflowReports = mgr.get_reports_for_type(overflowType);
  ASSERT_NE(overflowReports, nullptr);
  EXPECT_FALSE(overflowReports->empty());

  int nullType = mgr.find_bug_type("AE Null Dereference");
  ASSERT_GE(nullType, 0);
  const auto *nullReports = mgr.get_reports_for_type(nullType);
  ASSERT_NE(nullReports, nullptr);
  EXPECT_FALSE(nullReports->empty());
}
TEST_F(AECheckerTest, PosixMemalignStoresAllocatedPointerThroughOutParam) {
  const char *source = R"(
    declare i32 @posix_memalign(i8**, i64, i64)

    define void @test_posix_memalign() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot
      %res = call i32 @posix_memalign(i8** %slot, i64 16, i64 32)
      %p = load i8*, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  const auto *load = dyn_cast<LoadInst>(
      findNamedInstruction(module.get(), "test_posix_memalign", "p"));
  ASSERT_NE(load, nullptr);
  AbstractValue value = getInstructionValue(load);
  EXPECT_TRUE(value.isAddr());
  EXPECT_FALSE(value.getAddrs().empty());
  EXPECT_FALSE(value.getAddrs().contains(NullMemAddr));
}
TEST_F(AECheckerTest, AsprintfStoresAllocatedPointerThroughOutParam) {
  const char *source = R"(
    @.fmt = private unnamed_addr constant [3 x i8] c"%d\00"
    declare i32 @asprintf(i8**, i8*, ...)

    define void @test_asprintf() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot
      %fmt = getelementptr inbounds [3 x i8], [3 x i8]* @.fmt, i64 0, i64 0
      %res = call i32 (i8**, i8*, ...) @asprintf(i8** %slot, i8* %fmt, i32 7)
      %p = load i8*, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  const auto *load = dyn_cast<LoadInst>(
      findNamedInstruction(module.get(), "test_asprintf", "p"));
  ASSERT_NE(load, nullptr);
  AbstractValue value = getInstructionValue(load);
  EXPECT_TRUE(value.isAddr());
  EXPECT_FALSE(value.getAddrs().empty());
  EXPECT_FALSE(value.getAddrs().contains(NullMemAddr));
}
TEST_F(AECheckerTest, ScanfUnsignedFormatProducesNonNegativeRange) {
  const char *source = R"(
    @.fmtu = private unnamed_addr constant [3 x i8] c"%u\00"
    declare i32 @scanf(i8*, ...)

    define i32 @read_unsigned() {
    entry:
      %x = alloca i32, align 4
      %fmt = getelementptr inbounds [3 x i8], [3 x i8]* @.fmtu, i64 0, i64 0
      %rv = call i32 (i8*, ...) @scanf(i8* %fmt, i32* %x)
      %v = load i32, i32* %x
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  IntervalValue ret = getFunctionReturnInterval(module.get(), "read_unsigned");
  EXPECT_GE(ret.lb().getIntNumeralOrZero(), 0);
}
TEST_F(AECheckerTest, StrtoullProducesNonNegativeRange) {
  const char *source = R"(
    @.num = private unnamed_addr constant [2 x i8] c"0\00"
    declare i64 @strtoull(i8*, i8**, i32)

    define i64 @read_num() {
    entry:
      %num = getelementptr inbounds [2 x i8], [2 x i8]* @.num, i64 0, i64 0
      %val = call i64 @strtoull(i8* %num, i8** null, i32 10)
      ret i64 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  IntervalValue ret = getFunctionReturnInterval(module.get(), "read_num");
  EXPECT_GE(ret.lb().getIntNumeralOrZero(), 0);
}
TEST_F(AECheckerTest, NegativeGEPProducesConcreteAddressSet) {
  const char *source = R"(
    define void @test_negative_gep_addr() {
    entry:
      %arr = alloca [8 x i8], align 1
      %p = getelementptr inbounds [8 x i8], [8 x i8]* %arr, i64 0, i64 5
      %q = getelementptr inbounds i8, i8* %p, i64 -1
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  runAE(module.get());

  const auto *q =
      findNamedInstruction(module.get(), "test_negative_gep_addr", "q");
  ASSERT_NE(q, nullptr);

  AbstractValue qVal = getInstructionValue(q);
  EXPECT_TRUE(qVal.isAddr());
  EXPECT_FALSE(qVal.getAddrs().empty());
}
TEST_F(AECheckerTest, NegativeGEPMemcpyRespectsBacktrackedCapacity) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @test_negative_gep_memcpy_safe() {
    entry:
      %dst = alloca [8 x i8], align 1
      %src = alloca [4 x i8], align 1
      %dst_tail = getelementptr inbounds [8 x i8], [8 x i8]* %dst, i64 0, i64 5
      %dst_back = getelementptr inbounds i8, i8* %dst_tail, i64 -1
      %src_ptr = getelementptr inbounds [4 x i8], [4 x i8]* %src, i64 0, i64 0
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_back, i8* %src_ptr, i64 4, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}
