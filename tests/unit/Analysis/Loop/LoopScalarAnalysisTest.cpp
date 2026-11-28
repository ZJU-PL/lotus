#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/Scalar/InductionVariables.h"
#include "Analysis/Loop/Scalar/LoopInvariants.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::LoopDependenceEdgeKind;
using lotus::unittest::findInstructionByName;
using lotus::unittest::findPhi;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopScalarAnalysisTest : public ::testing::Test {
protected:
  void SetUp() override { graph.reset(); }
  void TearDown() override { graph.reset(); }

  void buildPDG(llvm::Module &module) {
    auto &registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(registry);
    llvm::initializeAnalysis(registry);
    llvm::initializeTransformUtils(registry);

    llvm::legacy::PassManager pm;
    pm.add(new DataDependencyGraph());
    pm.add(new ControlDependencyGraph());
    pm.add(new ProgramDependencyGraph());
    pm.run(module);
  }

  ProgramGraph &graph = ProgramGraph::getInstance();
};

TEST_F(LoopScalarAnalysisTest, MaterializesInvariantsAndInductionVariables) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_scalar(i32 %n) {
    entry:
      %limit = add i32 %n, 5
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, %limit
      br i1 %cmp, label %body, label %exit

    body:
      %tmp = add i32 %i, 1
      %sum.next = add i32 %sum, %tmp
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("loop_scalar");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->hasInvariantManager());
  ASSERT_TRUE(content->hasInductionVariableManager());

  auto *invariants = content->getInvariantManager();
  auto *ivs = content->getInductionVariableManager();

  auto *limit = findInstructionByName(function, "limit");
  auto *cmp = findInstructionByName(function, "cmp");
  auto *tmp = findInstructionByName(function, "tmp");
  auto *iPhi = findPhi(function, "i");
  auto *sumPhi = findPhi(function, "sum");
  ASSERT_NE(limit, nullptr);
  ASSERT_NE(cmp, nullptr);
  ASSERT_NE(tmp, nullptr);
  ASSERT_NE(iPhi, nullptr);
  ASSERT_NE(sumPhi, nullptr);

  EXPECT_TRUE(invariants->isLoopInvariant(limit));
  EXPECT_FALSE(invariants->isLoopInvariant(cmp));
  EXPECT_FALSE(invariants->isLoopInvariant(tmp));

  auto detectedIVs = ivs->getInductionVariables(*content->getLoopStructure());
  ASSERT_EQ(detectedIVs.size(), 1u);
  auto *iv = *detectedIVs.begin();
  ASSERT_NE(iv, nullptr);
  EXPECT_EQ(iv->getLoopEntryPHI(), iPhi);
  EXPECT_NE(iv->getStartValue(), nullptr);
  EXPECT_NE(iv->getSingleComputedStepValue(), nullptr);
  EXPECT_TRUE(iv->isIVInstruction(iPhi));
  EXPECT_FALSE(iv->isIVInstruction(sumPhi));

  auto *governing =
      ivs->getLoopGoverningInductionVariable(*content->getLoopStructure());
  ASSERT_NE(governing, nullptr);
  EXPECT_TRUE(governing->isSCCContainingIVWellFormed());
  EXPECT_EQ(governing->getInductionVariable(), iv);
  EXPECT_EQ(governing->getHeaderCompareInstructionToComputeExitCondition(),
            cmp);
  EXPECT_EQ(governing->getExitConditionValue(), limit);
}

TEST_F(LoopScalarAnalysisTest,
       KeepsEquivalentPhiInvariantAndRecognizesReadNoneCalls) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i64 @hash_ptr(i8*) readnone nounwind

    define i64 @loop_scalar_pure(i8* %s, i32 %n) {
    entry:
      %seed = add i64 0, 7
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %stable = phi i64 [ %seed, %entry ], [ %seed, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %len = call i64 @hash_ptr(i8* %s)
      %mix = add i64 %stable, %len
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i64 %stable
    }
  )");
  auto *function = module->getFunction("loop_scalar_pure");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *invariants = content->getInvariantManager();
  ASSERT_NE(invariants, nullptr);

  auto *seed = findInstructionByName(function, "seed");
  auto *stablePhi = findPhi(function, "stable");
  auto *len = findInstructionByName(function, "len");
  auto *mix = findInstructionByName(function, "mix");
  ASSERT_NE(seed, nullptr);
  ASSERT_NE(stablePhi, nullptr);
  ASSERT_NE(len, nullptr);
  ASSERT_NE(mix, nullptr);

  EXPECT_TRUE(invariants->isLoopInvariant(seed));
  EXPECT_TRUE(invariants->isLoopInvariant(stablePhi));
  EXPECT_TRUE(invariants->isLoopInvariant(len));
  EXPECT_TRUE(invariants->isLoopInvariant(mix));
}

TEST_F(LoopScalarAnalysisTest, KeepsReadonlyCallVariantWhenLoopMutatesMemory) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i64 @strlen(i8*) readonly nounwind

    define i64 @loop_scalar_readonly(i8* %s, i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      store i8 0, i8* %s, align 1
      %len = call i64 @strlen(i8* %s)
      br label %latch

    latch:
      %i.next = add nuw nsw i32 %i, 1
      br label %header

    exit:
      ret i64 0
    }
  )");
  auto *function = module->getFunction("loop_scalar_readonly");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *invariants = content->getInvariantManager();
  auto *len = findInstructionByName(function, "len");
  ASSERT_NE(invariants, nullptr);
  ASSERT_NE(len, nullptr);
  EXPECT_FALSE(invariants->isLoopInvariant(len));
}

TEST_F(LoopScalarAnalysisTest, KeepsObservedImpureLibraryCallsVariant) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @puts(i8*)

    @.str = private unnamed_addr constant [3 x i8] c"x\0A\00"

    define i32 @loop_scalar_impure(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %msg = getelementptr inbounds [3 x i8], [3 x i8]* @.str, i64 0, i64 0
      %r = call i32 @puts(i8* %msg)
      %mix = add i32 %r, 1
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 0
    }
  )");
  auto *function = module->getFunction("loop_scalar_impure");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *invariants = content->getInvariantManager();
  ASSERT_NE(invariants, nullptr);

  auto *randCall = findInstructionByName(function, "r");
  auto *mix = findInstructionByName(function, "mix");
  ASSERT_NE(randCall, nullptr);
  ASSERT_NE(mix, nullptr);

  EXPECT_FALSE(invariants->isLoopInvariant(randCall));
  EXPECT_FALSE(invariants->isLoopInvariant(mix));
}

TEST_F(LoopScalarAnalysisTest, KeepsTimeVaryingLibraryCallsVariant) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i64 @clock()

    define i64 @loop_scalar_clock(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %ticks = call i64 @clock()
      %mix = add i64 %ticks, 1
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i64 0
    }
  )");
  auto *function = module->getFunction("loop_scalar_clock");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *invariants = content->getInvariantManager();
  ASSERT_NE(invariants, nullptr);

  auto *ticks = findInstructionByName(function, "ticks");
  auto *mix = findInstructionByName(function, "mix");
  ASSERT_NE(ticks, nullptr);
  ASSERT_NE(mix, nullptr);

  EXPECT_FALSE(invariants->isLoopInvariant(ticks));
  EXPECT_FALSE(invariants->isLoopInvariant(mix));
}

TEST_F(LoopScalarAnalysisTest, AttributesDerivedExitConditionValueForGoverningIV) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_scalar_derived_condition_giv(i32 %n) {
    entry:
      %limit = add i32 %n, 5
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp.limit = add i32 %limit, 0
      %cmp = icmp slt i32 %i, %cmp.limit
      br i1 %cmp, label %body, label %exit

    body:
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %i
    }
  )");
  auto *function = module->getFunction("loop_scalar_derived_condition_giv");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *ivs = content->getInductionVariableManager();
  ASSERT_NE(ivs, nullptr);
  auto *governing =
      ivs->getLoopGoverningInductionVariable(*content->getLoopStructure());
  ASSERT_NE(governing, nullptr);
  EXPECT_TRUE(governing->isSCCContainingIVWellFormed());

  auto *cmpLimit = findInstructionByName(function, "cmp.limit");
  auto *cmp = findInstructionByName(function, "cmp");
  ASSERT_NE(cmpLimit, nullptr);
  ASSERT_NE(cmp, nullptr);
  auto *iPhi = findPhi(function, "i");
  ASSERT_NE(iPhi, nullptr);

  EXPECT_EQ(governing->getHeaderCompareInstructionToComputeExitCondition(), cmp);
  EXPECT_EQ(governing->getValueToCompareAgainstExitConditionValue(), iPhi);
  EXPECT_EQ(governing->getExitConditionValue(), cmpLimit);
  EXPECT_TRUE(governing->getConditionValueDerivation().empty());
}

} // namespace
