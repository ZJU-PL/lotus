#include "IR/GraphView.h"

#include "IR/ICFG/ICFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

namespace {

/// Bridging is a control-flow-level device. Encode the policy as a compile-time
/// fact so a value-flow graph can never silently acquire fabricated edges.
static_assert(ViewBridgingPolicy<ICFG>::value,
              "ICFG edges are control flow and may be bridged");
static_assert(!ViewBridgingPolicy<SVFG>::value,
              "bridging an SVFG would fabricate value-flow paths");

ICFGNode *blockNode(ICFG &icfg, Function *fn, const char *name) {
  BasicBlock *bb = nullptr;
  for (BasicBlock &candidate : *fn)
    if (candidate.getName() == name)
      bb = &candidate;
  return bb ? icfg.getIntraBlockNode(bb) : nullptr;
}

} // namespace

class GraphViewTest : public LlvmModuleTest {};

TEST_F(GraphViewTest, ICFGViewIsAnInducedNonOwningSubgraph) {
  auto module = parseModule(R"(
    define i32 @main(i1 %c) {
    entry:
      br i1 %c, label %left, label %right
    left:
      br label %exit
    right:
      br label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  ICFGNode *entry = icfg.getIntraBlockNode(&main->getEntryBlock());
  ICFGNode *left = icfg.getIntraBlockNode(
      main->getBasicBlockList().getNextNode(main->getEntryBlock()));
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(left, nullptr);

  FilteredICFGView::NodeSet retained{entry, left};
  FilteredICFGView view(icfg, std::move(retained));

  EXPECT_EQ(view.nodeCount(), 2u);
  EXPECT_TRUE(view.contains(entry));
  ASSERT_EQ(view.successors(entry).size(), 1u);
  EXPECT_EQ(view.successors(entry).front(), left);
  EXPECT_EQ(view.predecessors(left).size(), 1u);
  EXPECT_EQ(view.edgeCount(), 1u);

  // Induced semantics: nothing is contracted.
  EXPECT_EQ(view.bridgeMode(), BridgeMode::Induced);
  EXPECT_EQ(view.bridgeCount(), 0u);
  EXPECT_EQ(view.bridgedSuccessors(entry), nullptr);
  EXPECT_EQ(view.condensedSuccessors(entry), view.successors(entry));
}

TEST_F(GraphViewTest, ICFGViewBridgesAChainOfRemovedNodes) {
  // entry -> a -> b -> tail, plus a direct entry -> tail edge.
  // Retaining only {entry, tail} must keep entry able to reach tail.
  auto module = parseModule(R"(
    define i32 @main(i1 %c) {
    entry:
      br i1 %c, label %a, label %tail
    a:
      br label %b
    b:
      br label %tail
    tail:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  ICFGNode *entry = blockNode(icfg, main, "entry");
  ICFGNode *exit = blockNode(icfg, main, "tail");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(exit, nullptr);

  FilteredICFGView view(icfg, FilteredICFGView::NodeSet{entry, exit},
                        BridgeMode::Contract);

  EXPECT_EQ(view.bridgeMode(), BridgeMode::Contract);
  EXPECT_EQ(view.nodeCount(), 2u);

  // The real edge entry -> tail survives, and it is deduplicated against the
  // bridge that also lands on tail.
  ASSERT_EQ(view.successors(entry).size(), 1u);
  EXPECT_EQ(view.successors(entry).front(), exit);

  const FilteredICFGView::NodeList *bridged = view.bridgedSuccessors(entry);
  ASSERT_NE(bridged, nullptr);
  ASSERT_EQ(bridged->size(), 1u);
  EXPECT_EQ(bridged->front(), exit);
  EXPECT_EQ(view.bridgeCount(), 1u);

  ASSERT_EQ(view.condensedSuccessors(entry).size(), 1u);
  EXPECT_EQ(view.condensedSuccessors(entry).front(), exit);
  ASSERT_EQ(view.condensedPredecessors(exit).size(), 1u);
  EXPECT_EQ(view.condensedPredecessors(exit).front(), entry);

  // The removed nodes are genuinely gone from the induced layer.
  EXPECT_EQ(view.edgeCount(), 1u);
}

TEST_F(GraphViewTest, ICFGViewBridgesThroughCyclesOfRemovedNodes) {
  // x <-> y is a cycle made only of removed nodes; it must terminate and must
  // not change the retained reachability (entry -> z).
  auto module = parseModule(R"(
    define i32 @main(i1 %c) {
    entry:
      br label %x
    x:
      br i1 %c, label %y, label %z
    y:
      br label %x
    z:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  ICFGNode *entry = blockNode(icfg, main, "entry");
  ICFGNode *exit = blockNode(icfg, main, "z");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(exit, nullptr);

  FilteredICFGView view(icfg, FilteredICFGView::NodeSet{entry, exit},
                        BridgeMode::Contract);

  const FilteredICFGView::NodeList *bridged = view.bridgedSuccessors(entry);
  ASSERT_NE(bridged, nullptr);
  ASSERT_EQ(bridged->size(), 1u);
  EXPECT_EQ(bridged->front(), exit);
  EXPECT_EQ(view.successors(entry).size(), 0u);
}

TEST_F(GraphViewTest, ICFGViewBridgesBackIntoTheSameNode) {
  // x -> y -> x with only y removed: the contraction yields the self-loop x->x.
  auto module = parseModule(R"(
    define i32 @main(i1 %c) {
    entry:
      br label %x
    x:
      br i1 %c, label %y, label %z
    y:
      br label %x
    z:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  ICFGNode *x = blockNode(icfg, main, "x");
  ICFGNode *z = blockNode(icfg, main, "z");
  ASSERT_NE(x, nullptr);
  ASSERT_NE(z, nullptr);

  FilteredICFGView view(icfg, FilteredICFGView::NodeSet{x, z},
                        BridgeMode::Contract);

  const FilteredICFGView::NodeList *bridged = view.bridgedSuccessors(x);
  ASSERT_NE(bridged, nullptr);
  ASSERT_EQ(bridged->size(), 1u);
  EXPECT_EQ(bridged->front(), x); // self-loop

  // Condensed neighbours are deduplicated and ordered by node id.
  FilteredICFGView::NodeList condensed = view.condensedSuccessors(x);
  ASSERT_EQ(condensed.size(), 2u);
  EXPECT_LT(condensed[0]->getId(), condensed[1]->getId());
}

TEST_F(GraphViewTest, ContractModeIsIgnoredForValueFlowGraphs) {
  // Guards the policy at run time as well as at compile time: asking the SVFG
  // view to contract must not produce bridges.
  auto module = parseModule(R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  // An empty SVFG is enough: only the mode resolution is under test.
  SVFG svfg;
  FilteredSVFGView view(svfg, FilteredSVFGView::NodeSet{},
                        BridgeMode::Contract);
  EXPECT_EQ(view.bridgeMode(), BridgeMode::Induced);
  EXPECT_EQ(view.bridgeCount(), 0u);
  EXPECT_FALSE(view.hasBridges());
}
