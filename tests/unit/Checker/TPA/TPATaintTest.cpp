#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"
#include "Checker/TPA/FrontEnd/DefUseModuleBuilder.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>

namespace {
struct Result {
  size_t violations = 0;
  size_t losses = 0;
  size_t ambiguous = 0;
  tpa::taint::TaintAnalysis::Statistics stats;
};

Result run(llvm::StringRef ir, unsigned k = 0, bool track = false,
           bool repeat = false) {
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
  context::KLimitContext::setLimit(k);
  context::setContextStrategy(context::ContextStrategy::KLimit);
  tpa::taint::prepareModule(*module);
  tpa::SemiSparseProgramBuilder pb;
  auto program = pb.runOnModule(*module);
  tpa::SemiSparsePointerAnalysis pointer;
  pointer.loadExternalPointerTable(TPA_TAINT_TEST_INPUTS "/ptr.spec");
  pointer.runOnProgram(program);
  tpa::taint::DefUseModuleBuilder builder(pointer);
  builder.loadExternalModRefTable(TPA_TAINT_TEST_INPUTS "/modref.spec");
  auto graph = builder.buildDefUseModule(*module);
  tpa::taint::TaintAnalysis analysis(pointer);
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
      %x = atomicrmw add i32* %p, i32 1 seq_cst
      ret i32 0
    }
  )",
                                          diagnostic, context);
  ASSERT_NE(module, nullptr);
  std::string text;
  llvm::raw_string_ostream errors(text);
  EXPECT_FALSE(tpa::taint::validateModule(*module, errors));
  EXPECT_NE(errors.str().find("atomicrmw"), std::string::npos);
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

} // namespace
