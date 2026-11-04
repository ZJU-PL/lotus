#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/GuardedValueFlowSolver.h"
#include "IR/GVFG/LotusAAWrapper.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

#include <cctype>

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

static std::string solverSymbolForNode(GuardedValueFlowNode *node) {
  std::string description = node ? node->getDescription() : "";
  for (char &ch : description) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
      ch = '_';
  }
  return ("gvfg_" + std::to_string(node ? node->getNodeId() : 0) + "_" +
          description);
}

struct SummaryProducerChain {
  GuardedValueFlowNode *summary_return{nullptr};
  GuardedValueFlowNode *summary_mem{nullptr};
  GuardedValueFlowNode *producer_mem{nullptr};
  GuardedValueFlowNode *summary_sentinel{nullptr};
};

class GuardedValueFlowSolverTest : public LlvmModuleTest {
protected:
  struct BuilderPipeline {
    std::unique_ptr<legacy::PassManager> pm;
    GuardedValueFlowGraphBuilderPass *builder{nullptr};
  };

  struct AdapterPipeline {
    std::unique_ptr<legacy::PassManager> pm;
    LotusAA *lotus{nullptr};
    GuardedValueFlowGraphBuilderPass *builder{nullptr};
  };

  static void initializePassInfra() {
    static bool initialized = false;
    if (initialized)
      return;

    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);
    initialized = true;
  }

  BuilderPipeline runBuilder(Module &M) {
    initializePassInfra();
    BuilderPipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->run(M);
    return pipeline;
  }

  AdapterPipeline runAdapter(Module &M) {
    initializePassInfra();
    AdapterPipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.lotus = new LotusAA();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.lotus);
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->add(new LotusAAWrapper());
    pipeline.pm->run(M);
    return pipeline;
  }
};

TEST_F(GuardedValueFlowSolverTest, EncodesArithmeticValueFlow) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %sum = add i32 %a, %b
      ret i32 %sum
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *sum_inst = dyn_cast<Instruction>(F->getEntryBlock().getTerminator()->getPrevNode());
  ASSERT_NE(sum_inst, nullptr);
  auto *sum_node = graph.findNode(sum_inst);
  ASSERT_NE(sum_node, nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getDataDeps(sum_node));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(0))) == 4);
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(1))) == 6);
  solver.add(solver.getOrInsertExpr(sum_node) != 10);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, EncodesRepeatedOperandOccurrences) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32 %x) {
    entry:
      %sum = add i32 %x, %x
      %same = icmp eq i32 %x, %x
      %selected = select i1 %cond, i32 %x, i32 %x
      ret i32 %selected
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  auto pipeline = runBuilder(*module);
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto find_named = [&](StringRef name) -> GuardedValueFlowNode * {
    for (Instruction &I : instructions(*F)) {
      if (I.getName() == name)
        return graph.findNode(&I);
    }
    return nullptr;
  };
  auto *x = graph.findNode(F->getArg(1));
  auto *sum = find_named("sum");
  auto *same = find_named("same");
  auto *selected = find_named("selected");
  ASSERT_NE(x, nullptr);
  ASSERT_NE(sum, nullptr);
  ASSERT_NE(same, nullptr);
  ASSERT_NE(selected, nullptr);

  {
    SMTFactory factory;
    GuardedValueFlowSolver solver(factory, module->getDataLayout());
    solver.addAll(solver.getDataDeps(sum));
    solver.add(solver.getOrInsertExpr(x) == 4);
    solver.add(solver.getOrInsertExpr(sum) != 8);
    EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
  }
  {
    SMTFactory factory;
    GuardedValueFlowSolver solver(factory, module->getDataLayout());
    solver.addAll(solver.getDataDeps(same));
    solver.add(solver.getOrInsertExpr(same) != 1);
    EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
  }
  {
    SMTFactory factory;
    GuardedValueFlowSolver solver(factory, module->getDataLayout());
    solver.addAll(solver.getDataDeps(selected));
    solver.add(solver.getOrInsertExpr(x) == 9);
    solver.add(solver.getOrInsertExpr(selected) != 9);
    EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
  }
}

TEST_F(GuardedValueFlowSolverTest, UsesLLVMInsertElementOperandOrder) {
  const char *source = R"(
    define <2 x i32> @test(<2 x i32> %vec, i32 %value) {
    entry:
      %result = insertelement <2 x i32> %vec, i32 %value, i32 0
      ret <2 x i32> %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  auto pipeline = runBuilder(*module);
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  auto *insert = &F->getEntryBlock().front();
  auto *result = graph.findNode(insert);
  auto *value = graph.findNode(F->getArg(1));
  ASSERT_NE(result, nullptr);
  ASSERT_NE(value, nullptr);
  auto *opcode = dyn_cast<GuardedValueFlowOpcodeNode>(result->getChild(0));
  ASSERT_NE(opcode, nullptr);
  ASSERT_EQ(opcode->getNumOperands(), 3u);
  EXPECT_EQ(opcode->insertedValue(), value);
  EXPECT_EQ(opcode->indexOperand()->getLLVMValue(), insert->getOperand(2));

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getDataDeps(result));
  solver.add(solver.getOrInsertExpr(value) == 1);
  solver.add(solver.getOrInsertExpr(result).array_elmt(2, 0) != 1);
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest,
       OpaqueIntrinsicDependencyDoesNotImplyEquality) {
  const char *source = R"(
    declare i32 @llvm.ctpop.i32(i32)

    define i32 @test(i32 %x) {
    entry:
      %result = call i32 @llvm.ctpop.i32(i32 %x)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  auto pipeline = runBuilder(*module);
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  auto *call = dyn_cast<CallBase>(&F->getEntryBlock().front());
  ASSERT_NE(call, nullptr);
  auto *result = graph.findNode(call);
  auto *x = graph.findNode(F->getArg(0));
  ASSERT_NE(result, nullptr);
  ASSERT_NE(x, nullptr);
  auto *opaque = result->getChild(0);
  ASSERT_NE(opaque, nullptr);
  ASSERT_EQ(opaque->getKind(), GuardedValueFlowNode::Kind::Unknown);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getDataDeps(result));
  solver.add(solver.getOrInsertExpr(x) == 3);
  solver.add(solver.getOrInsertExpr(result) != 3);
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);

  SMTFactory direct_factory;
  GuardedValueFlowSolver direct_solver(direct_factory,
                                       module->getDataLayout());
  direct_solver.addAll(direct_solver.getDeps(opaque, x));
  direct_solver.add(direct_solver.getOrInsertExpr(x) == 3);
  direct_solver.add(direct_solver.getOrInsertExpr(opaque) != 3);
  EXPECT_EQ(direct_solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
}

TEST_F(GuardedValueFlowSolverTest, EnforcesControlDependenciesForControlledBlock) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32 %x) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %inc = add i32 %x, 1
      ret i32 %inc
    else:
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  auto *then_bb = F->getBasicBlockList().getNextNode(F->getEntryBlock());
  ASSERT_NE(then_bb, nullptr);
  auto *inc_inst = dyn_cast<Instruction>(then_bb->begin());
  ASSERT_NE(inc_inst, nullptr);
  auto *inc_node = graph.findNode(inc_inst);
  ASSERT_NE(inc_node, nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getCtrlDeps(inc_node));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(0))) == 0);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, TerminatesOnCyclicControlDependencies) {
  auto module = parseModule(R"(
    define void @test(i1 %cond) {
    entry:
      br label %first
    first:
      br label %second
    second:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = runBuilder(*module);
  auto *function = module->getFunction("test");
  auto &graph = pipeline.builder->getGraph(*function);
  auto *condition = graph.findNode(function->getArg(0));
  ASSERT_NE(condition, nullptr);
  auto *first = function->getEntryBlock().getNextNode();
  auto *second = first->getNextNode();

  // Exercise both a self-dependency and a cycle spanning two blocks. These
  // shapes occur in loop control-dependence graphs, even for valid LLVM IR.
  GuardedValueFlowGraph::BlockCondition guard;
  guard.condition_node = condition;
  guard.control_block = first;
  graph.addBlockCondition(first, guard);
  guard.control_block = second;
  graph.addBlockCondition(first, guard);
  guard.control_block = first;
  graph.addBlockCondition(second, guard);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  for (auto *block : {first, second, first}) {
    solver.push();
    solver.addAll(solver.getCtrlDeps(block, &graph));
    solver.add(solver.getOrInsertExpr(condition) == 0);
    EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
    solver.pop();

    solver.push();
    solver.addAll(solver.getCtrlDeps(block, &graph));
    solver.add(solver.getOrInsertExpr(condition) == 1);
    EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
    solver.pop();
  }
  solver.reset();
  solver.addAll(solver.getCtrlDeps(first, &graph));
  solver.add(solver.getOrInsertExpr(condition) == 1);
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
}

TEST_F(GuardedValueFlowSolverTest, EnforcesPhiIncomingGuards) {
  const char *source = R"(
    define i32 @test(i1 %cond, i32 %x) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %inc = add i32 %x, 1
      br label %merge
    else:
      br label %merge
    merge:
      %phi = phi i32 [ %inc, %then ], [ %x, %else ]
      ret i32 %phi
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  BasicBlock *then_bb = nullptr;
  Instruction *inc_inst = nullptr;
  PHINode *phi_inst = nullptr;
  for (BasicBlock &BB : *F) {
    if (BB.getName() == "then")
      then_bb = &BB;
    for (Instruction &I : BB) {
      if (BB.getName() == "then" && isa<BinaryOperator>(&I))
        inc_inst = &I;
      if (auto *phi = dyn_cast<PHINode>(&I))
        phi_inst = phi;
    }
  }
  ASSERT_NE(then_bb, nullptr);
  ASSERT_NE(inc_inst, nullptr);
  ASSERT_NE(phi_inst, nullptr);

  auto *phi_node = dyn_cast<GuardedValueFlowPhiNode>(graph.findNode(phi_inst));
  auto *inc_node = graph.findNode(inc_inst);
  ASSERT_NE(phi_node, nullptr);
  ASSERT_NE(inc_node, nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getPhiGated(phi_node, inc_node, then_bb));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(0))) == 0);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, PropagatesAdaptedLoadStoreValueFlow) {
  const char *source = R"(
    define i32 @test(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %loaded = load i32, i32* %p
      ret i32 %loaded
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runAdapter(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  StoreInst *store_inst = nullptr;
  LoadInst *load_inst = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *store = dyn_cast<StoreInst>(&I))
      store_inst = store;
    if (auto *load = dyn_cast<LoadInst>(&I))
      load_inst = load;
  }
  ASSERT_NE(store_inst, nullptr);
  ASSERT_NE(load_inst, nullptr);

  auto *load_node = graph.findNode(load_inst);
  auto *load_mem = graph.findLoadMemoryNode(load_inst);
  auto *store_mem =
      graph.findStoreMemoryNode(store_inst->getValueOperand(), store_inst);
  ASSERT_NE(load_node, nullptr);
  ASSERT_NE(load_mem, nullptr);
  ASSERT_NE(store_mem, nullptr);
  ASSERT_NE(load_mem->getMatchingRegion(store_mem), nullptr);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  solver.addAll(solver.getDataDeps(load_node));
  solver.add(solver.getOrInsertExpr(graph.findNode(F->getArg(1))) == 7);
  solver.add(solver.getOrInsertExpr(load_node) != 7);

  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Unsat);
}

TEST_F(GuardedValueFlowSolverTest, ExcludesSummarySentinelChainsFromBaseSolver) {
  const char *source = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());

  GuardedValueFlowGraph graph(F);
  SummaryProducerChain chain;
  chain.summary_return = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(context),
      &graph, &F->getEntryBlock(), nullptr, ret_inst);
  chain.summary_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context), &graph,
      &F->getEntryBlock(), nullptr, ret_inst);
  chain.producer_mem = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context), &F->getEntryBlock(), ret_inst,
      "summary.producer.mem");
  chain.summary_sentinel = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst, F, 0);
  chain.summary_sentinel->setDescription("summary.output.value");

  chain.summary_return->addChild(chain.summary_mem);
  chain.summary_mem->addChild(chain.producer_mem);
  chain.summary_mem->addMatchingRegion(chain.producer_mem,
                                       graph.getAlwaysTrueRegion());
  chain.producer_mem->addChild(chain.summary_sentinel);

  SMTFactory factory;
  GuardedValueFlowSolver solver(factory, module->getDataLayout());
  SMTExprVec deps = solver.getDataDeps(chain.summary_mem);

  std::string deps_str;
  ASSERT_TRUE(deps.SMTExprVecToStream(deps_str));
  EXPECT_EQ(deps_str.find(solverSymbolForNode(chain.summary_sentinel)),
            std::string::npos);

  solver.addAll(deps);
  solver.add(solver.getOrInsertExpr(chain.summary_mem) !=
             solver.getOrInsertExpr(chain.producer_mem));
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
}

TEST_F(GuardedValueFlowSolverTest, ExcludesSummarySentinelChainsFromDTSolver) {
  const char *source = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  auto *ret_inst = cast<ReturnInst>(F->getEntryBlock().getTerminator());

  GuardedValueFlowGraph graph(F);
  SummaryProducerChain chain;
  chain.summary_return = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, Type::getInt32Ty(context),
      &graph, &F->getEntryBlock(), nullptr, ret_inst);
  chain.summary_mem = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::LoadMemory, Type::getInt32Ty(context), &graph,
      &F->getEntryBlock(), nullptr, ret_inst);
  chain.producer_mem = graph.createAnonymousStoreMemoryNode(
      Type::getInt32Ty(context), &F->getEntryBlock(), ret_inst,
      "summary.producer.mem");
  chain.summary_sentinel = graph.createNode<GuardedValueFlowCallSummaryNode>(
      GuardedValueFlowNode::Kind::CallSiteReturnSummary,
      Type::getInt32Ty(context), &graph, &F->getEntryBlock(), ret_inst, F, 0);
  chain.summary_sentinel->setDescription("summary.output.value");

  chain.summary_return->addChild(chain.summary_mem);
  chain.summary_mem->addChild(chain.producer_mem);
  chain.summary_mem->addMatchingRegion(chain.producer_mem,
                                       graph.getAlwaysTrueRegion());
  chain.producer_mem->addChild(chain.summary_sentinel);

  DominatorTree dt(*F);
  SMTFactory factory;
  DTGuardedValueFlowSolver solver(factory, module->getDataLayout(), &dt);
  GuardedValueFlowSolver::QueryContext context{&F->getEntryBlock()};
  SMTExprVec deps = solver.getDataDeps(chain.summary_mem, &context);

  std::string deps_str;
  ASSERT_TRUE(deps.SMTExprVecToStream(deps_str));
  EXPECT_EQ(deps_str.find(solverSymbolForNode(chain.summary_sentinel)),
            std::string::npos);

  solver.addAll(deps);
  solver.add(solver.getOrInsertExpr(chain.summary_mem) !=
             solver.getOrInsertExpr(chain.producer_mem));
  EXPECT_EQ(solver.check(), GuardedValueFlowSolver::SMTRT_Sat);
}

} // namespace
