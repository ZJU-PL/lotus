#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/StorePruner.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/InclusionBased/TPA/Transforms/LowerGlobalCtor.h"
#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"
#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"
#include "Checker/TPA/FrontEnd/DefUseModuleBuilder.h"
#include "Checker/TPA/FrontEnd/ModRefModuleAnalysis.h"
#include "Checker/TPA/FrontEnd/ReachingDefModuleAnalysis.h"
#include "Checker/TPA/Program/DefUseModule.h"
#include "Checker/TPA/Program/Reachability.h"

#include <cstddef>
#include <map>
#include <set>
#include <tuple>

#include <gtest/gtest.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
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

TEST(TPATaint, ValidationDiagnosticsAreCappedWithoutPrintingValues) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  std::string ir = "define i32 @main() { %p = alloca i8\n";
  for (unsigned i = 0; i < 50; ++i)
    ir += "%v" + std::to_string(i) + " = va_arg i8* %p, i32\n";
  ir += "ret i32 0 }";
  auto module = llvm::parseAssemblyString(ir, diagnostic, context);
  ASSERT_NE(module, nullptr);
  std::string text;
  llvm::raw_string_ostream errors(text);
  EXPECT_FALSE(tpa::taint::validateModule(*module, errors));
  errors.flush();
  size_t count = 0;
  for (size_t pos = 0; (pos = text.find("Unsupported TPA taint instruction",
                                        pos)) != std::string::npos;
       ++pos)
    ++count;
  EXPECT_EQ(count, 20u);
  EXPECT_NE(
      text.find("30 additional unsupported instructions omitted (50 total)"),
      std::string::npos);
  EXPECT_EQ(text.find("%v"), std::string::npos);
}

TEST(TPATaint, InvokeAndLandingPadCarryExplicitTaint) {
  auto result = run(R"(
    declare i32 @source()
    declare void @sink(i32)
    declare i32 @__gxx_personality_v0(...)
    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
      %value = invoke i32 @source() to label %normal unwind label %unwind
    normal:
      invoke void @sink(i32 %value) to label %done unwind label %unwind
    done:
      ret i32 0
    unwind:
      %exception = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %exception
    }
  )",
                    1, true);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, ResumePropagatesPointerAndTaintMemoryToCaller) {
  auto result = run(R"(
    declare void @may_throw()
    declare void @sink(i32)
    declare i32 @__gxx_personality_v0(...)
    define void @throw_after_store(i32** %slot, i32* %value)
        personality i32 (...)* @__gxx_personality_v0 {
      store i32* %value, i32** %slot
      invoke void @may_throw() to label %never unwind label %cleanup
    never:
      unreachable
    cleanup:
      %exception = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %exception
    }
    define i32 @main(i32 %argc) personality i32 (...)* @__gxx_personality_v0 {
      %value = alloca i32
      %slot = alloca i32*
      store i32 %argc, i32* %value
      store i32* null, i32** %slot
      invoke void @throw_after_store(i32** %slot, i32* %value)
          to label %normal unwind label %handler
    normal:
      ret i32 0
    handler:
      %exception = landingpad { i8*, i32 } catch i8* null
      %pointer = load i32*, i32** %slot
      %loaded = load i32, i32* %pointer
      call void @sink(i32 %loaded)
      ret i32 0
    }
  )",
                    1, true);
  EXPECT_EQ(result.violations, 1u);
}

TEST(TPATaint, CallBrInlineAssemblyUsesUnresolvedPolicyAndKeepsSuccessors) {
  const char *ir = R"(
    declare void @sink(i32)
    define i32 @main(i32 %argc) {
      callbr void asm sideeffect "", "i"(i8* blockaddress(@main, %indirect))
          to label %normal [label %indirect]
    normal:
      ret i32 0
    indirect:
      call void @sink(i32 %argc)
      ret i32 0
    }
  )";
  auto result = run(ir, 0, true);
  EXPECT_EQ(result.violations, 1u);
  EXPECT_EQ(result.unresolved, 1u);
  EXPECT_THROW(run(ir, 0, false, false, tpa::taint::UnmodeledPolicy::Error),
               std::invalid_argument);
}

TEST(TPATaint, StorePruningMatchesReferenceClosureWithBackwardFieldEdges) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(R"(
    %S = type { i8*, i8*, i8*, i8* }
    @global = global i8* null
    declare void @callee(i8*)
    define i32 @main() {
      %block = alloca %S
      %a = alloca i8*
      %b = alloca i8*
      %dead = alloca i8*
      %root = bitcast %S* %block to i8*
      call void @callee(i8* %root)
      ret i32 0
    }
  )",
                                          error, context);
  ASSERT_NE(module, nullptr);
  std::map<std::string, const llvm::Instruction *> values;
  const llvm::CallBase *call = nullptr;
  for (const auto &inst : module->getFunction("main")->front()) {
    if (inst.hasName())
      values[inst.getName().str()] = &inst;
    if (auto *candidate = llvm::dyn_cast<llvm::CallBase>(&inst))
      call = candidate;
  }
  ASSERT_NE(call, nullptr);
  const auto *ctx = context::Context::getGlobalContext();
  tpa::MemoryManager memory;
  const auto *layout = tpa::TypeLayout::getTypeLayout(32, {}, {0, 8, 16, 24});
  const auto *pointerLayout = tpa::TypeLayout::getPointerTypeLayoutWithSize(8);
  const auto *base =
      memory.allocateStackMemory(ctx, values.at("block"), layout);
  const auto *f8 = memory.offsetMemory(base, 8);
  const auto *f16 = memory.offsetMemory(base, 16);
  const auto *f24 = memory.offsetMemory(base, 24);
  const auto *a =
      memory.allocateStackMemory(ctx, values.at("a"), pointerLayout);
  const auto *b =
      memory.allocateStackMemory(ctx, values.at("b"), pointerLayout);
  const auto *dead =
      memory.allocateStackMemory(ctx, values.at("dead"), pointerLayout);
  const auto *global = memory.allocateGlobalMemory(
      module->getGlobalVariable("global"), pointerLayout);
  tpa::Store store;
  auto bind = [&](const tpa::MemoryObject *from, const tpa::MemoryObject *to) {
    store.strongUpdate(from, tpa::PtsSet::getSingletonSet(to));
  };
  bind(base, a);
  bind(f8, b);
  bind(f16, base);
  bind(f24, tpa::MemoryManager::getNullObject());
  bind(a, tpa::MemoryManager::getNullObject());
  bind(b, f16);
  bind(dead, tpa::MemoryManager::getUniversalObject());
  bind(global, a);
  tpa::PointerManager pointers;
  const auto *root = pointers.getOrCreatePointer(ctx, values.at("root"));
  tpa::CFG cfg(*module->getFunction("main"));
  auto *node =
      cfg.create<tpa::CallCFGNode>(module->getFunction("callee"), call);
  node->addArgument(values.at("root"));
  for (const auto *start : {base, f8, f16, f24, a}) {
    tpa::Env env;
    env.weakUpdate(root, tpa::PtsSet::getSingletonSet(start));
    auto actual = tpa::StorePruner(env, pointers, memory)
                      .pruneStore(store, tpa::ProgramPoint(ctx, node));
    // Reference: ordinary graph reachability, with every structural edge
    // expanded independently. This deliberately has no block-frontier shortcut.
    std::unordered_set<const tpa::MemoryObject *> reachable;
    std::vector<const tpa::MemoryObject *> work{start, global};
    for (size_t next = 0; next < work.size(); ++next) {
      const auto *object = work[next];
      if (!reachable.insert(object).second)
        continue;
      for (const auto *field : memory.getReachablePointerObjects(object, false))
        work.push_back(field);
      for (const auto *target : store.lookup(object))
        work.push_back(target);
    }
    size_t expected = 0;
    for (const auto &entry : store) {
      if (reachable.count(entry.first)) {
        ++expected;
        EXPECT_EQ(actual.lookup(entry.first), entry.second);
      } else
        EXPECT_TRUE(actual.lookup(entry.first).empty());
    }
    EXPECT_EQ(actual.size(), expected);
    EXPECT_TRUE(actual.lookup(dead).empty());
  }
}

TEST(TPATaint, PointerValuedAssemblyOutputHasUnknownPointsToBinding) {
  llvm::LLVMContext llvmContext;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
    define i32 @main() {
      %pointer = call i8* asm sideeffect "", "=r"()
      ret i32 0
    }
  )",
                                          diagnostic, llvmContext);
  ASSERT_NE(module, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  const auto *value = &module->getFunction("main")->front().front();
  tpa::taint::prepareModule(*module);
  tpa::SemiSparseProgramBuilder builder;
  auto program = builder.runOnModule(*module);
  tpa::SemiSparsePointerAnalysis pointer;
  pointer.runOnProgram(program);
  auto pointsTo =
      pointer.getPtsSet(context::Context::getGlobalContext(), value);
  EXPECT_TRUE(pointsTo.has(tpa::MemoryManager::getUniversalObject()));
}

TEST(TPATaint,
     ConstructorLoweringRetainsEqualPrioritiesAndValidDebugLocations) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(R"(
    @llvm.global_ctors = appending global [3 x { i32, void ()*, i8* }] [
      { i32, void ()*, i8* } { i32 200, void ()* @a, i8* null },
      { i32, void ()*, i8* } { i32 100, void ()* @b, i8* null },
      { i32, void ()*, i8* } { i32 200, void ()* @c, i8* null }]
    define void @a() { ret void }
    define void @b() { ret void }
    define void @c() { ret void }
    define i32 @main() { ret i32 0 }
  )",
                                          error, context);
  ASSERT_NE(module, nullptr);
  llvm::DIBuilder debug(*module);
  auto *file = debug.createFile("constructors.cpp", ".");
  debug.createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus, file, "test", false,
                          "", 0);
  auto *type = debug.createSubroutineType(debug.getOrCreateTypeArray({}));
  for (const auto *name : {"main", "a", "b", "c"}) {
    auto *scope = debug.createFunction(file, name, name, file, 1, type, 1,
                                       llvm::DINode::FlagZero,
                                       llvm::DISubprogram::SPFlagDefinition);
    module->getFunction(name)->setSubprogram(scope);
  }
  debug.finalize();
  module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                        llvm::DEBUG_METADATA_VERSION);
  ASSERT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  llvm::ModuleAnalysisManager manager;
  transform::LowerGlobalCtorPass().run(*module, manager);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  std::vector<std::string> calls;
  for (const auto &inst : module->getFunction("main")->front())
    if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      calls.push_back(call->getCalledFunction()->getName().str());
      EXPECT_TRUE(static_cast<bool>(call->getDebugLoc()));
    }
  EXPECT_EQ(calls, (std::vector<std::string>{"b", "a", "c"}));
}

TEST(TPATaint, InputVerifierRecoversDebugOnlyButRejectsBrokenExecutableIR) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto makeModule = [&]() {
    auto module = llvm::parseAssemblyString(R"(
      define i32 @callee() { ret i32 0 }
      define i32 @main() { %result = call i32 @callee() ret i32 %result }
    )",
                                            diagnostic, context);
    llvm::DIBuilder debug(*module);
    auto *file = debug.createFile("debug.c", ".");
    debug.createCompileUnit(llvm::dwarf::DW_LANG_C99, file, "test", false, "",
                            0);
    auto *type = debug.createSubroutineType(debug.getOrCreateTypeArray({}));
    for (const auto *name : {"main", "callee"})
      module->getFunction(name)->setSubprogram(debug.createFunction(
          file, name, name, file, 1, type, 1, llvm::DINode::FlagZero,
          llvm::DISubprogram::SPFlagDefinition));
    debug.finalize();
    module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                          llvm::DEBUG_METADATA_VERSION);
    return module;
  };
  auto debugOnly = makeModule();
  bool recovered = false;
  std::string text;
  llvm::raw_string_ostream errors(text);
  EXPECT_TRUE(tpa::taint::verifyInputModule(*debugOnly, errors, recovered));
  EXPECT_TRUE(recovered);
  EXPECT_EQ(debugOnly->getFunction("main")->getSubprogram(), nullptr);
  EXPECT_FALSE(llvm::verifyModule(*debugOnly, &llvm::errs()));
  auto broken = makeModule();
  broken->getFunction("main")->front().getTerminator()->eraseFromParent();
  recovered = true;
  EXPECT_FALSE(tpa::taint::verifyInputModule(*broken, errors, recovered));
  EXPECT_FALSE(recovered);
  EXPECT_NE(broken->getFunction("main")->getSubprogram(), nullptr);
}

TEST(TPATaint, GroupedReachingDefMatchesReferencePerInstructionSolver) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
    @g0 = global i32 0
    @g1 = global i32 0
    @g2 = global i32 0
    @g3 = global i32 0
    @g4 = global i32 0

    define void @helper(i32 %v) {
      store i32 %v, i32* @g1
      store i32 %v, i32* @g2
      store i32 %v, i32* @g3
      store i32 %v, i32* @g4
      ret void
    }

    define i32 @worker(i1 %cond, i32 %n) {
    entry:
      %x = alloca i32
      %y = alloca i32
      store i32 1, i32* %x
      store i32 2, i32* %y
      store i32 3, i32* @g0
      br i1 %cond, label %then, label %else
    then:
      store i32 10, i32* %x
      call void @helper(i32 %n)
      br label %loop
    else:
      %p = select i1 %cond, i32* %x, i32* %y
      store i32 20, i32* %p
      br label %loop
    loop:
      %i = phi i32 [ 0, %then ], [ 0, %else ], [ %next, %loop ]
      %v0 = load i32, i32* @g0
      %v1 = load i32, i32* @g1
      %v2 = load i32, i32* @g2
      %vx = load i32, i32* %x
      %old = atomicrmw add i32* @g0, i32 1 seq_cst
      call void @helper(i32 %old)
      %next = add i32 %i, 1
      %again = icmp slt i32 %next, %n
      br i1 %again, label %loop, label %exit
    exit:
      %sum1 = add i32 %v0, %v1
      %sum2 = add i32 %v2, %vx
      %res = add i32 %sum1, %sum2
      ret i32 %res
    }

    define i32 @main(i32 %argc) {
      %c = icmp sgt i32 %argc, 1
      %r = call i32 @worker(i1 %c, i32 %argc)
      ret i32 %r
    }
  )",
                                          diagnostic, context);
  ASSERT_NE(module, nullptr);
  tpa::taint::prepareModule(*module);
  tpa::SemiSparseProgramBuilder pb;
  auto program = pb.runOnModule(*module);
  tpa::SemiSparsePointerAnalysis pointer;
  pointer.loadExternalPointerTable(TPA_TAINT_TEST_INPUTS "/ptr.spec");
  pointer.runOnProgram(program);

  auto modRefTable = annotation::ExternalModRefTable::loadFromFile(
      TPA_TAINT_TEST_INPUTS "/modref.spec");
  const auto policy = tpa::taint::UnmodeledPolicy::Ignore;
  auto moduleSummary =
      tpa::taint::ModRefModuleAnalysis(pointer, modRefTable, policy)
          .runOnModule(*module);

  tpa::taint::DefUseModule duModule(
      *module, tpa::taint::reachableFunctions(*module, pointer, policy));
  tpa::taint::ReachingDefModuleAnalysis rdAnalysis(pointer, moduleSummary,
                                                   modRefTable, policy);

  using Edge = std::tuple<const tpa::taint::DefUseInstruction *,
                          const tpa::MemoryObject *,
                          const tpa::taint::DefUseInstruction *>;

  for (auto &duFunc : duModule) {
    rdAnalysis.buildMemoryDefUseEdges(duFunc);
    const auto &func = duFunc.getFunction();
    auto refMap = rdAnalysis.runOnFunction(func);
    const auto &funcSummary = moduleSummary.getSummary(&func);
    auto *entryDu = duFunc.getEntryInst();

    std::set<Edge> expected;
    auto recordFromStore = [&](const tpa::taint::ReachingDefStore<llvm::Instruction> &st,
                               const tpa::MemoryObject *obj,
                               const tpa::taint::DefUseInstruction *dstDu) {
      const auto *defs = st.getReachingDefs(obj);
      if (!defs || defs->empty()) {
        expected.emplace(entryDu, obj, dstDu);
        return;
      }
      for (const auto *defInst : *defs) {
        const auto *srcDu =
            defInst ? duFunc.getDefUseInstruction(defInst) : entryDu;
        ASSERT_NE(srcDu, nullptr);
        expected.emplace(srcDu, obj, dstDu);
      }
    };

    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        const auto *dstDu = duFunc.getDefUseInstruction(&inst);
        if (!dstDu)
          continue;
        const auto &st = refMap.getReachingDefStore(&inst);
        if (auto *li = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          for (const auto *obj : pointer.getPtsSet(li->getPointerOperand()))
            recordFromStore(st, obj, dstDu);
        } else if (auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst)) {
          for (const auto *obj : pointer.getPtsSet(rmw->getPointerOperand()))
            recordFromStore(st, obj, dstDu);
        } else if (auto *cx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst)) {
          for (const auto *obj : pointer.getPtsSet(cx->getPointerOperand()))
            recordFromStore(st, obj, dstDu);
        } else if (auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          for (const auto *callee :
               tpa::taint::resolveCallTargets(pointer, *cb, policy)) {
            if (!callee->isDeclaration()) {
              const auto &csum = moduleSummary.getSummary(callee);
              for (const auto *obj : csum.mem_reads())
                recordFromStore(st, obj, dstDu);
              for (const auto *obj : csum.mem_writes())
                recordFromStore(st, obj, dstDu);
            }
          }
        } else if (llvm::isa<llvm::ReturnInst>(&inst) ||
                   llvm::isa<llvm::ResumeInst>(&inst)) {
          for (const auto *obj : funcSummary.mem_writes())
            recordFromStore(st, obj, dstDu);
        }
      }
    }

    std::set<Edge> actualFwd;
    auto collectFwd = [&](const tpa::taint::DefUseInstruction &src) {
      src.forEachMemSucc(
          [&](const tpa::MemoryObject *obj, const auto &succs) {
            for (const auto *dst : succs)
              actualFwd.emplace(&src, obj, dst);
          });
    };
    collectFwd(*entryDu);
    for (const auto &bb : func)
      for (const auto &inst : bb)
        if (const auto *node = duFunc.getDefUseInstruction(&inst))
          collectFwd(*node);
    EXPECT_EQ(actualFwd, expected) << "Function: " << func.getName().str();

    // Reverse edges built lazily via ensureMemPreds() must match forward edges.
    duFunc.ensureMemPreds();
    std::set<Edge> actualRev;
    auto collectRev = [&](const tpa::taint::DefUseInstruction &dst) {
      dst.forEachMemPred(
          [&](const tpa::MemoryObject *obj, const auto &preds) {
            for (const auto *src : preds)
              actualRev.emplace(src, obj, &dst);
          });
    };
    collectRev(*entryDu);
    for (const auto &bb : func)
      for (const auto &inst : bb)
        if (const auto *node = duFunc.getDefUseInstruction(&inst))
          collectRev(*node);
    EXPECT_EQ(actualRev, expected) << "Reverse edges in: " << func.getName().str();

    // Confirm equivalent globals (@g3, @g4) collapsed into a shared group in
    // @worker.
    if (func.getName() == "worker") {
      const auto &groups = duFunc.getMemoryGroups();
      EXPECT_LT(groups.groupLocs.size(), groups.locToGroup.size());
    }
  }
}

TEST(TPATaint, ModRefSCCBitvectorHandlesMutualRecursionAndLocalStackFiltering) {
  const char *ir = R"(
    @shared_g = global i32 0
    declare void @sink(i32)

    define void @f1(i32 %n) {
      %local_s = alloca i32
      store i32 %n, i32* %local_s
      %cmp = icmp sgt i32 %n, 0
      br i1 %cmp, label %go, label %done
    go:
      call void @f2(i32* %local_s, i32 %n)
      br label %done
    done:
      ret void
    }

    define void @f2(i32* %p, i32 %n) {
      %v = load i32, i32* %p
      call void @f3(i32* %p, i32 %v)
      ret void
    }

    define void @f3(i32* %p, i32 %v) {
      store i32 %v, i32* @shared_g
      %v2 = load i32, i32* %p
      %n1 = sub i32 %v2, 1
      %cmp = icmp sgt i32 %n1, 0
      br i1 %cmp, label %rec, label %done
    rec:
      call void @f1(i32 %n1)
      br label %done
    done:
      ret void
    }

    define i32 @main(i32 %argc) {
      call void @f1(i32 %argc)
      %out = load i32, i32* @shared_g
      call void @sink(i32 %out)
      ret i32 0
    }
  )";

  // 1. Check ModRef summaries directly on the 3-node SCC {f1, f2, f3}.
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(ir, diagnostic, context);
  ASSERT_NE(module, nullptr);
  tpa::taint::prepareModule(*module);
  tpa::SemiSparseProgramBuilder pb;
  auto program = pb.runOnModule(*module);
  tpa::SemiSparsePointerAnalysis pointer;
  pointer.loadExternalPointerTable(TPA_TAINT_TEST_INPUTS "/ptr.spec");
  pointer.runOnProgram(program);
  auto modRefTable = annotation::ExternalModRefTable::loadFromFile(
      TPA_TAINT_TEST_INPUTS "/modref.spec");
  auto summary = tpa::taint::ModRefModuleAnalysis(
                     pointer, modRefTable, tpa::taint::UnmodeledPolicy::Ignore)
                     .runOnModule(*module);

  const auto *f1 = module->getFunction("f1");
  const auto *f2 = module->getFunction("f2");
  const auto *f3 = module->getFunction("f3");
  const auto *mainFn = module->getFunction("main");
  const auto *localAlloca = &f1->front().front();
  const auto *gVar = module->getGlobalVariable("shared_g");

  auto localPts = pointer.getPtsSet(localAlloca);
  auto globalPts = pointer.getPtsSet(gVar);
  ASSERT_EQ(localPts.size(), 1u);
  ASSERT_EQ(globalPts.size(), 1u);
  const auto *localObj = *localPts.begin();
  const auto *globalObj = *globalPts.begin();

  // f2 and f3 read f1's stack object; f1 filters out its own local stack, so
  // neither f1 nor main exposes localObj in its ModRef summary.
  EXPECT_TRUE(summary.getSummary(f2).isMemoryRead(localObj));
  EXPECT_TRUE(summary.getSummary(f3).isMemoryRead(localObj));
  EXPECT_FALSE(summary.getSummary(f1).isMemoryRead(localObj));
  EXPECT_FALSE(summary.getSummary(mainFn).isMemoryRead(localObj));

  // Global write in f3 propagates across the entire SCC and up to main.
  EXPECT_TRUE(summary.getSummary(f3).isMemoryWritten(globalObj));
  EXPECT_TRUE(summary.getSummary(f2).isMemoryWritten(globalObj));
  EXPECT_TRUE(summary.getSummary(f1).isMemoryWritten(globalObj));
  EXPECT_TRUE(summary.getSummary(mainFn).isMemoryWritten(globalObj));

  // 2. End-to-end taint analysis must detect the tainted flow through the SCC.
  auto res = run(ir, 0, false);
  EXPECT_EQ(res.violations, 1u);
}

TEST(TPATaint, PrecisionLossTrackingWithGroupedDefUseAndVoidReturn) {
  auto res = run(R"(
    @g = global i32 0
    @g2 = global i32 0
    declare i32 @source()
    declare void @sink(i32)

    define void @writer(i32 %v) {
      store i32 %v, i32* @g2
      ret void
    }

    define i32 @reader() {
      %v = load i32, i32* @g
      ret i32 %v
    }

    define i32 @main() {
      %t = call i32 @source()
      call void @writer(i32 0)
      call void @writer(i32 %t)
      %void_smear = load i32, i32* @g2
      call void @sink(i32 %void_smear)
      store i32 0, i32* @g
      %clean = call i32 @reader()
      store i32 %t, i32* @g
      %dirty = call i32 @reader()
      call void @sink(i32 %clean)
      call void @sink(i32 %dirty)
      ret i32 0
    }
  )",
                 0, /*track=*/true, /*repeat=*/true);
  EXPECT_EQ(res.violations, 3u);
  EXPECT_EQ(res.ambiguous, 3u);
  EXPECT_GT(res.losses, 0u);
}

TEST(TPATaint, DeltaPropagationHandlesDynamicCallersAndIndirectCalls) {
  auto res = run(R"(
    @g1 = global i32 0
    @g2 = global i32 0
    declare i32 @source()
    declare void @sink(i32)

    define i32 @callee(i32 %x) {
      %v1 = load i32, i32* @g1
      store i32 %v1, i32* @g2
      %sum = add i32 %x, %v1
      ret i32 %sum
    }

    define i32 @main(i1 %cond) {
      %fp_slot = alloca i32 (i32)*
      store i32 (i32)* @callee, i32 (i32)** %fp_slot
      store i32 0, i32* @g1
      ; First call site with clean state (establishes initial broadcast sync)
      %r1 = call i32 @callee(i32 1)
      ; Mutate @g1 to tainted after initial sync, then invoke via indirect call
      %t = call i32 @source()
      store i32 %t, i32* @g1
      %fp = load i32 (i32)*, i32 (i32)** %fp_slot
      %r2 = call i32 %fp(i32 2)
      %g2_val = load i32, i32* @g2
      call void @sink(i32 %r2)
      call void @sink(i32 %g2_val)
      ret i32 0
    }
  )",
                 0, false, /*repeat=*/true);
  EXPECT_EQ(res.violations, 2u);
}

TEST(TPATaint, SuffixKCFASeparatesDeepCallSitesUnderOneCFA) {
  const char *ir = R"(
    declare i32 @source()
    declare void @sink(i32)

    define i32 @identity(i32 %x) {
      ret i32 %x
    }

    define void @wrapper(i32 %t) {
      %dirty = call i32 @identity(i32 %t)
      %clean = call i32 @identity(i32 0)
      call void @sink(i32 %dirty)
      call void @sink(i32 %clean)
      ret void
    }

    define i32 @main() {
      %t = call i32 @source()
      call void @wrapper(i32 %t)
      ret i32 0
    }
  )";
  // 0-CFA merges both calls to @identity inside @wrapper.
  auto k0 = run(ir, 0, false);
  EXPECT_EQ(k0.violations, 2u);
  EXPECT_EQ(k0.ambiguous, 2u);

  // Standard suffix 1-CFA distinguishes the two depth-2 call sites to
  // @identity inside @wrapper, eliminating the false positive on %clean.
  auto k1 = run(ir, 1, false);
  EXPECT_EQ(k1.violations, 1u);
  EXPECT_EQ(k1.ambiguous, 0u);
}

} // namespace
