/**
 * @file KINTPrecisionAndRecallTest.cpp
 * @brief Regression tests for KINT crash fixes, precision, and Incorrectness-Logic recall.
 */

#include "KINTCheckerFixture.h"

#include <llvm/IR/Metadata.h>

using namespace llvm;
using namespace kint_test;

namespace {

struct KintOptionScope {
  bool overflow;
  bool divZero;
  bool badShift;
  bool arrayOOB;
  bool deadBranch;
  bool analyzeAll;
  kint::SummaryMode summaryMode;
  unsigned maxLoopUnroll;

  KintOptionScope()
      : overflow(kint::CheckIntOverflow), divZero(kint::CheckDivByZero),
        badShift(kint::CheckBadShift), arrayOOB(kint::CheckArrayOOB),
        deadBranch(kint::CheckDeadBranch),
        analyzeAll(kint::AnalyzeAllFunctions.getValue()),
        summaryMode(kint::InterprocSummaryMode.getValue()),
        maxLoopUnroll(kint::MaxLoopUnroll.getValue()) {
    kint::CheckIntOverflow = false;
    kint::CheckDivByZero = false;
    kint::CheckBadShift = false;
    kint::CheckArrayOOB = false;
    kint::CheckDeadBranch = false;
    kint::AnalyzeAllFunctions = true;
    kint::InterprocSummaryMode = kint::SummaryMode::On;
    kint::MaxLoopUnroll = 3;
  }

  ~KintOptionScope() {
    kint::CheckIntOverflow = overflow;
    kint::CheckDivByZero = divZero;
    kint::CheckBadShift = badShift;
    kint::CheckArrayOOB = arrayOOB;
    kint::CheckDeadBranch = deadBranch;
    kint::AnalyzeAllFunctions = analyzeAll;
    kint::InterprocSummaryMode = summaryMode;
    kint::MaxLoopUnroll = maxLoopUnroll;
  }
};

bool hasAnyKintError(const Function &F, StringRef substr = "") {
  for (const Instruction &I : instructions(F)) {
    if (const MDNode *md = I.getMetadata("mkint.err")) {
      if (substr.empty())
        return true;
      if (md->getNumOperands() > 0) {
        if (const auto *ms = dyn_cast<MDString>(md->getOperand(0))) {
          if (ms->getString().contains(substr))
            return true;
        }
      }
    }
  }
  return false;
}

} // namespace

TEST_F(KINTCheckerTest, BadShiftUsesUnsignedComparison) {
  const char *source = R"(
    define i32 @neg_shift(i32 %x) {
    entry:
      %r = shl i32 %x, -1
      ret i32 %r
    }

    define i1 @valid_i1_shift(i1 %x) {
    entry:
      %r = shl i1 %x, 0
      ret i1 %r
    }
  )";

  KintOptionScope opts;
  kint::CheckBadShift = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_TRUE(hasAnyKintError(*module->getFunction("neg_shift"), "bad shift"));
  EXPECT_FALSE(
      hasAnyKintError(*module->getFunction("valid_i1_shift"), "bad shift"));
}

TEST_F(KINTCheckerTest, OverflowRespectsSignedAndUnsignedFlags) {
  const char *source = R"(
    define i32 @signed_well_defined() {
    entry:
      %a = add nsw i32 -1, 1
      %b = sub nsw i32 1, 2
      %c = mul nsw i32 -1, 2
      %r = add nsw i32 %a, %b
      %r2 = add nsw i32 %r, %c
      ret i32 %r2
    }

    define i32 @signed_real_overflow() {
    entry:
      %a = add nsw i32 2147483647, 1
      ret i32 %a
    }

    define i64 @unsigned_real_underflow() {
    entry:
      %a = sub i64 0, 2
      ret i64 %a
    }
  )";

  KintOptionScope opts;
  kint::CheckIntOverflow = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_FALSE(hasAnyKintError(*module->getFunction("signed_well_defined"),
                               "integer overflow"));
  EXPECT_TRUE(hasAnyKintError(*module->getFunction("signed_real_overflow"),
                              "integer overflow"));
  EXPECT_TRUE(hasAnyKintError(*module->getFunction("unsigned_real_underflow"),
                              "integer overflow"));
}

TEST_F(KINTCheckerTest, DeadBranchAggregatesFeasibilityAcrossPaths) {
  const char *source = R"(
    define i32 @path_dependent(i1 %flag) {
    entry:
      br i1 %flag, label %p1, label %p2
    p1:
      br label %merge
    p2:
      br label %merge
    merge:
      %x = phi i32 [ 0, %p1 ], [ 10, %p2 ]
      %cmp = icmp sgt i32 %x, 5
      br i1 %cmp, label %hi, label %lo
    hi:
      ret i32 1
    lo:
      ret i32 2
    }

    define i32 @truly_dead(i32 %x) {
    entry:
      %gt = icmp sgt i32 %x, 10
      br i1 %gt, label %then, label %done
    then:
      %lt = icmp slt i32 %x, 0
      br i1 %lt, label %dead, label %done
    dead:
      ret i32 99
    done:
      ret i32 0
    }
  )";

  KintOptionScope opts;
  kint::CheckDeadBranch = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_FALSE(hasAnyKintError(*module->getFunction("path_dependent"),
                               "impossible"));
  EXPECT_TRUE(hasAnyKintError(*module->getFunction("truly_dead"),
                              "impossible true branch"));
}

TEST_F(KINTCheckerTest, SwitchMultiCaseDisjunctionFindsBugInSecondCase) {
  const char *source = R"(
    define i32 @switch_multi() {
    entry:
      %x = add i32 1, 1
      switch i32 %x, label %def [
        i32 1, label %target
        i32 2, label %target
      ]
    target:
      %d = sub i32 %x, 2
      %q = sdiv i32 10, %d
      ret i32 %q
    def:
      ret i32 0
    }
  )";

  KintOptionScope opts;
  kint::CheckDivByZero = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_TRUE(
      hasAnyKintError(*module->getFunction("switch_multi"), "divide by zero"));
}

TEST_F(KINTCheckerTest, ArrayOOBSkipsUnknownParamSizeAndCatchesMemcpyOverRead) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @param_deref(i32* %p) {
    entry:
      %q = getelementptr inbounds i32, i32* %p, i64 0
      %v = load i32, i32* %q
      ret i32 %v
    }

    define void @memcpy_overread() {
    entry:
      %dst = alloca [16 x i8]
      %src = alloca [4 x i8]
      %d = bitcast [16 x i8]* %dst to i8*
      %s = bitcast [4 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %d, i8* %s, i64 16, i1 false)
      ret void
    }
  )";

  KintOptionScope opts;
  kint::CheckArrayOOB = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_FALSE(hasAnyKintError(*module->getFunction("param_deref"),
                               "array index out of bound"));
  EXPECT_TRUE(hasAnyKintError(*module->getFunction("memcpy_overread"),
                              "array index out of bound"));
}

TEST_F(KINTCheckerTest, LLVMIntegerIntrinsicsEliminateClampedFalsePositives) {
  const char *source = R"(
    declare i32 @llvm.smax.i32(i32, i32)
    declare i32 @llvm.umin.i32(i32, i32)

    define i32 @clamped_ops(i32 %x, i32 %s) {
    entry:
      %d = call i32 @llvm.smax.i32(i32 %x, i32 1)
      %q = sdiv i32 100, %d
      %k = call i32 @llvm.umin.i32(i32 %s, i32 31)
      %sh = shl i32 %q, %k
      ret i32 %sh
    }
  )";

  KintOptionScope opts;
  kint::CheckDivByZero = true;
  kint::CheckBadShift = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_FALSE(hasAnyKintError(*module->getFunction("clamped_ops")));
}

TEST_F(KINTCheckerTest, ConstantGlobalInitAndCallocZeroInit) {
  const char *source = R"(
    @divisor = constant i32 7
    declare i8* @calloc(i64, i64)

    define i32 @const_global_div(i32 %x) {
    entry:
      %d = load i32, i32* @divisor
      %q = sdiv i32 %x, %d
      ret i32 %q
    }

    define i32 @calloc_zero_div(i32 %x) {
    entry:
      %raw = call i8* @calloc(i64 1, i64 4)
      %p = bitcast i8* %raw to i32*
      %v = load i32, i32* %p
      %q = sdiv i32 %x, %v
      ret i32 %q
    }
  )";

  KintOptionScope opts;
  kint::CheckDivByZero = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_FALSE(hasAnyKintError(*module->getFunction("const_global_div"),
                               "divide by zero"));
  EXPECT_TRUE(hasAnyKintError(*module->getFunction("calloc_zero_div"),
                              "divide by zero"));
}

TEST_F(KINTCheckerTest, BoundedLoopUnrollingFindsPostLoopBugWithoutDeadBranchFP) {
  const char *source = R"(
    declare i32 @close(i32)
    declare i32 @open64(i8*, i32, ...)
    declare i32 @setsockopt(i32, i32, i32, i8*, i32)
    @.path = private unnamed_addr constant [10 x i8] c"/dev/null\00"

    define i32 @main() {
    entry:
      %i = alloca i32, align 4
      store i32 1, i32* %i, align 4
      %ip = bitcast i32* %i to i8*
      %s = call i32 @setsockopt(i32 0, i32 1, i32 2, i8* %ip, i32 4)
      store i32 0, i32* %i, align 4
      br label %loop.hdr

    loop.hdr:
      %cur = load i32, i32* %i, align 4
      %cond = icmp slt i32 %cur, 3
      br i1 %cond, label %loop.body, label %loop.exit

    loop.body:
      %c1 = call i32 @close(i32 %cur)
      %p = getelementptr inbounds [10 x i8], [10 x i8]* @.path, i64 0, i64 0
      %c2 = call i32 (i8*, i32, ...) @open64(i8* %p, i32 2)
      %cur2 = load i32, i32* %i, align 4
      %next = add nsw i32 %cur2, 1
      store i32 %next, i32* %i, align 4
      br label %loop.hdr

    loop.exit:
      %final = load i32, i32* %i, align 4
      %d = sub nsw i32 %final, 3
      %q = sdiv i32 42, %d
      ret i32 %q
    }
  )";

  KintOptionScope opts;
  kint::CheckIntOverflow = true;
  kint::CheckDivByZero = true;
  kint::CheckDeadBranch = true;
  kint::MaxLoopUnroll = 3;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  EXPECT_FALSE(hasAnyKintError(*mainFn, "integer overflow"));
  EXPECT_FALSE(hasAnyKintError(*mainFn, "impossible"));
  EXPECT_TRUE(hasAnyKintError(*mainFn, "divide by zero"));
}

TEST_F(KINTCheckerTest, NonIntStoreHavocsStaleUnionMemory) {
  const char *source = R"(
    define i32 @union_store(float %f) {
    entry:
      %u = alloca i32, align 4
      store i32 1, i32* %u, align 4
      %fp = bitcast i32* %u to float*
      store float %f, float* %fp, align 4
      %v = load i32, i32* %u, align 4
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  KintOptionScope opts;
  kint::CheckDivByZero = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  EXPECT_TRUE(
      hasAnyKintError(*module->getFunction("union_store"), "divide by zero"));
}

TEST_F(KINTCheckerTest, CrashRegressionsOpaqueGlobalCastSortsCyclicPhiShortSink) {
  const char *source = R"(
    %struct.opaque = type opaque
    @ext = external global %struct.opaque
    declare void @memcpy(i8*)

    define i32* @cyclic_phi_ret(i32* %p, i1 %c) {
    entry:
      br label %loop
    loop:
      %q = phi i32* [ %p, %entry ], [ %q, %loop ]
      br i1 %c, label %loop, label %exit
    exit:
      ret i32* %q
    }

    define i64 @main(double %d, i32* %p, i1 %c) {
    entry:
      %a = fptosi double %d to i32
      %b = fptosi double %d to i64
      %r = call i32* @cyclic_phi_ret(i32* %p, i1 %c)
      %g = bitcast %struct.opaque* @ext to i8*
      call void @memcpy(i8* %g)
      %ext_a = sext i32 %a to i64
      %sum = add i64 %ext_a, %b
      ret i64 %sum
    }
  )";

  KintOptionScope opts;
  kint::CheckIntOverflow = true;
  kint::CheckDivByZero = true;
  kint::CheckBadShift = true;
  kint::CheckArrayOOB = true;
  kint::CheckDeadBranch = true;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);

  // Verify TaintAnalysis::mark_func_sinks doesn't crash on short-arity memcpy.
  kint::TaintAnalysis taint;
  SetVector<StringRef> callbacks;
  taint.mark_func_sinks(*module->getFunction("main"), callbacks);
  SUCCEED();
}
