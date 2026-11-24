#include "IR/PDG/Core/GraphBuilder.h"

#include "IR/PDG/Core/PDGCallGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Query/SliceQuery.h"
#include "IR/PDG/Analysis/RuleQuery.h"
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
  RuleQuery rules(graph);
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
