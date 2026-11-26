#include "llvm/IR/IRBuilder.h"

#include "IR/PDG/Models/ValueFacts.h"
#include "IR/PDG/Construction/GraphBuilder.h"
#include "IR/PDG/Rules/RuleEngine.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace pdg;

namespace {
class PDGRuleCoverageTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> module;
  ProgramGraph &graph = ProgramGraph::getInstance();
  void SetUp() override { graph.reset(); }
  void TearDown() override { graph.reset(); }
  void load(const char *ir) {
    module =
        lotus::unittest::parseModuleChecked(context, ir, "RuleCoverageTest");
    ASSERT_NE(module, nullptr);
    pdg::GraphBuilder(graph).build(*module);
  }
  RuleEngineResult run(const std::string &id) {
    return RuleEngine(graph).analyze({id}, {}, {}, module.get());
  }
};

TEST_F(PDGRuleCoverageTest,
       IndirectTargetsRequireImmutableOrUnclobberedValues) {
  load(R"(
    @fixed = constant i8* (i8*)* @gets
    @mutable = global i8* (i8*)* @gets
    declare i8* @gets(i8*)
    declare i8* @other(i8*)
    declare void @clobber(i8* (i8*)**)
    define void @calls(i8* %p, i1 %condition) {
      %slot = alloca i8* (i8*)*
      store i8* (i8*)* @gets, i8* (i8*)** %slot
      %local = load i8* (i8*)*, i8* (i8*)** %slot
      %bad_local = call i8* %local(i8* %p)
      %constant = load i8* (i8*)*, i8* (i8*)** @fixed
      %bad_constant = call i8* %constant(i8* %p)
      %unknown = load i8* (i8*)*, i8* (i8*)** @mutable
      %not_proven = call i8* %unknown(i8* %p)
      %choice = select i1 %condition, i8* (i8*)* @gets, i8* (i8*)* @other
      %ambiguous = call i8* %choice(i8* %p)
      %same = select i1 %condition, i8* (i8*)* @gets, i8* (i8*)* @gets
      %bad_same = call i8* %same(i8* %p)
      call void @clobber(i8* (i8*)** %slot)
      %changed = load i8* (i8*)*, i8* (i8*)** %slot
      %not_stale = call i8* %changed(i8* %p)
      ret void
    }
  )");
  EXPECT_EQ(run("cpp/dangerous-function-overflow").findings.size(), 3u);
}

TEST_F(PDGRuleCoverageTest, DynamicIndicesHonorRangesAndControllingEdges) {
  load(R"(
    target datalayout = "e-p:64:64"
    define i8 @ranges(i8 %input) {
      %a = alloca [8 x i8]
      %masked = and i8 %input, 7
      %outside = or i8 %masked, 8
      %bad_pointer = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i8 %outside
      %bad = load i8, i8* %bad_pointer
      %good_pointer = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i8 %masked
      %good = load i8, i8* %good_pointer
      ret i8 %bad
    }
    define i8 @guarded(i64 %index) {
      %a = alloca [8 x i8]
      %out = icmp uge i64 %index, 8
      br i1 %out, label %bad_block, label %good_block
    bad_block:
      %bad_pointer = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i64 %index
      %bad = load i8, i8* %bad_pointer
      ret i8 %bad
    good_block:
      %good_pointer = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i64 %index
      %good = load i8, i8* %good_pointer
      ret i8 %good
    }
    define i8 @unknown(i64 %index) {
      %a = alloca [8 x i8]
      %p = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i64 %index
      %value = load i8, i8* %p
      ret i8 %value
    }
  )");
  auto result = run("cpp/overflow-buffer");
  ASSERT_EQ(result.findings.size(), 2u);
  for (const auto &finding : result.findings)
    EXPECT_EQ(finding.site->getValue()->getName(), "bad");
}

TEST_F(PDGRuleCoverageTest, DynamicIndicesUseTheTargetPointerIndexWidth) {
  load(R"(
    target datalayout = "e-p:32:32"
    define i8 @wrapped(i64 %input) {
      %a = alloca [8 x i8]
      %masked = and i64 %input, 7
      %wrapped = add i64 %masked, 4294967296
      %safe_pointer = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i64 %wrapped
      %safe = load i8, i8* %safe_pointer
      %outside = add i64 %wrapped, 8
      %bad_pointer = getelementptr [8 x i8], [8 x i8]* %a, i64 0, i64 %outside
      %bad = load i8, i8* %bad_pointer
      ret i8 %bad
    }
  )");
  auto result = run("cpp/overflow-buffer");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings[0].site->getValue()->getName(), "bad");
}

TEST_F(PDGRuleCoverageTest, UniquePtrCleanupReleaseAndResetKeepObjectIdentity) {
  load(R"(
    target datalayout = "e-p:64:64"
    %Owner = type { i32* }
    declare i8* @_Znwm(i64)
    declare void @_ZdlPv(i8*)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%Owner*, i32*)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner*)
    declare i32* @_ZNKSt10unique_ptrIiSt14default_deleteIiEE3getEv(%Owner*)
    declare i32* @_ZNSt10unique_ptrIiSt14default_deleteIiEE7releaseEv(%Owner*)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEE5resetEPi(%Owner*, i32*)
    define i32 @cleanup() {
      %owner = alloca %Owner
      %raw = call i8* @_Znwm(i64 4)
      %p = bitcast i8* %raw to i32*
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%Owner* %owner, i32* %p)
      %borrowed = call i32* @_ZNKSt10unique_ptrIiSt14default_deleteIiEE3getEv(%Owner* %owner)
      store i32 1, i32* %borrowed
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner* %owner)
      %bad = load i32, i32* %p
      ret i32 %bad
    }
    define void @released() {
      %owner = alloca %Owner
      %raw = call i8* @_Znwm(i64 4)
      %p = bitcast i8* %raw to i32*
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%Owner* %owner, i32* %p)
      %released = call i32* @_ZNSt10unique_ptrIiSt14default_deleteIiEE7releaseEv(%Owner* %owner)
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner* %owner)
      store i32 1, i32* %released
      %r = bitcast i32* %released to i8*
      call void @_ZdlPv(i8* %r)
      ret void
    }
    define void @reset() {
      %owner = alloca %Owner
      %raw = call i8* @_Znwm(i64 4)
      %p = bitcast i8* %raw to i32*
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%Owner* %owner, i32* %p)
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEE5resetEPi(%Owner* %owner, i32* null)
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner* %owner)
      ret void
    }
  )");
  auto uaf = run("cpp/use-after-free");
  ASSERT_EQ(uaf.findings.size(), 1u);
  EXPECT_EQ(uaf.findings[0].site->getValue()->getName(), "bad");
  EXPECT_TRUE(run("cpp/double-free").findings.empty());
  EXPECT_TRUE(run("cpp/memory-never-freed").findings.empty());
}

TEST_F(PDGRuleCoverageTest, UniquePtrCleanupFollowsExceptionalControlFlow) {
  load(R"(
    %Owner = type { i32* }
    declare i8* @_Znwm(i64)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%Owner*, i32*)
    declare void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner*)
    declare void @work()
    declare i32 @__gxx_personality_v0(...)
    define void @cleanup() personality i8* bitcast (i32 (...)* @__gxx_personality_v0 to i8*) {
      %owner = alloca %Owner
      %raw = call i8* @_Znwm(i64 4)
      %p = bitcast i8* %raw to i32*
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEEC1EPi(%Owner* %owner, i32* %p)
      invoke void @work() to label %normal unwind label %exception
    normal:
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner* %owner)
      ret void
    exception:
      %landing = landingpad { i8*, i32 } cleanup
      call void @_ZNSt10unique_ptrIiSt14default_deleteIiEED1Ev(%Owner* %owner)
      %bad = load i32, i32* %p
      resume { i8*, i32 } %landing
    }
  )");
  auto result = run("cpp/use-after-free");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings[0].site->getValue()->getName(), "bad");
  EXPECT_TRUE(run("cpp/double-free").findings.empty());
}

TEST_F(PDGRuleCoverageTest, FilesystemChecksRequireStablePathsAndSeparateUse) {
  load(R"(
    @path = private constant [5 x i8] c"file\00"
    @other = private constant [6 x i8] c"other\00"
    declare i32 @access(i8*, i32)
    declare i32 @open(i8*, i32, ...)
    declare void @mutate(i8*)
    define void @cases(i8* %mutable) {
      %path = getelementptr [5 x i8], [5 x i8]* @path, i64 0, i64 0
      %other = getelementptr [6 x i8], [6 x i8]* @other, i64 0, i64 0
      %check = call i32 @access(i8* %path, i32 0)
      %ok = icmp eq i32 %check, 0
      br i1 %ok, label %use, label %exit
    use:
      %bad = call i32 (i8*, i32, ...) @open(i8* %path, i32 0)
      br label %exit
    exit:
      %check2 = call i32 @access(i8* %path, i32 0)
      %different = call i32 (i8*, i32, ...) @open(i8* %other, i32 0)
      %check3 = call i32 @access(i8* %mutable, i32 0)
      call void @mutate(i8* %mutable)
      %changed = call i32 (i8*, i32, ...) @open(i8* %mutable, i32 0)
      %check4 = call i32 @access(i8* %path, i32 0)
      %exclusive = call i32 (i8*, i32, ...) @open(i8* %path, i32 192, i32 384)
      ret void
    }
    !llvm.dbg.cu = !{!0}
    !llvm.module.flags = !{!5}
    !0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, macros: !2)
    !1 = !DIFile(filename: "file.c", directory: "/tmp")
    !2 = !{!3, !4}
    !3 = !DIMacro(type: DW_MACINFO_define, line: 1, name: "O_CREAT", value: "64")
    !4 = !DIMacro(type: DW_MACINFO_define, line: 2, name: "O_EXCL", value: "128")
    !5 = !{i32 2, !"Debug Info Version", i32 3}
  )");
  auto result = run("lotus/filesystem-toctou");
  ASSERT_EQ(result.findings.size(), 1u);
  EXPECT_EQ(result.findings[0].site->getValue()->getName(), "bad");
  PDGQueryOptions limited;
  limited.limits.max_states = 1;
  EXPECT_TRUE(
      RuleEngine(graph)
          .analyze({"lotus/filesystem-toctou"}, {}, limited, module.get())
          .diagnostics.state_limit_hit);
}

TEST_F(PDGRuleCoverageTest,
       ConcurrentWorkersSeparateAtomicsLocksAndJoinedTasks) {
  load(R"(
    @shared = global i32 0
    @atomic = global i32 0
    @protected = global i32 0
    @tls = thread_local global i32 0
    @first = global i8 0
    @second = global i8 0
    declare i32 @pthread_create(i64*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i64, i8**)
    declare void @_ZNSt5mutex4lockEv(i8*)
    declare void @_ZNSt5mutex6unlockEv(i8*)
    define i8* @left(i8* %arg) {
      store i32 1, i32* @shared
      store atomic i32 1, i32* @atomic monotonic, align 4
      store i32 1, i32* @tls
      call void @_ZNSt5mutex4lockEv(i8* @first)
      store i32 1, i32* @protected
      call void @_ZNSt5mutex4lockEv(i8* @second)
      call void @_ZNSt5mutex6unlockEv(i8* @second)
      call void @_ZNSt5mutex6unlockEv(i8* @first)
      ret i8* null
    }
    define i8* @right(i8* %arg) {
      %bad_read = load i32, i32* @shared
      %atomic_read = load atomic i32, i32* @atomic monotonic, align 4
      %local_read = load i32, i32* @tls
      call void @_ZNSt5mutex4lockEv(i8* @first)
      %protected_read = load i32, i32* @protected
      call void @_ZNSt5mutex6unlockEv(i8* @first)
      call void @_ZNSt5mutex4lockEv(i8* @second)
      call void @_ZNSt5mutex4lockEv(i8* @first)
      call void @_ZNSt5mutex6unlockEv(i8* @first)
      call void @_ZNSt5mutex6unlockEv(i8* @second)
      ret i8* null
    }
    define void @launch() {
      %a = call i32 @pthread_create(i64* null, i8* null, i8* (i8*)* @left, i8* null)
      %b = call i32 @pthread_create(i64* null, i8* null, i8* (i8*)* @right, i8* null)
      ret void
    }
  )");
  auto races = run("lotus/shared-global-race");
  ASSERT_EQ(races.findings.size(), 1u);
  EXPECT_EQ(races.findings[0].site->getFunc()->getName(), "left");
  auto cycles = run("lotus/lock-order-cycle");
  ASSERT_EQ(cycles.findings.size(), 1u);
  EXPECT_FALSE(cycles.findings[0].evidence.empty());
  // A join separates these launch epochs; neither rule should infer overlap.
  Function *launch = module->getFunction("launch");
  auto second = std::next(launch->getEntryBlock().begin());
  IRBuilder<> builder(&*second);
  builder.CreateCall(
      module->getFunction("pthread_join"),
      {builder.getInt64(0),
       ConstantPointerNull::get(builder.getInt8PtrTy()->getPointerTo())});
  EXPECT_TRUE(run("lotus/shared-global-race").findings.empty());
  EXPECT_TRUE(run("lotus/lock-order-cycle").findings.empty());
}
} // namespace
