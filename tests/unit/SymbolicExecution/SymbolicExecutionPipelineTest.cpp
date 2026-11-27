#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Checker/Framework/BugReportMgr.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowBuilder.h"
#include "IR/GVFG/LotusAAWrapper.h"
#include "SymbolicExecution/Integration/SymbolicExecutionWrapper.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>

using namespace llvm;
using namespace lotus::unittest;

namespace {

void initializePassInfra() {
  static bool initialized = false;
  if (initialized)
    return;
  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  initialized = true;
}

class SymbolicExecutionPipelineTest : public LlvmModuleTest {
protected:
  void runSymex(unsigned bugTypes) {
    initializePassInfra();
    BugReportMgr::get_instance().clear_all_reports();

    legacy::PassManager PM;
    PM.add(new gsa::ControlDependenceAnalysisPass());
    PM.add(new gsa::GateAnalysisPass());
    PM.add(new LotusAA());
    PM.add(new lotus::gvfg::GuardedValueFlowGraphBuilderPass());
    PM.add(new lotus::gvfg::LotusAAWrapper());
    PM.add(new SymbolicExecution::SymbolicExecutionWrapper(bugTypes));
    PM.run(*module);
  }

  size_t countReportsFor(StringRef bugTypeName) {
    auto &mgr = BugReportMgr::get_instance();
    int id = mgr.find_bug_type(bugTypeName);
    if (id < 0)
      return 0;
    const auto *reports = mgr.get_reports_for_type(id);
    return reports ? reports->size() : 0;
  }
};

TEST_F(SymbolicExecutionPipelineTest, DetectsConcreteBufferOverflow) {
  ASSERT_TRUE(loadModule(R"(
    define i8 @bof_concrete() {
    entry:
      %buf = alloca [4 x i8], align 1
      %ptr = getelementptr inbounds [4 x i8], [4 x i8]* %buf, i64 0, i64 8
      %val = load i8, i8* %ptr, align 1
      ret i8 %val
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_BOF);
  EXPECT_GE(countReportsFor("Symbolic Execution Buffer Overflow"), 1u);
}

TEST_F(SymbolicExecutionPipelineTest, SafeBufferAccessReportsNoBof) {
  ASSERT_TRUE(loadModule(R"(
    define i8 @bof_safe() {
    entry:
      %buf = alloca [8 x i8], align 1
      %ptr = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 2
      %val = load i8, i8* %ptr, align 1
      ret i8 %val
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_BOF);
  EXPECT_EQ(countReportsFor("Symbolic Execution Buffer Overflow"), 0u);
}

TEST_F(SymbolicExecutionPipelineTest, DetectsInterproceduralBufferOverflow) {
  ASSERT_TRUE(loadModule(R"(
    declare i8* @malloc(i64)

    define i8 @read_at(i8* %p, i64 %idx) {
    entry:
      %elem = getelementptr inbounds i8, i8* %p, i64 %idx
      %v = load i8, i8* %elem, align 1
      ret i8 %v
    }

    define i8 @caller() {
    entry:
      %buf = call i8* @malloc(i64 4)
      %v = call i8 @read_at(i8* %buf, i64 10)
      ret i8 %v
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_BOF);
  EXPECT_GE(countReportsFor("Symbolic Execution Buffer Overflow"), 1u);
}

TEST_F(SymbolicExecutionPipelineTest, DetectsConcreteDivideByZero) {
  ASSERT_TRUE(loadModule(R"(
    define i32 @dbz_concrete(i32 %x) {
    entry:
      %q = sdiv i32 %x, 0
      ret i32 %q
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_DBZ);
  EXPECT_GE(countReportsFor("Symbolic Execution Divide by Zero"), 1u);
}

TEST_F(SymbolicExecutionPipelineTest, DetectsInterproceduralDivideByZero) {
  ASSERT_TRUE(loadModule(R"(
    define i32 @div_helper(i32 %a, i32 %b) {
    entry:
      %q = sdiv i32 %a, %b
      ret i32 %q
    }

    define i32 @caller(i32 %a) {
    entry:
      %r = call i32 @div_helper(i32 %a, i32 0)
      ret i32 %r
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_DBZ);
  EXPECT_GE(countReportsFor("Symbolic Execution Divide by Zero"), 1u);
}

TEST_F(SymbolicExecutionPipelineTest, DetectsDoubleFreeAndUseAfterFree) {
  ASSERT_TRUE(loadModule(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @uaf_and_df() {
    entry:
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %v = load i8, i8* %p, align 1
      call void @free(i8* %p)
      ret i8 %v
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_UAF |
           SymbolicExecution::BUG_TY_DOUBLE_FREE);
  EXPECT_GE(countReportsFor("Symbolic Execution Use After Free"), 1u);
  EXPECT_GE(countReportsFor("Symbolic Execution Double Free"), 1u);
}

TEST_F(SymbolicExecutionPipelineTest,
       HandlesLoopLatchPlacedBeforeHeaderWithoutOutOfRange) {
  // Regression test: when a loop latch precedes its dominating header in IR
  // block order, RPO traversal must still visit the header before the latch.
  ASSERT_TRUE(loadModule(R"(
    define i32 @loop_latch_first(i32 %n) {
    entry:
      %ok = icmp sgt i32 %n, 0
      br i1 %ok, label %header, label %exit

    latch:
      %next = add nsw i32 %i, 1
      %cond = icmp slt i32 %next, %n
      br i1 %cond, label %header, label %exit

    header:
      %i = phi i32 [ 0, %entry ], [ %next, %latch ]
      %sum = add nsw i32 %i, 2
      %done = icmp eq i32 %sum, 100
      br i1 %done, label %exit, label %latch

    exit:
      ret i32 0
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_BOF | SymbolicExecution::BUG_TY_DBZ |
           SymbolicExecution::BUG_TY_INT_OVERFLOW |
           SymbolicExecution::BUG_TY_INT_UNDERFLOW);
  EXPECT_EQ(countReportsFor("Symbolic Execution Divide by Zero"), 0u);
}

TEST_F(SymbolicExecutionPipelineTest,
       SignedIntOverflowUsesActualBitWidthBounds) {
  ASSERT_TRUE(loadModule(R"(
    define i32 @safe_small_add() {
    entry:
      %r = add i32 1, 2
      ret i32 %r
    }

    define i32 @overflow_i32_add() {
    entry:
      %r = add i32 2147483647, 1
      ret i32 %r
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_INT_OVERFLOW);
  EXPECT_EQ(countReportsFor("Symbolic Execution Integer Overflow"), 1u);
}

TEST_F(SymbolicExecutionPipelineTest,
       HandlesMalformedLibCallAndVectorBinarySafely) {
  ASSERT_TRUE(loadModule(R"(
    declare void @my_custom_strlen()

    define <2 x i32> @vec_and_custom_lib(<2 x i32> %a, <2 x i32> %b) {
    entry:
      call void @my_custom_strlen()
      %s = add <2 x i32> %a, %b
      ret <2 x i32> %s
    }
  )"));

  runSymex(SymbolicExecution::BUG_TY_BOF | SymbolicExecution::BUG_TY_DBZ |
           SymbolicExecution::BUG_TY_INT_OVERFLOW |
           SymbolicExecution::BUG_TY_INT_TRUNCATION);
  EXPECT_EQ(countReportsFor("Symbolic Execution Buffer Overflow"), 0u);
}

} // namespace
