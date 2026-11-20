#include "Dataflow/IFDS/Core/AnalysisSession.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/IRBuilder.h>

namespace ifds {
namespace {
TEST(SharedAnalysisServicesTest, NumberingRecursionAndGlobalPropagation) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    @counter = global i32 0
    @other = global i32 0
    @constant = constant i32 7
    @table = global i32* @other
    @alias = alias i32, i32* @counter
    define void @a() {
      %v = load i32, i32* @alias
      call void @b()
      ret void
    }
    define void @b() {
      %v = load i32, i32* @constant
      call void @a()
      ret void
    }
    define void @isolated() {
      %v = load i32*, i32** @table
      ret void
    }
    define i32 @main() {
      call void @a()
      ret i32 0
    })",
                                                    "shared_services");
  AnalysisSession::Options options;
  options.call_graph = lotus::CallGraphAnalysisType::CHA;
  AnalysisSession session(*module, options);
  auto functions = session.function_index();
  ASSERT_EQ(functions->size(), module->size());
  uint32_t next = 0;
  for (const auto &function : *module) {
    auto id = functions->id(&function);
    ASSERT_TRUE(id);
    EXPECT_EQ(id->value, next++);
    EXPECT_EQ(functions->function(*id), &function);
  }
  EXPECT_FALSE(functions->id(nullptr));
  EXPECT_EQ(functions->function({999}), nullptr);
  auto a = *functions->id(module->getFunction("a"));
  auto b = *functions->id(module->getFunction("b"));
  auto main = *functions->id(module->getFunction("main"));
  auto isolated = *functions->id(module->getFunction("isolated"));
  auto sccs = session.call_graph_sccs();
  EXPECT_EQ(sccs->functions(), functions);
  EXPECT_EQ(sccs->graph(), session.graph());
  EXPECT_EQ(sccs->componentOf(a), sccs->componentOf(b));
  EXPECT_TRUE(sccs->components()->cyclic(sccs->componentOf(a)));
  EXPECT_LT(sccs->componentOf(a), sccs->componentOf(main));
  auto globals = session.used_globals();
  auto *counter = module->getGlobalVariable("counter");
  auto *constant = module->getGlobalVariable("constant");
  auto *other = module->getGlobalVariable("other");
  EXPECT_TRUE(globals->direct(a).referenced.count(counter));
  EXPECT_FALSE(globals->direct(main).referenced.count(counter));
  EXPECT_TRUE(globals->transitive(main).referenced.count(counter));
  EXPECT_TRUE(globals->transitive(a).referenced.count(constant));
  EXPECT_FALSE(globals->transitive(main).referenced.count(other));
  EXPECT_FALSE(globals->transitive(main).may_access_unknown_globals);
  EXPECT_TRUE(globals->transitive(isolated).referenced.count(other));
  auto reach = session.call_graph_reachability();
  EXPECT_EQ(reach->components(), sccs->components());
  EXPECT_TRUE(reach->reaches(main.value, b.value));
  EXPECT_TRUE(reach->reaches(b.value, a.value));
  EXPECT_FALSE(reach->reaches(main.value, isolated.value));
  EXPECT_EQ(session.used_globals(), globals);
  EXPECT_EQ(session.call_graph_sccs(), sccs);
  EXPECT_EQ(session.call_graph_reachability(), reach);
}
TEST(SharedAnalysisServicesTest,
     UnknownCalleesAndIndirectMemoryRemainConservative) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    @g = global i32 0
    declare void @external()
    declare void @pure() readnone
    define void @indirect(void ()* %fp) {
      call void %fp()
      ret void
    }
    define i32 @memory(i32* %p) {
      %v = load i32, i32* %p
      ret i32 %v
    }
    define void @local() {
      %p = alloca i32
      store i32 2, i32* %p
      ret void
    }
    define i32 @main() {
      call void @external()
      call void @pure()
      ret i32 0
    })",
                                                    "unknown_globals");
  auto functions = std::make_shared<FunctionIndex>(*module);
  auto graph = std::make_shared<AnalysisGraph>(*module);
  auto sccs = std::make_shared<CallGraphSCCs>(functions, graph);
  UsedGlobals globals(sccs);
  auto get = [&](const char *name) -> const GlobalUsageSummary & {
    return globals.transitive(*functions->id(module->getFunction(name)));
  };
  EXPECT_TRUE(get("main").may_access_unknown_globals);
  EXPECT_TRUE(get("external").may_access_unknown_globals);
  EXPECT_TRUE(get("indirect").may_access_unknown_globals);
  EXPECT_TRUE(get("memory").may_access_unknown_globals);
  EXPECT_FALSE(get("pure").may_access_unknown_globals);
  EXPECT_FALSE(get("local").may_access_unknown_globals);
  EXPECT_TRUE(get("indirect").mayUse(module->getGlobalVariable("g")));
  EXPECT_FALSE(get("pure").mayUse(module->getGlobalVariable("g")));
}
TEST(SharedAnalysisServicesTest,
     PartialIndirectTargetsKeepKnownEdgesAndUnknownUsage) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    @array = global [2 x i32] zeroinitializer
    define void @leaf() {
      %v = load i32, i32* getelementptr ([2 x i32], [2 x i32]* @array, i64 0, i64 1)
      ret void
    }
    define i32 @main(void ()* %fp) {
      call void %fp()
      ret i32 0
    })",
                                                    "partial_call_graph");
  auto functions = std::make_shared<FunctionIndex>(*module);
  auto *leaf = module->getFunction("leaf");
  auto graph = std::make_shared<AnalysisGraph>(
      *module, nullptr,
      [leaf](const llvm::CallBase &) { return CalleeTargets{{leaf}, false}; });
  auto sccs = std::make_shared<CallGraphSCCs>(functions, graph);
  auto main_id = *functions->id(module->getFunction("main"));
  auto leaf_id = *functions->id(leaf);
  lotus::graph::IntervalReachabilityIndex reach(sccs->components());
  EXPECT_TRUE(reach.reaches(main_id.value, leaf_id.value));
  UsedGlobals globals(sccs);
  EXPECT_TRUE(globals.transitive(main_id).referenced.count(
      module->getGlobalVariable("array")));
  EXPECT_TRUE(globals.transitive(main_id).may_access_unknown_globals);
  EXPECT_FALSE(globals.transitive(leaf_id).may_access_unknown_globals);
}
TEST(SharedAnalysisServicesTest, InvalidationRebuildsAllDerivedSnapshots) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(context, R"(
    define i32 @main() { ret i32 0 }
  )",
                                                    "invalidate_services");
  AnalysisSession::Options options;
  options.call_graph = lotus::CallGraphAnalysisType::CHA;
  AnalysisSession session(*module, options);
  auto functions = session.function_index();
  auto sccs = session.call_graph_sccs();
  auto globals = session.used_globals();
  auto reach = session.call_graph_reachability();
  auto *added = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
      llvm::GlobalValue::ExternalLinkage, "added", module.get());
  auto *block = llvm::BasicBlock::Create(context, "entry", added);
  llvm::IRBuilder<>(block).CreateRetVoid();
  auto *main = module->getFunction("main");
  llvm::IRBuilder<>(main->getEntryBlock().getTerminator()).CreateCall(added);
  session.invalidate();
  auto fresh = session.function_index();
  EXPECT_NE(fresh, functions);
  EXPECT_EQ(fresh->size(), 2U);
  EXPECT_NE(session.call_graph_sccs(), sccs);
  EXPECT_NE(session.used_globals(), globals);
  EXPECT_NE(session.call_graph_reachability(), reach);
  EXPECT_TRUE(session.call_graph_reachability()->reaches(
      fresh->id(main)->value, fresh->id(added)->value));
  // Retained old objects only keep ownership alive; they are not queried after
  // mutation, consistent with AnalysisSession's existing invalidation contract.
}
TEST(SharedAnalysisServicesTest, RejectsMismatchedModuleAndHandlesEmptyModule) {
  llvm::LLVMContext context;
  llvm::Module first("first", context), second("second", context);
  auto functions = std::make_shared<FunctionIndex>(first);
  auto graph = std::make_shared<AnalysisGraph>(second);
  EXPECT_THROW(CallGraphSCCs(functions, graph), std::invalid_argument);
  EXPECT_THROW(CallGraphSCCs(nullptr, graph), std::invalid_argument);
  auto empty_graph = std::make_shared<AnalysisGraph>(first);
  auto sccs = std::make_shared<CallGraphSCCs>(functions, empty_graph);
  EXPECT_EQ(sccs->components()->size(), 0U);
  UsedGlobals globals(sccs);
}
} // namespace
} // namespace ifds
