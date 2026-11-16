#include "AECheckerTestSupport.h"

#include <array>
#include <cctype>
#include <climits>

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

TEST_F(AEInstructionParityTest, EqualPointerAlternativesMayCompareUnequal) {
  const char *source = R"(
    define i32 @main(i1 %c, i1 %d) {
      %x = alloca i32
      %y = alloca i32
      %a = alloca [4 x i32]
      %p = select i1 %c, i32* %x, i32* %y
      %q = select i1 %d, i32* %x, i32* %y
      %different = icmp ne i32* %p, %q
      %bit = zext i1 %different to i32
      %index = mul i32 %bit, 4
      %ptr = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, EqualPointerAlternativesMayCompareEqual) {
  const char *source = R"(
    define i32 @main(i1 %c, i1 %d) {
      %x = alloca i32
      %y = alloca i32
      %p = select i1 %c, i32* %x, i32* %y
      %q = select i1 %d, i32* %x, i32* %y
      %equal = icmp eq i32* %p, %q
      %value = zext i1 %equal to i32
      ret i32 %value
    }
  )";
  for (StateLayout layout :
       {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
          false, false, layout);
    EXPECT_TRUE(getFunctionReturnInterval(module.get(), "main")
                    .equals(IntervalValue(0, 1)));
  }
}

TEST_F(AEInstructionParityTest, UnknownPointerTargetsDoNotProveEquality) {
  const char *source = R"(
    define i32 @main(i32* %p, i32* %q) {
      %equal = icmp eq i32* %p, %q
      %value = zext i1 %equal to i32
      ret i32 %value
    }
  )";
  for (StateLayout layout :
       {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
          false, false, layout);
    EXPECT_TRUE(getFunctionReturnInterval(module.get(), "main")
                    .equals(IntervalValue(0, 1)));
  }
}

TEST_F(AEInstructionParityTest, UnknownPointerMayReachNullEqualityBranch) {
  const char *source = R"(
    define i32 @main(i32* %p) {
      %isnull = icmp eq i32* %p, null
      br i1 %isnull, label %body, label %exit
    body:
      %value = load i32, i32* %p
      ret i32 %value
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AEInstructionParityTest, CastsMaterializeDirectConstantOperands) {
  const char *source = R"(
    define i32 @main() {
      %array = alloca [4 x i32]
      %index = trunc i64 4294967299 to i32
      %wide = sext i32 %index to i64
      %ptr = getelementptr [4 x i32], [4 x i32]* %array, i64 0, i64 %wide
      store i32 1, i32* %ptr
      ret i32 %index
    }
  )";
  for (StateLayout layout :
       {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    const auto result =
        runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW,
              3u, false, false, layout);
    EXPECT_EQ(result.overflow_bugs, 0u);
    EXPECT_TRUE(getFunctionReturnInterval(module.get(), "main")
                    .equals(IntervalValue(3)));
  }
}

TEST_F(AEInstructionParityTest, GuardedSiblingExtensionsShareBranchFacts) {
  const char *source = R"(
    define i32 @main(i32 %n) {
      %array = alloca [4 x i32]
      %i = trunc i32 %n to i8
      %index = sext i8 %i to i64
      %first = sext i8 %i to i32
      %nonnegative = icmp sge i32 %first, 0
      br i1 %nonnegative, label %upper, label %exit
    upper:
      %second = sext i8 %i to i32
      %bounded = icmp slt i32 %second, 4
      br i1 %bounded, label %body, label %exit
    body:
      %ptr = getelementptr [4 x i32], [4 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, GuardedZeroExtensionRefinesLaterIndex) {
  const char *source = R"(
    define i32 @main(i8 %n) {
      %array = alloca [8 x i32]
      %guard = zext i8 %n to i32
      %bounded = icmp slt i32 %guard, 8
      br i1 %bounded, label %body, label %exit
    body:
      %index = zext i8 %n to i64
      %ptr = getelementptr [8 x i32], [8 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StaleLoopBodyExtensionCannotConstrainNextPhi) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [4 x i32]
      br label %head
    head:
      %i = phi i32 [0, %entry], [%next, %body]
      %condition = icmp sle i32 %i, 4
      br i1 %condition, label %body, label %exit
    body:
      %index = sext i32 %i to i64
      %ptr = getelementptr [4 x i32], [4 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      %next = add i32 %i, 1
      br label %head
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest,
       HighUnsignedExtensionKeepsNegativeSignedSource) {
  const char *source = R"(
    define i32 @main(i8 %n) {
      %array = alloca [8 x i32]
      %guard = zext i8 %n to i32
      %high = icmp sge i32 %guard, 250
      br i1 %high, label %body, label %exit
    body:
      %index = sext i8 %n to i64
      %ptr = getelementptr [8 x i32], [8 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, WideUnsignedExtensionInverseDoesNotOverflow) {
  const char *source = R"(
    define i32 @main(i63 %n) {
      %array = alloca [8 x i32]
      %guard = zext i63 %n to i64
      %high = icmp sge i64 %guard, 9223372036854775806
      br i1 %high, label %body, label %exit
    body:
      %index = sext i63 %n to i64
      %ptr = getelementptr [8 x i32], [8 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SignedI1GuardRefinesBooleanSiblingExtension) {
  const char *source = R"(
    define i32 @main(i1 %flag) {
      %array = alloca [1 x i32]
      %index = zext i1 %flag to i64
      %signed = sext i1 %flag to i32
      %negative = icmp eq i32 %signed, -1
      br i1 %negative, label %body, label %exit
    body:
      %ptr = getelementptr [1 x i32], [1 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, UnsignedRangeAcrossSignBoundaryKeepsBothSides) {
  const char *source = R"(
    define i32 @main(i8 %n) {
      %array = alloca [128 x i32]
      %guard = zext i8 %n to i32
      %lower = icmp sge i32 %guard, 120
      br i1 %lower, label %upper, label %exit
    upper:
      %bound = icmp sle i32 %guard, 140
      br i1 %bound, label %body, label %exit
    body:
      %index = sext i8 %n to i64
      %ptr = getelementptr [128 x i32], [128 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, NoMainModeStartsWithCallerInsteadOfHelper) {
  const char *source = R"(
    define void @helper(i32 %i) {
      %array = alloca [4 x i32]
      %ptr = getelementptr [4 x i32], [4 x i32]* %array, i32 0, i32 %i
      store i32 1, i32* %ptr
      ret void
    }
    define void @root() {
      call void @helper(i32 0)
      ret void
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, NoMainModeVisitsIndependentRootSCCs) {
  const char *source = R"(
    define void @safe_root() {
      ret void
    }
    define void @unsafe_root() {
      %array = alloca [4 x i32]
      %ptr = getelementptr [4 x i32], [4 x i32]* %array, i32 0, i32 4
      store i32 1, i32* %ptr
      ret void
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, NoMainModeDoesNotStartAtMainWithACaller) {
  const char *source = R"(
    define i32 @main(i32 %i) {
      %array = alloca [4 x i32]
      %ptr = getelementptr [4 x i32], [4 x i32]* %array, i32 0, i32 %i
      store i32 1, i32* %ptr
      ret i32 0
    }
    define void @root() {
      %v = call i32 @main(i32 0)
      ret void
    }
  )";
  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, NoMainModeSelectsOneEntryForRootRecursiveSCC) {
  const char *source = R"(
    define i32 @left() {
    entry:
      ret i32 42
    unreachable:
      %v = call i32 @right()
      ret i32 %v
    }
    define i32 @right() {
      %v = call i32 @left()
      ret i32 %v
    }
  )";
  for (StateLayout layout :
       {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
          false, false, layout);
    const auto *right = findNamedInstruction(module.get(), "right", "v");
    ASSERT_NE(right, nullptr);
    EXPECT_FALSE(
        AbstractInterpretation::getAEInstance().hasAbsStateFromTrace(right));
    EXPECT_TRUE(getFunctionReturnInterval(module.get(), "left")
                    .equals(IntervalValue(42)));
  }
}

TEST_F(AEInstructionParityTest, CtypeConstantsPreserveLibcClassification) {
  for (const auto &item : {std::make_pair("isdigit", std::isdigit(65)),
                           std::make_pair("isalpha", std::isalpha(65)),
                           std::make_pair("isspace", std::isspace(65))}) {
    const std::string source =
        "declare i32 @" + std::string(item.first) +
        "(i32)\ndefine i32 @main() { %result = call i32 @" + item.first +
        "(i32 65) ret i32 %result }";
    for (StateLayout layout :
         {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
      auto module = parseModule(source.c_str());
      ASSERT_NE(module, nullptr);
      runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
            false, false, layout);
      EXPECT_TRUE(getFunctionReturnInterval(module.get(), "main")
                      .equals(IntervalValue(item.second)));
    }
  }
}

TEST_F(AEInstructionParityTest,
       CtypeUnknownAndInvalidArgumentsAreConservative) {
  for (const char *argument : {"%n", "256", "-2"}) {
    const std::string source =
        "declare i32 @isdigit(i32)\n"
        "define i32 @main(i32 %n) { %result = call i32 @isdigit(i32 " +
        std::string(argument) + ") ret i32 %result }";
    for (StateLayout layout :
         {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
      auto module = parseModule(source.c_str());
      ASSERT_NE(module, nullptr);
      runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
            false, false, layout);
      EXPECT_TRUE(getFunctionReturnInterval(module.get(), "main")
                      .equals(IntervalValue(INT_MIN, INT_MAX)));
    }
  }
}

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

TEST_F(AEInstructionParityTest, EmptyModuleReturnsWithoutPointerAnalysis) {
  llvm::Module module("empty", context);
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  ae.reset();
  EXPECT_NO_THROW(ae.runOnModule(&module));
}

TEST_F(AEInstructionParityTest, CastedReloadBranchRefinesBackingMemory) {
  const char *source = R"(
    define i32 @main(i32 %value) {
    entry:
      %array = alloca [8 x i32]
      %slot = alloca i8
      %low = trunc i32 %value to i8
      store i8 %low, i8* %slot
      %first = load i8, i8* %slot
      %first.ext = zext i8 %first to i32
      %in.bounds = icmp ult i32 %first.ext, 8
      br i1 %in.bounds, label %body, label %exit

    body:
      %second = load i8, i8* %slot
      %index = zext i8 %second to i64
      %ptr = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, SignedCastedReloadsShareBranchFacts) {
  const char *source = R"(
    define i32 @main(i32 %value) {
    entry:
      %array = alloca [8 x i32]
      %slot = alloca i8
      %low = trunc i32 %value to i8
      store i8 %low, i8* %slot
      %first = load i8, i8* %slot
      %first.ext = sext i8 %first to i32
      %nonnegative = icmp sge i32 %first.ext, 0
      br i1 %nonnegative, label %upper, label %exit

    upper:
      %second = load i8, i8* %slot
      %second.ext = sext i8 %second to i32
      %bounded = icmp slt i32 %second.ext, 8
      br i1 %bounded, label %body, label %exit

    body:
      %third = load i8, i8* %slot
      %index = sext i8 %third to i64
      %ptr = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, i64 %index
      store i32 1, i32* %ptr
      ret i32 0

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, AndMaskBoundsUnknownIndex) {
  const char *source = R"(
    define i32 @main(i32 %value) {
    entry:
      %array = alloca [8 x i32]
      %index = and i32 %value, 7
      %ptr = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, UnsignedRemainderBoundsUnknownIndex) {
  const char *source = R"(
    define i32 @main(i32 %value) {
    entry:
      %array = alloca [8 x i32]
      %index = urem i32 %value, 8
      %ptr = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, i32 %index
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, BoundedLoopUpperBoundRefinesReloadedIndex) {
  const char *source = R"(
    define i32 @main(i32 %value) {
    entry:
      %array = alloca [8 x i32]
      %bound = alloca i32
      %index = alloca i32
      store i32 %value, i32* %bound
      %first.bound = load i32, i32* %bound
      %negative = icmp slt i32 %first.bound, 0
      br i1 %negative, label %exit, label %check.upper

    check.upper:
      %second.bound = load i32, i32* %bound
      %too.large = icmp sgt i32 %second.bound, 8
      br i1 %too.large, label %exit, label %loop.entry

    loop.entry:
      store i32 0, i32* %index
      br label %loop

    loop:
      %current = load i32, i32* %index
      %limit = load i32, i32* %bound
      %continue = icmp slt i32 %current, %limit
      br i1 %continue, label %body, label %exit

    body:
      %access.index = load i32, i32* %index
      %ptr = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, i32 %access.index
      store i32 1, i32* %ptr
      %next = add i32 %access.index, 1
      store i32 %next, i32* %index
      br label %loop

    exit:
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, VectorGepLaneTracksOutOfBoundsOffset) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [8 x i32]
      %lanes = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, <2 x i64> <i64 7, i64 8>
      %ptr = extractelement <2 x i32*> %lanes, i32 1
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, VectorGepInBoundsLaneStaysSafe) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %array = alloca [8 x i32]
      %lanes = getelementptr inbounds [8 x i32], [8 x i32]* %array, i64 0, <2 x i64> <i64 6, i64 7>
      %ptr = extractelement <2 x i32*> %lanes, i32 1
      store i32 1, i32* %ptr
      ret i32 0
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_EQ(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, MemchrChecksSearchedByteCount) {
  const char *source = R"(
    declare i8* @memchr(i8*, i32, i64)

    define i32 @main() {
    entry:
      %buffer = alloca [4 x i8]
      %ptr = bitcast [4 x i8]* %buffer to i8*
      %found = call i8* @memchr(i8* %ptr, i32 0, i64 5)
      %isnull = icmp eq i8* %found, null
      %result = zext i1 %isnull to i32
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, MemcmpChecksBothByteRanges) {
  const char *source = R"(
    declare i32 @memcmp(i8*, i8*, i64)

    define i32 @main() {
    entry:
      %left = alloca [4 x i8]
      %right = alloca [4 x i8]
      %left.ptr = bitcast [4 x i8]* %left to i8*
      %right.ptr = bitcast [4 x i8]* %right to i8*
      %result = call i32 @memcmp(i8* %left.ptr, i8* %right.ptr, i64 5)
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.overflow_bugs, 0u);
}

TEST_F(AEInstructionParityTest, StrnlenChecksLengthAndNullPointer) {
  const char *source = R"(
    declare i64 @strnlen(i8*, i64)

    define i32 @main() {
    entry:
      %buffer = alloca [4 x i8]
      %ptr = bitcast [4 x i8]* %buffer to i8*
      %length = call i64 @strnlen(i8* %ptr, i64 5)
      %null.length = call i64 @strnlen(i8* null, i64 4)
      %combined = add i64 %length, %null.length
      %result = trunc i64 %combined to i32
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source)) {
    EXPECT_GT(result.overflow_bugs, 0u);
    EXPECT_GT(result.null_bugs, 0u);
  }
}

TEST_F(AEInstructionParityTest, GetlineChecksStreamPointer) {
  const char *source = R"(
    declare i64 @getline(i8**, i64*, i8*)

    define i32 @main() {
    entry:
      %line = alloca i8*
      %capacity = alloca i64
      store i8* null, i8** %line
      store i64 0, i64* %capacity
      %length = call i64 @getline(i8** %line, i64* %capacity, i8* null)
      %result = trunc i64 %length to i32
      ret i32 %result
    }
  )";

  for (const AEResult &result : runLayouts(source))
    EXPECT_GT(result.null_bugs, 0u);
}
