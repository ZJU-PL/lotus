/**
 * @file AEStateStoreTest.cpp
 * @brief Tests for the AE per-instruction state store and its retention modes.
 *
 * Two things are checked:
 *
 *  1. `AbstractStateStore` bookkeeping: which nodes survive, which are dropped,
 *     and that the dense mode never drops anything.
 *  2. End-to-end equivalence: switching `StateLayout` only changes retention,
 *     so a run with `StateLayout::Sparse` must report exactly the same findings
 *     as `StateLayout::Dense`.
 */

#include "AECheckerTestSupport.h"

#include "Checker/AE/AbstractStateStore.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

const Instruction *nthInstruction(Function *fn, unsigned index) {
  unsigned seen = 0;
  for (BasicBlock &bb : *fn)
    for (Instruction &inst : bb) {
      if (seen == index)
        return &inst;
      ++seen;
    }
  return nullptr;
}

} // namespace

class AEStateStoreTest : public AECheckerTest {
protected:
  /// A module with one block of straight-line instructions plus a terminator.
  std::unique_ptr<Module> straightLineModule() {
    return parseModule(R"(
      define i32 @main() {
      entry:
        %a = add i32 1, 2
        %b = add i32 %a, 3
        %c = mul i32 %b, 4
        %d = sub i32 %c, 5
        ret i32 %d
      }
    )");
  }
};

TEST_F(AEStateStoreTest, DenseLayoutNeverDropsAState) {
  auto module = straightLineModule();
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  AbstractStateStore store(StateLayout::Dense);
  EXPECT_EQ(store.layout(), StateLayout::Dense);
  EXPECT_FALSE(store.isSparse());

  const Instruction *prev = nullptr;
  for (unsigned i = 0; i < 4; ++i) {
    const Instruction *inst = nthInstruction(main, i);
    ASSERT_NE(inst, nullptr);
    store.inherit(inst, AbstractState());
    prev = inst;
  }

  // Every inherited state is still there: dense mode is the historical
  // behaviour of keeping a full state per instruction.
  EXPECT_EQ(store.size(), 4u);
  EXPECT_NE(prev, nullptr);
  EXPECT_TRUE(store.contains(prev));
  EXPECT_EQ(store.stats().released, 0u);
}

TEST_F(AEStateStoreTest, SparseLayoutDropsTheIntraBlockLookBehind) {
  auto module = straightLineModule();
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  AbstractStateStore store(StateLayout::Sparse);
  ASSERT_TRUE(store.isSparse());

  const Instruction *first = nthInstruction(main, 0);
  const Instruction *second = nthInstruction(main, 1);
  const Instruction *third = nthInstruction(main, 2);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(third, nullptr);

  store.inherit(first, AbstractState());
  EXPECT_TRUE(store.contains(first));

  store.inherit(second, AbstractState());
  // The first state has been consumed by its only reader.
  EXPECT_FALSE(store.contains(first));
  EXPECT_TRUE(store.contains(second));

  store.inherit(third, AbstractState());
  EXPECT_FALSE(store.contains(second));
  EXPECT_TRUE(store.contains(third));
  EXPECT_EQ(store.size(), 1u);
  EXPECT_EQ(store.stats().released, 2u);
  EXPECT_LE(store.stats().peak, 2u);
}

TEST_F(AEStateStoreTest, SparseLayoutKeepsPinnedStates) {
  auto module = straightLineModule();
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  AbstractStateStore store(StateLayout::Sparse);

  const Instruction *head = nthInstruction(main, 0);
  const Instruction *second = nthInstruction(main, 1);
  const Instruction *third = nthInstruction(main, 2);
  ASSERT_NE(head, nullptr);
  ASSERT_NE(second, nullptr);
  ASSERT_NE(third, nullptr);

  // A block head: written by handleBlock, observable by widening and joins.
  store.set(head, AbstractState());
  EXPECT_TRUE(store.isPinned(head));

  store.inherit(second, AbstractState());
  store.inherit(third, AbstractState());

  // Pinned states survive the look-behind sweep.
  EXPECT_TRUE(store.contains(head));
  EXPECT_TRUE(store.contains(third));
  EXPECT_EQ(store.size(), 2u);
}

TEST_F(AEStateStoreTest, ReleaseIsANoOpForPinnedStates) {
  auto module = straightLineModule();
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  AbstractStateStore store(StateLayout::Sparse);
  const Instruction *head = nthInstruction(main, 0);
  const Instruction *tail = nthInstruction(main, 3);
  ASSERT_NE(head, nullptr);
  ASSERT_NE(tail, nullptr);

  store.set(head, AbstractState());
  store.release(head);
  EXPECT_TRUE(store.contains(head));

  store.inherit(tail, AbstractState());
  store.release(tail);
  EXPECT_FALSE(store.contains(tail));

  store.clear();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.pinnedCount(), 0u);
}

TEST_F(AEStateStoreTest, ParsesLayoutNames) {
  StateLayout layout = StateLayout::Dense;
  ASSERT_TRUE(parseStateLayout("dense", layout));
  EXPECT_EQ(layout, StateLayout::Dense);

  // SVF's middle level: def-site intermediate values, pruned states.
  ASSERT_TRUE(parseStateLayout("semi-sparse", layout));
  EXPECT_EQ(layout, StateLayout::SemiSparse);
  EXPECT_TRUE(AbstractStateStore(layout).usesDefSiteVars());
  EXPECT_TRUE(AbstractStateStore(layout).isSparse());

  ASSERT_TRUE(parseStateLayout("sparse", layout));
  EXPECT_EQ(layout, StateLayout::Sparse);
  EXPECT_TRUE(AbstractStateStore(layout).usesDefSiteVars());

  EXPECT_FALSE(parseStateLayout("bogus", layout));
  EXPECT_FALSE(parseStateLayout("", layout));

  EXPECT_STREQ(toString(StateLayout::Dense), "dense");
  EXPECT_STREQ(toString(StateLayout::SemiSparse), "semi-sparse");
  EXPECT_STREQ(toString(StateLayout::Sparse), "sparse");
}

TEST_F(AEStateStoreTest, DenseLayoutKeepsValuesPerInstruction) {
  auto module = straightLineModule();
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  AbstractStateStore store(StateLayout::Dense);
  EXPECT_FALSE(store.usesDefSiteVars());

  for (unsigned i = 0; i < 3; ++i) {
    const Instruction *inst = nthInstruction(main, i);
    ASSERT_NE(inst, nullptr);
    AbstractState state;
    state[100 + i] = AbstractValue(IntervalValue(static_cast<int64_t>(i)));
    store.set(inst, state);
  }
  // Nothing is shared: every state owns its own map.
  EXPECT_TRUE(store.defSiteVars().empty());
}

TEST_F(AEStateStoreTest, DefSiteLayoutStoresValuesOnce) {
  auto module = straightLineModule();
  ASSERT_NE(module, nullptr);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);

  AbstractStateStore store(StateLayout::SemiSparse);
  ASSERT_TRUE(store.usesDefSiteVars());

  const Instruction *first = nthInstruction(main, 0);
  const Instruction *second = nthInstruction(main, 1);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  AbstractState a;
  a[42] = AbstractValue(IntervalValue(7));
  store.set(first, a);

  // The value lives in the run-wide table...
  ASSERT_EQ(store.defSiteVars().count(42), 1u);
  // ...and the per-instruction state reads it through the shared table.
  const AbstractState *state = store.find(first);
  ASSERT_NE(state, nullptr);
  ASSERT_TRUE(state->usesDefSiteVarTable());
  EXPECT_EQ((*state)[42].getInterval().getIntNumeral(), 7);

  // A second state sees the same entry without copying it.
  store.inherit(second, *state);
  const AbstractState *other = store.find(second);
  ASSERT_NE(other, nullptr);
  ASSERT_TRUE(other->usesDefSiteVarTable());
  EXPECT_EQ((*other)[42].getInterval().getIntNumeral(), 7);
  EXPECT_EQ(store.defSiteVars().size(), 1u);
}

//===----------------------------------------------------------------------===//
// End-to-end: the layout must not change a single finding.
//===----------------------------------------------------------------------===//

TEST_F(AEStateStoreTest, SparseLayoutMatchesDenseOnLoopOverflow) {
  auto module = parseModule(R"(
    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %arr = alloca [10 x i32]
      %idx = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 %i
      store i32 %i, i32* %idx
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, 20
      br i1 %cond, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  const AEResult dense = runAE(module.get(), true, false,
                               AbstractInterpretation::WIDEN_NARROW, 3u, false,
                               false, StateLayout::Dense);
  const AEResult sparse = runAE(module.get(), true, false,
                                AbstractInterpretation::WIDEN_NARROW, 3u, false,
                                false, StateLayout::Sparse);

  EXPECT_EQ(dense.overflow_bugs, sparse.overflow_bugs);
  EXPECT_EQ(dense.null_bugs, sparse.null_bugs);
  EXPECT_EQ(dense.uaf_bugs, sparse.uaf_bugs);
  EXPECT_EQ(dense.invalid_free_bugs, sparse.invalid_free_bugs);
  EXPECT_EQ(dense.pending_checkpoints, sparse.pending_checkpoints);
}

TEST_F(AEStateStoreTest, SparseLayoutMatchesDenseInterprocedurally) {
  auto module = parseModule(R"(
    define void @set_field(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32
      call void @set_field(i32* %slot, i32 41)
      %v = load i32, i32* %slot
      %oob = getelementptr inbounds i32, i32* %slot, i32 7
      store i32 %v, i32* %oob
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  const AEResult dense = runAE(module.get(), true, false,
                               AbstractInterpretation::WIDEN_NARROW, 3u, false,
                               false, StateLayout::Dense);
  const AEResult sparse = runAE(module.get(), true, false,
                                AbstractInterpretation::WIDEN_NARROW, 3u, false,
                                false, StateLayout::Sparse);

  EXPECT_EQ(dense.overflow_bugs, sparse.overflow_bugs);
  EXPECT_EQ(dense.null_bugs, sparse.null_bugs);
  EXPECT_EQ(dense.uaf_bugs, sparse.uaf_bugs);
  EXPECT_EQ(dense.invalid_free_bugs, sparse.invalid_free_bugs);
  EXPECT_EQ(dense.mem_leak_bugs, sparse.mem_leak_bugs);
  EXPECT_EQ(dense.pending_checkpoints, sparse.pending_checkpoints);
}

TEST_F(AEStateStoreTest, SparseLayoutMatchesDenseOnHeapLifecycle) {
  auto module = parseModule(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      %p = call i8* @malloc(i64 16)
      store i8 1, i8* %p
      call void @free(i8* %p)
      store i8 2, i8* %p
      %q = call i8* @malloc(i64 8)
      %r = call i8* @malloc(i64 8)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  const AEResult dense = runAE(module.get(), true, true,
                               AbstractInterpretation::WIDEN_NARROW, 3u, false,
                               false, StateLayout::Dense);
  const AEResult sparse = runAE(module.get(), true, true,
                                AbstractInterpretation::WIDEN_NARROW, 3u, false,
                                false, StateLayout::Sparse);

  EXPECT_EQ(dense.uaf_bugs, sparse.uaf_bugs);
  EXPECT_EQ(dense.invalid_free_bugs, sparse.invalid_free_bugs);
  EXPECT_EQ(dense.mem_leak_bugs, sparse.mem_leak_bugs);
  EXPECT_EQ(dense.pending_checkpoints, sparse.pending_checkpoints);
}

TEST_F(AEStateStoreTest, SparseLayoutRetainsFewerStatesThanDense) {
  auto module = parseModule(R"(
    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %a = add i32 %i, 1
      %b = mul i32 %a, 2
      %c = sub i32 %b, 3
      %next = add i32 %c, 1
      %cond = icmp slt i32 %next, 8
      br i1 %cond, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
        false, false, StateLayout::Dense);
  const std::size_t denseSurvivors = ae.getStateStore().size();

  runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
        false, false, StateLayout::Sparse);
  const AbstractStateStore &sparseStore = ae.getStateStore();
  EXPECT_EQ(sparseStore.layout(), StateLayout::Sparse);

  // The sparse layout must retain a strict subset of what dense keeps.
  EXPECT_LT(sparseStore.size(), denseSurvivors);
  EXPECT_GT(sparseStore.stats().released, 0u);
}
