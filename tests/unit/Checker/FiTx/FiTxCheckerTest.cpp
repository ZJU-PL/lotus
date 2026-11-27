#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Detector/DoubleFreeDetector.h"
#include "Checker/FiTx/Detector/DoubleLockDetector.h"
#include "Checker/FiTx/Detector/DoubleUnlockDetector.h"
#include "Checker/FiTx/Detector/MemoryLeakDetector.h"
#include "Checker/FiTx/Detector/NullPointerDetector.h"
#include "Checker/FiTx/Detector/ReferenceCounterDetector.h"
#include "Checker/FiTx/Detector/UnreferenceCounterDetector.h"
#include "Checker/FiTx/Detector/UseAfterFreeDetector.h"
#include "Checker/FiTx/Detector/UseBeforeInitDetector.h"
#include "Checker/FiTx/FrameworkIR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/Framework/BugReportMgr.h"
#include "TestUtils/LLVMHelpers.h"

#include <string>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::parseModule;
using DetectorDef = void (*)(fitx::StateManager &);

class TestFiTxPass final : public fitx::FrameworkPass {
public:
  explicit TestFiTxPass(std::vector<DetectorDef> defs)
      : defs_(std::move(defs)) {}

  void defineStates() override {
    for (DetectorDef def : defs_) {
      fitx::StateManager mgr;
      def(mgr);
      addStateManager(std::move(mgr));
    }
  }

private:
  std::vector<DetectorDef> defs_;
};

void runFiTx(llvm::Module &M, std::vector<DetectorDef> defs) {
  fitx::setDebugLogging(false);
  BugReportMgr::get_instance().clear_all_reports();

  llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeLoopInfoWrapperPassPass(registry);

  llvm::legacy::PassManager pm;
  pm.add(new llvm::LoopInfoWrapperPass());
  pm.add(new ir_generator::IRGenerator());
  pm.add(new TestFiTxPass(std::move(defs)));
  pm.run(M);
}

int countReportsOfType(const std::string &bug_name) {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  int ty_id = mgr.find_bug_type(bug_name);
  if (ty_id < 0)
    return 0;
  const auto *reports = mgr.get_reports_for_type(ty_id);
  return reports ? static_cast<int>(reports->size()) : 0;
}

TEST(FiTxCheckerTest, DetectsDoubleFreeAndSkipsReassignedPointer) {
  llvm::LLVMContext ctx;
  auto bad = parseModule(ctx, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @bad_df() {
    entry:
      %p = alloca i8*, align 8
      %m = call i8* @malloc(i64 16)
      store i8* %m, i8** %p, align 8
      %v1 = load i8*, i8** %p, align 8
      call void @free(i8* %v1)
      %v2 = load i8*, i8** %p, align 8
      call void @free(i8* %v2)
      ret void
    }
  )");
  ASSERT_NE(bad, nullptr);

  runFiTx(*bad, {DoubleFree::define_states});
  EXPECT_GE(countReportsOfType("Double Free"), 1);

  auto clean = parseModule(ctx, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @clean_reassigned() {
    entry:
      %p = alloca i8*, align 8
      %m1 = call i8* @malloc(i64 16)
      store i8* %m1, i8** %p, align 8
      %v1 = load i8*, i8** %p, align 8
      call void @free(i8* %v1)
      %m2 = call i8* @malloc(i64 32)
      store i8* %m2, i8** %p, align 8
      %v2 = load i8*, i8** %p, align 8
      call void @free(i8* %v2)
      ret void
    }
  )");
  ASSERT_NE(clean, nullptr);

  runFiTx(*clean, {DoubleFree::define_states});
  EXPECT_EQ(countReportsOfType("Double Free"), 0);
}

TEST(FiTxCheckerTest, DetectsUseAfterFree) {
  llvm::LLVMContext ctx;
  auto mod = parseModule(ctx, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @bad_uaf() {
    entry:
      %p = alloca i8*, align 8
      %m = call i8* @malloc(i64 16)
      store i8* %m, i8** %p, align 8
      %v1 = load i8*, i8** %p, align 8
      call void @free(i8* %v1)
      %v2 = load i8*, i8** %p, align 8
      %c = load i8, i8* %v2, align 1
      %add = add i8 %c, 1
      ret i8 %add
    }
  )");
  ASSERT_NE(mod, nullptr);

  runFiTx(*mod, {UseAfterFree::defineStates});
  EXPECT_GE(countReportsOfType("Use After Free"), 1);
}

TEST(FiTxCheckerTest, DetectsMemoryLeakAndAllowsFreedAllocation) {
  llvm::LLVMContext ctx;
  auto bad = parseModule(ctx, R"(
    declare i8* @malloc(i64)

    define void @bad_leak() {
    entry:
      %p = alloca i8*, align 8
      %m = call i8* @malloc(i64 64)
      store i8* %m, i8** %p, align 8
      ret void
    }
  )");
  ASSERT_NE(bad, nullptr);

  runFiTx(*bad, {MemoryLeak::defineStates});
  EXPECT_GE(countReportsOfType("Memory Leak"), 1);

  auto clean = parseModule(ctx, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @clean_freed() {
    entry:
      %p = alloca i8*, align 8
      %m = call i8* @malloc(i64 64)
      store i8* %m, i8** %p, align 8
      %v = load i8*, i8** %p, align 8
      call void @free(i8* %v)
      ret void
    }
  )");
  ASSERT_NE(clean, nullptr);

  runFiTx(*clean, {MemoryLeak::defineStates});
  EXPECT_EQ(countReportsOfType("Memory Leak"), 0);
}

TEST(FiTxCheckerTest, DetectsNullPointerDereferenceDirectAndViaBranch) {
  llvm::LLVMContext ctx;
  auto direct = parseModule(ctx, R"(
    define i32 @bad_null_direct() {
    entry:
      %p = alloca i32*, align 8
      store i32* null, i32** %p, align 8
      %q = load i32*, i32** %p, align 8
      %v = load i32, i32* %q, align 4
      %r = add i32 %v, 1
      ret i32 %r
    }
  )");
  ASSERT_NE(direct, nullptr);

  runFiTx(*direct, {NullPointer::defineStates});
  EXPECT_GE(countReportsOfType("Null Pointer Dereference"), 1);

  auto branch_null = parseModule(ctx, R"(
    declare i32* @get_ptr()

    define i32 @bad_null_branch() {
    entry:
      %p = alloca i32*, align 8
      %raw = call i32* @get_ptr()
      store i32* %raw, i32** %p, align 8
      %v0 = load i32*, i32** %p, align 8
      %isnull = icmp eq i32* %v0, null
      br i1 %isnull, label %on_null, label %on_nonnull

    on_null:
      %v1 = load i32*, i32** %p, align 8
      %x = load i32, i32* %v1, align 4
      %r = add i32 %x, 1
      ret i32 %r

    on_nonnull:
      ret i32 0
    }
  )");
  ASSERT_NE(branch_null, nullptr);

  runFiTx(*branch_null, {[](fitx::StateManager &manager) {
            auto init_args = fitx::StateArgs("init", fitx::StateType::INIT);
            fitx::State &init = manager.createState(init_args);
            auto null_args = fitx::StateArgs("null");
            fitx::State &null = manager.createState(null_args);
            auto use_args =
                fitx::StateArgs("used", fitx::StateType::BUG,
                                fitx::BugNotificationTiming::IMMEDIATE, false);
            fitx::State &use = manager.createState(use_args);
            auto use_rule = fitx::UseValueTransitionRule();
            manager.addTransition(null, use, use_rule);
            auto store_null_rule = fitx::StoreValueTransitionRule(
                fitx::StoreValueTransitionRule::NULL_VAL);
            store_null_rule.setConsiderNullBranch(true);
            manager.addTransition(init, null, store_null_rule);
          }});
  EXPECT_GE(countReportsOfType("Null Pointer Dereference"), 1);

  auto branch_nonnull = parseModule(ctx, R"(
    declare i32* @get_ptr()

    define i32 @clean_nonnull_branch() {
    entry:
      %p = alloca i32*, align 8
      %raw = call i32* @get_ptr()
      store i32* %raw, i32** %p, align 8
      %v0 = load i32*, i32** %p, align 8
      %isnull = icmp eq i32* %v0, null
      br i1 %isnull, label %on_null, label %on_nonnull

    on_null:
      ret i32 0

    on_nonnull:
      %v1 = load i32*, i32** %p, align 8
      %x = load i32, i32* %v1, align 4
      %r = add i32 %x, 1
      ret i32 %r
    }
  )");
  ASSERT_NE(branch_nonnull, nullptr);

  runFiTx(*branch_nonnull, {NullPointer::defineStates});
  EXPECT_EQ(countReportsOfType("Null Pointer Dereference"), 0);
}

TEST(FiTxCheckerTest, DetectsUseBeforeInitialization) {
  llvm::LLVMContext ctx;
  auto bad = parseModule(ctx, R"(
    define i32 @bad_ubi() {
    entry:
      %x = alloca i32, align 4
      %v = load i32, i32* %x, align 4
      %r = add i32 %v, 1
      ret i32 %r
    }
  )");
  ASSERT_NE(bad, nullptr);

  runFiTx(*bad, {UseBeforeInitialization::defineStates});
  EXPECT_GE(countReportsOfType("Use of Uninitialized Value"), 1);

  auto clean = parseModule(ctx, R"(
    define i32 @clean_init() {
    entry:
      %x = alloca i32, align 4
      store i32 42, i32* %x, align 4
      %v = load i32, i32* %x, align 4
      %r = add i32 %v, 1
      ret i32 %r
    }
  )");
  ASSERT_NE(clean, nullptr);

  runFiTx(*clean, {UseBeforeInitialization::defineStates});
  EXPECT_EQ(countReportsOfType("Use of Uninitialized Value"), 0);
}

TEST(FiTxCheckerTest, DetectsDoubleLockAndDoubleUnlock) {
  llvm::LLVMContext ctx;
  auto dl = parseModule(ctx, R"(
    declare void @mutex_lock(i8*)
    declare void @mutex_unlock(i8*)

    define void @bad_double_lock(i8* %m) {
    entry:
      call void @mutex_lock(i8* %m)
      call void @mutex_lock(i8* %m)
      call void @mutex_unlock(i8* %m)
      ret void
    }
  )");
  ASSERT_NE(dl, nullptr);

  runFiTx(*dl, {DoubleLock::define_states});
  EXPECT_GE(countReportsOfType("Double Lock"), 1);

  auto dul = parseModule(ctx, R"(
    declare void @mutex_lock(i8*)
    declare void @mutex_unlock(i8*)

    define void @bad_double_unlock(i8* %m) {
    entry:
      call void @mutex_lock(i8* %m)
      call void @mutex_unlock(i8* %m)
      call void @mutex_unlock(i8* %m)
      ret void
    }
  )");
  ASSERT_NE(dul, nullptr);

  runFiTx(*dul, {DoubleUnlock::defineStates});
  EXPECT_GE(countReportsOfType("Double Unlock"), 1);
}

TEST(FiTxCheckerTest, DetectsReferenceCountImbalanceAndUnderflow) {
  llvm::LLVMContext ctx;
  auto rc = parseModule(ctx, R"(
    declare void @kref_get(i8*)
    declare void @kref_put(i8*)

    define void @bad_ref_inc(i8* %r) {
    entry:
      call void @kref_get(i8* %r)
      call void @kref_get(i8* %r)
      ret void
    }
  )");
  ASSERT_NE(rc, nullptr);

  runFiTx(*rc, {ReferenceCounter::defineStates});
  EXPECT_GE(countReportsOfType("Reference Count Imbalance"), 1);

  auto urc = parseModule(ctx, R"(
    declare void @kref_get(i8*)
    declare void @kref_put(i8*)

    define void @bad_ref_dec(i8* %r) {
    entry:
      call void @kref_put(i8* %r)
      call void @kref_put(i8* %r)
      ret void
    }
  )");
  ASSERT_NE(urc, nullptr);

  runFiTx(*urc, {UnreferenceCounter::defineStates});
  EXPECT_GE(countReportsOfType("Reference Count Underflow"), 1);
}

TEST(FiTxCheckerTest, HandlesAnonymousLiteralStructWithoutCrashing) {
  llvm::LLVMContext ctx;
  auto mod = parseModule(ctx, R"(
    declare void @free(i8*)

    define void @literal_struct_df({ i8*, i32 }* %s) {
    entry:
      %f0 = getelementptr inbounds { i8*, i32 }, { i8*, i32 }* %s, i64 0, i32 0
      %p1 = load i8*, i8** %f0, align 8
      call void @free(i8* %p1)
      %p2 = load i8*, i8** %f0, align 8
      call void @free(i8* %p2)
      ret void
    }
  )");
  ASSERT_NE(mod, nullptr);

  runFiTx(*mod, {DoubleFree::define_states});
  EXPECT_GE(countReportsOfType("Double Free"), 1);
}

TEST(FiTxCheckerTest, HandlesInt128ConstantsAndNegativeGepSafely) {
  llvm::LLVMContext ctx;
  auto mod = parseModule(ctx, R"(
    %struct.Box = type { i8*, i64 }
    declare void @free(i8*)

    define i128 @wide_and_neg_gep(%struct.Box* %b, i128 %k) {
    entry:
      %prev = getelementptr %struct.Box, %struct.Box* %b, i64 -1, i32 0
      %p = load i8*, i8** %prev, align 8
      switch i128 %k, label %def [
        i128 340282366920938463463374607431768211455, label %case_big
      ]

    case_big:
      call void @free(i8* %p)
      ret i128 170141183460469231731687303715884105727

    def:
      ret i128 0
    }
  )");
  ASSERT_NE(mod, nullptr);

  runFiTx(*mod,
          {DoubleFree::define_states, UseAfterFree::defineStates,
           MemoryLeak::defineStates, NullPointer::defineStates,
           UseBeforeInitialization::defineStates});
  EXPECT_EQ(countReportsOfType("Double Free"), 0);
}

TEST(FiTxCheckerTest, TerminatesOnIrreducibleLoopAndUnreachableBlocks) {
  llvm::LLVMContext ctx;
  auto mod = parseModule(ctx, R"(
    declare void @free(i8*)

    define void @irreducible(i8* %p, i1 %c1, i1 %c2) {
    entry:
      br i1 %c1, label %loopA, label %loopB

    loopA:
      br i1 %c2, label %loopB, label %exit

    loopB:
      br i1 %c1, label %loopA, label %exit

    dead_cycle:
      br label %dead_cycle

    exit:
      call void @free(i8* %p)
      ret void
    }
  )");
  ASSERT_NE(mod, nullptr);

  runFiTx(*mod, {DoubleFree::define_states, NullPointer::defineStates});
  EXPECT_EQ(countReportsOfType("Double Free"), 0);
}

TEST(FiTxCheckerTest, PropagatesReturnCodeAwareInterproceduralSummary) {
  llvm::LLVMContext ctx;
  auto mod = parseModule(ctx, R"(
    declare void @free(i8*)

    define i32 @maybe_free(i8* %p, i1 %ok) {
    entry:
      %ret = alloca i32, align 4
      br i1 %ok, label %do_free, label %fail

    do_free:
      call void @free(i8* %p)
      store i32 0, i32* %ret, align 4
      br label %exit

    fail:
      store i32 -1, i32* %ret, align 4
      br label %exit

    exit:
      %rv = load i32, i32* %ret, align 4
      ret i32 %rv
    }

    define void @caller_bad(i8* %buf, i1 %flag) {
    entry:
      %rc = call i32 @maybe_free(i8* %buf, i1 %flag)
      %succeeded = icmp sge i32 %rc, 0
      br i1 %succeeded, label %then_free_again, label %done

    then_free_again:
      call void @free(i8* %buf)
      br label %done

    done:
      ret void
    }
  )");
  ASSERT_NE(mod, nullptr);

  runFiTx(*mod, {DoubleFree::define_states});
  EXPECT_GE(countReportsOfType("Double Free"), 1);

  auto clean = parseModule(ctx, R"(
    declare void @free(i8*)

    define i32 @maybe_free(i8* %p, i1 %ok) {
    entry:
      %ret = alloca i32, align 4
      br i1 %ok, label %do_free, label %fail

    do_free:
      call void @free(i8* %p)
      store i32 0, i32* %ret, align 4
      br label %exit

    fail:
      store i32 -1, i32* %ret, align 4
      br label %exit

    exit:
      %rv = load i32, i32* %ret, align 4
      ret i32 %rv
    }

    define void @caller_clean(i8* %buf, i1 %flag) {
    entry:
      %rc = call i32 @maybe_free(i8* %buf, i1 %flag)
      %failed = icmp slt i32 %rc, 0
      br i1 %failed, label %cleanup_free, label %done

    cleanup_free:
      call void @free(i8* %buf)
      br label %done

    done:
      ret void
    }
  )");
  ASSERT_NE(clean, nullptr);

  runFiTx(*clean, {DoubleFree::define_states});
  EXPECT_EQ(countReportsOfType("Double Free"), 0);
}

TEST(FiTxCheckerTest, RepeatedMultiModuleRunsResetSingletonsCleanly) {
  const std::vector<DetectorDef> all_defs = {
      DoubleFree::define_states,
      DoubleLock::define_states,
      DoubleUnlock::defineStates,
      MemoryLeak::defineStates,
      NullPointer::defineStates,
      UseAfterFree::defineStates,
      UseBeforeInitialization::defineStates,
      ReferenceCounter::defineStates,
      UnreferenceCounter::defineStates,
  };

  for (int iter = 0; iter < 3; ++iter) {
    llvm::LLVMContext ctx;
    auto mod = parseModule(ctx, R"(
      declare i8* @malloc(i64)
      declare void @free(i8*)

      define void @multi_run_df() {
      entry:
        %p = alloca i8*, align 8
        %m = call i8* @malloc(i64 8)
        store i8* %m, i8** %p, align 8
        %a = load i8*, i8** %p, align 8
        call void @free(i8* %a)
        %b = load i8*, i8** %p, align 8
        call void @free(i8* %b)
        ret void
      }
    )");
    ASSERT_NE(mod, nullptr);

    runFiTx(*mod, all_defs);
    EXPECT_GE(countReportsOfType("Double Free"), 1) << "iteration " << iter;
  }
}

} // namespace
