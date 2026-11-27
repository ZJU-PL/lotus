#include "Checker/GSAF/API/Trace.h"
#include "Checker/GSAF/API/VulnerabilityRegistry.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/GSAF/Engine/Solver.h"
#include "Checker/GSAF/Engine/Summaries.h"
#include "Checker/GSAF/Support/MaskMap.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"
#include "TestUtils/LLVMHelpers.h"

#include <set>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::gsaf;
using namespace lotus::unittest;

namespace {

class GSAFTest : public LlvmModuleTest {
protected:
  struct Pipeline {
    std::unique_ptr<legacy::PassManager> manager;
    GuardedValueFlowGraphBuilderPass *builder;
  };

  Pipeline build(Module &module) {
    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);
    Pipeline result{std::make_unique<legacy::PassManager>(),
                    new GuardedValueFlowGraphBuilderPass};
    result.manager->add(new gsa::ControlDependenceAnalysisPass);
    result.manager->add(new gsa::GateAnalysisPass);
    result.manager->add(result.builder);
    result.manager->run(module);
    return result;
  }
};

TEST_F(GSAFTest, NativeObjectsPreserveIdentityAndTypedSiteQueries) {
  auto module = parseModule(R"(
    define i32 @test(i32* %p, i32 %x) {
      %address = getelementptr i32, i32* %p, i32 1
      %loaded = load i32, i32* %address
      %div = sdiv i32 %loaded, %x
      ret i32 %div
    }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto &graph = pipeline.builder->getGraph(*module->getFunction("test"));
  std::set<unsigned> ids;
  for (const auto &node : graph.nodes()) {
    const GuardedValueFlowObject *object = node.get();
    EXPECT_TRUE(isa<GuardedValueFlowNode>(object));
    EXPECT_FALSE(isa<GuardedValueFlowSite>(object));
    EXPECT_EQ(object->getParentFunction(), graph.getBaseFunction());
    EXPECT_TRUE(ids.insert(object->getObjectId()).second);
  }
  for (const auto &site : graph.sites()) {
    const GuardedValueFlowObject *object = site.get();
    EXPECT_TRUE(isa<GuardedValueFlowSite>(object));
    EXPECT_FALSE(isa<GuardedValueFlowNode>(object));
    EXPECT_TRUE(ids.insert(object->getObjectId()).second);
    EXPECT_EQ(graph.findSite<GuardedValueFlowSite>(site->getInstruction()),
              site.get());
    if (site->getKind() == GuardedValueFlowSite::Kind::Div) {
      EXPECT_NE(dyn_cast<GuardedValueFlowDivSite>(object), nullptr);
      EXPECT_EQ(dyn_cast<GuardedValueFlowCallSite>(object), nullptr);
      EXPECT_EQ(graph.findSite<GuardedValueFlowDivSite>(site->getInstruction()),
                site.get());
    }
  }
}

TEST_F(GSAFTest, CheckerTerminatesOnCyclicPointerValueFlow) {
  auto module = parseModule(R"(
    define i8* @test(i8* %p, i1 %cond) {
    entry:
      br label %loop
    loop:
      %value = phi i8* [ %p, %entry ], [ %next, %loop ]
      %offset = getelementptr i8, i8* %value, i64 1
      %next = select i1 %cond, i8* %offset, i8* %p
      br i1 %cond, label %loop, label %exit
    exit:
      ret i8* %next
    }
  )");
  ASSERT_NE(module, nullptr);
  initializeBuiltinVulnerabilities();
  std::shared_ptr<Vulnerability> vulnerability;
  for (auto *info : registeredVulnerabilities())
    if (info->id() == "gsaf.use-after-free")
      vulnerability = info->getVulnerability();
  ASSERT_NE(vulnerability, nullptr);
  auto wrapper = std::make_shared<VulnerabilityWrapper>();
  wrapper->addVulnerability(vulnerability);
  auto pipeline = build(*module);
  legacy::PassManager checkerPipeline;
  auto *checker = new GSAFChecker(wrapper);
  checkerPipeline.add(checker);
  checkerPipeline.run(*module);
  EXPECT_TRUE(checker->traces(vulnerability).empty());
}

TEST_F(GSAFTest, TraceSnapshotsRetainNullableSourcesAcrossScopePop) {
  auto module = parseModule("define void @test(i32 %x) { ret void }");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto &graph = pipeline.builder->getGraph(*module->getFunction("test"));
  auto *argument = graph.getCommonArgument(0);
  auto *site =
      graph.findReturnSite(graph.getBaseFunction()->front().getTerminator());
  ASSERT_NE(site, nullptr);

  VulnerabilityTraceBuilder history;
  history.add(argument);
  history.add(static_cast<const GuardedValueFlowObject *>(nullptr));
  history.push();
  history.add(site);
  auto snapshot = history.snapshot();
  history.pop();
  EXPECT_EQ(history.size(), 2u);
  EXPECT_EQ(history.sourceNode(), argument);
  EXPECT_EQ(history.sourceSite(), nullptr);
  ASSERT_EQ(snapshot->get_length(), 3);
  EXPECT_EQ(snapshot->head(), argument);
  EXPECT_EQ(snapshot->tail(), site);
  auto copy = *snapshot;
  snapshot->setReported(true);
  EXPECT_FALSE(copy.reported());
  EXPECT_TRUE(snapshot->reported());
}

TEST_F(GSAFTest, SummaryCompositionRetainsConstraintsInSharedContext) {
  auto module = parseModule("define void @test() { ret void }");
  ASSERT_NE(module, nullptr);
  SymbolicSummary source(module->getFunction("test"), 1);
  SymbolicSummary destination(module->getFunction("test"), 2);
  {
    SMTFactory factory;
    auto variable = factory.createBitVecConst("x", 32);
    auto lower = variable > 3;
    auto upper = variable < 8;
    source.addNonSymDeps(SummaryCacheItem(&lower, "", 0));
    source.addSymbDeps(SummaryCacheItem(&upper, "_CS1", 1));
  }
  for (const auto &item : source.getNonSymDepsCache())
    destination.addNonSymDeps(item);
  for (const auto &item : source.getSymbDepsCache())
    destination.addSymbDeps(item);
  // Repeated composition exercises conjunction with an already stored AST.
  for (const auto &item : source.getSymbDepsCache())
    destination.addSymbDeps(item);

  SMTFactory factory;
  auto solver = factory.createSMTSolver();
  for (const auto &item : destination.getNonSymDepsCache())
    solver.add(item.getSMTExprFromCache(&factory).first);
  for (const auto &item : destination.getSymbDepsCache())
    solver.add(item.getSMTExprFromCache(&factory).first);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  solver.add(factory.createBitVecConst("x_CS1", 32) >= 8);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
}

TEST_F(GSAFTest, TraceMetadataHandlesSparseIndicesAndCopy) {
  GuardedValueFlowTrace trace;
  for (int index = 0; index != 10; ++index)
    trace.push(static_cast<const GuardedValueFlowObject *>(nullptr));
  trace.setStepType(9, GuardedValueFlowTrace::STY_USE_SITE);
  EXPECT_EQ(trace.getStepType(0), trace.DEFAULT_STEP_TYPE);
  EXPECT_EQ(trace.getStepType(8), trace.DEFAULT_STEP_TYPE);
  EXPECT_EQ(trace.getStepType(9), GuardedValueFlowTrace::STY_USE_SITE);
  trace.set_trace_type(lotus::trace::SOURCE_NO_SINK);
  trace.set_score(137);
  trace.set_constructive_confidence(0.75f);
  GuardedValueFlowTrace copy(trace);
  EXPECT_EQ(copy.getKeyIdx(), 0);
  EXPECT_EQ(copy.getLEVFSrcIdx(false), 9);
  EXPECT_EQ(copy.get_score(), 100);
  EXPECT_FLOAT_EQ(copy.get_constructive_confidence(), 0.75f);
}

TEST_F(GSAFTest, LLVMTraceConvertsCallerAndCalleeBoundaries) {
  auto module = parseModule(R"(
    define i32 @callee(i32 %argument) { ret i32 %argument }
    define i32 @caller(i32 %x) {
      %result = call i32 @callee(i32 %x)
      ret i32 %result
    }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto *caller = module->getFunction("caller");
  auto *callee = module->getFunction("callee");
  auto *call = &caller->front().front();
  auto *ret = callee->front().getTerminator();
  lotus::trace::LLVMValueTrace values;
  values.push({caller->getArg(0), call});
  values.push({callee->getArg(0), callee});
  values.push({callee->getArg(0), ret});
  values.push({callee, call});
  GuardedValueFlowTrace trace(&values, pipeline.builder);
  auto &caller_graph = pipeline.builder->getGraph(*caller);
  auto &callee_graph = pipeline.builder->getGraph(*callee);
  ASSERT_GT(trace.get_length(), 4);
  EXPECT_EQ(trace.head(), caller_graph.findNode(caller->getArg(0)));
  EXPECT_EQ(trace.tail(), caller_graph.findNode(call));
  bool has_call = false;
  bool has_return = false;
  for (const auto *object : trace) {
    has_call |= object == caller_graph.findCallSite(call);
    has_return |= object == callee_graph.findReturnSite(ret);
  }
  EXPECT_TRUE(has_call);
  EXPECT_TRUE(has_return);
  trace.resetWithLLVMValueTrace(nullptr, pipeline.builder);
  EXPECT_EQ(trace.get_length(), 0);
}

TEST_F(GSAFTest, SummaryCachesSurviveSourceSMTContextDestruction) {
  auto module = parseModule("define void @test() { ret void }");
  ASSERT_NE(module, nullptr);
  SymbolicSummary summary(module->getFunction("test"), 2);
  {
    SMTFactory factory;
    auto variable = factory.createBitVecConst("x", 32);
    auto lower = variable > 3;
    auto upper = variable < 8;
    summary.addNonSymDeps(SummaryCacheItem(&lower, "_CS1", 1));
    summary.addNonSymDeps(SummaryCacheItem(&upper, "_CS1", 1));
    summary.addSymbDeps(SummaryCacheItem(&lower, "_CS2", 2));
    summary.addSymbDeps(SummaryCacheItem(&upper, "_CS2", 2));
  }
  ASSERT_EQ(summary.getNonSymDepsCache().size(), 1u);
  ASSERT_EQ(summary.getSymbDepsCache().size(), 1u);
  SMTFactory destination;
  auto solver = destination.createSMTSolver();
  solver.add(
      summary.getNonSymDepsCache()[0].getSMTExprFromCache(&destination).first);
  solver.add(
      summary.getSymbDepsCache()[0].getSMTExprFromCache(&destination).first);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  solver.add(destination.createBitVecConst("x_CS1", 32) == 2);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
  EXPECT_EQ(summary.getInlineDepth(), 2u);
}

TEST_F(GSAFTest, SymbolicAndTraceSummaryMasksRemainIndependent) {
  auto module = parseModule("define void @test(i32 %x) { ret void }");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto *function = module->getFunction("test");
  auto &graph = pipeline.builder->getGraph(*function);
  auto *argument = graph.getCommonArgument(0);
  auto trace = std::make_shared<VulnerabilityTrace>(argument, nullptr);
  InputSummary input(function, trace, 3);
  SymbolicSummary symbolic(function, argument, 4);
  input.setInputs({argument});
  input.setVulnerabilityMask(0x5);
  symbolic.setVulnerabilityMask(0x2);
  EXPECT_EQ(input.getInputs().count(argument), 1u);
  EXPECT_EQ(input.getVulnerabilityMask(), 0x5);
  EXPECT_EQ(symbolic.getVulnerabilityMask(), 0x2);
  EXPECT_EQ(symbolic.getSummarizedNode(), argument);
  EXPECT_TRUE(isa<TraceSummary>(&input));
  EXPECT_FALSE(isa<TraceSummary>(&symbolic));
}

TEST_F(GSAFTest, SolverPreservesSourceScalarAndPackedAggregateEncoding) {
  auto module = parseModule(R"(
    target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
    define void @test({i8, i32} %argument) { ret void }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto &graph = pipeline.builder->getGraph(*module->getFunction("test"));
  auto *floating = ConstantFP::get(Type::getFloatTy(context), 1.25);
  auto *floating_node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, floating->getType(), &graph,
      &graph.getBaseFunction()->front(), floating);
  auto *wide_integer =
      ConstantInt::get(context, APInt(128, "18446744073709551619", 10));
  auto *wide_node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, wide_integer->getType(),
      &graph, &graph.getBaseFunction()->front(), wide_integer);

  SMTFactory native_factory;
  GuardedValueFlowSolver native(native_factory, module->getDataLayout());
  EXPECT_EQ(native.getOrInsertExpr(floating_node).getNumeralUint64(),
            cast<ConstantFP>(floating)
                ->getValueAPF()
                .bitcastToAPInt()
                .getZExtValue());
  EXPECT_EQ(native.getOrInsertExpr(graph.getCommonArgument(0)).getBitVecSize(),
            64u);

  SMTFactory gsaf_factory;
  GSAFSolver gsaf(gsaf_factory, module->getDataLayout());
  EXPECT_EQ(gsaf.getOrInsertExpr(floating_node).getNumeralUint64(), 1u);
  EXPECT_EQ(gsaf.getOrInsertExpr(wide_node).getNumeralUint64(), 3u);
  EXPECT_EQ(gsaf.getOrInsertExpr(graph.getCommonArgument(0)).getBitVecSize(),
            40u);
}

TEST_F(GSAFTest, SolverRetainsGlobalSymbolIdentityAcrossFunctions) {
  auto module = parseModule(R"(
    @global_value = global i32 0
    define i32* @first() { ret i32* @global_value }
    define i32* @second() { ret i32* @global_value }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto *global = module->getGlobalVariable("global_value");
  auto &first = pipeline.builder->getGraph(*module->getFunction("first"));
  auto &second = pipeline.builder->getGraph(*module->getFunction("second"));
  SMTFactory factory;
  GSAFSolver solver(factory, module->getDataLayout());
  auto first_expr = solver.getOrInsertExpr(first.findNode(global));
  auto second_expr = solver.getOrInsertExpr(second.findNode(global));
  EXPECT_EQ(first_expr.getSymbol(), "global_global_value");
  EXPECT_EQ(first_expr.getSymbol(), second_expr.getSymbol());
  solver.add(first_expr != second_expr);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
}

TEST_F(GSAFTest, PerCheckerMaskResultsUpdateAndResetIndependently) {
  MaskMap<int> results;
  results.insert(0x3, 10);
  results.insert(0x2, 20);
  MaskMap<int>::ResultVecTy query;
  results.find(0x7, query, 30);
  ASSERT_EQ(query.size(), 3u);
  EXPECT_EQ(query[0], std::make_pair(0x1, 10));
  EXPECT_EQ(query[1], std::make_pair(0x2, 20));
  EXPECT_EQ(query[2], std::make_pair(0x4, 30));
  results.clear();
  results.insert(0x4, 40);
  ASSERT_EQ(results.find(0x7)->size(), 1u);
  EXPECT_EQ(results.find(0x7)->front(), std::make_pair(0x4, 40));

  MultiMaskMap<int> summaries;
  summaries.insert(0x3, 1);
  summaries.insert(0x3, 2);
  auto matching = summaries.find(0x2);
  ASSERT_EQ(matching->size(), 1u);
  EXPECT_EQ(matching->front().first, 0x2);
  EXPECT_EQ(*matching->front().second, (std::set<int>{1, 2}));
  summaries.clear();
  EXPECT_FALSE(summaries.contains(0x2));
}

TEST_F(GSAFTest, SolverKeepsPseudoInputsOutOfTheCallOutputCache) {
  auto module = parseModule(R"(
    declare void @effect()
    define void @test() { call void @effect() ret void }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto &graph = pipeline.builder->getGraph(*module->getFunction("test"));
  auto *block = &graph.getBaseFunction()->front();
  auto *pseudo_input = graph.createNode<GuardedValueFlowCallOutputNode>(
      GuardedValueFlowNode::Kind::CallSitePseudoInput,
      Type::getInt32Ty(context), &graph, block, nullptr, &block->front(),
      module->getFunction("effect"));
  SMTFactory native_factory;
  GuardedValueFlowSolver native(native_factory, module->getDataLayout());
  native.getOrInsertExpr(pseudo_input);
  auto native_outputs = native.getUsedCallSiteOutputs(true);
  EXPECT_EQ(std::distance(native_outputs.first, native_outputs.second), 1);
  SMTFactory factory;
  GSAFSolver solver(factory, module->getDataLayout());
  solver.getOrInsertExpr(pseudo_input);
  auto outputs = solver.getUsedCallSiteOutputs(true);
  EXPECT_EQ(outputs.first, outputs.second);
}

TEST_F(GSAFTest, SolverHandlesOpaqueStructWithoutZeroWidthBitVector) {
  auto module = parseModule(R"(
    %opaque = type opaque
    define void @test(%opaque* %p) { ret void }
  )");
  ASSERT_NE(module, nullptr);
  auto pipeline = build(*module);
  auto &graph = pipeline.builder->getGraph(*module->getFunction("test"));
  auto *opaque_ty = StructType::getTypeByName(context, "opaque");
  ASSERT_NE(opaque_ty, nullptr);
  auto *opaque_node = graph.createNode<GuardedValueFlowNode>(
      GuardedValueFlowNode::Kind::SimpleOperand, opaque_ty, &graph,
      &graph.getBaseFunction()->front(), nullptr);

  SMTFactory factory;
  GSAFSolver solver(factory, module->getDataLayout());
  auto expr = solver.getOrInsertExpr(opaque_node);
  EXPECT_EQ(expr.getBitVecSize(),
            module->getDataLayout().getPointerSizeInBits());
  solver.add(expr != 0);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
}

TEST_F(GSAFTest, DeadOutputSummaryChainTerminatesQuicklyWithoutExplosion) {
  auto module = parseModule(R"(
    define i8* @id_two_paths(i8* %p, i1 %c) {
    entry:
      br i1 %c, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %r = phi i8* [ %p, %then ], [ %p, %else ]
      ret i8* %r
    }

    define void @caller(i8* %p, i1 %c) {
    entry:
      %c0 = call i8* @id_two_paths(i8* %p, i1 %c)
      %c1 = call i8* @id_two_paths(i8* %c0, i1 %c)
      %c2 = call i8* @id_two_paths(i8* %c1, i1 %c)
      %c3 = call i8* @id_two_paths(i8* %c2, i1 %c)
      %c4 = call i8* @id_two_paths(i8* %c3, i1 %c)
      %c5 = call i8* @id_two_paths(i8* %c4, i1 %c)
      %c6 = call i8* @id_two_paths(i8* %c5, i1 %c)
      %c7 = call i8* @id_two_paths(i8* %c6, i1 %c)
      %c8 = call i8* @id_two_paths(i8* %c7, i1 %c)
      %c9 = call i8* @id_two_paths(i8* %c8, i1 %c)
      %c10 = call i8* @id_two_paths(i8* %c9, i1 %c)
      %c11 = call i8* @id_two_paths(i8* %c10, i1 %c)
      %c12 = call i8* @id_two_paths(i8* %c11, i1 %c)
      %c13 = call i8* @id_two_paths(i8* %c12, i1 %c)
      %c14 = call i8* @id_two_paths(i8* %c13, i1 %c)
      %c15 = call i8* @id_two_paths(i8* %c14, i1 %c)
      %c16 = call i8* @id_two_paths(i8* %c15, i1 %c)
      %c17 = call i8* @id_two_paths(i8* %c16, i1 %c)
      %c18 = call i8* @id_two_paths(i8* %c17, i1 %c)
      %c19 = call i8* @id_two_paths(i8* %c18, i1 %c)
      ret void
    }

    define void @root(i8* %p, i1 %c) {
    entry:
      call void @caller(i8* %p, i1 %c)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  initializeBuiltinVulnerabilities();
  for (const char *id : {"gsaf.taint", "gsaf.use-after-free"}) {
    std::shared_ptr<Vulnerability> vulnerability;
    for (auto *info : registeredVulnerabilities())
      if (info->id() == id)
        vulnerability = info->getVulnerability();
    ASSERT_NE(vulnerability, nullptr);
    auto wrapper = std::make_shared<VulnerabilityWrapper>();
    wrapper->addVulnerability(vulnerability);
    legacy::PassManager pm;
    auto *checker = new GSAFChecker(wrapper);
    pm.add(checker);
    pm.run(*module);
    EXPECT_TRUE(checker->traces(vulnerability).empty());
  }
}

TEST_F(GSAFTest, MergeDisjunctiveConstraintsFactorsSharedDeferredItems) {
  auto module = parseModule("define void @test() { ret void }");
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("test");

  SymbolicSummary s1(F, 1);
  SymbolicSummary s2(F, 2);
  SymbolicSummary merged(F, 1);
  {
    SMTFactory factory;
    SMTExpr x = factory.createBitVecConst("x", 32);
    SMTExpr y = factory.createBitVecConst("y", 32);
    SMTExpr shared = (y == 7);
    SMTExpr c1 = (x == 1);
    SMTExpr c2 = (x == 2);
    s1.addNonSymDeps(SummaryCacheItem(&c1, "", 0));
    s1.addNonSymDeps(SummaryCacheItem(&shared, "_CS_shared", 1));
    s2.addNonSymDeps(SummaryCacheItem(&c2, "", 0));
    s2.addNonSymDeps(SummaryCacheItem(&shared, "_CS_shared", 1));
  }

  std::vector<const SummaryBase *> group{&s1, &s2};
  merged.mergeDisjunctiveConstraints(group, nullptr);

  // Shared deferred item stays factored out instead of duplicated/renamed eagerly.
  bool foundSharedDeferred = false;
  for (const auto &item : merged.getNonSymDepsCache()) {
    if (item.suffix == "_CS_shared")
      foundSharedDeferred = true;
  }
  EXPECT_TRUE(foundSharedDeferred);

  SMTFactory checkFactory;
  SMTSolver solver = checkFactory.createSMTSolverForLogic("QF_BV");
  for (const auto &item : merged.getNonSymDepsCache())
    solver.add(item.getSMTExprFromCache(&checkFactory).first);

  SMTExpr x = checkFactory.createBitVecConst("x", 32);
  SMTExpr ycs = checkFactory.createBitVecConst("y_CS_shared", 32);

  solver.push();
  solver.add(x == 2);
  solver.add(ycs == 7);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  solver.pop();

  solver.push();
  solver.add(x == 3);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
  solver.pop();

  solver.push();
  solver.add(x == 1);
  solver.add(ycs != 7);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
  solver.pop();
}

TEST_F(GSAFTest, CyclicArgumentSummaryWithDirectUseAndDominatedDeadBranch) {
  // Exercises:
  // 1) summarizeArgumentDAG on a cyclic argument graph that also has a direct
  //    use-site on the argument node itself (LocalTarget == LocalArg), ensuring
  //    TraceSummary::getTailNode() never sees nullptr.
  // 2) Dead dominated branch does not falsely suppress a feasible sibling use
  //    site (properDominators -> properlyDominatedBlocks regression).
  auto module = parseModule(R"(
    declare void @free(i8*)
    declare i8* @malloc(i64)

    define i8* @cyclic_with_direct_use(i8* %p, i1 %c1, i1 %c2) {
    entry:
      %direct = load i8, i8* %p
      br i1 %c1, label %loop, label %early
    early:
      ret i8* %p
    loop:
      %cur = phi i8* [ %p, %entry ], [ %next, %loop ]
      %step = getelementptr i8, i8* %cur, i64 1
      %next = select i1 %c2, i8* %step, i8* %p
      br i1 %c2, label %loop, label %exit
    exit:
      %after = load i8, i8* %next
      ret i8* %next
    }

    define i8 @uaf_with_dead_dominated_branch(i1 %flag) {
    entry:
      %buf = call i8* @malloc(i64 16)
      call void @free(i8* %buf)
      br i1 %flag, label %dead_check, label %live_use
    dead_check:
      %not_flag = xor i1 %flag, true
      br i1 %not_flag, label %unreachable_use, label %live_use
    unreachable_use:
      %dead_v = load i8, i8* %buf
      ret i8 %dead_v
    live_use:
      %live_v = load i8, i8* %buf
      %passed = call i8* @cyclic_with_direct_use(i8* %buf, i1 %flag, i1 false)
      %ret_v = load i8, i8* %passed
      ret i8 %ret_v
    }
  )");
  ASSERT_NE(module, nullptr);
  initializeBuiltinVulnerabilities();
  std::shared_ptr<Vulnerability> uaf;
  for (auto *info : registeredVulnerabilities())
    if (info->id() == "gsaf.use-after-free")
      uaf = info->getVulnerability();
  ASSERT_NE(uaf, nullptr);

  auto wrapper = std::make_shared<VulnerabilityWrapper>();
  wrapper->addVulnerability(uaf);
  legacy::PassManager pm;
  auto *checker = new GSAFChecker(wrapper);
  pm.add(checker);
  pm.run(*module);
  EXPECT_FALSE(checker->traces(uaf).empty());
}

} // namespace
