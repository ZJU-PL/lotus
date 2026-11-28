#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/Dependence/LoopCarriedDependencies.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::LoopCarriedDependencies;
using lotus::analysis::loop::LoopDependenceEdgeKind;
using lotus::unittest::findInstructionByName;
using lotus::unittest::findPhi;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopEnvironmentAndCarriedDepsTest : public ::testing::Test {
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

TEST_F(LoopEnvironmentAndCarriedDepsTest, BuildsEnvironmentAndMarksCarriedEdges) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_env(i32* %p, i32 %n) {
    entry:
      %limit = add i32 %n, 4
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, %limit
      br i1 %cmp, label %body, label %exit

    body:
      %ld = load i32, i32* %p, align 4
      %sum.next = add i32 %sum, %ld
      store i32 %sum.next, i32* %p, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("loop_env");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeLoopEnvironments();
  analyses.materializeLoopCarriedDependencies(DT, PDT);

  auto *loop = *LI.begin();
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->hasEnvironment());

  auto *env = content->getEnvironment();
  EXPECT_GE(env->getNumberOfLiveIns(), 1u);
  EXPECT_GE(env->getNumberOfLiveOuts(), 1u);

  auto *limit = findInstructionByName(function, "limit");
  ASSERT_NE(limit, nullptr);
  EXPECT_TRUE(env->isLiveIn(limit));
  auto liveInConsumers = env->consumersOf(limit);
  auto *cmp = findInstructionByName(function, "cmp");
  ASSERT_NE(cmp, nullptr);
  EXPECT_NE(liveInConsumers.find(cmp), liveInConsumers.end());

  auto *sumPhi = findPhi(function, "sum");
  ASSERT_NE(sumPhi, nullptr);
  EXPECT_TRUE(env->isProducer(sumPhi));
  auto liveOutConsumers = env->consumersOf(sumPhi);
  EXPECT_FALSE(liveOutConsumers.empty());

  auto *graphView = content->getLoopDependenceGraph();
  ASSERT_NE(graphView, nullptr);

  bool hasCarriedVariable = false;
  bool hasCarriedMemory = false;
  for (auto *edge : graphView->getEdges()) {
    if (!edge->isLoopCarried()) {
      continue;
    }
    if (edge->getKind() == LoopDependenceEdgeKind::Variable
        && edge->getDst()->getValue() == sumPhi) {
      hasCarriedVariable = true;
    }
    if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
      hasCarriedMemory = true;
    }
  }

  EXPECT_TRUE(hasCarriedVariable);
  EXPECT_TRUE(hasCarriedMemory);

  auto carriedForLoop = LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(
      *content->getLoopStructure(),
      content->getLoopHierarchyStructures(),
      *graphView);
  EXPECT_FALSE(carriedForLoop.empty());
}

TEST_F(LoopEnvironmentAndCarriedDepsTest, TracksDistinctExitEnvironmentSlotForMultiExitLoop) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_env_multi_exit(i32 %n, i1 %flag) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit.one

    body:
      br i1 %flag, label %exit.two, label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit.one:
      ret i32 %i

    exit.two:
      ret i32 %i
    }
  )");
  auto *function = module->getFunction("loop_env_multi_exit");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeLoopEnvironments();

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *env = content->getEnvironment();
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(env->getExitBlockID(), static_cast<int64_t>(env->size() - 1));
  EXPECT_GE(env->size(), env->getNumberOfLiveIns() + 1);
}

TEST_F(LoopEnvironmentAndCarriedDepsTest, DistinguishesComputedPointersFromSharedPointerCarriedMemory) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @loop_env_pointer_modes(i32* %base, i32* %shared, i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %idx = sext i32 %i to i64
      %ptr = getelementptr inbounds i32, i32* %base, i64 %idx
      %ld.indexed = load i32, i32* %ptr, align 4
      store i32 %ld.indexed, i32* %ptr, align 4
      %ld.shared = load i32, i32* %shared, align 4
      store i32 %ld.shared, i32* %shared, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("loop_env_pointer_modes");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeLoopCarriedDependencies(DT, PDT);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *graphView = content->getLoopDependenceGraph();
  ASSERT_NE(graphView, nullptr);

  auto *indexedLoad = findInstructionByName(function, "ld.indexed");
  auto *sharedLoad = findInstructionByName(function, "ld.shared");
  ASSERT_NE(indexedLoad, nullptr);
  ASSERT_NE(sharedLoad, nullptr);

  bool indexedHasCarriedMemory = false;
  bool sharedHasCarriedMemory = false;
  for (auto *edge : graphView->getEdges()) {
    if (!edge->isLoopCarried() ||
        edge->getKind() != LoopDependenceEdgeKind::Memory) {
      continue;
    }
    if (edge->getSrc()->getValue() == indexedLoad ||
        edge->getDst()->getValue() == indexedLoad) {
      indexedHasCarriedMemory = true;
    }
    if (edge->getSrc()->getValue() == sharedLoad ||
        edge->getDst()->getValue() == sharedLoad) {
      sharedHasCarriedMemory = true;
    }
  }

  EXPECT_FALSE(indexedHasCarriedMemory);
  EXPECT_TRUE(sharedHasCarriedMemory);
}

} // namespace
