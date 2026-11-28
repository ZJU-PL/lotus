//===-- SifaDagInterpreterTest.cpp - Full Sifa DAG interpreter pipeline ----===//
//
// Exercises ProcedureResources -> DagInterpreter -> FixpointLoopSummarizer
// with ReachabilityDomain (same pipeline as isReachable()).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Caches/TopsortCache.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Fluid/AlwaysFluid.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Interpreter/IEnterCallRegistrar.h"
#include "Verification/Sifa/Procedure/ProcedureGraphBuilder.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/RegexDag/RegexDagCompressor.h"
#include "Verification/Sifa/RegexDag/RegexDagUtils.h"
#include "Verification/Sifa/RegexDag/RegexToDag.h"
#include "Verification/Sifa/Sifa.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/Summarizers/ReUseSupersetCallSummarizer.h"

#include "TestUtils/LLVMHelpers.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/SourceMgr.h>

#include "gtest/gtest.h"

#include <limits>

using namespace llvm;
using namespace lotus::unittest;

namespace {

template <typename FunctionT>
static auto getBlockByName(FunctionT &F, const char *name) {
  return findBlock(F, name);
}

static const PHINode *getPhiByName(const BasicBlock &BB, const char *name) {
  return findPhi(BB, name);
}

static void expectOctagonPoint(const lotus::sifa::OctagonState &state,
                               const llvm::Value *value, int64_t point) {
  auto it = state.varToIndex().find(value);
  ASSERT_NE(it, state.varToIndex().end());
  const lotus::sifa::OctagonMatrix closed = state.matrix().strongClosure();
  auto lowerConstraint = closed.get(2 * it->second, 2 * it->second + 1);
  auto upperConstraint = closed.get(2 * it->second + 1, 2 * it->second);
  ASSERT_TRUE(lowerConstraint.has_value());
  ASSERT_TRUE(upperConstraint.has_value());
  EXPECT_EQ(-(*lowerConstraint) / 2, point);
  EXPECT_EQ((*upperConstraint) / 2, point);
}

class CountingEdgeDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  bool leq(const int &a, const int &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return false;
    return a <= b;
  }
  int join(const int &a, const int &b) const override {
    if (isBottom(a)) return b;
    if (isBottom(b)) return a;
    return std::max(a, b);
  }
  int widen(const int &previous, const int &next) const override {
    return join(previous, next);
  }
  int meet(const int &a, const int &b) const override {
    if (isBottom(a) || isBottom(b)) return bottom();
    return std::min(a, b);
  }
  int post(const lotus::sifa::Transition &t, const int &in) const override {
    if (isBottom(in)) return in;
    return t.kind == lotus::sifa::TransitionKind::Edge ? in + 1 : in;
  }
};

class AlteringSubsetDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  bool leq(const int &a, const int &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return false;
    return a <= b;
  }
  bool equal(const int &a, const int &b) const override { return a == b; }
  int join(const int &a, const int &b) const override { return std::max(a, b); }
  int widen(const int &previous, const int &next) const override {
    return join(previous, next);
  }
  int meet(const int &a, const int &b) const override { return std::min(a, b); }
  int post(const lotus::sifa::Transition &, const int &in) const override {
    return in;
  }
  SubsetEqResult subsetEq(const int &subset, const int &superset) const override {
    return SubsetEqResult(subset, superset + 1, true, true);
  }
};

class NoMeetSubsetDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  bool leq(const int &a, const int &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return false;
    return a <= b;
  }
  bool equal(const int &a, const int &b) const override { return a == b; }
  int join(const int &a, const int &b) const override { return std::max(a, b); }
  int widen(const int &previous, const int &next) const override {
    return join(previous, next);
  }
  int post(const lotus::sifa::Transition &, const int &in) const override {
    return in;
  }
};

class AlphaDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  int alpha(const int &) const override { return 100; }
  bool leq(const int &a, const int &b) const override { return a <= b; }
  int join(const int &a, const int &b) const override { return std::max(a, b); }
  int widen(const int &previous, const int &next) const override {
    return std::max(previous, next);
  }
  int post(const lotus::sifa::Transition &t, const int &in) const override {
    if (t.kind == lotus::sifa::TransitionKind::Edge) {
      return in + 1;
    }
    return in;
  }
};

class RecordingEnterCallRegistrar final
    : public lotus::sifa::IEnterCallRegistrar<int> {
public:
  void registerEnterCall(const std::string &calleeName,
                         const int &calleeInput) override {
    calls.emplace_back(calleeName, calleeInput);
  }

  std::vector<std::pair<std::string, int>> calls;
};

class CountingCallSummarizer final : public lotus::sifa::ICallSummarizer<int> {
public:
  int summarize(const std::string &, const int &) override { return ++calls; }

  int calls = 0;
};

TEST(SifaDagInterpreter, FullPipelineReachableAndUnreachable) {
  const char *ir = R"IR(
    define i32 @f(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      br label %loop

    exit:
      ret i32 0

    unreach:
      ret i32 1
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *body = getBlockByName(*F, "body");
  const llvm::BasicBlock *exit = getBlockByName(*F, "exit");
  const llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, bool> ipr(stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, bool> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *F,
      {const_cast<llvm::BasicBlock *>(body)});

  bool bodyReachable =
      ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(), /*initialInput=*/true);
  EXPECT_TRUE(bodyReachable);

  lotus::sifa::ProcedureResources resExit(stats, *F,
                                          {const_cast<llvm::BasicBlock *>(exit)});
  bool exitReachable =
      ipr.interpretForSingleMarker(resExit.getRegexDag(), resExit.getDagOverlayPathToLois(), true);
  EXPECT_TRUE(exitReachable);

  lotus::sifa::ProcedureResources resUnreach(stats, *F,
                                            {const_cast<llvm::BasicBlock *>(unreach)});
  bool unreachReachable =
      ipr.interpretForSingleMarker(resUnreach.getRegexDag(), resUnreach.getDagOverlayPathToLois(), true);
  EXPECT_FALSE(unreachReachable);
}

TEST(SifaDagInterpreter, RawProcedureGraphDoesNotAddCallSummaryEdges) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ProcedureGraphBuilder builder(stats, *F);
  lotus::sifa::ProcedureGraph graph = builder.graphOfProcedure({}, false);

  for (const auto &edge : graph.graph().getEdges()) {
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(edge->getLabel().kind, lotus::sifa::TransitionKind::Edge);
  }
}

TEST(SifaDagInterpreter, ReturnOverlayIgnoresNoReturnUnreachableExits) {
  const char *ir = R"IR(
    declare void @panic() noreturn

    define void @main() {
    entry:
      call void @panic()
      unreachable
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, bool> ipr(stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, bool> loopSum(
      stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(stats, *F, std::vector<llvm::BasicBlock *>{});
  bool reachesReturn = ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToReturn(), true);

  EXPECT_FALSE(reachesReturn);
}

TEST(SifaDagInterpreter, SingleSinkLocationReportsSyntheticExitAsNull) {
  const char *ir = R"IR(
    define void @main() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ProcedureResources res(stats, *F, std::vector<llvm::BasicBlock *>{});

  EXPECT_EQ(lotus::sifa::singleSinkLocation(
                res.getRegexDag(), res.getDagOverlayPathToReturn()),
            nullptr);
}

TEST(SifaDagInterpreter, TopsortCacheRefreshesAfterDagReuseAtSameAddress) {
  using Transition = lotus::sifa::Transition;
  using Regex = lotus::pathexpressions::Regex<Transition>;

  lotus::sifa::TopsortCache<Transition> cache;
  lotus::sifa::RegexToDag<Transition> toDag;
  auto a = Regex::literal(Transition::makeMarker(1, nullptr));
  auto b = Regex::literal(Transition::makeMarker(2, nullptr));

  toDag.add(a);
  lotus::sifa::RegexDag<Transition> dag = toDag.getDagAndReset();
  auto firstOrder = cache.topsort(dag);
  EXPECT_EQ(firstOrder.size(), 3u);

  toDag.add(Regex::concat(a, b));
  dag = toDag.getDagAndReset();
  auto secondOrder = cache.topsort(dag);
  EXPECT_EQ(secondOrder.size(), 4u);
}

TEST(SifaDagInterpreter, ProcedureResourcesIgnoreIrrelevantEnterCalls) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @h() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *hFn = M->getFunction("h");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(hFn, nullptr);

  lotus::sifa::SifaStats stats;
  EXPECT_NO_THROW((void)lotus::sifa::ProcedureResources(
      stats, *mainFn, std::vector<llvm::BasicBlock *>{}, {hFn}));
}

TEST(SifaDagInterpreter, NativeDomainsApplyDestinationPhisAtBlockEntry) {
  const char *ir = R"IR(
    define i32 @f() {
    entry:
      br label %left

    left:
      br label %merge

    dead:
      br label %merge

    merge:
      %x = phi i32 [ 7, %left ], [ 42, %dead ]
      ret i32 %x
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *merge = getBlockByName(*F, "merge");
  const llvm::BasicBlock *left = getBlockByName(*F, "left");
  ASSERT_NE(merge, nullptr);
  ASSERT_NE(left, nullptr);

  const llvm::PHINode *phi = getPhiByName(*merge, "x");
  ASSERT_NE(phi, nullptr);

  lotus::sifa::IntervalState intervalState =
      lotus::sifa::analyzeToWithIntervalDomain(*F, *merge, lotus::sifa::IntervalState{});
  auto interval = intervalState.get(phi);
  ASSERT_TRUE(interval.has_value());
  EXPECT_EQ(*interval, lotus::sifa::Interval::point(7));

  lotus::sifa::EqState eqState =
      lotus::sifa::analyzeToWithEqDomain(*F, *merge, lotus::sifa::EqState{});
  EXPECT_EQ(eqState.find(phi),
            eqState.find(phi->getIncomingValueForBlock(left)));

  lotus::sifa::ExplicitValueState explicitState =
      lotus::sifa::analyzeToWithExplicitValueDomain(
          *F, *merge, lotus::sifa::ExplicitValueState{});
  auto explicitValue = explicitState.get(phi);
  ASSERT_TRUE(explicitValue.has_value());
  ASSERT_TRUE(explicitValue->value.has_value());
  EXPECT_EQ(*explicitValue->value, 7);

  lotus::sifa::OctagonState octagonState =
      lotus::sifa::analyzeToWithOctagonDomain(*F, *merge, lotus::sifa::OctagonState{});
  EXPECT_EQ(octagonState.varToIndex().count(phi), 1u);
}

TEST(SifaDagInterpreter, NativeDomainsRefineTakenBranchGuards) {
  const char *ir = R"IR(
    define i32 @f() {
    entry:
      %cond = icmp eq i32 0, 0
      br i1 %cond, label %then, label %else

    then:
      br label %merge

    else:
      br label %merge

    merge:
      %x = phi i32 [ 7, %then ], [ 42, %else ]
      ret i32 %x
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *merge = getBlockByName(*F, "merge");
  const llvm::BasicBlock *thenBB = getBlockByName(*F, "then");
  ASSERT_NE(merge, nullptr);
  ASSERT_NE(thenBB, nullptr);

  const llvm::PHINode *phi = getPhiByName(*merge, "x");
  ASSERT_NE(phi, nullptr);

  lotus::sifa::IntervalState intervalState =
      lotus::sifa::analyzeToWithIntervalDomain(*F, *merge,
                                               lotus::sifa::IntervalState{});
  auto interval = intervalState.get(phi);
  ASSERT_TRUE(interval.has_value());
  EXPECT_EQ(*interval, lotus::sifa::Interval::point(7));

  lotus::sifa::EqState eqState =
      lotus::sifa::analyzeToWithEqDomain(*F, *merge, lotus::sifa::EqState{});
  EXPECT_EQ(eqState.find(phi),
            eqState.find(phi->getIncomingValueForBlock(thenBB)));

  lotus::sifa::ExplicitValueState explicitState =
      lotus::sifa::analyzeToWithExplicitValueDomain(
          *F, *merge, lotus::sifa::ExplicitValueState{});
  auto explicitValue = explicitState.get(phi);
  ASSERT_TRUE(explicitValue.has_value());
  ASSERT_TRUE(explicitValue->value.has_value());
  EXPECT_EQ(*explicitValue->value, 7);

  lotus::sifa::OctagonState octagonState =
      lotus::sifa::analyzeToWithOctagonDomain(*F, *merge,
                                              lotus::sifa::OctagonState{});
  expectOctagonPoint(octagonState, phi, 7);
}

TEST(SifaDagInterpreter, EnterCallReceivesPrefixStateBeforeCallSite) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      %x = add i32 1, 2
      call void @g()
      %y = add i32 %x, 1
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  lotus::sifa::SifaStats stats;
  CountingEdgeDomain domain;
  lotus::sifa::NeverFluid<int> fluid;
  lotus::sifa::DagInterpreter<lotus::sifa::Transition, int> ipr(stats, domain,
                                                                fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, int> loopSum(
      stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(stats, *mainFn, {}, {gFn});
  lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, int> storage;
  RecordingEnterCallRegistrar registrar;

  ipr.interpretWithCalls(res.getRegexDag(),
                         res.getDagOverlayPathToLoisAndEnterCalls(), 0,
                         storage, registrar);

  ASSERT_EQ(registrar.calls.size(), 1u);
  EXPECT_EQ(registrar.calls.front().first, "g");
  EXPECT_EQ(registrar.calls.front().second, 1);
}

TEST(SifaDagInterpreter, ReUseSupersetRequiresUnchangedSupersetInput) {
  AlteringSubsetDomain domain;
  CountingCallSummarizer inner;
  lotus::sifa::ReUseSupersetCallSummarizer<lotus::sifa::Transition, int>
      summarizer(domain, inner);

  EXPECT_EQ(summarizer.summarize("callee", 1), 1);
  EXPECT_EQ(inner.calls, 1);
  EXPECT_EQ(summarizer.summarize("callee", 1), 2);
  EXPECT_EQ(inner.calls, 2);
}

TEST(SifaDagInterpreter, ReUseSupersetFallsBackWhenDomainDoesNotSupportMeet) {
  NoMeetSubsetDomain domain;
  CountingCallSummarizer inner;
  lotus::sifa::ReUseSupersetCallSummarizer<lotus::sifa::Transition, int>
      summarizer(domain, inner);

  EXPECT_EQ(summarizer.summarize("callee", 2), 1);
  EXPECT_EQ(summarizer.summarize("callee", 3), 2);
  EXPECT_EQ(inner.calls, 2);

  EXPECT_EQ(summarizer.summarize("callee", 1), 3);
  EXPECT_EQ(inner.calls, 3);

  EXPECT_EQ(summarizer.summarize("callee", 2), 1);
  EXPECT_EQ(inner.calls, 3);
}

TEST(SifaDagInterpreter, ProcedureGraphBuilderEnterCallOverloadHonorsRestrictFlag) {
  const char *ir = R"IR(
    define void @callee() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @callee()
      ret void

    dead:
      unreachable
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *calleeFn = M->getFunction("callee");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(calleeFn, nullptr);

  const llvm::BasicBlock *dead = getBlockByName(*mainFn, "dead");
  ASSERT_NE(dead, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ProcedureGraphBuilder builder(stats, *mainFn);

  lotus::sifa::ProcedureGraph full =
      builder.graphOfProcedure({}, {calleeFn}, /*restrictToReachable=*/false);
  EXPECT_NE(full.getBlockEntryNode(*dead), nullptr);

  lotus::sifa::ProcedureGraph restricted =
      builder.graphOfProcedure({}, {calleeFn}, /*restrictToReachable=*/true);
  EXPECT_EQ(restricted.getBlockEntryNode(*dead), nullptr);
}

TEST(SifaDagInterpreter, TransitionEqualityIncludesCfgIdentity) {
  const char *ir = R"IR(
    define void @f() {
    entry:
      br label %exit

    exit:
      ret void
    }

    define void @g() {
    entry:
      br label %exit

    exit:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *f = M->getFunction("f");
  llvm::Function *g = M->getFunction("g");
  ASSERT_NE(f, nullptr);
  ASSERT_NE(g, nullptr);

  auto *fEntry = &f->getEntryBlock();
  auto *gEntry = &g->getEntryBlock();
  auto *fExit = getBlockByName(*f, "exit");
  auto *gExit = getBlockByName(*g, "exit");
  ASSERT_NE(fExit, nullptr);
  ASSERT_NE(gExit, nullptr);

  const lotus::sifa::Transition a =
      lotus::sifa::Transition::makeEdge(0, fEntry, fExit);
  const lotus::sifa::Transition b =
      lotus::sifa::Transition::makeEdge(0, gEntry, gExit);

  EXPECT_FALSE(a == b);
}

TEST(SifaDagInterpreter, AnalyzeToCanUseCustomFluidPolicy) {
  const char *ir = R"IR(
    define void @main() {
    entry:
      br label %target

    target:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);
  const llvm::BasicBlock *target = getBlockByName(*F, "target");
  ASSERT_NE(target, nullptr);

  AlphaDomain domain;
  lotus::sifa::AlwaysFluid<int> fluid;

  EXPECT_EQ(lotus::sifa::analyzeTo<int>(*F, *target, 1, domain), 2);
  EXPECT_EQ(lotus::sifa::analyzeTo<int>(*F, *target, 1, domain, fluid), 100);
}

TEST(SifaDagInterpreter, AnalyzeToReturnPreservesBottomInitialState) {
  const char *ir = R"IR(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::IntervalState intervalBottom(true);
  lotus::sifa::IntervalState intervalResult =
      lotus::sifa::analyzeToReturnWithIntervalDomain(*F, intervalBottom);
  EXPECT_TRUE(intervalResult.isBottom());

  lotus::sifa::OctagonState octagonBottom(true);
  lotus::sifa::OctagonState octagonResult =
      lotus::sifa::analyzeToReturnWithOctagonDomain(*F, octagonBottom);
  EXPECT_TRUE(octagonResult.isBottom());
}

TEST(SifaDagInterpreter, RegexDagCompressorEliminatesGhostsAndHandlesDiamonds) {
  using Transition = lotus::sifa::Transition;
  using Regex = lotus::pathexpressions::Regex<Transition>;

  lotus::sifa::RegexToDag<Transition> toDag;
  auto a = Regex::literal(Transition::makeMarker(1, nullptr));
  auto b = Regex::literal(Transition::makeMarker(2, nullptr));
  auto c = Regex::literal(Transition::makeMarker(3, nullptr));
  auto eps = Regex::epsilon();

  auto r1 = Regex::concat(Regex::union_(a, eps), Regex::union_(b, c));
  auto r2 = Regex::concat(Regex::union_(a, eps), b);
  toDag.add(r1);
  toDag.add(r2);

  lotus::sifa::RegexDag<Transition> dag = toDag.getDagAndReset();
  lotus::sifa::RegexDagCompressor<Transition> compressor;
  compressor.compress(dag);

  lotus::sifa::TopsortCache<Transition> topsortCache;
  std::vector<lotus::sifa::RegexDagNode<Transition> *> order;
  EXPECT_NO_THROW(order = topsortCache.topsort(dag));
  ASSERT_FALSE(order.empty());
  for (auto *n : order) {
    ASSERT_NE(n, nullptr);
  }
}

TEST(SifaDagInterpreter, RegexDagCompressorHandlesWideDuplicateFanout) {
  using Transition = lotus::sifa::Transition;
  using Regex = lotus::pathexpressions::Regex<Transition>;

  lotus::sifa::RegexToDag<Transition> toDag;
  auto common = Regex::literal(Transition::makeMarker(1, nullptr));
  auto tail = Regex::literal(Transition::makeMarker(2, nullptr));
  for (int i = 0; i < 200; ++i) {
    toDag.add(Regex::concat(common, tail));
  }
  lotus::sifa::RegexDag<Transition> dag = toDag.getDagAndReset();
  lotus::sifa::RegexDagCompressor<Transition> compressor;
  compressor.compress(dag);

  lotus::sifa::TopsortCache<Transition> topsortCache;
  auto order = topsortCache.topsort(dag);
  EXPECT_FALSE(order.empty());
  EXPECT_LT(order.size(), 10u);
}

TEST(SifaDagInterpreter, ProcedureResourcesUnreachableLoiWithMultipleExits) {
  const char *ir = R"IR(
    define void @f(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      ret void
    right:
      ret void
    dead:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "UnreachableLoi");
  const llvm::Function *F = getFunctionChecked(*M, "f");
  const llvm::BasicBlock *dead = getBlockByName(*F, "dead");
  ASSERT_NE(dead, nullptr);

  EXPECT_FALSE(lotus::sifa::isReachable(*F, *dead));
  auto iv = lotus::sifa::analyzeToWithIntervalDomain(
      *F, *dead, lotus::sifa::IntervalState{});
  EXPECT_TRUE(iv.isBottom());
  auto oct = lotus::sifa::analyzeToWithOctagonDomain(
      *F, *dead, lotus::sifa::OctagonState{});
  EXPECT_TRUE(oct.isBottom());
}

TEST(SifaDagInterpreter, IntervalDomainLatticeAndTransferEdgeCases) {
  const char *ir = R"IR(
    define i32 @f(i32 %x, i64 %a) {
    entry:
      %t = trunc i32 -1 to i1
      %d = sdiv i64 -9223372036854775808, -1
      %r = srem i64 -9223372036854775808, -1
      %s = ashr i32 -16, 2
      br label %loop

    loop:
      %pa = phi i32 [ 10, %entry ], [ %pb, %loop ]
      %pb = phi i32 [ 20, %entry ], [ %pa, %loop ]
      %cmp = icmp eq i32 %x, 0
      br i1 %cmp, label %loop, label %exit

    exit:
      ret i32 %pa
    }
  )IR";

  llvm::LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "IntervalEdgeCases");
  llvm::Function *F = getFunctionChecked(*M, "f");
  const llvm::BasicBlock *entry = getBlockByName(*F, "entry");
  const llvm::BasicBlock *loop = getBlockByName(*F, "loop");
  const llvm::BasicBlock *exitBB = getBlockByName(*F, "exit");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(loop, nullptr);
  ASSERT_NE(exitBB, nullptr);

  const llvm::Value *xArg = F->getArg(0);
  const llvm::Value *yArg = F->getArg(1);

  lotus::sifa::IntervalDomain dom;
  lotus::sifa::IntervalState s1(false);
  s1.set(xArg, lotus::sifa::Interval{0, 10, false});
  EXPECT_TRUE(dom.leq(s1, dom.top()));
  EXPECT_FALSE(dom.leq(dom.top(), s1));

  lotus::sifa::IntervalState sNegInf(false);
  sNegInf.set(xArg, lotus::sifa::Interval{std::nullopt, 5, false});
  lotus::sifa::IntervalState sNegInfWide(false);
  sNegInfWide.set(xArg, lotus::sifa::Interval{std::nullopt, 10, false});
  EXPECT_TRUE(dom.leq(sNegInf, sNegInfWide));
  EXPECT_FALSE(dom.leq(sNegInf, s1));

  lotus::sifa::IntervalState sOtherKey(false);
  sOtherKey.set(yArg, lotus::sifa::Interval{1, 2, false});
  lotus::sifa::IntervalState joined = dom.join(s1, sOtherKey);
  EXPECT_FALSE(joined.get(xArg).has_value());
  EXPECT_FALSE(joined.get(yArg).has_value());

  auto widened = sNegInf.get(xArg)->widen(*sNegInfWide.get(xArg));
  EXPECT_FALSE(widened.hasLower());
  EXPECT_FALSE(widened.hasUpper());

  auto out = lotus::sifa::analyzeToWithIntervalDomain(
      *F, *exitBB, lotus::sifa::IntervalState(false));
  EXPECT_FALSE(out.isBottom());
  const llvm::Instruction *truncInst = &*entry->begin();
  auto tVal = out.get(truncInst);
  ASSERT_TRUE(tVal.has_value());
  EXPECT_FALSE(tVal->isBottom());

  // Single-step parallel PHI swap on loop -> loop.
  lotus::sifa::IntervalState phiIn(false);
  const llvm::PHINode *pa = getPhiByName(*loop, "pa");
  const llvm::PHINode *pb = getPhiByName(*loop, "pb");
  ASSERT_NE(pa, nullptr);
  ASSERT_NE(pb, nullptr);
  phiIn.set(pa, lotus::sifa::Interval::point(10));
  phiIn.set(pb, lotus::sifa::Interval::point(20));
  lotus::sifa::Transition loopEdge = lotus::sifa::Transition::makeEdge(
      0, const_cast<llvm::BasicBlock *>(loop),
      const_cast<llvm::BasicBlock *>(loop));
  lotus::sifa::IntervalState swapped = dom.post(loopEdge, phiIn);
  EXPECT_EQ(swapped.get(pa), lotus::sifa::Interval::point(20));
  EXPECT_EQ(swapped.get(pb), lotus::sifa::Interval::point(10));
}

TEST(SifaDagInterpreter, OctagonDomainNegativeEqualityAndLoopConvergence) {
  const char *ir = R"IR(
    define i32 @neg_eq() {
    entry:
      %x = add i32 0, -5
      %c = icmp eq i32 %x, -5
      br i1 %c, label %then, label %else
    then:
      ret i32 %x
    else:
      ret i32 0
    }

    define i8 @loop_i8() {
    entry:
      br label %loop
    loop:
      %i = phi i8 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp ne i8 %i, 120
      br i1 %cmp, label %body, label %exit
    body:
      %inc = add i8 %i, 1
      br label %loop
    exit:
      ret i8 %i
    }
  )IR";

  llvm::LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "OctagonSoundness");
  llvm::Function *negFn = getFunctionChecked(*M, "neg_eq");
  const llvm::BasicBlock *entry = getBlockByName(*negFn, "entry");
  const llvm::BasicBlock *thenBB = getBlockByName(*negFn, "then");
  const llvm::BasicBlock *elseBB = getBlockByName(*negFn, "else");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(thenBB, nullptr);
  ASSERT_NE(elseBB, nullptr);
  const llvm::Value *xInst = &*entry->begin();

  auto thenState = lotus::sifa::analyzeToWithOctagonDomain(
      *negFn, *thenBB, lotus::sifa::OctagonState(false));
  EXPECT_FALSE(thenState.isBottom());
  EXPECT_FALSE(thenState.hasNegativeSelfLoop());
  expectOctagonPoint(thenState, xInst, -5);

  auto elseState = lotus::sifa::analyzeToWithOctagonDomain(
      *negFn, *elseBB, lotus::sifa::OctagonState(false));
  EXPECT_TRUE(elseState.isBottom() || elseState.hasNegativeSelfLoop());

  llvm::Function *loopFn = getFunctionChecked(*M, "loop_i8");
  const llvm::BasicBlock *loopExit = getBlockByName(*loopFn, "exit");
  ASSERT_NE(loopExit, nullptr);
  auto exitState = lotus::sifa::analyzeToWithOctagonDomain(
      *loopFn, *loopExit, lotus::sifa::OctagonState(false));
  EXPECT_FALSE(exitState.isBottom());
  EXPECT_FALSE(exitState.hasNegativeSelfLoop());
}

TEST(SifaDagInterpreter, EqDomainJoinPartitionAndExplicitValueLeq) {
  const char *ir = R"IR(
    define void @f(i1 %cond, i32 %a, i32 %b, i32 %c) {
    entry:
      %cmp1 = icmp eq i32 %a, %b
      br i1 %cmp1, label %guard, label %exit
    guard:
      br i1 %cond, label %left, label %right
    left:
      %cmp2 = icmp eq i32 %b, %c
      br i1 %cmp2, label %leftOk, label %exit
    leftOk:
      br label %merge
    right:
      br label %merge
    merge:
      ret void
    exit:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "EqAndExplicit");
  llvm::Function *F = getFunctionChecked(*M, "f");
  const llvm::BasicBlock *merge = getBlockByName(*F, "merge");
  ASSERT_NE(merge, nullptr);
  const llvm::Value *a = F->getArg(1);
  const llvm::Value *b = F->getArg(2);
  const llvm::Value *c = F->getArg(3);

  auto eqOut = lotus::sifa::analyzeToWithEqDomain(
      *F, *merge, lotus::sifa::EqState(false));
  EXPECT_FALSE(eqOut.isBottom());
  EXPECT_EQ(eqOut.find(a), eqOut.find(b));
  EXPECT_NE(eqOut.find(a), eqOut.find(c));

  // Representation-independent equality and ordering.
  lotus::sifa::EqState u1, u2;
  u1.unite(a, b);
  u2.unite(b, a);
  lotus::sifa::EqDomain eqDom;
  EXPECT_TRUE(eqDom.leq(u1, u2));
  EXPECT_TRUE(eqDom.leq(u2, u1));
  EXPECT_TRUE(u1 == u2);

  lotus::sifa::ExplicitValueDomain expDom;
  lotus::sifa::ExplicitValueState topState(false);
  lotus::sifa::ExplicitValueState constState(false);
  constState.set(a, lotus::sifa::ExplicitValue::constant(7));
  EXPECT_FALSE(expDom.leq(topState, constState));
  EXPECT_TRUE(expDom.leq(constState, topState));
}

} // namespace
