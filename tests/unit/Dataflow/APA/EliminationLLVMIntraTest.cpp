#include "EliminationTestSupport.h"

TEST_F(APATest, ContextSummariesAreBuiltOnceAfterCalleeFactsChange) {
  const char *Source = R"(
    define i32 @helper() {
    entry:
      ret i32 42
    }
    define i32 @main() {
    entry:
      %r = call i32 @helper()
      ret i32 %r
    }
  )";
  auto M = lotus::unittest::parseModule(Context, Source, "APAContextCache");
  ASSERT_NE(M, nullptr);
  auto *Entry = M->getFunction("main");
  elimination::EliminationOptions Options;
  Options.Ordering = elimination::OrderingPolicy::Hybrid;
  Options.Order.RecordTrace = true;
  const auto R = elimination::runInterElimConstantPropagation(
      Entry, nullptr, nullptr, nullptr, nullptr, nullptr, Options);
  ASSERT_EQ(R.solveStatus(), elimination::SolveStatus::Ok);
  ASSERT_TRUE(R.hasContextSolveDiagnostics());
  const auto &D = R.contextSolveDiagnostics();
  EXPECT_EQ(D.procedure_summary_builds, D.procedure_context_count);
  EXPECT_EQ(D.procedure_summary_builds, 2u);
  EXPECT_GT(D.procedure_summary_reuses, 0u);
  EXPECT_EQ(R.procedureContextDiagnostics().size(), 2u);
  for (const auto &Pair : R.procedureContextDiagnostics()) {
    EXPECT_EQ(Pair.Diagnostics.procedure_summary_builds, 1u);
    EXPECT_EQ(Pair.Diagnostics.ordering.trace.size(), Pair.Nodes.size());
  }
  auto *Return = findFirst<llvm::ReturnInst>(Entry);
  ASSERT_NE(Return, nullptr);
  bool Checked = false;
  for (const auto &Key : R.contextsForInstruction(Return)) {
    const auto *Fact = R.tryIN(Key);
    ASSERT_NE(Fact, nullptr);
    auto Value = Fact->find(Return->getReturnValue());
    ASSERT_NE(Value, Fact->end());
    const auto *Constant = Value->second.asConstantInteger();
    ASSERT_NE(Constant, nullptr);
    EXPECT_EQ(Constant->getSExtValue(), 42);
    Checked = true;
  }
  EXPECT_TRUE(Checked);
}

TEST_F(APATest, OnlinePoliciesPreserveInterproceduralLoopFactsAndDiagnostics) {
  const char *Source = R"(
    define void @helper(i1 %again) {
    entry:
      br label %loop
    loop:
      br i1 %again, label %loop, label %exit
    exit:
      ret void
    }
    define i32 @main(i1 %again) {
    entry:
      call void @helper(i1 %again)
      ret i32 0
    }
  )";
  auto M = lotus::unittest::parseModule(Context, Source, "APAOnlineOrder");
  ASSERT_TRUE(M != nullptr);
  auto *Entry = M->getFunction("main");
  const auto Baseline = elimination::runInterSummaryElimReachability(Entry);
  for (auto Policy : {elimination::OrderingPolicy::Structural,
                      elimination::OrderingPolicy::ExpressionAware,
                      elimination::OrderingPolicy::StarRisk,
                      elimination::OrderingPolicy::Hybrid}) {
    elimination::PathSummaryEquationOptions Opts;
    Opts.Ordering = Policy;
    Opts.Order.RecordTrace = true;
    const auto R =
        elimination::runInterSummaryElimReachability(Entry, nullptr, Opts);
    ASSERT_EQ(R.solveStatus(), elimination::SolveStatus::Ok);
    for (const auto &Point : Baseline.getINMap()) {
      ASSERT_NE(R.tryIN(Point.first), nullptr);
      EXPECT_EQ(*R.tryIN(Point.first), Point.second);
    }
    ASSERT_TRUE(R.hasSummarySolveDiagnostics());
    EXPECT_GT(R.summarySolveDiagnostics().ordering.selected_nodes, 0u);
    EXPECT_GT(R.summarySolveDiagnostics().semantic_star_time_ns, 0u);
    EXPECT_GT(R.summarySolveDiagnostics().star_iterations_total, 0u);

    const auto Modular =
        elimination::runModularInterReachability(Entry, nullptr, Opts);
    ASSERT_TRUE(Modular.hasSummarySolveDiagnostics());
    EXPECT_GT(Modular.summarySolveDiagnostics().ordering.selected_nodes, 0u);
    EXPECT_GE(Modular.summarySolveDiagnostics().semantic_star_time_ns, 0u);
  }
}

TEST_F(APATest, LLVMReachabilitySkipsUnreachableBlock) {
  const char *Source = R"(
    define i32 @test(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %live = add i32 1, 2
      br label %exit
    else:
      br label %exit
    dead:
      %deadv = add i32 40, 2
      br label %exit
    exit:
      %phi = phi i32 [ %live, %then ], [ 0, %else ]
      ret i32 %phi
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimReachability(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Live = findInstructionByName(F, "live");
  auto *Dead = findInstructionByName(F, "deadv");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Live, nullptr);
  ASSERT_NE(Dead, nullptr);
  ASSERT_NE(Ret, nullptr);

  ASSERT_NE(Result.tryIN(Live), nullptr);
  EXPECT_TRUE(*Result.tryIN(Live));
  ASSERT_NE(Result.tryIN(Ret), nullptr);
  EXPECT_TRUE(*Result.tryIN(Ret));
  EXPECT_EQ(Result.tryIN(Dead), nullptr);
}
TEST_F(APATest, LLVMConstantPropagationTracksFoldedValuesAtReturn) {
  const char *Source = R"(
    define i32 @test() {
    entry:
      %sum = add i32 1, 2
      %scaled = mul i32 %sum, 4
      ret i32 %scaled
    }
  )";

  auto Module = lotus::unittest::parseModule(Context, Source, "APATest");
  ASSERT_NE(Module, nullptr);

  auto *F = Module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = elimination::runIntraElimConstantPropagation(F);
  ASSERT_TRUE(Result.hasSolveMetadata());
  EXPECT_EQ(Result.solveStatus(), elimination::SolveStatus::Ok);

  auto *Scaled = findInstructionByName(F, "scaled");
  auto *Ret = findFirst<llvm::ReturnInst>(F);
  ASSERT_NE(Scaled, nullptr);
  ASSERT_NE(Ret, nullptr);

  auto *Facts = Result.tryIN(Ret);
  ASSERT_NE(Facts, nullptr);

  auto ScaledIt = Facts->find(Scaled);
  ASSERT_NE(ScaledIt, Facts->end());
}
