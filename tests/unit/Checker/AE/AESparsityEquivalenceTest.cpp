/**
 * @file AESparsityEquivalenceTest.cpp
 * @brief Dense vs def-site (semi-sparse / sparse) equivalence for AE.
 *
 * The sparsity level is a *storage* decision: intermediate values move from a
 * per-instruction copy into one run-wide def-site table, and unobservable
 * states are dropped. Neither may change a single finding.
 *
 * Widening is the one place where that is not free: a loop head must still join
 * and widen its cycle's values, which are materialized out of the shared table
 * and scattered back (`AbstractInterpretation::cycleValueIds`). These programs
 * exercise exactly those shapes -- nested loops, self/mutual recursion,
 * switch-heavy diamonds and a mix of all of them.
 */

#include "AECheckerTestSupport.h"

#include <array>
#include <string>

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;

class AESparsityEquivalenceTest : public AECheckerTest {
protected:
  struct Findings {
    size_t overflow;
    size_t null;
    size_t uaf;
    size_t invalidFree;
    size_t leak;
    size_t checkpoints;
  };

  Findings run(Module *module, StateLayout layout, bool memLeak = false) {
    const AEResult r =
        runAE(module, true, memLeak, AbstractInterpretation::WIDEN_NARROW, 3u,
              false, false, layout);
    return {r.overflow_bugs,     r.null_bugs,     r.uaf_bugs,
            r.invalid_free_bugs, r.mem_leak_bugs, r.pending_checkpoints};
  }

  /// Runs all three layouts and requires identical findings.
  void expectEquivalent(Module *module, const char *what,
                        bool memLeak = false) {
    const Findings dense = run(module, StateLayout::Dense, memLeak);
    const Findings semi = run(module, StateLayout::SemiSparse, memLeak);
    const Findings sparse = run(module, StateLayout::Sparse, memLeak);

    EXPECT_EQ(dense.overflow, semi.overflow) << what << " (semi-sparse)";
    EXPECT_EQ(dense.null, semi.null) << what << " (semi-sparse)";
    EXPECT_EQ(dense.uaf, semi.uaf) << what << " (semi-sparse)";
    EXPECT_EQ(dense.invalidFree, semi.invalidFree) << what << " (semi-sparse)";
    EXPECT_EQ(dense.leak, semi.leak) << what << " (semi-sparse)";
    EXPECT_EQ(dense.checkpoints, semi.checkpoints) << what << " (semi-sparse)";

    EXPECT_EQ(dense.overflow, sparse.overflow) << what << " (sparse)";
    EXPECT_EQ(dense.null, sparse.null) << what << " (sparse)";
    EXPECT_EQ(dense.uaf, sparse.uaf) << what << " (sparse)";
    EXPECT_EQ(dense.invalidFree, sparse.invalidFree) << what << " (sparse)";
    EXPECT_EQ(dense.leak, sparse.leak) << what << " (sparse)";
    EXPECT_EQ(dense.checkpoints, sparse.checkpoints) << what << " (sparse)";
  }

  std::unique_ptr<Module> ir(const char *source) { return parseModule(source); }
};

TEST_F(AESparsityEquivalenceTest, NestedLoops) {
  auto module = ir(R"(
    define i32 @main() {
    entry:
      %a = alloca [16 x i32]
      br label %outer
    outer:
      %i = phi i32 [ 0, %entry ], [ %i.next, %outer.latch ]
      br label %inner
    inner:
      %j = phi i32 [ 0, %outer ], [ %j.next, %inner ]
      %idx = add i32 %i, %j
      %p = getelementptr inbounds [16 x i32], [16 x i32]* %a, i32 0, i32 %idx
      store i32 %idx, i32* %p
      %j.next = add i32 %j, 1
      %jc = icmp slt i32 %j.next, 5
      br i1 %jc, label %inner, label %outer.latch
    outer.latch:
      %i.next = add i32 %i, 1
      %ic = icmp slt i32 %i.next, 4
      br i1 %ic, label %outer, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  expectEquivalent(module.get(), "nested loops");
}

TEST_F(AESparsityEquivalenceTest, SelfRecursion) {
  auto module = ir(R"(
    define void @fill(i32* %base, i32 %n) {
    entry:
      %c = icmp sgt i32 %n, 0
      br i1 %c, label %body, label %done
    body:
      %idx = sext i32 %n to i64
      %q = getelementptr inbounds i32, i32* %base, i64 %idx
      store i32 %n, i32* %q
      %n2 = sub i32 %n, 1
      call void @fill(i32* %base, i32 %n2)
      br label %done
    done:
      ret void
    }

    define i32 @main() {
    entry:
      %a = alloca [8 x i32]
      %p = getelementptr inbounds [8 x i32], [8 x i32]* %a, i32 0, i32 0
      call void @fill(i32* %p, i32 20)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  expectEquivalent(module.get(), "self recursion");
}

TEST_F(AESparsityEquivalenceTest, MutualRecursion) {
  auto module = ir(R"(
    define void @ping(i32* %p, i32 %n) {
    entry:
      %c = icmp sgt i32 %n, 0
      br i1 %c, label %body, label %done
    body:
      %q = getelementptr inbounds i32, i32* %p, i32 %n
      store i32 %n, i32* %q
      %n2 = sub i32 %n, 2
      call void @pong(i32* %p, i32 %n2)
      br label %done
    done:
      ret void
    }

    define void @pong(i32* %p, i32 %n) {
    entry:
      %c = icmp sgt i32 %n, 1
      br i1 %c, label %body, label %done
    body:
      %q = getelementptr inbounds i32, i32* %p, i32 %n
      store i32 %n, i32* %q
      %n2 = sub i32 %n, 1
      call void @ping(i32* %p, i32 %n2)
      br label %done
    done:
      ret void
    }

    define i32 @main() {
    entry:
      %a = alloca [4 x i32]
      %p = getelementptr inbounds [4 x i32], [4 x i32]* %a, i32 0, i32 0
      call void @ping(i32* %p, i32 9)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  expectEquivalent(module.get(), "mutual recursion");
}

TEST_F(AESparsityEquivalenceTest, SwitchHeavy) {
  auto module = ir(R"(
    define i32 @main(i32 %k) {
    entry:
      switch i32 %k, label %dflt [
        i32 0, label %c0
        i32 1, label %c1
        i32 2, label %c2
      ]
    c0:
      br label %join
    c1:
      br label %join
    c2:
      br label %join
    dflt:
      br label %join
    join:
      %v = phi i32 [ 1, %c0 ], [ 2, %c1 ], [ 3, %c2 ], [ 9, %dflt ]
      %a = alloca [4 x i32]
      %p = getelementptr inbounds [4 x i32], [4 x i32]* %a, i32 0, i32 %v
      store i32 %v, i32* %p
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  expectEquivalent(module.get(), "switch heavy");
}

TEST_F(AESparsityEquivalenceTest, HeapLifecycleInsideLoop) {
  auto module = ir(R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
      %p = call i8* @malloc(i64 16)
      store i8 1, i8* %p
      call void @free(i8* %p)
      store i8 2, i8* %p
      %i.next = add i32 %i, 1
      %c = icmp slt i32 %i.next, 3
      br i1 %c, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  expectEquivalent(module.get(), "heap lifecycle in loop", /*memLeak=*/true);
}

TEST_F(AESparsityEquivalenceTest, LoopCallingARecursiveHelper) {
  auto module = ir(R"(
    define void @touch(i32* %p, i32 %n) {
    entry:
      %c = icmp sgt i32 %n, 0
      br i1 %c, label %body, label %done
    body:
      %q = getelementptr inbounds i32, i32* %p, i32 %n
      store i32 %n, i32* %q
      %n2 = sub i32 %n, 1
      call void @touch(i32* %p, i32 %n2)
      br label %done
    done:
      ret void
    }

    define i32 @main() {
    entry:
      %a = alloca [6 x i32]
      %base = getelementptr inbounds [6 x i32], [6 x i32]* %a, i32 0, i32 0
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
      call void @touch(i32* %base, i32 %i)
      %i.next = add i32 %i, 1
      %c = icmp slt i32 %i.next, 8
      br i1 %c, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);
  expectEquivalent(module.get(), "loop calling recursive helper");
}

TEST_F(AESparsityEquivalenceTest, DefSiteLayoutActuallySharesValues) {
  // Guards the mechanism itself, not just the findings: in the def-site layout
  // the run-wide table must hold the intermediate values and the per-node
  // states must be bound to it.
  auto module = ir(R"(
    define i32 @main() {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %next = add i32 %i, 1
      %c = icmp slt i32 %next, 4
      br i1 %c, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();

  runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
        false, false, StateLayout::Dense);
  EXPECT_TRUE(ae.getStateStore().defSiteVars().empty());

  runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
        false, false, StateLayout::SemiSparse);
  const AbstractStateStore &store = ae.getStateStore();
  EXPECT_TRUE(store.usesDefSiteVars());
  EXPECT_FALSE(store.defSiteVars().empty());
  EXPECT_LT(store.defSiteVars().size(), 64u)
      << "values must be stored per definition, not per instruction";
}

TEST_F(AESparsityEquivalenceTest, SparseBuildsSVFGGraph) {
  auto module = ir(R"(
    define i32 @main() {
      %slot = alloca i32
      store i32 1, i32* %slot
      %value = load i32, i32* %slot
      ret i32 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  runAE(module.get(), true, false, AbstractInterpretation::WIDEN_NARROW, 3u,
        false, false, StateLayout::Sparse);
  AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
  EXPECT_TRUE(ae.isSVFGBackedSparseActive());
  EXPECT_GT(ae.getSVFGSparseLoadQueryCount(), 0u);
}
