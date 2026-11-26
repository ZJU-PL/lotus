#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopIterationSpaceTest : public ::testing::Test {
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

TEST_F(LoopIterationSpaceTest, ClearsCarriedMemoryEdgesForAffineSelfRecurrence) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @array_loop(i32* %base, i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %idx = sext i32 %i to i64
      %ptr = getelementptr inbounds i32, i32* %base, i64 %idx
      %ld = load i32, i32* %ptr, align 4
      store i32 %ld, i32* %ptr, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("array_loop");
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
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
  ASSERT_NE(iterationSpace, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
  ASSERT_NE(ld, nullptr);
  ASSERT_NE(store, nullptr);
  EXPECT_TRUE(
      iterationSpace->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
          ld, store));
}

TEST_F(LoopIterationSpaceTest, StaysConservativeWhenAccessSpaceCannotBeLinearized) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32* @choose_ptr(i32*, i32*)

    define void @array_loop_nonlinear(i32* %a, i32* %b, i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %selected = call i32* @choose_ptr(i32* %a, i32* %b)
      %ld = load i32, i32* %selected, align 4
      store i32 %ld, i32* %selected, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("array_loop_nonlinear");
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
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
  ASSERT_NE(iterationSpace, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
  ASSERT_NE(ld, nullptr);
  ASSERT_NE(store, nullptr);
  EXPECT_FALSE(
      iterationSpace->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
          ld, store));
}

TEST_F(LoopIterationSpaceTest, StaysConservativeForDescendingAffineTraversal) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @reverse_array_loop(i32* %base, i32 %n) {
    entry:
      %start = add i32 %n, -1
      br label %header

    header:
      %i = phi i32 [ %start, %entry ], [ %i.next, %latch ]
      %cmp = icmp sge i32 %i, 0
      br i1 %cmp, label %body, label %exit

    body:
      %idx = sext i32 %i to i64
      %ptr = getelementptr inbounds i32, i32* %base, i64 %idx
      %ld = load i32, i32* %ptr, align 4
      store i32 %ld, i32* %ptr, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, -1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("reverse_array_loop");
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
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
  ASSERT_NE(iterationSpace, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
  ASSERT_NE(ld, nullptr);
  ASSERT_NE(store, nullptr);

  EXPECT_TRUE(
      iterationSpace->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
          ld, store));
}

TEST_F(LoopIterationSpaceTest, StaysConservativeForNonInjectiveStride) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @wrapped_stride_loop(i8* %base) {
    entry:
      br label %header

    header:
      %i = phi i16 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i16 %i, 40000
      br i1 %cmp, label %body, label %exit

    body:
      %scaled = mul i16 %i, 2
      %idx = zext i16 %scaled to i64
      %ptr = getelementptr i8, i8* %base, i64 %idx
      %ld = load i8, i8* %ptr, align 1
      store i8 %ld, i8* %ptr, align 1
      br label %latch

    latch:
      %i.next = add i16 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("wrapped_stride_loop");
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
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
  ASSERT_NE(iterationSpace, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
  ASSERT_NE(ld, nullptr);
  ASSERT_NE(store, nullptr);

  EXPECT_FALSE(
      iterationSpace->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
          ld, store));
}

TEST_F(LoopIterationSpaceTest, RejectsUnprovedInjectiveIVExpressions) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @truncated_iv(i8* %base, i64 %n) {
    entry:
      br label %header
    header:
      %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i64 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      %narrow = trunc i64 %i to i32
      %idx = zext i32 %narrow to i64
      %ptr = getelementptr i8, i8* %base, i64 %idx
      %ld = load i8, i8* %ptr, align 1
      store i8 %ld, i8* %ptr, align 1
      br label %latch
    latch:
      %i.next = add nuw i64 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @duplicated_iv(i8* %base, i64 %n) {
    entry:
      br label %header
    header:
      %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i64 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      %idx = add nuw i64 %i, %i
      %ptr = getelementptr i8, i8* %base, i64 %idx
      %ld = load i8, i8* %ptr, align 1
      store i8 %ld, i8* %ptr, align 1
      br label %latch
    latch:
      %i.next = add nuw i64 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @cancelled_iv(i8* %base, i64 %n) {
    entry:
      br label %header
    header:
      %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i64 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      %idx = sub i64 %i, %i
      %ptr = getelementptr i8, i8* %base, i64 %idx
      %ld = load i8, i8* %ptr, align 1
      store i8 %ld, i8* %ptr, align 1
      br label %latch
    latch:
      %i.next = add nuw i64 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @wrapping_iv(i8* %base) {
    entry:
      br label %header
    header:
      %i = phi i8 [ 0, %entry ], [ %i.next, %latch ]
      %count = phi i16 [ 0, %entry ], [ %count.next, %latch ]
      %cmp = icmp ult i16 %count, 300
      br i1 %cmp, label %body, label %exit
    body:
      %idx = zext i8 %i to i64
      %ptr = getelementptr i8, i8* %base, i64 %idx
      %ld = load i8, i8* %ptr, align 1
      store i8 %ld, i8* %ptr, align 1
      br label %latch
    latch:
      %i.next = add i8 %i, 1
      %count.next = add nuw nsw i16 %count, 1
      br label %header
    exit:
      ret void
    }
  )");

  buildPDG(*module);
  auto expectConservative = [&](llvm::StringRef functionName) {
    SCOPED_TRACE(functionName.str());
    auto *function = module->getFunction(functionName);
    ASSERT_NE(function, nullptr);

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
    analyses.materializeLoopCarriedDependencies(DT, PDT);
    analyses.materializeIterationSpaceAnalyses(SE);

    auto *content = analyses.getLoopContent(**LI.begin());
    ASSERT_NE(content, nullptr);
    auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
    auto *ld = findInstructionByName(function, "ld");
    auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
    ASSERT_NE(iterationSpace, nullptr);
    ASSERT_NE(ld, nullptr);
    ASSERT_NE(store, nullptr);
    EXPECT_FALSE(iterationSpace
                     ->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                         ld, store));
  };

  expectConservative("truncated_iv");
  expectConservative("duplicated_iv");
  expectConservative("cancelled_iv");
  expectConservative("wrapping_iv");
}

TEST_F(LoopIterationSpaceTest,
       RejectsNonDereferencesOverlappingWidthsAndRuntimeZeroStride) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @shared = global i8* null

    define void @pointer_stored_as_value(i8* %base, i64 %n) {
    entry:
      br label %header
    header:
      %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i64 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      %p = getelementptr i8, i8* %base, i64 %i
      %ld = load i8, i8* %p, align 1
      store i8* %p, i8** @shared, align 8
      br label %latch
    latch:
      %i.next = add nuw i64 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @wide_unit_stride(i8* %base, i64 %n) {
    entry:
      br label %header
    header:
      %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i64 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      %p = getelementptr i8, i8* %base, i64 %i
      %wide = bitcast i8* %p to i64*
      %ld = load i64, i64* %wide, align 1
      store i64 %ld, i64* %wide, align 1
      br label %latch
    latch:
      %i.next = add nuw i64 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @runtime_stride(i8* %base, i64 %n, i64 %stride) {
    entry:
      br label %header
    header:
      %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp ult i64 %i, %n
      br i1 %cmp, label %body, label %exit
    body:
      %idx = mul nuw i64 %i, %stride
      %p = getelementptr i8, i8* %base, i64 %idx
      %ld = load i8, i8* %p, align 1
      store i8 %ld, i8* %p, align 1
      br label %latch
    latch:
      %i.next = add nuw i64 %i, 1
      br label %header
    exit:
      ret void
    }

    define void @wide_constants(i8* %base) {
    entry:
      br label %header
    header:
      %i = phi i128 [ 170141183460469231731687303715884105700, %entry ],
                    [ %i.next, %latch ]
      %cmp = icmp ult i128 %i, 170141183460469231731687303715884105704
      br i1 %cmp, label %body, label %exit
    body:
      %p = getelementptr i8, i8* %base, i128 %i
      %ld = load i8, i8* %p, align 1
      store i8 %ld, i8* %p, align 1
      br label %latch
    latch:
      %i.next = add nuw i128 %i, 1
      br label %header
    exit:
      ret void
    }
  )");

  buildPDG(*module);
  auto analyze = [&](llvm::StringRef functionName, bool checkResult = true) {
    SCOPED_TRACE(functionName.str());
    auto *function = module->getFunction(functionName);
    ASSERT_NE(function, nullptr);

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
    analyses.materializeLoopCarriedDependencies(DT, PDT);
    analyses.materializeIterationSpaceAnalyses(SE);

    auto *content = analyses.getLoopContent(**LI.begin());
    ASSERT_NE(content, nullptr);
    auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
    auto *ld = findInstructionByName(function, "ld");
    auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
    ASSERT_NE(iterationSpace, nullptr);
    ASSERT_NE(ld, nullptr);
    ASSERT_NE(store, nullptr);
    bool disjoint = iterationSpace
                        ->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
                            ld, store);
    if (checkResult) {
      EXPECT_FALSE(disjoint);
    }
  };

  analyze("pointer_stored_as_value");
  analyze("wide_unit_stride");
  analyze("runtime_stride");
  analyze("wide_constants", false);
}

TEST_F(LoopIterationSpaceTest, KeepsMultiBlockSameIterationDependencesConservative) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @split_body_loop(i32* %base, i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body.load, label %exit

    body.load:
      %idx = sext i32 %i to i64
      %ptr = getelementptr inbounds i32, i32* %base, i64 %idx
      %ld = load i32, i32* %ptr, align 4
      br label %body.store

    body.store:
      store i32 %ld, i32* %ptr, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("split_body_loop");
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
  analyses.materializeLoopCarriedDependencies(DT, PDT);
  analyses.materializeIterationSpaceAnalyses(SE);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *iterationSpace = content->getLoopIterationSpaceAnalysis();
  ASSERT_NE(iterationSpace, nullptr);
  auto *graphView = content->getLoopDependenceGraph();
  ASSERT_NE(graphView, nullptr);
  auto *ld = findInstructionByName(function, "ld");
  auto *store = lotus::unittest::findInstruction<llvm::StoreInst>(*function);
  ASSERT_NE(ld, nullptr);
  ASSERT_NE(store, nullptr);

  EXPECT_TRUE(
      iterationSpace->areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
          ld, store));

  bool hasLoopCarriedMemory = false;
  for (auto *edge : graphView->getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != lotus::analysis::loop::LoopDependenceEdgeKind::Memory) {
      continue;
    }
    if (edge->getSrc()->getValue() == ld && edge->getDst()->getValue() == store) {
      hasLoopCarriedMemory = true;
      break;
    }
  }
  EXPECT_FALSE(hasLoopCarriedMemory);
}

} // namespace
