#include "IR/PDG/Construction/GraphBuilder.h"

#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/PDGCallGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "IR/PDG/Query/SliceQuery.h"
#include "IR/PDG/Rules/RuleEngine.h"
#include "TestUtils/LLVMHelpers.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace pdg;

TEST(PDGGraphBuilderTest, RebuildReplacesModuleAndInvalidatesQueryCaches) {
  LLVMContext context;
  auto first = lotus::unittest::parseModuleChecked(
      context, "define void @first() { ret void }", "GraphBuilderTest.first");
  auto second = lotus::unittest::parseModuleChecked(
      context, "define void @second() { ret void }", "GraphBuilderTest.second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  ProgramGraph graph;
  GraphBuilder builder(graph);
  builder.build(*first);
  SliceQuery query(graph);
  PDGCriteria criteria;
  criteria.function_names.push_back("first");
  EXPECT_FALSE(query.forward(criteria).empty());
  EXPECT_GT(query.forward(criteria).diagnostics.summary_cache_hits, 0u);
  const auto epoch = graph.getEpoch();

  builder.build(*second);
  EXPECT_GT(graph.getEpoch(), epoch);
  EXPECT_FALSE(graph.isBuiltForModule(*first));
  EXPECT_TRUE(graph.isBuiltForModule(*second));
  EXPECT_FALSE(graph.hasFuncWrapper(*first->getFunction("first")));
  ASSERT_EQ(graph.getFuncWrapperMap().size(), 1u);
  EXPECT_TRUE(query.forward(criteria).empty());
  EXPECT_EQ(
      query.forward(PDGCriteria::nodeSet({})).diagnostics.summary_cache_hits,
      0u);
}

TEST(PDGGraphBuilderTest, IndependentInstancesKeepTheirOwnNodesAndWrappers) {
  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, "define void @f() { ret void }", "GraphBuilderTest.independent");
  ASSERT_NE(module, nullptr);
  ProgramGraph left;
  ProgramGraph right;
  GraphBuilder(left).build(*module);
  GraphBuilder(right).build(*module);
  auto &function = *module->getFunction("f");
  ASSERT_NE(left.getNode(function), nullptr);
  ASSERT_NE(right.getNode(function), nullptr);
  EXPECT_NE(left.getNode(function), right.getNode(function));
  EXPECT_NE(left.getFuncWrapper(function), right.getFuncWrapper(function));
  left.reset();
  EXPECT_TRUE(right.isBuiltForModule(*module));
  EXPECT_NE(right.getNode(function), nullptr);
}

TEST(PDGGraphBuilderTest, CallGraphRebuildDropsPriorModulesAndTheirEdges) {
  LLVMContext context;
  auto first = lotus::unittest::parseModuleChecked(
      context,
      "define void @callee() { ret void }\n"
      "define void @caller() { call void @callee() ret void }",
      "GraphBuilderTest.callGraphFirst");
  auto second = lotus::unittest::parseModuleChecked(
      context, "define void @other() { ret void }",
      "GraphBuilderTest.callGraphSecond");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  PDGCallGraph calls;
  calls.build(*first);
  auto *caller = calls.getNode(*first->getFunction("caller"));
  auto *callee = calls.getNode(*first->getFunction("callee"));
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);
  EXPECT_TRUE(caller->getOutNeighbors().count(callee));

  calls.build(*second);
  EXPECT_EQ(calls.getNode(*first->getFunction("caller")), nullptr);
  EXPECT_EQ(calls.numNode(), 1);
  EXPECT_NE(calls.getNode(*second->getFunction("other")), nullptr);
}

TEST(PDGGraphBuilderTest, EmptyScopeExcludesOtherwiseValidCriteria) {
  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, "define void @f() { ret void }", "GraphBuilderTest.emptyScope");
  ASSERT_NE(module, nullptr);
  ProgramGraph graph;
  GraphBuilder(graph).build(*module);
  PDGCriteria criteria;
  criteria.function_names.push_back("f");
  PDGQueryOptions options;
  options.scope = PDGQueryScope::nodeSet({});
  EXPECT_TRUE(SliceQuery(graph).forward(criteria, options).empty());
}

TEST(PDGGraphBuilderTest, DependencePassRebuildsAfterChangesToTheSameModule) {
  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context, "define void @first() { ret void }", "GraphBuilderTest.refresh");
  ASSERT_NE(module, nullptr);
  auto &graph = ProgramGraph::getInstance();
  GraphBuilder(graph).build(*module);
  auto *function = Function::Create(
      FunctionType::get(Type::getVoidTy(context), false),
      GlobalValue::ExternalLinkage, "added", *module);
  auto *block = BasicBlock::Create(context, "entry", function);
  ReturnInst::Create(context, block);
  EXPECT_EQ(graph.getNode(*function), nullptr);

  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  legacy::PassManager passes;
  passes.add(new DataDependencyGraph());
  passes.run(*module);
  EXPECT_NE(graph.getNode(*function), nullptr);
  EXPECT_TRUE(graph.hasFuncWrapper(*function));
  graph.reset();
}

TEST(PDGGraphBuilderTest, EmptySelectionDoesNotFallBackToAllRuleSites) {
  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      "declare i8* @gets(i8*)\n"
      "define void @f(i8* %p) { %r = call i8* @gets(i8* %p) ret void }",
      "GraphBuilderTest.selection");
  ASSERT_NE(module, nullptr);
  ProgramGraph graph;
  GraphBuilder(graph).build(*module);
  RuleEngine rules(graph);
  EXPECT_EQ(
      rules.analyze({"cpp/dangerous-function-overflow"}, {}, {}, module.get())
          .findings.size(),
      1u);
  auto selected = PDGCriteria::nodeSet({});
  EXPECT_FALSE(selected.empty());
  EXPECT_TRUE(rules
                  .analyze({"cpp/dangerous-function-overflow"}, selected, {},
                           module.get())
                  .findings.empty());
}

TEST(PDGGraphBuilderTest, MismatchedArityBitcastCallAndEdgeCountAreSafe) {
  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        define void @callee(i32 %a, i32 %b) {
        entry:
          ret void
        }
        define i32 @caller(i32* %p, i32* %q) {
        entry:
          %slot = alloca i32
          store i32 7, i32* %slot
          %v = load i32, i32* %slot
          %p_alias = bitcast i32* %p to i8*
          %p_back = bitcast i8* %p_alias to i32*
          call void bitcast (void (i32, i32)* @callee to void (i32)*)(i32 %v)
          ret i32 %v
        }
      )",
      "GraphBuilderTest.mismatchedArity");
  ASSERT_NE(module, nullptr);

  auto &graph = ProgramGraph::getInstance();
  graph.reset();

  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  legacy::PassManager passes;
  passes.add(new DataDependencyGraph());
  passes.add(new ControlDependencyGraph());
  passes.add(new ProgramDependencyGraph());
  passes.run(*module);

  EXPECT_GT(graph.numNode(), 0);
  // Regression: GenericGraph::numEdge() previously always returned 0.
  EXPECT_GT(graph.numEdge(), 0);

  // Verify alias edges between cast-equivalent pointers are preserved under
  // default -pdg-aa-under=none (sound over-approximate alias handling).
  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  Instruction *p_alias = nullptr;
  Instruction *p_back = nullptr;
  for (Instruction &inst : caller->getEntryBlock()) {
    if (inst.getName() == "p_alias")
      p_alias = &inst;
    if (inst.getName() == "p_back")
      p_back = &inst;
  }
  ASSERT_NE(p_alias, nullptr);
  ASSERT_NE(p_back, nullptr);
  Node *n1 = graph.getNode(*p_alias);
  Node *n2 = graph.getNode(*p_back);
  ASSERT_NE(n1, nullptr);
  ASSERT_NE(n2, nullptr);
  EXPECT_TRUE(n1->hasOutNeighborWithEdgeType(*n2, EdgeType::DATA_ALIAS));
  graph.reset();
}

TEST(PDGGraphBuilderTest, CallGraphIndirectSignatureAndCyclesDoNotCrash) {
  LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        define i32 @target_a(i32 %x) {
          %r = call i32 @target_b(i32 %x)
          ret i32 %r
        }
        define i32 @target_b(i32 %x) {
          %r = call i32 @target_a(i32 %x)
          ret i32 %r
        }
        define void @wrong_sig(i8* %p) {
          ret void
        }
        define i32 @dispatch(i32 (i32)* %fp, i32 %x) {
          %r = call i32 %fp(i32 %x)
          ret i32 %r
        }
      )",
      "GraphBuilderTest.indirectCycle");
  ASSERT_NE(module, nullptr);

  PDGCallGraph cg;
  cg.build(*module);
  EXPECT_GT(cg.numEdge(), 0);

  Function *dispatch = module->getFunction("dispatch");
  Function *target_a = module->getFunction("target_a");
  Function *target_b = module->getFunction("target_b");
  Function *wrong_sig = module->getFunction("wrong_sig");
  ASSERT_NE(dispatch, nullptr);
  ASSERT_NE(target_a, nullptr);
  ASSERT_NE(target_b, nullptr);
  ASSERT_NE(wrong_sig, nullptr);

  CallBase *ind_call = nullptr;
  for (Instruction &inst : dispatch->getEntryBlock())
    if (auto *cb = dyn_cast<CallBase>(&inst))
      ind_call = cb;
  ASSERT_NE(ind_call, nullptr);

  const auto &cands = cg.getIndirectCallCandidatesFast(*ind_call, *module);
  EXPECT_NE(std::find(cands.begin(), cands.end(), target_a), cands.end());
  EXPECT_NE(std::find(cands.begin(), cands.end(), target_b), cands.end());
  EXPECT_EQ(std::find(cands.begin(), cands.end(), wrong_sig), cands.end());

  Node *src = cg.getNode(*dispatch);
  Node *dst = cg.getNode(*target_b);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);
  EXPECT_TRUE(cg.canReach(*src, *dst));
  auto paths = cg.computePaths(*src, *dst);
  EXPECT_FALSE(paths.empty());
}
