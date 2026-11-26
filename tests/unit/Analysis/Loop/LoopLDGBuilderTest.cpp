#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Passes/PassBuilder.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "Analysis/Loop/LoopDependenceGraph.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::LoopAwareDependenceRefinementPass;
using lotus::analysis::loop::LoopDependenceEdgeKind;
using lotus::analysis::loop::LoopDependenceEdgeOrigin;
using lotus::analysis::loop::LoopDependenceGraph;
using lotus::analysis::loop::LoopLDGBuilderOptions;
using lotus::analysis::loop::LoopTree;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopLDGBuilderTest : public ::testing::Test {
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

class CountingLoopAwarePass : public LoopAwareDependenceRefinementPass {
public:
  unsigned Count{0};

  void refine(LoopDependenceGraph &,
              LoopTree *,
              llvm::ScalarEvolution &,
              llvm::LoopInfo &,
              const noelle::DominatorSummary &) override {
    ++this->Count;
  }
};

class RemoveOneEdgePass : public LoopAwareDependenceRefinementPass {
public:
  bool Removed{false};

  void refine(LoopDependenceGraph &graph,
              LoopTree *,
              llvm::ScalarEvolution &,
              llvm::LoopInfo &,
              const noelle::DominatorSummary &) override {
    for (auto *edge : graph.getEdges()) {
      edge->setLoopCarried(false);
      graph.removeEdge(edge);
      this->Removed = true;
      return;
    }
  }
};

std::unique_ptr<llvm::Module> parseLDGHookModule(llvm::LLVMContext &context) {
  return parseModuleChecked(context, R"(
    define void @hook_loop(i32* %base, i32 %n) {
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
}

uint64_t countEdges(LoopDependenceGraph &graph) {
  uint64_t count = 0;
  for (auto *edge : graph.getEdges()) {
    (void)edge;
    ++count;
  }
  return count;
}

TEST_F(LoopLDGBuilderTest, RecordsDeterministicPhaseSnapshots) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @loop_lgd_dump(i32* %base, i32 %n) {
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
  auto *function = module->getFunction("loop_lgd_dump");
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
  auto const &dumps = content->getDependenceGraphDebugDumps();
  ASSERT_EQ(dumps.size(), 6u);
  EXPECT_EQ(dumps[0].first, "base");
  EXPECT_EQ(dumps[1].first, "after_affine");
  EXPECT_EQ(dumps[2].first, "after_loop_aware");
  EXPECT_EQ(dumps[3].first, "after_memory_cloning");
  EXPECT_EQ(dumps[4].first, "after_thread_safe_library");
  EXPECT_EQ(dumps[5].first, "final");

  for (auto const &dump : dumps) {
    EXPECT_NE(dump.second.find("nodes\n"), std::string::npos);
    EXPECT_NE(dump.second.find("edges\n"), std::string::npos);
  }
}

TEST_F(LoopLDGBuilderTest, PreservesExternalContextWithoutSyntheticBoundaryEdges) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_boundary(i32* %p, i32 %n) {
    entry:
      %limit = add i32 %n, 1
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp = icmp slt i32 %i, %limit
      br i1 %cmp, label %body, label %exit

    body:
      %ld = load i32, i32* %p, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 0
    }
  )");
  auto *function = module->getFunction("loop_boundary");
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
  auto *ldg = content->getLoopDependenceGraph();
  ASSERT_NE(ldg, nullptr);

  bool sawBoundaryEdge = false;
  bool sawRefinementEdge = false;
  for (auto *edge : ldg->getEdges()) {
    if (edge->getOrigin() == LoopDependenceEdgeOrigin::SynthesizedBoundaryValue) {
      sawBoundaryEdge = true;
    }
    if (edge->getOrigin() == LoopDependenceEdgeOrigin::SynthesizedRefinement) {
      sawRefinementEdge = true;
    }
  }

  auto *limit = findInstructionByName(function, "limit");
  ASSERT_NE(limit, nullptr);
  EXPECT_TRUE(ldg->isExternal(limit));
  EXPECT_FALSE(ldg->isInternal(limit));
  EXPECT_FALSE(ldg->getExternalNodes().empty());
  EXPECT_FALSE(sawBoundaryEdge);
  EXPECT_FALSE(sawRefinementEdge);
}

TEST_F(LoopLDGBuilderTest, RegisteredLoopAwarePassRunsWithoutBuiltinBackend) {
  llvm::LLVMContext context;
  auto module = parseLDGHookModule(context);
  auto *function = module->getFunction("hook_loop");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  CountingLoopAwarePass pass;
  LoopLDGBuilderOptions options;
  options.hasLoopAwareDependenceBackend = false;
  options.addLoopAwareRefinementPass(&pass);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI, options);

  EXPECT_EQ(pass.Count, 1u);
}

TEST_F(LoopLDGBuilderTest, RegisteredLoopAwarePassCanRefineGraph) {
  llvm::LLVMContext context;
  auto module = parseLDGHookModule(context);
  auto *function = module->getFunction("hook_loop");
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
  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto before = countEdges(*content->getLoopDependenceGraph());

  RemoveOneEdgePass pass;
  LoopLDGBuilderOptions options;
  options.enableAffineIterationSpaceRefinement = false;
  options.hasLoopAwareDependenceBackend = false;
  options.addLoopAwareRefinementPass(&pass);
  analyses.materializeScalarAnalyses(SE, LI, options);

  auto after = countEdges(*content->getLoopDependenceGraph());
  EXPECT_TRUE(pass.Removed);
  EXPECT_LT(after, before);
}

TEST_F(LoopLDGBuilderTest, RemovedLoopAwarePassDoesNotRun) {
  llvm::LLVMContext context;
  auto module = parseLDGHookModule(context);
  auto *function = module->getFunction("hook_loop");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::PassBuilder PB;
  llvm::FunctionAnalysisManager FAM;
  PB.registerFunctionAnalyses(FAM);
  auto &DT = FAM.getResult<llvm::DominatorTreeAnalysis>(*function);
  auto &PDT = FAM.getResult<llvm::PostDominatorTreeAnalysis>(*function);
  auto &LI = FAM.getResult<llvm::LoopAnalysis>(*function);
  auto &SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(*function);

  CountingLoopAwarePass pass;
  LoopLDGBuilderOptions options;
  options.hasLoopAwareDependenceBackend = false;
  options.addLoopAwareRefinementPass(&pass);
  options.removeLoopAwareRefinementPass(&pass);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);
  analyses.materializeScalarAnalyses(SE, LI, options);

  EXPECT_EQ(pass.Count, 0u);
}

} // namespace
