#include "IR/ICFG/ICFGBuilder.h"
#include "Verification/Automata/NwaSearch.h"
#include "Verification/TraceAbstraction/InterpolantAutomatonBuilder.h"
#include "Verification/TraceAbstraction/TraceAbstractionEngine.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace lotus::verification::traceabstraction;

class TraceAbstractionTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::Function &parse(const std::string &ir) {
    llvm::SMDiagnostic diagnostic;
    module = llvm::parseAssemblyString(ir, diagnostic, context);
    if (!module) {
      diagnostic.print("trace-test", llvm::errs());
      throw std::runtime_error("invalid test IR");
    }
    return *module->getFunction("main");
  }
  std::string straight(unsigned expected) {
    return "declare void @__assert_fail()\n"
           "define void @main() {\n"
           "entry:\n %x = alloca i32\n store i32 0, i32* %x\n"
           " %v = load i32, i32* %x\n %next = add i32 %v, 1\n"
           " store i32 %next, i32* %x\n %out = load i32, i32* %x\n"
           " %ok = icmp eq i32 %out, " +
           std::to_string(expected) +
           "\n"
           " br i1 %ok, label %done, label %error\n"
           "error:\n call void @__assert_fail()\n unreachable\n"
           "done:\n ret void\n}\n";
  }
};

TEST_F(TraceAbstractionTest, StraightLineSafeInOneRefinement) {
  auto &f = parse(straight(1));
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
  EXPECT_EQ(result.iterations, 1u);
}

TEST_F(TraceAbstractionTest, StraightLineUnsafeHasConcreteValues) {
  auto &f = parse(straight(0));
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
  EXPECT_FALSE(result.counterexample.empty());
  EXPECT_FALSE(result.assignments.empty());
  bool foundOne = false;
  for (const auto &value : result.assignments)
    foundOne |= value.first.find("next#") == 0 && value.second == "1";
  EXPECT_TRUE(foundOne);
}

TEST_F(TraceAbstractionTest, LoopDoesNotUnrollOneHundredTimes) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main() {
    entry: br label %loop
    loop:
      %x = phi i32 [0, %entry], [%next, %body]
      %less = icmp slt i32 %x, 100
      br i1 %less, label %body, label %exit
    body:
      %next = add nsw i32 %x, 1
      br label %loop
    exit:
      %ok = icmp sge i32 %x, 0
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  TraceAbstractionOptions options;
  options.maxIterations = 5;
  auto result = TraceAbstractionEngine(options).verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
  EXPECT_LE(result.iterations, 5u);
  options.enableInductiveGeneralization = false;
  options.maxIterations = 2;
  auto spineOnly = TraceAbstractionEngine(options).verify(f);
  EXPECT_EQ(spineOnly.status, VerificationResult::Unknown);
  EXPECT_EQ(spineOnly.iterations, 2u);
}

TEST_F(TraceAbstractionTest, InfeasibleDiamond) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main(i32 %x) {
    entry:
      %positive = icmp sgt i32 %x, 0
      br i1 %positive, label %left, label %right
    left: br label %merge
    right: br label %merge
    merge:
      %a = phi i32 [1, %left], [2, %right]
      %bad = icmp eq i32 %a, 3
      br i1 %bad, label %error, label %done
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
  EXPECT_LE(result.iterations, 3u);
}

TEST_F(TraceAbstractionTest, NestedCallsAndTwoCallsInOneBlock) {
  auto &f = parse(R"(
    declare void @abort()
    define i32 @inc(i32 %x) {
      %y = add i32 %x, 1
      ret i32 %y
    }
    define i32 @twice(i32 %x) {
      %a = call i32 @inc(i32 %x)
      %b = call i32 @inc(i32 %a)
      ret i32 %b
    }
    define void @main() {
      %a = call i32 @twice(i32 0)
      %b = call i32 @inc(i32 %a)
      %ok = icmp eq i32 %b, 3
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto program = IcfgToNwa::build(f);
  auto search = nwa::isEmpty(program.automaton);
  ASSERT_FALSE(search.empty());
  EXPECT_TRUE(nwa::validateRun(*program.automaton, *search.run));
  unsigned calls = 0, returns = 0;
  for (unsigned i = 0; i < search.run->word.size(); ++i) {
    auto kind = program.automaton->alphabet().kind(search.run->word[i]);
    if (kind == nwa::LetterKind::Call) {
      ++calls;
      EXPECT_GT(search.run->matching[i], i);
    }
    if (kind == nwa::LetterKind::Return) {
      ++returns;
      EXPECT_GE(search.run->matching[i], 0);
    }
  }
  EXPECT_EQ(calls, 4u);
  EXPECT_EQ(returns, calls);
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionTest, NoErrorAndResourceLimits) {
  auto &f = parse("define void @main() { ret void }");
  EXPECT_EQ(TraceAbstractionEngine().verify(f).status,
            VerificationResult::Safe);
  auto &withError = parse(straight(1));
  TraceAbstractionOptions options;
  options.maxIterations = 0;
  EXPECT_EQ(TraceAbstractionEngine(options).verify(withError).status,
            VerificationResult::Unknown);
  options.timeout = std::chrono::milliseconds(0);
  EXPECT_EQ(TraceAbstractionEngine(options).verify(withError).status,
            VerificationResult::Unknown);
}

TEST_F(TraceAbstractionTest, SpacerCertificateAndInvalidCertificateRejection) {
  auto &f = parse(straight(1));
  auto program = IcfgToNwa::build(f);
  auto search = nwa::isEmpty(program.automaton);
  ASSERT_FALSE(search.empty());
  TraceChecker checker(program);
  auto proof = checker.check(*search.run);
  ASSERT_EQ(proof.status, TraceStatus::Infeasible) << proof.reason;
  ASSERT_EQ(proof.interpolants.size(), search.run->word.size() + 1);
  EXPECT_TRUE(checker.isTrue(proof.interpolants.front()));
  EXPECT_TRUE(checker.isFalse(proof.interpolants.back()));
  for (unsigned i = 0; i < search.run->word.size(); ++i)
    EXPECT_TRUE(checker.isInductive(proof.interpolants[i], search.run->word[i],
                                    proof.interpolants[i + 1]));
  auto spine = InterpolantAutomatonBuilder::build(program, *search.run, proof,
                                                  checker, false);
  ASSERT_NE(spine, nullptr);
  EXPECT_TRUE(nwa::accepts(spine, search.run->word));
  auto invalidEdge = proof;
  invalidEdge.interpolants[1] = proof.context->bool_val(false);
  EXPECT_EQ(InterpolantAutomatonBuilder::build(program, *search.run,
                                               invalidEdge, checker),
            nullptr);
  proof.interpolants.front() = proof.context->bool_val(false);
  EXPECT_EQ(
      InterpolantAutomatonBuilder::build(program, *search.run, proof, checker),
      nullptr);
  TraceAbstractionOptions options;
  options.enableInductiveGeneralization = false;
  EXPECT_EQ(TraceAbstractionEngine(options).verify(f).status,
            VerificationResult::Safe);
}

TEST_F(TraceAbstractionTest, RealBugSurvivesAnInfeasibleTrace) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main() {
    entry: br i1 false, label %error, label %longer
    longer:
      %a = add i32 0, 1
      %b = add i32 %a, 1
      br label %error
    error: call void @abort()
      unreachable
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
  EXPECT_GE(result.iterations, 2u);
  ASSERT_FALSE(result.counterexample.empty());
  auto *call = llvm::dyn_cast<llvm::CallBase>(result.counterexample.back());
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "abort");
}

TEST_F(TraceAbstractionTest, IntegerWrapAndUnsignedComparison) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main() {
      %x = add i8 255, 1
      %wrap = icmp eq i8 %x, 0
      %unsigned = icmp ugt i8 255, 0
      %ok = and i1 %wrap, %unsigned
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionTest, PoisonCannotProduceFalseSafetyOrConcreteBug) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main() {
      %x = add nsw i8 127, 1
      call void @abort()
      unreachable
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unknown) << result.reason;
  EXPECT_NE(result.reason.find("poison"), std::string::npos);
}

TEST_F(TraceAbstractionTest, SimultaneousLoopPhis) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main() {
    entry: br label %loop
    loop:
      %a = phi i8 [1, %entry], [%b, %body]
      %b = phi i8 [2, %entry], [%a, %body]
      %count = phi i8 [0, %entry], [%next, %body]
      %again = icmp eq i8 %count, 0
      br i1 %again, label %body, label %exit
    body:
      %next = add i8 %count, 1
      br label %loop
    exit:
      %ok = icmp eq i8 %b, 1
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionTest, AssumesAndNondeterministicInput) {
  auto &f = parse(R"(
    declare i32 @__VERIFIER_nondet_int()
    declare void @__VERIFIER_assume(i1)
    declare void @__VERIFIER_error()
    define void @main() {
      %x = call i32 @__VERIFIER_nondet_int()
      %positive = icmp sgt i32 %x, 0
      call void @__VERIFIER_assume(i1 %positive)
      %bad = icmp slt i32 %x, 0
      br i1 %bad, label %error, label %done
    error: call void @__VERIFIER_error()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionTest, UnknownForUnmodeledExternalEffects) {
  auto &f = parse(R"(
    declare void @external()
    declare void @abort()
    define void @main() {
      call void @external()
      call void @abort()
      unreachable
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unknown);
  EXPECT_NE(result.reason.find("external"), std::string::npos);
}

TEST_F(TraceAbstractionTest, UnknownForUninitializedAndAliasedMemory) {
  auto &uninitialized = parse(R"(
    declare void @abort()
    define void @main() {
      %a = alloca i32
      %x = load i32, i32* %a
      call void @abort()
      unreachable
    })");
  auto result = TraceAbstractionEngine().verify(uninitialized);
  EXPECT_EQ(result.status, VerificationResult::Unknown);
  EXPECT_NE(result.reason.find("uninitialized"), std::string::npos);
  auto &aliased = parse(R"(
    declare void @abort()
    define void @main(i32* %p) {
      store i32 0, i32* %p
      call void @abort()
      unreachable
    })");
  EXPECT_EQ(TraceAbstractionEngine().verify(aliased).status,
            VerificationResult::Unknown);
}

TEST_F(TraceAbstractionTest, ErrorInsideCalleeAllowsPendingCall) {
  auto &f = parse(R"(
    declare void @abort()
    define void @fail(i32 %x) {
      %bad = icmp eq i32 %x, 7
      br i1 %bad, label %error, label %done
    error: call void @abort()
      unreachable
    done: ret void
    }
    define void @main() {
      call void @fail(i32 7)
      ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
  bool pending = false;
  for (unsigned i = 0; i < result.counterexample.size(); ++i)
    if (auto *call =
            llvm::dyn_cast_or_null<llvm::CallBase>(result.counterexample[i]))
      if (call->getCalledFunction()->getName() == "fail")
        pending = result.matching[i] == -1;
  EXPECT_TRUE(pending);
}

TEST_F(TraceAbstractionTest, ReturnValueCanExposeRealBug) {
  auto &f = parse(R"(
    declare void @abort()
    define i32 @inc(i32 %x) {
      %y = add i32 %x, 1
      ret i32 %y
    }
    define void @main() {
      %a = call i32 @inc(i32 1)
      %ok = icmp eq i32 %a, 1
      br i1 %ok, label %done, label %error
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
}

TEST_F(TraceAbstractionTest, RecursiveActivationFindsConcreteBug) {
  auto &f = parse(R"(
    declare void @abort()
    define void @recur(i32 %x) {
      %done = icmp eq i32 %x, 0
      br i1 %done, label %error, label %again
    again:
      %next = sub i32 %x, 1
      call void @recur(i32 %next)
      ret void
    error: call void @abort()
      unreachable
    }
    define void @main() {
      call void @recur(i32 1)
      ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
  EXPECT_GE(result.iterations, 2u);
}

TEST_F(TraceAbstractionTest, ExplicitErrorInstructionAndIcfgNode) {
  auto &f = parse(R"(
    define void @main() {
    entry: br i1 false, label %target, label %done
    target: %x = add i32 0, 1
      ret void
    done: ret void
    })");
  auto *target = &*std::next(f.begin());
  EXPECT_EQ(TraceAbstractionEngine().verify(f, &target->front()).status,
            VerificationResult::Safe);
  ICFG graph;
  ICFGBuilder(&graph).build(module.get());
  auto *node = graph.getIntraBlockNode(target);
  auto result = TraceAbstractionEngine().verify(graph, node);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
  auto *entry = graph.getIntraBlockNode(&f.getEntryBlock());
  EXPECT_EQ(TraceAbstractionEngine().verify(graph, entry).status,
            VerificationResult::Unsafe);
}

TEST_F(TraceAbstractionTest, SwitchAndCasts) {
  auto &f = parse(R"(
    declare void @abort()
    define void @main() {
      %x = zext i8 255 to i32
      %y = trunc i32 %x to i16
      switch i16 %y, label %error [i16 255, label %done]
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Safe) << result.reason;
}

TEST_F(TraceAbstractionTest, FunctionPointersAreNotReturnValueSlots) {
  auto &f = parse(R"(
    declare void @abort()
    define i32 @a() { ret i32 1 }
    define i32 @b() { ret i32 1 }
    define void @main() {
      %same = icmp eq i32 ()* @a, @b
      br i1 %same, label %error, label %done
    error: call void @abort()
      unreachable
    done: ret void
    })");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unknown) << result.reason;
}

TEST_F(TraceAbstractionTest, LifetimeAndRangeEffectsAreNotSilentlyIgnored) {
  auto &lifetime = parse(R"(
    declare void @abort()
    declare void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture)
    define void @main() {
      %a = alloca i8
      store i8 1, i8* %a
      call void @llvm.lifetime.end.p0i8(i64 1, i8* %a)
      %x = load i8, i8* %a
      call void @abort()
      unreachable
    })");
  EXPECT_EQ(TraceAbstractionEngine().verify(lifetime).status,
            VerificationResult::Unknown);
  auto &range = parse(R"(
    declare void @abort()
    define void @main() {
      %a = alloca i8
      store i8 2, i8* %a
      %x = load i8, i8* %a, !range !0
      call void @abort()
      unreachable
    }
    !0 = !{i8 0, i8 1}
    )");
  EXPECT_EQ(TraceAbstractionEngine().verify(range).status,
            VerificationResult::Unknown);
}

TEST_F(TraceAbstractionTest, FrontendCancellationAndStateBudget) {
  auto &f = parse(straight(1));
  auto budget = std::make_shared<nwa::OperationContext>();
  budget->maxStates = 1;
  EXPECT_THROW(IcfgToNwa::build(f, nullptr, budget), nwa::ResourceLimit);
  std::atomic<bool> cancel{true};
  budget->cancel = &cancel;
  EXPECT_THROW(IcfgToNwa::build(f, nullptr, budget), nwa::OperationCancelled);
}

TEST_F(TraceAbstractionTest, NonEntryFunctionAndLegacyIcfgRoots) {
  parse(R"(
    declare void @abort()
    define void @main() { ret void }
    define void @other() {
      call void @abort()
      unreachable
    })");
  auto &f = *module->getFunction("other");
  auto result = TraceAbstractionEngine().verify(f);
  EXPECT_EQ(result.status, VerificationResult::Unsafe) << result.reason;
  ICFG legacy;
  auto *node = legacy.getIntraBlockNode(&f.getEntryBlock());
  EXPECT_EQ(TraceAbstractionEngine().verify(legacy, node).status,
            VerificationResult::Unsafe);
}

TEST_F(TraceAbstractionTest, PhiAndUnreachableInstructionTargets) {
  auto &f = parse(R"(
    declare void @stop() noreturn
    define void @main() {
    entry: br label %merge
    merge:
      %x = phi i32 [1, %entry]
      call void @stop()
      %dead = add i32 %x, 1
      unreachable
    })");
  auto &merge = *std::next(f.begin());
  EXPECT_EQ(TraceAbstractionEngine().verify(f, &merge.front()).status,
            VerificationResult::Unsafe);
  auto *dead = merge.front().getNextNode()->getNextNode();
  EXPECT_EQ(TraceAbstractionEngine().verify(f, dead).status,
            VerificationResult::Safe);
}
