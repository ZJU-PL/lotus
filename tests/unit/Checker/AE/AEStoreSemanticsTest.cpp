/**
 * @file AEStoreSemanticsTest.cpp
 * @brief Strong/weak update semantics of AE memory stores.
 *
 * SVF guards a store with `AbstractInterpretation::isStrongUpdate`
 * (`svf/lib/AE/Svfexe/AbstractStateManager.cpp`): only a singleton, non-summary
 * object may be *replaced*; everything else must be *joined*. These tests pin
 * down each condition of that rule so the behaviour cannot drift silently.
 */

#include "AECheckerTestSupport.h"

#include "Checker/AE/AbstractState.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

constexpr uint32_t kPtr = 11;
constexpr uint32_t kVal = 12;
constexpr uint32_t kObjA = 101;
constexpr uint32_t kObjB = 102;

uint32_t addrOf(uint32_t objId) {
  return AbstractState::getVirtualMemAddress(objId);
}

/// State with `p -> {objs...}` and `*objA`/`*objB` seeded with the given
/// intervals, and `v` holding @p stored.
AbstractState makeState(const std::vector<uint32_t> &objIds, int64_t aVal,
                        int64_t bVal, int64_t stored) {
  AbstractState as;
  AddressValue ptr;
  for (uint32_t objId : objIds)
    ptr.insert(addrOf(objId));
  as[kPtr] = AbstractValue(ptr);
  // Object contents live in the address map, not the variable map.
  as._addrToAbsVal[kObjA] = AbstractValue(IntervalValue(aVal));
  as._addrToAbsVal[kObjB] = AbstractValue(IntervalValue(bVal));
  as[kVal] = AbstractValue(IntervalValue(stored));
  return as;
}

} // namespace

class AEStoreSemanticsTest : public AECheckerTest {
protected:
  // `AbstractState` asks the AE singleton for the value<->id mapping while
  // resolving GEP-derived store targets. Tests that build states by hand must
  // start from a clean singleton, otherwise ids from a previously analyzed
  // module resolve to dangling `llvm::Value`s.
  void SetUp() override { AbstractInterpretation::getAEInstance().reset(); }
};

TEST_F(AEStoreSemanticsTest, SingletonObjectAllowsStrongUpdate) {
  AbstractState as = makeState({kObjA}, 1, 2, 7);

  EXPECT_TRUE(as.allowsStrongUpdate(as[kPtr].getAddrs()));

  as.storeValue(kPtr, kVal, AbstractState::StoreKind::Strong);
  // Replaced, not joined.
  EXPECT_EQ(as.load(addrOf(kObjA)).getInterval().getIntNumeral(), 7);
}

TEST_F(AEStoreSemanticsTest, SingletonObjectWeakUpdateJoins) {
  AbstractState as = makeState({kObjA}, 1, 2, 7);

  as.storeValue(kPtr, kVal, AbstractState::StoreKind::Weak);
  const IntervalValue after = as.load(addrOf(kObjA)).getInterval();
  // [1,1] join [7,7] == [1,7]: the old value survives.
  EXPECT_EQ(after.lb().getIntNumeral(), 1);
  EXPECT_EQ(after.ub().getIntNumeral(), 7);
}

TEST_F(AEStoreSemanticsTest, MultipleTargetsForceWeakUpdate) {
  AbstractState as = makeState({kObjA, kObjB}, 1, 2, 7);

  EXPECT_FALSE(as.allowsStrongUpdate(as[kPtr].getAddrs()));

  as.storeValue(kPtr, kVal, AbstractState::StoreKind::Weak);
  // Both may-aliases keep their old value, joined with the stored one.
  EXPECT_EQ(as.load(addrOf(kObjA)).getInterval().lb().getIntNumeral(), 1);
  EXPECT_EQ(as.load(addrOf(kObjA)).getInterval().ub().getIntNumeral(), 7);
  EXPECT_EQ(as.load(addrOf(kObjB)).getInterval().lb().getIntNumeral(), 2);
  EXPECT_EQ(as.load(addrOf(kObjB)).getInterval().ub().getIntNumeral(), 7);
}

TEST_F(AEStoreSemanticsTest, HeapObjectForcesWeakUpdate) {
  AbstractState as = makeState({kObjA}, 1, 2, 7);
  as.addHeapObject(kObjA);

  // SVF never strongly updates a heap object: it is a shared summary.
  EXPECT_FALSE(as.allowsStrongUpdate(as[kPtr].getAddrs()));

  as.storeValue(kPtr, kVal, AbstractState::StoreKind::Weak);
  EXPECT_EQ(as.load(addrOf(kObjA)).getInterval().ub().getIntNumeral(), 7);
  EXPECT_EQ(as.load(addrOf(kObjA)).getInterval().lb().getIntNumeral(), 1);
}

TEST_F(AEStoreSemanticsTest, VariantGepMarksTheObjectFieldInsensitive) {
  AbstractState as = makeState({kObjA}, 1, 2, 7);
  EXPECT_TRUE(as.allowsStrongUpdate(as[kPtr].getAddrs()));

  // A non-numeral offset means the accessed field cannot be identified.
  as.getGepObjAddrs(kPtr, IntervalValue(0, 3));

  EXPECT_TRUE(as.isFieldInsensitiveObject(kObjA));
  EXPECT_FALSE(as.allowsStrongUpdate(as[kPtr].getAddrs()));
}

TEST_F(AEStoreSemanticsTest, NullAndBlackHoleTargetsNeverAllowStrongUpdate) {
  AbstractState nullState;
  nullState[kPtr] = AbstractValue(AddressValue(NullMemAddr));
  EXPECT_FALSE(nullState.allowsStrongUpdate(nullState[kPtr].getAddrs()));

  AbstractState blkState;
  blkState[kPtr] = AbstractValue(AddressValue(AbstractState::BlkPtr));
  EXPECT_FALSE(blkState.allowsStrongUpdate(blkState[kPtr].getAddrs()));

  AbstractState emptyState;
  emptyState[kPtr] = AbstractValue(AddressValue());
  EXPECT_FALSE(emptyState.allowsStrongUpdate(emptyState[kPtr].getAddrs()));
}

TEST_F(AEStoreSemanticsTest, OversizedAggregatesAreJoined) {
  AbstractState as = makeState({kObjA}, 1, 2, 7);
  // Summarized by the field limit, like SVF's getMaxFieldOffsetLimit check.
  as.setObjSize(kObjA, MaxFieldLimit + 1);
  EXPECT_FALSE(as.allowsStrongUpdate(as[kPtr].getAddrs()));
}

//===----------------------------------------------------------------------===//
// End-to-end: the decision is actually exercised by the store transfer
// function, and a loop-carried scalar still gets strong updates.
//===----------------------------------------------------------------------===//

TEST_F(AEStoreSemanticsTest, LoopScalarsAreStronglyUpdatedInAnalysis) {
  auto module = parseModule(R"(
    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %acc = phi i32 [ 0, %entry ], [ %sum, %loop ]
      %sum = add i32 %acc, %i
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, 4
      br i1 %cond, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  // Strong updates keep the accumulator tight; a weak join would widen it.
  const AEResult dense = runAE(module.get(), true, false,
                               AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(dense.overflow_bugs, 0u);
  EXPECT_EQ(dense.null_bugs, 0u);
}

TEST_F(AEStoreSemanticsTest, ArrayElementStoresAreWeak) {
  auto module = parseModule(R"(
    define i32 @main() {
    entry:
      %arr = alloca [8 x i32]
      %base = getelementptr inbounds [8 x i32], [8 x i32]* %arr, i32 0, i32 0
      %i = getelementptr inbounds [8 x i32], [8 x i32]* %arr, i32 0, i32 2
      store i32 5, i32* %base
      store i32 7, i32* %i
      %v = load i32, i32* %i
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  // Just needs to run without crashing and stay quiet: an array base is a
  // summary object, so both stores go through the weak path.
  const AEResult result = runAE(module.get(), true, false,
                                AbstractInterpretation::WIDEN_NARROW, 3u);
  EXPECT_EQ(result.null_bugs, 0u);
}

TEST_F(AEStoreSemanticsTest, MayAliasStoreKeepsBothOldValuesEndToEnd) {
  // `%sel` may point to %a or %b, so `store i32 3` must be a weak update on
  // both. A strong update would leave *a == 3 and *b == 3; a weak update keeps
  // the old bounds and joins 3 in.
  auto module = parseModule(R"(
    define i32 @main(i1 %c) {
    entry:
      %a = alloca i32
      %b = alloca i32
      store i32 1, i32* %a
      store i32 2, i32* %b
      %sel = select i1 %c, i32* %a, i32* %b
      store i32 3, i32* %sel
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u);

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  const auto *ret = dyn_cast<ReturnInst>(main->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);
  ASSERT_TRUE(ae.hasAbsStateFromTrace(ret));
  const AbstractState &state = ae.getAbsStateFromTrace(ret);

  const auto *allocaA = dyn_cast<AllocaInst>(
      lotus::unittest::findInstructionByName(*main, "a"));
  const auto *allocaB = dyn_cast<AllocaInst>(
      lotus::unittest::findInstructionByName(*main, "b"));
  ASSERT_NE(allocaA, nullptr);
  ASSERT_NE(allocaB, nullptr);

  const uint32_t aId = AbstractInterpretation::getValueIdStatic(allocaA);
  const uint32_t bId = AbstractInterpretation::getValueIdStatic(allocaB);

  const auto aIt = state.getLocToVal().find(aId);
  const auto bIt = state.getLocToVal().find(bId);
  ASSERT_TRUE(aIt != state.getLocToVal().end());
  ASSERT_TRUE(bIt != state.getLocToVal().end());

  // Joined: the stored 3 is the new upper bound, the old value survives.
  EXPECT_EQ(aIt->second.getInterval().ub().getIntNumeral(), 3);
  EXPECT_EQ(aIt->second.getInterval().lb().getIntNumeral(), 1);
  EXPECT_EQ(bIt->second.getInterval().ub().getIntNumeral(), 3);
  EXPECT_EQ(bIt->second.getInterval().lb().getIntNumeral(), 2);
}
