#include "AECheckerTestSupport.h"

#include <array>

#include <gtest/gtest.h>

using namespace lotus::analysis;

class AEInstructionParityTest : public AECheckerTest {
protected:
  using LayoutResult = std::pair<StateLayout, AEResult>;

  std::array<AEResult, 3> runLayouts(const char *source) {
    constexpr std::array<StateLayout, 3> layouts = {
        StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse};
    std::array<AEResult, 3> results{};
    for (size_t i = 0; i < layouts.size(); ++i) {
      auto module = parseModule(source);
      EXPECT_NE(module, nullptr);
      if (module)
        results[i] = runAE(module.get(), true, false,
                           AbstractInterpretation::WIDEN_NARROW, 3u, false,
                           false, layouts[i]);
    }
    return results;
  }
};

TEST_F(AEInstructionParityTest, FloatingPointAddRefinesArrayIndex) {
  const char *source = R"(
    define i32 @main(i32 %argc) {
    entry:
      %nonzero = icmp ne i32 %argc, 0
      %base = select i1 %nonzero, double 1.0, double 2.0
      %sum = fadd double %base, 1.0
      %idx = fptosi double %sum to i32
      %array = alloca [4 x i32]
      %safe = icmp slt i32 %idx, 4
      br i1 %safe, label %exit, label %overflow

    overflow:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %idx
      store i32 1, i32* %ptr
      br label %exit

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, FloatingPointNegationRefinesArrayIndex) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %value = fneg double 2.0
      %index = fptosi double %value to i32
      %array = alloca [4 x i32]
      %nonnegative = icmp sge i32 %index, 0
      br i1 %nonnegative, label %dead, label %exit

    dead:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SignExtendingI1ProducesNegativeIndex) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      %flag = icmp eq i32 1, 1
      %index = sext i1 %flag to i32
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, TruncatedIntervalPreservesWraparound) {
  const char *source = R"(
    define i32 @main(i32 %value) {
    entry:
      %array = alloca [4 x i32]
      %nonnegative = icmp sge i32 %value, 0
      br i1 %nonnegative, label %check.upper, label %exit

    check.upper:
      %bounded = icmp sle i32 %value, 256
      br i1 %bounded, label %body, label %exit

    body:
      %truncated = trunc i32 %value to i8
      %index = sext i8 %truncated to i32
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, NarrowIntegerWrapKeepsDynamicIndexOutOfBounds) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %sum = add i8 127, 1
      %index = sext i8 %sum to i32
      %array = alloca [4 x i32]
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, UnsignedComparisonKeepsNegativeValuesLive) {
  const char *source = R"(
    define i32 @main(i1 %choose) {
    entry:
      %array = alloca [4 x i32]
      %value = select i1 %choose, i32 -1, i32 -2
      %positive = icmp ugt i32 %value, 0
      br i1 %positive, label %body, label %exit

    body:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %value
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, UnsignedUpperBoundRefinesUnknownValue) {
  const char *source = R"(
    define i32 @main(i32 %index) {
    entry:
      %array = alloca [4 x i32]
      %inBounds = icmp ult i32 %index, 4
      br i1 %inBounds, label %body, label %exit

    body:
      %wide = zext i32 %index to i64
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 %wide
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, UnsignedComparisonValueUsesUnsignedOrder) {
  const char *source = R"(
    define i32 @main(i1 %choose) {
    entry:
      %array = alloca [4 x i32]
      %value = select i1 %choose, i32 -1, i32 -2
      %flag = icmp ugt i32 %value, 0
      %wide = zext i1 %flag to i32
      %nonzero = icmp ne i32 %wide, 0
      br i1 %nonzero, label %body, label %exit

    body:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i32 %value
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, AddrSpaceCastPreservesObjectIdentity) {
  const char *source = R"(
    @array = addrspace(1) global [4 x i32] zeroinitializer

    define i32 @main() {
    entry:
      %base = addrspacecast [4 x i32] addrspace(1)* @array to i32*
      %ptr = getelementptr inbounds i32, i32* %base, i64 5
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, FreezeCopiesPointerValue) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %slot = alloca i32
      store i32 7, i32* %slot
      %frozen = freeze i32* %slot
      %value = load i32, i32* %frozen
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SelectMaterializesConstantArms) {
  const char *source = R"(
    define i32 @main(i32 %argc) {
    entry:
      %nonzero = icmp ne i32 %argc, 0
      %idx = select i1 %nonzero, i32 0, i32 3
      %array = alloca [2 x i32]
      %safe = icmp slt i32 %idx, 2
      br i1 %safe, label %exit, label %overflow

    overflow:
      %ptr = getelementptr inbounds [2 x i32], [2 x i32]* %array, i64 0, i32 %idx
      store i32 1, i32* %ptr
      br label %exit

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SelectCopiesPointerArms) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %a = alloca i32
      %b = alloca i32
      store i32 1, i32* %a
      store i32 2, i32* %b
      %ptr = select i1 true, i32* %a, i32* %b
      %value = load i32, i32* %ptr
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SelectJoinsNullablePointerArm) {
  const char *source = R"(
    define i32 @main(i1 %chooseNull) {
    entry:
      %slot = alloca i32
      %ptr = select i1 %chooseNull, i32* null, i32* %slot
      %value = load i32, i32* %ptr
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ExtractValueResolvesConstantAggregateField) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %ptr = extractvalue { i32* } zeroinitializer, 0
      %value = load i32, i32* %ptr
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, InsertValueKeepsOtherAggregateFields) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %aggregate = insertvalue { i32, i32 } { i32 0, i32 0 }, i32 3, 1
      %index = extractvalue { i32, i32 } %aggregate, 0
      %array = alloca [2 x i32]
      %safe = icmp slt i32 %index, 2
      br i1 %safe, label %exit, label %overflow

    overflow:
      %ptr = getelementptr inbounds [2 x i32], [2 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StructMemberArrayRespectsSubobjectBound) {
  const char *source = R"(
    %struct.S = type { [4 x i32], i32 }

    define i32 @main() {
    entry:
      %aggregate = alloca %struct.S
      %member = getelementptr inbounds %struct.S, %struct.S* %aggregate, i64 0, i32 0
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %member, i64 0, i64 4
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StructMemberArrayKeepsValidIndexSafe) {
  const char *source = R"(
    %struct.S = type { [4 x i32], i32 }

    define i32 @main() {
    entry:
      %aggregate = alloca %struct.S
      %member = getelementptr inbounds %struct.S, %struct.S* %aggregate, i64 0, i32 0
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %member, i64 0, i64 3
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, MemcpyRespectsStructMemberBound) {
  const char *source = R"(
    %struct.S = type { [4 x i32], i32 }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @main() {
    entry:
      %aggregate = alloca %struct.S
      %source = alloca [32 x i8]
      %member = getelementptr inbounds %struct.S, %struct.S* %aggregate, i64 0, i32 0
      %dst = bitcast [4 x i32]* %member to i8*
      %src = bitcast [32 x i8]* %source to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst, i8* %src, i64 20, i1 false)
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, MemcpyFittingStructMemberStaysSafe) {
  const char *source = R"(
    %struct.S = type { [4 x i32], i32 }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @main() {
    entry:
      %aggregate = alloca %struct.S
      %source = alloca [32 x i8]
      %member = getelementptr inbounds %struct.S, %struct.S* %aggregate, i64 0, i32 0
      %dst = bitcast [4 x i32]* %member to i8*
      %src = bitcast [32 x i8]* %source to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst, i8* %src, i64 16, i1 false)
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, PhiPreservesPointerOffsetFromBothPaths) {
  const char *source = R"(
    define i32 @main(i1 %chooseFar) {
    entry:
      %array = alloca [4 x i32]
      br i1 %chooseFar, label %far, label %near

    far:
      %far.array = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 0
      %far.ptr = getelementptr inbounds i32, i32* %far.array, i64 3
      br label %join

    near:
      %near.array = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 0
      %near.ptr = getelementptr inbounds i32, i32* %near.array, i64 2
      br label %join

    join:
      %ptr = phi i32* [ %far.ptr, %far ], [ %near.ptr, %near ]
      %access = getelementptr inbounds i32, i32* %ptr, i64 1
      store i32 1, i32* %access
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, PhiPointerOffsetsKeepSafePathsSafe) {
  const char *source = R"(
    define i32 @main(i1 %chooseSecond) {
    entry:
      %array = alloca [4 x i32]
      br i1 %chooseSecond, label %second, label %first

    second:
      %second.array = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 0
      %second.ptr = getelementptr inbounds i32, i32* %second.array, i64 2
      br label %join

    first:
      %first.array = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 0
      %first.ptr = getelementptr inbounds i32, i32* %first.array, i64 1
      br label %join

    join:
      %ptr = phi i32* [ %second.ptr, %second ], [ %first.ptr, %first ]
      %access = getelementptr inbounds i32, i32* %ptr, i64 1
      store i32 1, i32* %access
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, PhiUndefIncomingValueIsUnknown) {
  const char *source = R"(
    define i32 @main(i1 %chooseUndef) {
    entry:
      %array = alloca [2 x i32]
      br i1 %chooseUndef, label %known, label %unknown

    known:
      br label %join

    unknown:
      br label %join

    join:
      %index = phi i32 [ 1, %known ], [ undef, %unknown ]
      %safe = icmp slt i32 %index, 2
      br i1 %safe, label %exit, label %overflow

    overflow:
      %ptr = getelementptr inbounds [2 x i32], [2 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ExtractElementResolvesConstantVectorLane) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %ptr = extractelement <1 x i32*> <i32* null>, i32 0
      %value = load i32, i32* %ptr
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ShuffleVectorResolvesConstantPointerLane) {
  const char *source = R"(
    @value = global i32 0

    define i32 @main() {
    entry:
      %shuffled = shufflevector <2 x i32*> <i32* null, i32* @value>,
                                <2 x i32*> poison, <1 x i32> zeroinitializer
      %ptr = extractelement <1 x i32*> %shuffled, i32 0
      %result = load i32, i32* %ptr
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ConstantCallArgumentPropagates) {
  const char *source = R"(
    define i32 @identity(i32 %value) {
    entry:
      ret i32 %value
    }

    define i32 @main() {
    entry:
      %result = call i32 @identity(i32 3)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runAE(module.get());

  const Instruction *call =
      findNamedInstruction(module.get(), "main", "result");
  AbstractValue value = getInstructionValue(call);
  ASSERT_TRUE(value.isInterval());
  EXPECT_EQ(value.getInterval().lb().getIntNumeral(), 3);
  EXPECT_EQ(value.getInterval().ub().getIntNumeral(), 3);
}

TEST_F(AEInstructionParityTest, SwitchDefaultIsPrunedForCoveredConstant) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      switch i32 1, label %default [
        i32 1, label %covered
      ]

    covered:
      ret i32 0

    default:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 5
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SwitchDefaultRemainsForUncoveredConstant) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      switch i32 2, label %default [
        i32 1, label %covered
      ]

    covered:
      ret i32 0

    default:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 5
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ConstantComparisonPrunesDeadEdge) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      %cmp = icmp slt i32 1, 0
      br i1 %cmp, label %dead, label %exit

    dead:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 5
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ConstantComparisonKeepsLiveEdge) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      %cmp = icmp slt i32 0, 1
      br i1 %cmp, label %live, label %exit

    live:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 5
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ConstantNotEqualProducesFalse) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %flag = icmp ne i32 1, 1
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runAE(module.get());

  const Instruction *flag = findNamedInstruction(module.get(), "main", "flag");
  AbstractValue value = getInstructionValue(flag);
  ASSERT_TRUE(value.isInterval());
  EXPECT_EQ(value.getInterval().lb().getIntNumeral(), 0);
  EXPECT_EQ(value.getInterval().ub().getIntNumeral(), 0);
}

TEST_F(AEInstructionParityTest, SignedLessEqualFalseEdgeRemainsReachable) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      %cmp = icmp sle i32 9, 0
      br i1 %cmp, label %dead, label %live

    dead:
      ret i32 0

    live:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* %array, i64 0, i64 5
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, AccessThroughZeroSizedAllocationOverflows) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      %raw = call i8* @malloc(i64 0)
      %ptr = bitcast i8* %raw to i32*
      store i32 1, i32* %ptr
      call void @free(i8* %raw)
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, AtomicRmwDetectsNullPointer) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %old = atomicrmw add i32* null, i32 1 seq_cst
      ret i32 %old
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, CmpXchgDetectsNullPointer) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %pair = cmpxchg i32* null, i32 1, i32 2 seq_cst seq_cst
      %old = extractvalue { i32, i1 } %pair, 0
      ret i32 %old
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, AccessThroughZeroSizedCallocOverflows) {
  const char *source = R"(
    declare i8* @calloc(i64, i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      %raw = call i8* @calloc(i64 0, i64 4)
      %base = bitcast i8* %raw to i32*
      %ptr = getelementptr inbounds i32, i32* %base, i64 0
      store i32 1, i32* %ptr
      call void @free(i8* %raw)
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, AccessThroughZeroSizedAllocaOverflows) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %ptr = alloca i32, i32 0
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ZeroLengthMemcpyStaysSafe) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @main() {
    entry:
      %dst = alloca [4 x i8]
      %src = alloca [4 x i8]
      %dst.ptr = bitcast [4 x i8]* %dst to i8*
      %src.ptr = bitcast [4 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst.ptr, i8* %src.ptr, i64 0, i1 false)
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, FreadChecksTotalElementBytes) {
  const char *source = R"(
    declare i64 @fread(i8*, i64, i64, i8*)

    define i32 @main() {
    entry:
      %buffer = alloca [8 x i8]
      %ptr = bitcast [8 x i8]* %buffer to i8*
      %read = call i64 @fread(i8* %ptr, i64 8, i64 2, i8* null)
      %small = trunc i64 %read to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, FreadFittingTotalElementBytesStaysSafe) {
  const char *source = R"(
    declare i64 @fread(i8*, i64, i64, i8*)

    define i32 @main() {
    entry:
      %buffer = alloca [8 x i8]
      %ptr = bitcast [8 x i8]* %buffer to i8*
      %read = call i64 @fread(i8* %ptr, i64 4, i64 2, i8* null)
      %small = trunc i64 %read to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, RecvChecksDestinationLength) {
  const char *source = R"(
    declare i64 @recv(i32, i8*, i64, i32)

    define i32 @main() {
    entry:
      %buffer = alloca [8 x i8]
      %ptr = bitcast [8 x i8]* %buffer to i8*
      %n = call i64 @recv(i32 0, i8* %ptr, i64 16, i32 0)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, RecvChecksNullDestination) {
  const char *source = R"(
    declare i64 @recv(i32, i8*, i64, i32)

    define i32 @main() {
    entry:
      %n = call i64 @recv(i32 0, i8* null, i64 8, i32 0)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrlenChecksNullPointer) {
  const char *source = R"(
    declare i64 @strlen(i8*)

    define i32 @main() {
    entry:
      %n = call i64 @strlen(i8* null)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrstrChecksBothInputStrings) {
  const char *source = R"(
    declare i8* @strstr(i8*, i8*)

    define i32 @main() {
    entry:
      %result = call i8* @strstr(i8* null, i8* null)
      %isNull = icmp eq i8* %result, null
      %value = zext i1 %isNull to i32
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ScanfChecksOutputPointers) {
  const char *source = R"(
    @format = private constant [3 x i8] c"%d\00"
    declare i32 @scanf(i8*, ...)

    define i32 @main() {
    entry:
      %format.ptr = getelementptr inbounds [3 x i8], [3 x i8]* @format, i64 0, i64 0
      %result = call i32 (i8*, ...) @scanf(i8* %format.ptr, i32* null)
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, PrintfChecksNullFormatString) {
  const char *source = R"(
    declare i32 @printf(i8*, ...)

    define i32 @main() {
    entry:
      %result = call i32 (i8*, ...) @printf(i8* null)
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SprintfChecksNullDestination) {
  const char *source = R"(
    @format = private constant [3 x i8] c"%s\00"
    declare i32 @sprintf(i8*, i8*, ...)

    define i32 @main() {
    entry:
      %format.ptr = getelementptr inbounds [3 x i8], [3 x i8]* @format, i64 0, i64 0
      %result = call i32 (i8*, i8*, ...) @sprintf(i8* null, i8* %format.ptr, i8* %format.ptr)
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrcmpChecksBothInputPointers) {
  const char *source = R"(
    @text = private constant [2 x i8] c"x\00"
    declare i32 @strcmp(i8*, i8*)

    define i32 @main() {
    entry:
      %text.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @text, i64 0, i64 0
      %result = call i32 @strcmp(i8* null, i8* %text.ptr)
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrdupChecksNullInput) {
  const char *source = R"(
    declare i8* @strdup(i8*)

    define i32 @main() {
    entry:
      %ptr = call i8* @strdup(i8* null)
      %isNull = icmp eq i8* %ptr, null
      %value = zext i1 %isNull to i32
      ret i32 %value
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrtolChecksNullInputString) {
  const char *source = R"(
    declare i64 @strtol(i8*, i8**, i32)

    define i32 @main() {
    entry:
      %value = call i64 @strtol(i8* null, i8** null, i32 10)
      %small = trunc i64 %value to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest,
       ReallocInvalidatesOldPointerWithoutNullFinding) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      %old = call i8* @malloc(i64 4)
      %new = call i8* @realloc(i8* %old, i64 8)
      %typed = bitcast i8* %old to i32*
      store i32 1, i32* %typed
      call void @free(i8* %new)
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source)) {
    EXPECT_GT(result.uaf_bugs, 0u);
  }
}

TEST_F(AEInstructionParityTest, PosixMemalignChecksOutputPointer) {
  const char *source = R"(
    declare i32 @posix_memalign(i8**, i64, i64)

    define i32 @main() {
    entry:
      %result = call i32 @posix_memalign(i8** null, i64 8, i64 16)
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ReadChecksDestinationLength) {
  const char *source = R"(
    declare i64 @read(i32, i8*, i64)

    define i32 @main() {
    entry:
      %buffer = alloca [8 x i8]
      %ptr = bitcast [8 x i8]* %buffer to i8*
      %n = call i64 @read(i32 0, i8* %ptr, i64 16)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ReadChecksNullDestination) {
  const char *source = R"(
    declare i64 @read(i32, i8*, i64)

    define i32 @main() {
    entry:
      %n = call i64 @read(i32 0, i8* null, i64 8)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, WriteChecksSourceLength) {
  const char *source = R"(
    declare i64 @write(i32, i8*, i64)

    define i32 @main() {
    entry:
      %buffer = alloca [8 x i8]
      %ptr = bitcast [8 x i8]* %buffer to i8*
      %n = call i64 @write(i32 1, i8* %ptr, i64 16)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, WriteChecksNullSource) {
  const char *source = R"(
    declare i64 @write(i32, i8*, i64)

    define i32 @main() {
    entry:
      %n = call i64 @write(i32 1, i8* null, i64 8)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SendChecksSourceLength) {
  const char *source = R"(
    declare i64 @send(i32, i8*, i64, i32)

    define i32 @main() {
    entry:
      %buffer = alloca [8 x i8]
      %ptr = bitcast [8 x i8]* %buffer to i8*
      %n = call i64 @send(i32 0, i8* %ptr, i64 16, i32 0)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, GetlineChecksOutputPointers) {
  const char *source = R"(
    declare i64 @getline(i8**, i64*, i8*)

    define i32 @main() {
    entry:
      %n = call i64 @getline(i8** null, i64* null, i8* null)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, ReadlinkChecksDestinationLength) {
  const char *source = R"(
    @path = private constant [2 x i8] c"/\00"
    declare i64 @readlink(i8*, i8*, i64)

    define i32 @main() {
    entry:
      %path.ptr = getelementptr inbounds [2 x i8], [2 x i8]* @path, i64 0, i64 0
      %buffer = alloca [8 x i8]
      %buffer.ptr = bitcast [8 x i8]* %buffer to i8*
      %n = call i64 @readlink(i8* %path.ptr, i8* %buffer.ptr, i64 16)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrxfrmChecksBothBuffersAgainstLength) {
  const char *source = R"(
    declare i64 @strxfrm(i8*, i8*, i64)

    define i32 @main() {
    entry:
      %dst = alloca [8 x i8]
      %src = alloca [8 x i8]
      %dst.ptr = bitcast [8 x i8]* %dst to i8*
      %src.ptr = bitcast [8 x i8]* %src to i8*
      %n = call i64 @strxfrm(i8* %dst.ptr, i8* %src.ptr, i64 16)
      %small = trunc i64 %n to i32
      ret i32 %small
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}
