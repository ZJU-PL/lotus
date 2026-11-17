#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"
#include "Checker/TPA/FrontEnd/DefUseModuleBuilder.h"

#include <gtest/gtest.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>

namespace {
struct Result {
  size_t violations = 0;
  size_t losses = 0;
  size_t ambiguous = 0;
  size_t unresolved = 0;
  tpa::taint::TaintAnalysis::Statistics stats;
};

Result
run(llvm::StringRef ir, unsigned k = 0, bool track = false, bool repeat = false,
    tpa::taint::UnmodeledPolicy policy = tpa::taint::UnmodeledPolicy::Ignore,
    bool normalize = true) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(ir, diagnostic, context);
  if (!module) {
    diagnostic.print("TPATaintTest", llvm::errs());
    ADD_FAILURE();
    return {};
  }
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  EXPECT_TRUE(tpa::taint::validateModule(*module, llvm::errs()));
  const unsigned savedK = context::KLimitContext::getLimit();
  const auto savedStrategy = context::getContextStrategy();
  auto restoreContext = llvm::make_scope_exit([&] {
    context::KLimitContext::setLimit(savedK);
    context::setContextStrategy(savedStrategy);
  });
  context::KLimitContext::setLimit(k);
  context::setContextStrategy(context::ContextStrategy::KLimit);
  tpa::taint::prepareModule(*module, normalize);
  tpa::SemiSparseProgramBuilder pb;
  auto program = pb.runOnModule(*module);
  tpa::SemiSparsePointerAnalysis pointer;
  pointer.loadExternalPointerTable(TPA_TAINT_TEST_INPUTS "/ptr.spec");
  pointer.runOnProgram(program);
  tpa::taint::DefUseModuleBuilder builder(pointer, policy);
  builder.loadExternalModRefTable(TPA_TAINT_TEST_INPUTS "/modref.spec");
  auto graph = builder.buildDefUseModule(*module);
  tpa::taint::TaintAnalysis analysis(pointer, policy);
  analysis.loadExternalTaintTable(TPA_TAINT_TEST_INPUTS "/taint.spec");
  analysis.runOnDefUseModule(graph, track);
  if (repeat) {
    size_t count = analysis.getStatistics().violations;
    analysis.runOnDefUseModule(graph, track);
    EXPECT_EQ(count, analysis.getStatistics().violations);
  }
  Result result;
  result.stats = analysis.getStatistics();
  result.violations = result.stats.violations;
  result.losses = analysis.getLossSites().size();
  result.unresolved = analysis.getIgnoredUnresolvedCalls().size();
  for (const auto &entry : analysis.getViolations())
    for (const auto &violation : entry.second)
      result.ambiguous +=
          violation.actualVal == tpa::taint::TaintLattice::Either;
  context::KLimitContext::setLimit(savedK);
  context::setContextStrategy(savedStrategy);
  return result;
}

TEST(TPATaint, ScalarSourceToSinkAndCleanConstant) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %x = call i32 @source()
      call void @sink(i32 %x)
      call void @sink(i32 0)
      ret i32 0
    }
  )",
                    0, false, true);
  EXPECT_EQ(result.violations, 1u);
  EXPECT_EQ(result.ambiguous, 0u);
  EXPECT_GT(result.stats.evaluations, 0u);
  EXPECT_GT(result.stats.topEdges, 0u);
}

TEST(TPATaint, StrongUpdateClearsTaintThroughAlias) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca i32
      %alias = getelementptr i32, i32* %p, i64 0
      %x = call i32 @source()
      store i32 %x, i32* %p
      %before = load i32, i32* %alias
      call void @sink(i32 %before)
      store i32 0, i32* %alias
      %after = load i32, i32* %p
      call void @sink(i32 %after)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 1u);
  EXPECT_GT(result.stats.memoryEdges, 0u);
}

TEST(TPATaint, StructFieldsRemainDistinct) {
  auto result = run(R"(
    %S = type { i32, i32 }
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca %S
      %a = getelementptr %S, %S* %p, i64 0, i32 0
      %b = getelementptr %S, %S* %p, i64 0, i32 1
      %x = call i32 @source()
      store i32 %x, i32* %a
      store i32 0, i32* %b
      %va = load i32, i32* %a
      %vb = load i32, i32* %b
      call void @sink(i32 %va)
      call void @sink(i32 %vb)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, ContextSensitivityAndPrecisionLoss) {
  const char *ir = R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @identity(i32 %x) { ret i32 %x }
    define i32 @main() {
      %x = call i32 @source()
      %dirty = call i32 @identity(i32 %x)
      %clean = call i32 @identity(i32 0)
      call void @sink(i32 %dirty)
      call void @sink(i32 %clean)
      ret i32 0
    }
  )";
  auto merged = run(ir, 0, true);
  EXPECT_EQ(merged.violations, 2u);
  EXPECT_EQ(merged.ambiguous, 2u);
  EXPECT_GT(merged.losses, 0u);
  auto split = run(ir, 1, true);
  EXPECT_EQ(split.violations, 1u);
  EXPECT_EQ(split.ambiguous, 0u);
  EXPECT_GT(split.stats.contexts, merged.stats.contexts);
}

TEST(TPATaint, MultipleReturnsCarryMemoryEffects) {
  auto result = run(R"(
    declare void @sink(i32)
    define void @write(i32* %p, i32 %x, i1 %cond) {
      br i1 %cond, label %a, label %b
    a:
      store i32 %x, i32* %p
      ret void
    b:
      store i32 0, i32* %p
      ret void
    }
    define i32 @main(i32 %argc) {
      %p = alloca i32
      store i32 0, i32* %p
      call void @write(i32* %p, i32 %argc, i1 true)
      %value = load i32, i32* %p
      call void @sink(i32 %value)
      ret i32 0
    }
  )",
                    1, true);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, ExternalMemorySourceAndCopyDirection) {
  auto result = run(R"(
    declare void @fill(i8*)
    declare void @copy(i8*, i8*)
    declare void @sink_mem(i8*)
    define i32 @main() {
      %src = alloca i8
      %dst = alloca i8
      store i8 0, i8* %src
      store i8 0, i8* %dst
      call void @fill(i8* %src)
      call void @copy(i8* %dst, i8* %src)
      call void @sink_mem(i8* %dst)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, IndirectCallResolvedByTPA) {
  auto result = run(R"(
    declare void @sink(i32)
    define i32 @identity(i32 %x) { ret i32 %x }
    define i32 @main(i32 %argc) {
      %fp = alloca i32 (i32)*
      store i32 (i32)* @identity, i32 (i32)** %fp
      %f = load i32 (i32)*, i32 (i32)** %fp
      %x = call i32 %f(i32 %argc)
      call void @sink(i32 %x)
      ret i32 0
    }
  )",
                    1);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, ParserRejectsInvalidIndicesAndAcceptsFinalComment) {
  annotation::ExternalTaintTable table;
  std::string text;
  llvm::raw_string_ostream errors(text);
  EXPECT_TRUE(annotation::ExternalTaintTable::parse(
      "SINK sink Arg0 V\n# no newline", table, errors));
  EXPECT_NE(table.lookup("sink"), nullptr);
  EXPECT_FALSE(annotation::ExternalTaintTable::parse("SINK sink Arg256 V",
                                                     table, errors));
  EXPECT_FALSE(annotation::ExternalTaintTable::parse("PIPE copy Arg0 R Ret R",
                                                     table, errors));
  EXPECT_NE(table.lookup("sink"),
            nullptr); // failed parse preserves previous table
}

TEST(TPATaint, UnsupportedIRProducesDiagnostic) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
    define i32 @main() {
      %p = alloca i32
      %ap = bitcast i32* %p to i8*
      %x = va_arg i8* %ap, i32
      ret i32 0
    }
  )",
                                          diagnostic, context);
  ASSERT_NE(module, nullptr);
  std::string text;
  llvm::raw_string_ostream errors(text);
  EXPECT_FALSE(tpa::taint::validateModule(*module, errors));
  EXPECT_NE(errors.str().find("va_arg"), std::string::npos);
}
TEST(TPATaint, NewCallerReceivesExistingReturn) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @produce() {
      %x = call i32 @source()
      ret i32 %x
    }
    define i32 @main() {
      %x = call i32 @produce()
      call void @sink(i32 %x)
      %y = call i32 @produce()
      call void @sink(i32 %y)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 2u);
}

TEST(TPATaint, ConditionalCalleeWritePreservesIncomingTaint) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define void @maybe_clear(i32* %p, i1 %condition) {
      br i1 %condition, label %clear, label %keep
    clear:
      store i32 0, i32* %p
      ret void
    keep:
      ret void
    }
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %p
      call void @maybe_clear(i32* %p, i1 true)
      %y = load i32, i32* %p
      call void @sink(i32 %y)
      ret i32 0
    }
  )",
                    1);
  // Path-insensitive: both the clearing and preserving returns contribute.
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, WeakUpdateDoesNotClearOtherPossibleTarget) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %a = alloca i32
      %b = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %a
      store i32 0, i32* %b
      %p = select i1 true, i32* %a, i32* %b
      store i32 0, i32* %p
      %y = load i32, i32* %a
      call void @sink(i32 %y)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, LoopReachesFixedPoint) {
  auto result = run(R"(
    declare void @sink(i32)
    define i32 @main(i32 %argc) {
      %p = alloca i32
      store i32 0, i32* %p
      br label %loop
    loop:
      %x = phi i32 [ %argc, %0 ], [ 0, %loop ]
      store i32 %x, i32* %p
      br i1 false, label %loop, label %exit
    exit:
      %y = load i32, i32* %p
      call void @sink(i32 %y)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, UncalledFunctionsAndUnreachableBlocksAreExcluded) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define void @unused() {
      %p = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %p
      %y = load i32, i32* %p
      call void @sink(i32 %y)
      ret void
    }
    define i32 @main() {
      ret i32 0
    dead:
      %p = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %p
      %y = load i32, i32* %p
      call void @sink(i32 %y)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 0u);
  EXPECT_EQ(result.stats.sinks, 0u);
}

TEST(TPATaint, ReachableMemoryReductionAndVariadicPipe) {
  auto result = run(R"(
    %S = type { i32, i32 }
    declare i32 @source()
    declare i32 @sum_bytes(i8*)
    declare i32 @combine(...)
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca %S
      %a = getelementptr %S, %S* %p, i64 0, i32 0
      %b = getelementptr %S, %S* %p, i64 0, i32 1
      %x = call i32 @source()
      store i32 0, i32* %a
      store i32 %x, i32* %b
      %buffer = bitcast %S* %p to i8*
      %sum = call i32 @sum_bytes(i8* %buffer)
      call void @sink(i32 %sum)
      %vararg = call i32 (...) @combine(i32 0, i32 %x)
      call void @sink(i32 %vararg)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 2u);
}

TEST(TPATaint, RecursiveContextsRemainBounded) {
  auto result = run(R"(
    declare void @sink(i32)
    define i32 @recursive(i32 %x) {
      %test = icmp eq i32 %x, 0
      br i1 %test, label %base, label %step
    base:
      ret i32 %x
    step:
      %next = call i32 @recursive(i32 %x)
      ret i32 %next
    }
    define i32 @main(i32 %argc) {
      %x = call i32 @recursive(i32 %argc)
      call void @sink(i32 %x)
      ret i32 0
    }
  )",
                    1);
  EXPECT_EQ(result.violations, 1u);
  EXPECT_LE(result.stats.contexts, 3u);
}

TEST(TPATaint, IgnoredExternalPreservesMemoryAndAllowsDownstreamCalls) {
  auto result = run(R"(
    declare i32 @source()
    declare i32 @opaque(i32*, i32)
    declare void @sink(i32)
    define i32 @second(i32 %unused, i32 %x) { ret i32 %x }
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %p
      %unknown = call i32 @opaque(i32* %p, i32 %x)
      call void @sink(i32 %unknown)
      %loaded = load i32, i32* %p
      call void @sink(i32 %loaded)
      %forwarded = call i32 @second(i32 %unknown, i32 %x)
      call void @sink(i32 %forwarded)
      ret i32 0
    }
  )",
                    1, true, true);
  EXPECT_EQ(result.violations, 2u);
}

TEST(TPATaint, AvailableModelsAreAppliedIndependently) {
  auto result = run(R"(
    declare i32 @taint_only_source()
    declare void @modref_only(i32*)
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @taint_only_source()
      store i32 %x, i32* %p
      call void @modref_only(i32* %p)
      %y = load i32, i32* %p
      call void @sink(i32 %y)
      ret i32 0
    }
  )");
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, StrictPolicyRejectsMissingModRefAndTaintModels) {
  using tpa::taint::UnmodeledPolicy;
  EXPECT_THROW(run(R"(
    declare i32 @taint_only_source()
    define i32 @main() { %x = call i32 @taint_only_source() ret i32 0 }
  )",
                   0, false, false, UnmodeledPolicy::Error),
               std::invalid_argument);
  EXPECT_THROW(run(R"(
    declare void @modref_only(i32*)
    define i32 @main() { %p = alloca i32 call void @modref_only(i32* %p) ret i32 0 }
  )",
                   0, false, false, UnmodeledPolicy::Error),
               std::invalid_argument);
}

TEST(TPATaint, DebugIntrinsicsAreNoOpsInStrictMode) {
  auto buffer = llvm::MemoryBuffer::getFile(TPA_TAINT_TEST_INPUTS "/debug.ll");
  ASSERT_TRUE(static_cast<bool>(buffer));
  auto result = run((*buffer)->getBuffer(), 1, false, false,
                    tpa::taint::UnmodeledPolicy::Error);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, ExplicitIgnoreReturnDoesNotBlockKnownFlows) {
  auto result = run(R"(
    declare i32 @source()
    declare i32 @trusted_noop()
    declare void @sink(i32)
    define i32 @second(i32 %unused, i32 %x) { ret i32 %x }
    define i32 @main() {
      %x = call i32 @source()
      %ignored = call i32 @trusted_noop()
      %y = call i32 @second(i32 %ignored, i32 %x)
      call void @sink(i32 %y)
      ret i32 0
    }
  )",
                    0, false, false, tpa::taint::UnmodeledPolicy::Error);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, FloatingNegationAndScalarAndPointerFreeze) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @source()
      %ambiguous = select i1 true, i32 %x, i32 0
      %fp = sitofp i32 %ambiguous to double
      %negative = fneg double %fp
      %wide = fpext double %negative to x86_fp80
      %frozen = freeze x86_fp80 %wide
      %value = fptosi x86_fp80 %frozen to i32
      call void @sink(i32 %value)
      store i32 %x, i32* %p
      %q = freeze i32* %p
      %loaded = load i32, i32* %q
      call void @sink(i32 %loaded)
      %clean = fneg float 1.000000e+00
      %clean_frozen = freeze float %clean
      %clean_value = fptosi float %clean_frozen to i32
      call void @sink(i32 %clean_value)
      ret i32 0
    }
  )",
                    0, true);
  EXPECT_EQ(result.violations, 2u);
}

TEST(TPATaint, AtomicExchangeReturnsOldValueAndOverwritesMemory) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @source()
      store atomic i32 %x, i32* %p seq_cst, align 4
      %old = atomicrmw xchg i32* %p, i32 0 seq_cst
      call void @sink(i32 %old)
      fence seq_cst
      %clean = load atomic i32, i32* %p seq_cst, align 4
      call void @sink(i32 %clean)
      %previous = atomicrmw add i32* %p, i32 %x seq_cst
      call void @sink(i32 %previous)
      %dirty = load i32, i32* %p
      call void @sink(i32 %dirty)
      ret i32 0
    }
  )",
                    0, true);
  EXPECT_EQ(result.violations, 2u);
}

TEST(TPATaint, AtomicCompareExchangeSeparatesOldValueFromReplacement) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %p = alloca i32
      store i32 0, i32* %p
      %x = call i32 @source()
      %pair = cmpxchg i32* %p, i32 0, i32 %x seq_cst seq_cst
      %old = extractvalue { i32, i1 } %pair, 0
      call void @sink(i32 %old)
      %new = load i32, i32* %p
      call void @sink(i32 %new)
      ret i32 0
    }
  )",
                    0, true);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, AtomicWritesFlowAcrossCallBoundaries) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define void @bump(i32* %p, i32 %x) {
      %old = atomicrmw add i32* %p, i32 %x seq_cst
      ret void
    }
    define i32 @main() {
      %p = alloca i32
      store i32 0, i32* %p
      %x = call i32 @source()
      call void @bump(i32* %p, i32 %x)
      %value = load i32, i32* %p
      call void @sink(i32 %value)
      ret i32 0
    }
  )",
                    1, true);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, IndirectBranchRetainsExplicitValueFlow) {
  auto result =
      run(R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @main() {
      %x = call i32 @source()
      indirectbr i8* blockaddress(@main, %target), [label %target]
    target:
      call void @sink(i32 %x)
      ret i32 0
    }
  )",
          0, false, false, tpa::taint::UnmodeledPolicy::Ignore, false);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, UnresolvedCallsPreserveMemoryAndPermitKnownFlows) {
  const char *ir = R"(
    declare i32 @source()
    declare void @sink(i32)
    define i32 @second(i32 %unused, i32 %x) { ret i32 %x }
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %p
      %ignored = call i32 undef(i32* %p)
      %continued = call i32 @second(i32 %ignored, i32 %x)
      call void @sink(i32 %continued)
      %preserved = load i32, i32* %p
      call void @sink(i32 %preserved)
      %asm = call i32 asm sideeffect "", "=r"()
      call void @sink(i32 %asm)
      ret i32 0
    }
  )";
  auto result = run(ir, 0, true, true);
  EXPECT_EQ(result.violations, 2u);
  EXPECT_EQ(result.unresolved, 2u);
  EXPECT_THROW(run(ir, 0, false, false, tpa::taint::UnmodeledPolicy::Error),
               std::invalid_argument);
}

TEST(TPATaint, MixedVariadicInputsPropagateValuesAndStringContents) {
  auto result = run(R"(
    @fmt = private constant [8 x i8] c"%d%s%Lf\00"
    declare i32 @source()
    declare void @fill(i8*)
    declare void @sink_mem(i8*)
    declare i32 @format(i8*, i8*, ...)
    define i32 @main() {
      %a = alloca i8
      %b = alloca i8
      %c = alloca i8
      %d = alloca i8
      %dirty_string = alloca i8
      %clean_string = alloca i8
      store i8 0, i8* %a
      store i8 0, i8* %b
      store i8 0, i8* %c
      store i8 0, i8* %d
      store i8 0, i8* %clean_string
      call void @fill(i8* %dirty_string)
      %x = call i32 @source()
      %fp = sitofp i32 %x to double
      %wide = fpext double %fp to x86_fp80
      %f = getelementptr [8 x i8], [8 x i8]* @fmt, i64 0, i64 0
      %r1 = call i32 (i8*, i8*, ...) @format(i8* %a, i8* %f, i32 %x, double 1.000000e+00, i8* %clean_string)
      %r2 = call i32 (i8*, i8*, ...) @format(i8* %b, i8* %f, i32 0, i8* %dirty_string)
      %r3 = call i32 (i8*, i8*, ...) @format(i8* %c, i8* %f, x86_fp80 %wide, i8* %clean_string)
      %r4 = call i32 (i8*, i8*, ...) @format(i8* %d, i8* %f, i32 0, double 1.000000e+00, i8* %clean_string)
      call void @sink_mem(i8* %a)
      call void @sink_mem(i8* %b)
      call void @sink_mem(i8* %c)
      call void @sink_mem(i8* %d)
      ret i32 0
    }
  )",
                    0, false, false, tpa::taint::UnmodeledPolicy::Error);
  EXPECT_EQ(result.violations, 3u);
}

TEST(TPATaint, VariadicStringReadsFlowThroughInternalWrappers) {
  auto result = run(R"(
    @fmt = private constant [3 x i8] c"%s\00"
    declare void @fill(i8*)
    declare void @sink_mem(i8*)
    declare i32 @format(i8*, i8*, ...)
    define void @wrapper(i8* %out, i8* %input) {
      %fmt = getelementptr [3 x i8], [3 x i8]* @fmt, i64 0, i64 0
      %r = call i32 (i8*, i8*, ...) @format(i8* %out, i8* %fmt, i32 0, i8* %input)
      ret void
    }
    define i32 @main() {
      %input = alloca i8
      %output = alloca i8
      store i8 0, i8* %output
      call void @fill(i8* %input)
      call void @wrapper(i8* %output, i8* %input)
      call void @sink_mem(i8* %output)
      ret i32 0
    }
  )",
                    1, false, false, tpa::taint::UnmodeledPolicy::Error);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, UniversalCallTargetsExcludeIncompatibleAllocator) {
  auto result = run(R"(
    @allocator_address = global i8* (i64)* @malloc
    @comparator_address = global i32 (i8*, i8*)* @compare
    @unknown_callback = external global i32 (i8*, i8*)*
    declare i8* @malloc(i64)
    declare i32 @source()
    declare void @sink(i32)
    define i32 @compare(i8* %a, i8* %b) {
      %p = bitcast i8* %a to i32*
      %x = load i32, i32* %p
      ret i32 %x
    }
    define i32 @main() {
      %p = alloca i32
      %x = call i32 @source()
      store i32 %x, i32* %p
      %a = bitcast i32* %p to i8*
      %callback = load i32 (i8*, i8*)*, i32 (i8*, i8*)** @unknown_callback
      %result = call i32 %callback(i8* %a, i8* %a)
      call void @sink(i32 %result)
      ret i32 0
    }
  )",
                    1, false, false, tpa::taint::UnmodeledPolicy::Error);
  EXPECT_EQ(result.violations, 1u);
  EXPECT_EQ(result.unresolved, 0u);
}

TEST(TPATaint,
     CallbackCompatibilityHandlesVarargsPointersAndCallingConvention) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(R"(
    declare i32 @typed_pointer(i32*)
    declare i32 @variadic(i8*, ...)
    declare i8* @wrong_return(i8*)
    declare i32 @wrong_argument(i64)
    declare i32 @wrong_arity(i8*, i8*)
    declare fastcc i32 @wrong_convention(i8*)
    define i32 @main() {
      %one = call i32 undef(i8* null)
      %many = call i32 (i8*, ...) undef(i8* null, i32 42)
      ret i32 0
    }
  )",
                                          error, context);
  ASSERT_NE(module, nullptr);
  const auto *one =
      llvm::cast<llvm::CallBase>(&module->getFunction("main")->front().front());
  const auto *many = llvm::cast<llvm::CallBase>(one->getNextNode());
  EXPECT_TRUE(
      tpa::compatibleCallTarget(*one, *module->getFunction("typed_pointer")));
  EXPECT_TRUE(
      tpa::compatibleCallTarget(*many, *module->getFunction("variadic")));
  EXPECT_FALSE(
      tpa::compatibleCallTarget(*many, *module->getFunction("typed_pointer")));
  for (const auto *name :
       {"wrong_return", "wrong_argument", "wrong_arity", "wrong_convention"})
    EXPECT_FALSE(tpa::compatibleCallTarget(*one, *module->getFunction(name)));
}

} // namespace
