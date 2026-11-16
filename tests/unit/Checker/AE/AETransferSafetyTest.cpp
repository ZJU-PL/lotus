#include "AECheckerTestSupport.h"

#include <cstdint>

class AETransferSafetyTest : public AECheckerTest {
protected:
  void expectReturn(const std::string &source, const IntervalValue &expected) {
    for (StateLayout layout :
         {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
      SCOPED_TRACE(source);
      auto module = parseModule(source.c_str());
      ASSERT_NE(module, nullptr);
      runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
            false, false, layout);
      EXPECT_TRUE(
          getFunctionReturnInterval(module.get(), "main").equals(expected));
    }
  }
};

TEST_F(AETransferSafetyTest, VectorInsertionUsesElementAndIndexOperands) {
  for (int element : {0, 1, 4})
    for (int insertedLane : {0, 1})
      for (int extractedLane : {0, 1})
        expectReturn("define i32 @main() { "
                     "%v = insertelement <2 x i32> <i32 0, i32 1>, i32 " +
                         std::to_string(element) + ", i32 " +
                         std::to_string(insertedLane) +
                         " %r = extractelement <2 x i32> %v, i32 " +
                         std::to_string(extractedLane) + " ret i32 %r }",
                     IntervalValue(insertedLane == extractedLane
                                       ? element
                                       : extractedLane));
}

TEST_F(AETransferSafetyTest,
       DynamicVectorInsertionMayOverwriteTheExtractedLane) {
  expectReturn(R"(
    define i32 @main(i1 %c) {
      %lane = select i1 %c, i32 0, i32 1
      %v = insertelement <2 x i32> <i32 0, i32 1>, i32 4, i32 %lane
      %r = extractelement <2 x i32> %v, i32 0
      ret i32 %r
    }
  )",
               IntervalValue(0, 4));
}

TEST_F(AETransferSafetyTest, WideVectorIndexDoesNotWrapToAValidLane) {
  expectReturn(R"(
    define i32 @main() {
      %r = extractelement <2 x i32> <i32 0, i32 1>, i64 4294967296
      ret i32 %r
    }
  )",
               IntervalValue::top());
}

TEST_F(AETransferSafetyTest, PointerVectorInsertionPreservesLaneTypes) {
  for (bool dynamic : {false, true}) {
    const std::string source =
        "define i32 @main(i1 %c) { %x = alloca i32 "
        "store i32 1, i32* %x %lane = select i1 %c, i32 0, i32 1 "
        "%v = insertelement <2 x i32*> zeroinitializer, i32* %x, i32 " +
        std::string(dynamic ? "%lane" : "0") +
        " %p = extractelement <2 x i32*> %v, i32 0 "
        "%value = load i32, i32* %p ret i32 0 }";
    for (StateLayout layout :
         {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
      auto module = parseModule(source.c_str());
      ASSERT_NE(module, nullptr);
      const auto result =
          runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW,
                3u, false, false, layout);
      if (dynamic)
        EXPECT_GT(result.null_bugs, 0u);
      else
        EXPECT_EQ(result.null_bugs, 0u);
    }
  }
}

TEST_F(AETransferSafetyTest, AtomicExchangeReturnsOldValueAndUpdatesMemory) {
  expectReturn(R"(
    define i32 @main() {
      %slot = alloca i32
      store i32 1, i32* %slot
      %old = atomicrmw xchg i32* %slot, i32 4 seq_cst
      %new = load i32, i32* %slot
      %scaled = mul i32 %old, 10
      %r = add i32 %scaled, %new
      ret i32 %r
    }
  )",
               IntervalValue(14));
}

TEST_F(AETransferSafetyTest, AtomicIntegerOperationsUseBitWidthAndSignedness) {
  struct Case {
    const char *op;
    int initial;
    int operand;
    int result;
  };
  const Case cases[] = {{"add", 127, 1, -128}, {"sub", -128, 1, 127},
                        {"and", 6, 3, 2},      {"nand", 6, 3, -3},
                        {"or", 6, 3, 7},       {"xor", 6, 3, 5},
                        {"max", -1, 2, 2},     {"min", -1, 2, -1},
                        {"umax", -1, 2, -1},   {"umin", -1, 2, 2}};
  for (const Case &c : cases)
    expectReturn("define i32 @main() { %slot = alloca i8 store i8 " +
                     std::to_string(c.initial) +
                     ", i8* %slot %old = atomicrmw " + c.op +
                     " i8* %slot, i8 " + std::to_string(c.operand) +
                     " seq_cst %new = load i8, i8* %slot "
                     "%r = sext i8 %new to i32 ret i32 %r }",
                 IntervalValue(c.result));
}

TEST_F(AETransferSafetyTest,
       AtomicExchangeThroughMayAliasPointerUsesWeakUpdate) {
  expectReturn(R"(
    define i32 @main(i1 %c) {
      %left = alloca i32
      %right = alloca i32
      store i32 0, i32* %left
      store i32 0, i32* %right
      %p = select i1 %c, i32* %left, i32* %right
      %old = atomicrmw xchg i32* %p, i32 4 seq_cst
      %r = load i32, i32* %left
      ret i32 %r
    }
  )",
               IntervalValue(0, 4));
}

TEST_F(AETransferSafetyTest,
       NonconstantAtomicOperationDoesNotLeaveStaleMemory) {
  expectReturn(R"(
    define i32 @main(i1 %c) {
      %slot = alloca i32
      %initial = select i1 %c, i32 0, i32 4
      store i32 %initial, i32* %slot
      %old = atomicrmw add i32* %slot, i32 1 seq_cst
      %r = load i32, i32* %slot
      ret i32 %r
    }
  )",
               IntervalValue::top());
}

TEST_F(AETransferSafetyTest,
       CompareExchangeModelsSuccessFailureAndWeakFailure) {
  for (int initial : {0, 3})
    for (bool weak : {false, true})
      expectReturn(
          "define i32 @main() { %slot = alloca i32 store i32 " +
              std::to_string(initial) + ", i32* %slot %pair = cmpxchg " +
              (weak ? "weak " : "") +
              "i32* %slot, i32 0, i32 4 seq_cst seq_cst "
              "%old = extractvalue {i32, i1} %pair, 0 "
              "%success = extractvalue {i32, i1} %pair, 1 "
              "%bit = zext i1 %success to i32 %new = load i32, i32* %slot "
              "%a = mul i32 %old, 100 %b = mul i32 %bit, 10 "
              "%c = add i32 %a, %b %r = add i32 %c, %new ret i32 %r }",
          initial == 3 ? IntervalValue(303)
                       : (weak ? IntervalValue(0, 14) : IntervalValue(14)));
}

TEST_F(AETransferSafetyTest, PointerCompareExchangeReturnsTypedFields) {
  expectReturn(R"(
    define i32 @main() {
      %slot = alloca i32*
      %x = alloca i32
      store i32* null, i32** %slot
      %pair = cmpxchg i32** %slot, i32* null, i32* %x seq_cst seq_cst
      %success = extractvalue {i32*, i1} %pair, 1
      %r = zext i1 %success to i32
      ret i32 %r
    }
  )",
               IntervalValue(1));
}

TEST_F(AETransferSafetyTest,
       SignedComparisonRespectsOperandWidthAndExtremeBounds) {
  expectReturn("define i32 @main(i64 %n) { %c = icmp sgt i64 %n, "
               "9223372036854775807 %r = zext i1 %c to i32 ret i32 %r }",
               IntervalValue(0));
  expectReturn("define i32 @main(i64 %n) { %c = icmp slt i64 %n, "
               "-9223372036854775808 %r = zext i1 %c to i32 ret i32 %r }",
               IntervalValue(0));
  expectReturn("define i32 @main(i32 %n) { %c = icmp sgt i32 %n, "
               "2147483647 %r = zext i1 %c to i32 ret i32 %r }",
               IntervalValue(0));
  expectReturn(
      "define i32 @main() { %flag = icmp eq i32 0, 0 "
      "%c = icmp slt i1 %flag, false %r = zext i1 %c to i32 ret i32 %r }",
      IntervalValue(1));
  expectReturn("define i32 @main() { %bits = add i8 127, 1 "
               "%c = icmp slt i8 %bits, 0 %r = zext i1 %c to i32 ret i32 %r }",
               IntervalValue(1));
}

TEST_F(AETransferSafetyTest, UnsignedConstantGuardUsesItsLLVMWidth) {
  expectReturn(R"(
    define i32 @main() {
      %bits = add i8 127, 1
      %same = icmp uge i8 %bits, -128
      br i1 %same, label %yes, label %no
    yes:
      ret i32 1
    no:
      ret i32 0
    }
  )",
               IntervalValue(1));
}

TEST_F(AETransferSafetyTest,
       UnsignedByteOperationsMatchConcreteBoundaryValues) {
  for (int lhs : {-128, -2, -1, 0, 1, 10, 127}) {
    for (int rhs : {-128, -2, -1, 1, 2, 7}) {
      for (const char *opcode : {"udiv", "urem"}) {
        const uint8_t a = static_cast<uint8_t>(lhs);
        const uint8_t b = static_cast<uint8_t>(rhs);
        const int expected = std::string(opcode) == "udiv" ? a / b : a % b;
        expectReturn("define i32 @main() { %v = " + std::string(opcode) +
                         " i8 " + std::to_string(lhs) + ", " +
                         std::to_string(rhs) +
                         " %r = zext i8 %v to i32 ret i32 %r }",
                     IntervalValue(expected));
      }
    }
    for (int shift : {0, 1, 7})
      expectReturn("define i32 @main() { %v = lshr i8 " + std::to_string(lhs) +
                       ", " + std::to_string(shift) +
                       " %r = zext i8 %v to i32 ret i32 %r }",
                   IntervalValue(static_cast<uint8_t>(lhs) >> shift));
  }
}

TEST_F(AETransferSafetyTest, Unsigned64BitOperationsPreserveHighBits) {
  expectReturn("define i64 @main() { %v = udiv i64 -1, 2 ret i64 %v }",
               IntervalValue(INT64_MAX));
  expectReturn("define i64 @main() { %v = urem i64 -1, -2 ret i64 %v }",
               IntervalValue(1));
  expectReturn("define i64 @main() { %v = lshr i64 -1, 63 ret i64 %v }",
               IntervalValue(1));
}

TEST_F(AETransferSafetyTest, UnsignedIntervalsInterpretNegativeBitPatterns) {
  for (const char *opcode : {"udiv", "lshr"})
    expectReturn("define i32 @main(i1 %c) { "
                 "%a = select i1 %c, i8 -4, i8 -2 %v = " +
                     std::string(opcode) + " i8 %a, " +
                     (std::string(opcode) == "udiv" ? "2" : "1") +
                     " %r = zext i8 %v to i32 ret i32 %r }",
                 IntervalValue(126, 127));
}

TEST_F(AETransferSafetyTest,
       InvalidUnsignedOperationsDoNotCrashOrInventConstants) {
  expectReturn("define i32 @main() { %v = udiv i8 10, 0 "
               "%r = zext i8 %v to i32 ret i32 %r }",
               IntervalValue(0, 255));
  expectReturn("define i32 @main() { %v = lshr i8 10, 8 "
               "%r = zext i8 %v to i32 ret i32 %r }",
               IntervalValue(0, 255));
}

TEST_F(AETransferSafetyTest, SharedConditionalDestinationRemainsReachable) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %a = alloca [4 x i32]
      br i1 false, label %body, label %body
    body:
      %index = phi i32 [4, %entry], [4, %entry]
      %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 %index
      store i32 1, i32* %p
      ret i32 0
    }
  )";
  for (StateLayout layout :
       {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    EXPECT_GT(runAE(module.get(), true, false,
                    AbstractInterpretation::WIDEN_NARROW, 3u, false, false,
                    layout)
                  .overflow_bugs,
              0u);
  }
}

TEST_F(AETransferSafetyTest,
       ZeroExtensionRefinesMemoryInItsSourceRepresentation) {
  expectReturn(R"(
    define i32 @main(i1 %choice) {
      %slot = alloca i8
      %initial = select i1 %choice, i8 -56, i8 -55
      store i8 %initial, i8* %slot
      %old = load i8, i8* %slot
      %wide = zext i8 %old to i32
      %guard = icmp eq i32 %wide, 200
      br i1 %guard, label %body, label %exit
    body:
      %reload = load i8, i8* %slot
      %index = zext i8 %reload to i32
      ret i32 %index
    exit:
      ret i32 200
    }
  )",
               IntervalValue(200));
}

TEST_F(AETransferSafetyTest, TruncationGuardDoesNotNarrowItsSourceCell) {
  expectReturn(R"(
    define i32 @main(i1 %choice) {
      %slot = alloca i32
      %initial = select i1 %choice, i32 256, i32 257
      store i32 %initial, i32* %slot
      %old = load i32, i32* %slot
      %small = trunc i32 %old to i8
      %guard = icmp eq i8 %small, 0
      br i1 %guard, label %body, label %exit
    body:
      %reload = load i32, i32* %slot
      ret i32 %reload
    exit:
      ret i32 256
    }
  )",
               IntervalValue(256, 257));
}

TEST_F(AETransferSafetyTest, OldLoadCannotConstrainAStoreAfterTheComparison) {
  expectReturn(R"(
    define i32 @main(i1 %choice) {
      %slot = alloca i32
      %initial = select i1 %choice, i32 0, i32 1
      store i32 %initial, i32* %slot
      %old = load i32, i32* %slot
      %guard = icmp eq i32 %old, 0
      store i32 4, i32* %slot
      br i1 %guard, label %body, label %exit
    body:
      %reload = load i32, i32* %slot
      ret i32 %reload
    exit:
      ret i32 4
    }
  )",
               IntervalValue(4));
}

TEST_F(AETransferSafetyTest, MayAliasLoadDoesNotRefineEveryPossibleCell) {
  expectReturn(R"(
    define i32 @main(i1 %choice) {
      %left = alloca i32
      %right = alloca i32
      store i32 0, i32* %left
      store i32 4, i32* %right
      %selected = select i1 %choice, i32* %left, i32* %right
      %old = load i32, i32* %selected
      %guard = icmp eq i32 %old, 0
      br i1 %guard, label %body, label %exit
    body:
      %reload = load i32, i32* %right
      ret i32 %reload
    exit:
      ret i32 4
    }
  )",
               IntervalValue(4));
}

TEST_F(AETransferSafetyTest, StalePointerLoadCannotPruneAFeasibleBranch) {
  const char *source = R"(
    define i32 @main(i1 %choice) {
      %slot = alloca i32*
      %x = alloca i32
      %a = alloca [4 x i32]
      %initial = select i1 %choice, i32* null, i32* %x
      store i32* %initial, i32** %slot
      %old = load i32*, i32** %slot
      store i32* %x, i32** %slot
      %guard = icmp eq i32* %old, null
      br i1 %guard, label %body, label %exit
    body:
      %p = getelementptr [4 x i32], [4 x i32]* %a, i32 0, i32 4
      store i32 1, i32* %p
      ret i32 0
    exit:
      ret i32 0
    }
  )";
  for (StateLayout layout :
       {StateLayout::Dense, StateLayout::SemiSparse, StateLayout::Sparse}) {
    auto module = parseModule(source);
    ASSERT_NE(module, nullptr);
    EXPECT_GT(runAE(module.get(), true, false,
                    AbstractInterpretation::WIDEN_NARROW, 3u, false, false,
                    layout)
                  .overflow_bugs,
              0u);
  }
}
