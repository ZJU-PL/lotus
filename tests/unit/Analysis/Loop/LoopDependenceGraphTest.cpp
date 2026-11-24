#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

#include "Analysis/Loop/FunctionLoopAnalyses.h"
#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

namespace {

using lotus::analysis::loop::FunctionLoopAnalyses;
using lotus::analysis::loop::LoopDependenceEdgeKind;
using lotus::analysis::loop::LoopDependenceMemoryKind;
using lotus::analysis::loop::LoopDependenceNode;
using lotus::analysis::loop::LoopSCC;
using lotus::unittest::findInstructionByName;
using lotus::unittest::findPhi;
using lotus::unittest::parseModuleChecked;
using pdg::ControlDependencyGraph;
using pdg::DataDependencyGraph;
using pdg::ProgramDependencyGraph;
using pdg::ProgramGraph;

class LoopDependenceGraphTest : public ::testing::Test {
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

TEST_F(LoopDependenceGraphTest, MaterializesGraphAndSCCsFromLegacyPDG) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @loop_graph(i32* %p, i32 %n) {
    entry:
      %limit = add i32 %n, 1
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %sum = phi i32 [ 0, %entry ], [ %sum.next, %latch ]
      %cmp = icmp slt i32 %i, %limit
      br i1 %cmp, label %body, label %exit

    body:
      %ld = load i32, i32* %p, align 4
      %add = add i32 %sum, %ld
      %sum.next = add i32 %add, 1
      %tmp = add i32 %i, %sum.next
      store i32 %tmp, i32* %p, align 4
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %sum
    }
  )");
  auto *function = module->getFunction("loop_graph");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);

  auto *content = analyses.getLoopContent(
      *LI.getLoopFor(findInstructionByName(function, "cmp")->getParent()));
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(content->hasDependenceGraph());
  ASSERT_TRUE(content->hasSCCDAG());

  auto *ldg = content->getLoopDependenceGraph();
  auto *sccdag = content->getSCCDAG();
  ASSERT_NE(ldg, nullptr);
  ASSERT_NE(sccdag, nullptr);

  auto internalNodes = ldg->getInternalNodes();
  auto externalNodes = ldg->getExternalNodes();
  EXPECT_FALSE(internalNodes.empty());
  EXPECT_FALSE(externalNodes.empty());

  auto *sumPhi = findPhi(function, "sum");
  auto *ld = findInstructionByName(function, "ld");
  ASSERT_NE(sumPhi, nullptr);
  ASSERT_NE(ld, nullptr);

  auto *sumNode = ldg->getNode(sumPhi);
  auto *ldNode = ldg->getNode(ld);
  ASSERT_NE(sumNode, nullptr);
  ASSERT_NE(ldNode, nullptr);
  EXPECT_TRUE(sumNode->isInternal());
  EXPECT_TRUE(ldNode->isInternal());

  auto *limit = findInstructionByName(function, "limit");
  ASSERT_NE(limit, nullptr);
  bool foundExternalValueNode = false;
  for (auto *node : externalNodes) {
    if (node->getValue() == limit) {
      foundExternalValueNode = true;
      break;
    }
  }
  EXPECT_TRUE(foundExternalValueNode);
  EXPECT_TRUE(ldg->isExternal(limit));
  EXPECT_FALSE(ldg->isInternal(limit));

  auto *limitSCC = sccdag->getSCC(limit);
  ASSERT_NE(limitSCC, nullptr);
  EXPECT_FALSE(limitSCC->isIncludedInLoop());

  bool foundControlEdge = false;
  bool foundVariableEdge = false;
  bool foundRawMemoryEdge = false;
  for (auto *edge : ldg->getEdges()) {
    if (edge->getKind() == LoopDependenceEdgeKind::Control) {
      foundControlEdge = true;
    }
    if (edge->getKind() == LoopDependenceEdgeKind::Variable) {
      foundVariableEdge = true;
    }
    if (edge->getKind() == LoopDependenceEdgeKind::Memory &&
        edge->getMemoryKind() == LoopDependenceMemoryKind::Raw) {
      foundRawMemoryEdge = true;
    }
    EXPECT_FALSE(edge->isLoopCarried());
  }
  EXPECT_TRUE(foundControlEdge);
  EXPECT_TRUE(foundVariableEdge);
  EXPECT_TRUE(foundRawMemoryEdge);

  auto sccs = sccdag->getSCCs();
  EXPECT_FALSE(sccs.empty());
  auto *sumSCC = sccdag->getSCC(sumPhi);
  ASSERT_NE(sumSCC, nullptr);
  EXPECT_TRUE(sumSCC->hasCycle());
  EXPECT_FALSE(sumSCC->getNodes().empty());
}

TEST_F(LoopDependenceGraphTest, CondensesAcyclicAndCyclicInternalRegions) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @two_sccs(i32 %n) {
    entry:
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %acc = phi i32 [ 0, %entry ], [ %acc.next, %latch ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %use = add i32 %acc, 1
      %acc.next = add i32 %use, %i
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret void
    }
  )");
  auto *function = module->getFunction("two_sccs");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);

  auto *loop = LI.begin()[0];
  auto *content = analyses.getLoopContent(*loop);
  ASSERT_NE(content, nullptr);
  auto *sccdag = content->getSCCDAG();
  ASSERT_NE(sccdag, nullptr);

  auto sccs = sccdag->getSCCs();
  ASSERT_GE(sccs.size(), 2u);

  auto *accPhi = findPhi(function, "acc");
  auto *iPhi = findPhi(function, "i");
  ASSERT_NE(accPhi, nullptr);
  ASSERT_NE(iPhi, nullptr);

  auto *accSCC = sccdag->getSCC(accPhi);
  auto *iSCC = sccdag->getSCC(iPhi);
  ASSERT_NE(accSCC, nullptr);
  ASSERT_NE(iSCC, nullptr);
  EXPECT_TRUE(accSCC->hasCycle());
  EXPECT_TRUE(iSCC->hasCycle());

  auto accSuccs = accSCC->getSuccessors();
  auto iSuccs = iSCC->getSuccessors();
  auto accPreds = accSCC->getPredecessors();
  auto iPreds = iSCC->getPredecessors();
  EXPECT_NE(std::find(iSuccs.begin(), iSuccs.end(), accSCC), iSuccs.end());
  EXPECT_NE(std::find(accPreds.begin(), accPreds.end(), iSCC), accPreds.end());

  EXPECT_TRUE(sccdag->orderedBefore(iSCC, accSCC));
  EXPECT_FALSE(sccdag->orderedBefore(accSCC, iSCC));

  for (auto *succ : accSuccs) {
    auto succPreds = succ->getPredecessors();
    EXPECT_NE(std::find(succPreds.begin(), succPreds.end(), accSCC),
              succPreds.end());
  }
  for (auto *succ : iSuccs) {
    auto succPreds = succ->getPredecessors();
    EXPECT_NE(std::find(succPreds.begin(), succPreds.end(), iSCC),
              succPreds.end());
  }

  for (auto *pred : accPreds) {
    auto predSuccs = pred->getSuccessors();
    EXPECT_NE(std::find(predSuccs.begin(), predSuccs.end(), accSCC),
              predSuccs.end());
  }
  for (auto *pred : iPreds) {
    auto predSuccs = pred->getSuccessors();
    EXPECT_NE(std::find(predSuccs.begin(), predSuccs.end(), iSCC),
              predSuccs.end());
  }
}

TEST_F(LoopDependenceGraphTest, PreservesExternalOnlySCCsInAllSCCsView) {
  llvm::LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @external_context_scc(i32 %n) {
    entry:
      %limit = add i32 %n, 4
      %stable = add i32 %n, 1
      br label %header

    header:
      %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
      %cmp.lhs = add i32 %stable, 0
      %cmp.rhs = add i32 %limit, 0
      %cmp = icmp slt i32 %i, %cmp.rhs
      br i1 %cmp, label %body, label %exit

    body:
      %use = add i32 %cmp.lhs, %i
      br label %latch

    latch:
      %i.next = add i32 %i, 1
      br label %header

    exit:
      ret i32 %use
    }
  )");
  auto *function = module->getFunction("external_context_scc");
  ASSERT_NE(function, nullptr);

  buildPDG(*module);

  llvm::DominatorTree DT(*function);
  llvm::PostDominatorTree PDT;
  PDT.recalculate(*function);
  llvm::LoopInfo LI(DT);

  FunctionLoopAnalyses analyses(*function, LI, DT, PDT);
  analyses.materializeDependenceGraphs(graph);

  auto *content = analyses.getLoopContent(**LI.begin());
  ASSERT_NE(content, nullptr);
  auto *sccdag = content->getSCCDAG();
  ASSERT_NE(sccdag, nullptr);

  auto *limit = findInstructionByName(function, "limit");
  auto *stable = findInstructionByName(function, "stable");
  ASSERT_NE(limit, nullptr);
  ASSERT_NE(stable, nullptr);
  auto *limitSCC = sccdag->getSCC(limit);
  auto *stableSCC = sccdag->getSCC(stable);
  ASSERT_NE(limitSCC, nullptr);
  ASSERT_NE(stableSCC, nullptr);
  EXPECT_FALSE(limitSCC->isIncludedInLoop());
  EXPECT_FALSE(stableSCC->isIncludedInLoop());

  auto includedSCCs = sccdag->getSCCs();
  auto allSCCs = sccdag->getAllSCCs();
  EXPECT_GT(allSCCs.size(), includedSCCs.size());
  EXPECT_EQ(std::find(includedSCCs.begin(), includedSCCs.end(), limitSCC),
            includedSCCs.end());
  EXPECT_NE(std::find(allSCCs.begin(), allSCCs.end(), limitSCC), allSCCs.end());
}

} // namespace
