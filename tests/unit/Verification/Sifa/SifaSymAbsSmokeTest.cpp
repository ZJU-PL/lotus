#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Sifa.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"
#include "Verification/SymAbsAI/Core/Engine/Analyzer.h"
#include "Verification/SymAbsAI/Core/Foundation/AbstractValue.h"
#include "Verification/SymAbsAI/Core/Foundation/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Foundation/FloatingPointModel.h"
#include "Verification/SymAbsAI/Core/Foundation/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/Integration/ModuleContext.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"

#include "TestUtils/LLVMHelpers.h"

#include "gtest/gtest.h"

#include <stdexcept>

using namespace llvm;
using namespace lotus::unittest;

namespace {

template <typename FunctionT>
static auto getBlockByName(FunctionT &F, const char *name) {
  return findBlock(F, name);
}

static symabs_ai::configparser::Config makeSymAbsConfig() {
  symabs_ai::configparser::Config cfg;
  cfg.set("ModuleContext", "Recursive", true);
  cfg.set("Analyzer", "Variant", llvm::StringRef("UnilateralAnalyzer"));
  cfg.set("AbstractDomain", "Variant", llvm::StringRef("Interval"));
  cfg.set("FunctionContext", "RepresentAllInstructions", true);
  return cfg;
}

TEST(SifaSymAbs, SmokeIntervalsAndCalls) {
  const char *ir = R"IR(
    define i32 @g(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @f(i32 %n) {
    entry:
      %c = call i32 @g(i32 %n)
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %c
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

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *F = getFunctionChecked(*M, "f");

  BasicBlock *body = getBlockChecked(*F, "body");
  BasicBlock *unreach = getBlockChecked(*F, "unreach");

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto bodyState = lotus::sifa::analyzeSymAbsTo(*M, *F, *body, opt);
  ASSERT_NE(bodyState, nullptr);
  EXPECT_FALSE(bodyState->isBottom());

  auto unreachState = lotus::sifa::analyzeSymAbsTo(*M, *F, *unreach, opt);
  EXPECT_TRUE(unreachState == nullptr || unreachState->isBottom());

  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, SingleBlockFunctionReturnStateNotBottom) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *F = getFunctionChecked(*M, "f");

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, AnalyzeSymAbsToSpecificBlock) {
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

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *F = getFunctionChecked(*M, "f");

  BasicBlock *body = getBlockChecked(*F, "body");
  BasicBlock *unreach = getBlockChecked(*F, "unreach");

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto stateBody = lotus::sifa::analyzeSymAbsTo(*M, *F, *body, opt);
  auto stateUnreach = lotus::sifa::analyzeSymAbsTo(*M, *F, *unreach, opt);

  EXPECT_NE(stateBody, nullptr);
  EXPECT_FALSE(stateBody->isBottom());
  EXPECT_TRUE(stateUnreach == nullptr || stateUnreach->isBottom());
}

TEST(SifaSymAbs, IntervalOnlyDomain) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *F = getFunctionChecked(*M, "f");

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  EXPECT_TRUE(lotus::sifa::isReachableSymAbs(*M, *F, F->getEntryBlock(), opt));
  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, HybridFallbackHandlesSplitCallTransitions) {
  const char *ir = R"IR(
    define i32 @callee(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @caller(i32 %n) {
    entry:
      %a = add i32 %n, 2
      %b = call i32 @callee(i32 %a)
      %c = add i32 %b, 3
      br label %exit

    exit:
      ret i32 %c
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *caller = getFunctionChecked(*M, "caller");

  BasicBlock *exit = getBlockChecked(*caller, "exit");

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto fctx = mctx.createFunctionContext(caller);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctx);
  symabs_ai::DomainConstructor dom(fctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*fctx, fragDecomp, dom);

  lotus::sifa::SifaStats stats;
  lotus::sifa::SifaSymAbsDomain domain(*fctx, dom, *analyzer);
  lotus::sifa::NeverFluid<lotus::sifa::SymAbsState> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, lotus::sifa::SymAbsState> ipr(
      stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, lotus::sifa::SymAbsState>
      loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *caller, {exit}, std::vector<const llvm::Function *>{});
  auto initial = domain.makeTopAt(&caller->getEntryBlock(), /*after=*/false);

  lotus::sifa::SymAbsState out;
  EXPECT_NO_THROW(
      out = ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(),
                                         initial));
  ASSERT_NE(out, nullptr);
  EXPECT_FALSE(out->isBottom());
}

TEST(SifaSymAbs, HybridFallbackHandlesInvokeReturnSummary) {
  const char *ir = R"IR(
    declare i32 @__gxx_personality_v0()

    define i32 @callee(i32 %x) {
    entry:
      %y = add i32 %x, 5
      ret i32 %y
    }

    define i32 @caller(i32 %n) personality i32 ()* @__gxx_personality_v0 {
    entry:
      %res = invoke i32 @callee(i32 %n)
        to label %cont unwind label %lpad

    cont:
      %sum = add i32 %res, 1
      ret i32 %sum

    lpad:
      %ex = landingpad i32 cleanup
      ret i32 0
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *caller = getFunctionChecked(*M, "caller");

  BasicBlock *cont = getBlockChecked(*caller, "cont");

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto fctx = mctx.createFunctionContext(caller);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctx);
  symabs_ai::DomainConstructor dom(fctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*fctx, fragDecomp, dom);

  lotus::sifa::SifaStats stats;
  lotus::sifa::SifaSymAbsDomain domain(*fctx, dom, *analyzer);
  lotus::sifa::NeverFluid<lotus::sifa::SymAbsState> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, lotus::sifa::SymAbsState> ipr(
      stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, lotus::sifa::SymAbsState>
      loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *caller, {cont}, std::vector<const llvm::Function *>{});
  auto initial = domain.makeTopAt(&caller->getEntryBlock(), /*after=*/false);

  lotus::sifa::SymAbsState out;
  EXPECT_NO_THROW(
      out = ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(),
                                         initial));
  ASSERT_NE(out, nullptr);
  EXPECT_FALSE(out->isBottom());
}

TEST(SifaSymAbs, InterproceduralEnterCallProjectsArguments) {
  const char *ir = R"IR(
    define void @callee(i32 %x) {
    entry:
      %cmp = icmp eq i32 %x, 42
      br i1 %cmp, label %good, label %bad

    good:
      ret void

    bad:
      ret void
    }

    define void @main(i32 %seed) {
    entry:
      %unused = add i32 %seed, 0
      call void @callee(i32 42)
      ret void
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SifaSymAbsSmokeTest");

  Function *mainFn = getFunctionChecked(*M, "main");
  Function *calleeFn = getFunctionChecked(*M, "callee");

  BasicBlock *good = getBlockChecked(*calleeFn, "good");
  BasicBlock *bad = getBlockChecked(*calleeFn, "bad");

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto rootFctx = mctx.createFunctionContext(mainFn);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*rootFctx);
  symabs_ai::DomainConstructor dom(rootFctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*rootFctx, fragDecomp, dom);

  lotus::sifa::SifaSymAbsDomain domain(*rootFctx, dom, *analyzer);

  auto goodState = lotus::sifa::analyzeInterproceduralTo<lotus::sifa::SymAbsState>(
      *M, mainFn, *calleeFn, *good, domain.top(), domain);
  ASSERT_NE(goodState, nullptr);
  EXPECT_FALSE(goodState->isBottom());

  auto badState = lotus::sifa::analyzeInterproceduralTo<lotus::sifa::SymAbsState>(
      *M, mainFn, *calleeFn, *bad, domain.top(), domain);
  EXPECT_TRUE(!badState || badState->isBottom());
}

TEST(SifaSymAbs, ReturnedSymAbsStateOutlivesAnalysisScopeSafely) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "SymAbsLifetime");
  Function *F = getFunctionChecked(*M, "f");

  for (const char *domName : {"SimpleConstProp", "NumRels.Zero"}) {
    lotus::sifa::SifaSymAbsOptions opt;
    opt.abstractDomain = domName;
    opt.representAllInstructions = true;
    auto state = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
    ASSERT_NE(state, nullptr) << domName;
    EXPECT_FALSE(state->isBottom()) << domName;
    (void)state->isTop();
    symabs_ai::PrettyPrinter pp(false);
    state->prettyPrint(pp);
    EXPECT_FALSE(pp.str().empty()) << domName;
  }
}

TEST(SifaSymAbs, HandlesEmptyProductUnnamedBlocksGlobalsAndValidRegion) {
  const char *ir = R"IR(
    @0 = global i32 7

    define void @empty_void() {
      ret void
    }

    define i32 @unnamed(i32 %0) {
      %2 = load i32, i32* @0
      %3 = add i32 %0, %2
      %4 = icmp eq i32 %3, 10
      br i1 %4, label %5, label %6
    5:
      ret i32 %3
    6:
      ret i32 0
    }

    define i32 @ptr_fn() {
    entry:
      %p = alloca i32
      store i32 42, i32* %p
      %v = load i32, i32* %p
      ret i32 %v
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "EmptyAndUnnamed");

  {
    Function *emptyFn = getFunctionChecked(*M, "empty_void");
    lotus::sifa::SifaSymAbsOptions opt;
    opt.abstractDomain = "Interval";
    opt.representAllInstructions = false;
    auto state = lotus::sifa::analyzeSymAbsToReturn(*M, *emptyFn, opt);
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(state->isBottom());
    EXPECT_TRUE(state->isTop());
  }

  {
    Function *unnamedFn = getFunctionChecked(*M, "unnamed");
    auto it = unnamedFn->begin();
    std::advance(it, 1);
    const BasicBlock &thenBB = *it;

    for (bool reprAll : {false, true}) {
      lotus::sifa::SifaSymAbsOptions opt;
      opt.abstractDomain = "Interval";
      opt.representAllInstructions = reprAll;
      auto st = lotus::sifa::analyzeSymAbsTo(*M, *unnamedFn, thenBB, opt);
      ASSERT_NE(st, nullptr);
      EXPECT_FALSE(st->isBottom());
    }
  }

  {
    Function *ptrFn = getFunctionChecked(*M, "ptr_fn");
    lotus::sifa::SifaSymAbsOptions opt;
    opt.abstractDomain = "ValidRegion";
    opt.representAllInstructions = true;
    auto st = lotus::sifa::analyzeSymAbsToReturn(*M, *ptrFn, opt);
    ASSERT_NE(st, nullptr);
    EXPECT_FALSE(st->isBottom());
  }

  // Ordered pointer comparisons (e.g. `icmp ult i8* %p, %q`) must not throw
  // Z3 sort mismatch when BlockModel represents pointers as pairs.
  {
    const char *ptrCmpIR = R"IR(
      define i1 @ptr_ult(i8* %p, i8* %q) {
      entry:
        %cmp = icmp ult i8* %p, %q
        ret i1 %cmp
      }
    )IR";
    LLVMContext pctx;
    auto PM = parseModuleChecked(pctx, ptrCmpIR, "PtrUltValidRegion");
    Function *PF = getFunctionChecked(*PM, "ptr_ult");
    lotus::sifa::SifaSymAbsOptions opt;
    opt.abstractDomain = "ValidRegion";
    opt.representAllInstructions = true;
    lotus::sifa::SymAbsState st;
    EXPECT_NO_THROW(st = lotus::sifa::analyzeSymAbsToReturn(*PM, *PF, opt));
    ASSERT_NE(st, nullptr);
    EXPECT_FALSE(st->isBottom());
  }
}

TEST(SifaSymAbs, UnilateralTransfCacheHandlesTopFollowedByNarrowInput) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      br label %exit
    exit:
      ret i32 %y
    }
  )IR";

  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "TransfCacheNarrow");
  Function *F = getFunctionChecked(*M, "f");
  BasicBlock *entry = getBlockChecked(*F, "entry");
  BasicBlock *exitBB = getBlockChecked(*F, "exit");
  llvm::Instruction *yInst = &*entry->begin();

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto fctx = mctx.createFunctionContext(F);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctx);
  symabs_ai::DomainConstructor dom(fctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*fctx, fragDecomp, dom);
  const symabs_ai::Fragment *frag =
      analyzer->findSingleEdgeFragment(entry, exitBB);
  ASSERT_NE(frag, nullptr);

  auto topIn = dom.makeBottom(*fctx, entry, false);
  topIn->havoc();
  auto outTop = dom.makeBottom(*fctx, exitBB, false);
  analyzer->bestTransformer(topIn.get(), *frag, outTop.get());
  EXPECT_FALSE(outTop->isBottom());

  // Constrain `%x` in [5, 5] via a formula on the same cached fragment.
  auto vmEntry = symabs_ai::ValueMapping::atBeginning(*fctx, *frag);
  auto narrowIn = dom.makeBottom(*fctx, entry, false);
  z3::expr eq5 = vmEntry[F->getArg(0)] == fctx->getZ3().bv_val(5, 32);
  analyzer->strongestConsequence(narrowIn.get(), eq5, vmEntry);
  ASSERT_FALSE(narrowIn->isBottom());

  auto outNarrow = dom.makeBottom(*fctx, exitBB, false);
  analyzer->bestTransformer(narrowIn.get(), *frag, outNarrow.get());
  ASSERT_FALSE(outNarrow->isBottom());

  auto vmExit = symabs_ai::ValueMapping::atEnd(*fctx, *frag);
  z3::solver chk(fctx->getZ3());
  chk.add(outNarrow->toFormula(vmExit, fctx->getZ3()));
  chk.add(vmExit[yInst] != fctx->getZ3().bv_val(6, 32));
  EXPECT_EQ(chk.check(), z3::unsat);
}

TEST(SifaSymAbs, DomainConstructorRejectsInvalidVariantAndX87CmpWorks) {
  symabs_ai::configparser::Config badCfg;
  badCfg.set("AbstractDomain", "Variant", llvm::StringRef("NoSuchDomain"));
  EXPECT_THROW((void)symabs_ai::DomainConstructor(badCfg), std::invalid_argument);

  const char *ir = R"IR(
    define i1 @fcmp_test(double %a, double %b) {
    entry:
      %c = fcmp oeq double %a, %b
      ret i1 %c
    }
  )IR";
  LLVMContext ctx;
  auto M = parseModuleChecked(ctx, ir, "X87Cmp");
  Function *F = getFunctionChecked(*M, "fcmp_test");

  symabs_ai::configparser::Config cfg = makeSymAbsConfig();
  cfg.set("FloatingPointModel", "Variant", llvm::StringRef("X87"));
  symabs_ai::ModuleContext mctx(M.get(), cfg);
  auto fctx = mctx.createFunctionContext(F);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctx);
  ASSERT_NE(fragDecomp.begin(), fragDecomp.end());
  EXPECT_NO_THROW((void)fctx->formulaFor(*fragDecomp.begin()));
}

} // namespace
