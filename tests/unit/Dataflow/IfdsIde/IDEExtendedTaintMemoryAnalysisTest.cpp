#include <gtest/gtest.h>

#include "Dataflow/IFDS/Analyses/IDEExtendedTaintAnalysis.h"
#include "Dataflow/IFDS/Solver/IDESolver.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/LLVMContext.h>

namespace ifds {
namespace {

TEST(IDEExtendedTaintMemoryAnalysisTest, InternsBoundedLocationsAndPrefixes) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        define i32 @main(i8* %ptr) {
        entry:
          %field = getelementptr i8, i8* %ptr, i64 4
          ret i32 0
        }
      )",
      "ide_extended_memory_location");
  const auto *main = module->getFunction("main");
  const auto *ptr = main->getArg(0);
  const auto *field = lotus::unittest::findInstructionByName(*main, "field");

  AbstractMemoryLocationFactory factory(&module->getDataLayout());
  const auto base = factory.create(ptr, 3);
  const auto gep = factory.create(field, 3);
  ASSERT_EQ(base.base(), ptr);
  ASSERT_EQ(gep.base(), ptr);
  ASSERT_EQ(gep.offsets().size(), 1u);
  EXPECT_EQ(gep.offsets().front(), 4);
  EXPECT_TRUE(base.is_proper_prefix_of(factory.with_indirection(base)));
  EXPECT_EQ(factory.create(field, 3), gep);
}

TEST(IDEExtendedTaintMemoryAnalysisTest, PreservesLoadIndirectionInPaths) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        define i8 @main(i8* %ptr) {
        entry:
          %slot = alloca i8*
          store i8* %ptr, i8** %slot
          %loaded = load i8*, i8** %slot
          ret i8 0
        }
      )",
      "ide_extended_memory_load_path");
  const auto *main = module->getFunction("main");
  const auto *slot = lotus::unittest::findInstructionByName(*main, "slot");
  const auto *loaded = lotus::unittest::findInstructionByName(*main, "loaded");

  AbstractMemoryLocationFactory factory(&module->getDataLayout());
  const auto slot_location = factory.create(slot, 3);
  const auto loaded_location = factory.create(loaded, 3);
  ASSERT_EQ(slot_location.offsets().size(), 1u);
  ASSERT_EQ(loaded_location.offsets().size(), 2u);
  EXPECT_EQ(loaded_location.offsets()[0], 0);
  EXPECT_EQ(loaded_location.offsets()[1], 0);
}

TEST(IDEExtendedTaintMemoryAnalysisTest, SourceResultUsesMemoryFact) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        declare i8* @recv()
        define i32 @main() {
        entry:
          %input = call i8* @recv()
          ret i32 0
        }
      )",
      "ide_extended_memory_source");
  const auto *call = lotus::unittest::findInstructionByName(
      *module->getFunction("main"), "input");

  TaintConfig config;
  config.sources.insert("recv");
  IDEAliasAwareExtendedTaintAnalysis problem(config);
  IDESolver<IDEAliasAwareExtendedTaintAnalysis> solver(problem);
  solver.solve(*module);

  const auto fact = problem.memory_location(call);
  const auto *ret = module->getFunction("main")->back().getTerminator();
  EXPECT_EQ(solver.get_value_at(ret, fact).kind, ExtendedTaintValue::Tainted);

  IDEAliasAwareExtendedTaintAnalysis sparse_problem(config);
  IDESolver<IDEAliasAwareExtendedTaintAnalysis> sparse_solver(sparse_problem);
  auto sparse_config = sparse_solver.get_solver_config();
  sparse_config.set_sparse_execution(true);
  sparse_solver.set_solver_config(sparse_config);
  sparse_solver.solve(*module);
  EXPECT_EQ(
      sparse_solver
          .get_value_at(ret, sparse_problem.memory_location(call))
          .kind,
      ExtendedTaintValue::Tainted);
}

TEST(IDEExtendedTaintMemoryAnalysisTest, AppliesConfiguredPipeToMemoryFacts) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        declare i8* @recv()
        declare void @copy(i8**, i8*)
        define i32 @main() {
        entry:
          %dst = alloca i8*
          %input = call i8* @recv()
          call void @copy(i8** %dst, i8* %input)
          %loaded = load i8*, i8** %dst
          ret i32 0
        }
      )",
      "ide_extended_memory_pipe");

  TaintConfig config;
  config.sources.insert("recv");
  PipeSpec pipe;
  pipe.from.location = TaintSpec::ARG;
  pipe.from.access_mode = TaintSpec::VALUE;
  pipe.from.arg_index = 1;
  pipe.to.location = TaintSpec::ARG;
  pipe.to.access_mode = TaintSpec::DIRECT_DEREF;
  pipe.to.arg_index = 0;
  config.function_specs["copy"].pipe_specs.push_back(pipe);

  IDEAliasAwareExtendedTaintAnalysis problem(config);
  IDESolver<IDEAliasAwareExtendedTaintAnalysis> solver(problem);
  solver.solve(*module);

  const auto *main = module->getFunction("main");
  const auto *loaded =
      lotus::unittest::findInstructionByName(*main, "loaded");
  const auto *ret = main->back().getTerminator();
  EXPECT_EQ(solver.get_value_at(ret, problem.memory_location(loaded)).kind,
            ExtendedTaintValue::Tainted);
}

TEST(IDEExtendedTaintMemoryAnalysisTest, SparseFlowRetainsSanitizerSummaries) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        declare i8* @recv()
        declare i8* @sanitize(i8*)
        define i32 @main() {
        entry:
          %input = call i8* @recv()
          %clean = call i8* @sanitize(i8* %input)
          ret i32 0
        }
      )",
      "ide_extended_memory_sparse_sanitizer");

  TaintConfig config;
  config.sources.insert("recv");
  config.sanitizers.insert("sanitize");
  IDEAliasAwareExtendedTaintAnalysis problem(config);
  IDESolver<IDEAliasAwareExtendedTaintAnalysis> solver(problem);
  auto solver_config = solver.get_solver_config();
  solver_config.set_sparse_execution(true);
  solver.set_solver_config(solver_config);
  solver.solve(*module);

  const auto *main = module->getFunction("main");
  const auto *clean = lotus::unittest::findInstructionByName(*main, "clean");
  const auto *ret = main->back().getTerminator();
  EXPECT_EQ(solver.get_value_at(ret, problem.memory_location(clean)).kind,
            ExtendedTaintValue::Sanitized);
}

TEST(IDEExtendedTaintMemoryAnalysisTest,
     MapsTaintedFormalMemoryBackToActualArgument) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        declare i8* @recv()
        define void @fill(i8** %out) {
        entry:
          %input = call i8* @recv()
          store i8* %input, i8** %out
          ret void
        }
        define i32 @main() {
        entry:
          %slot = alloca i8*
          call void @fill(i8** %slot)
          %loaded = load i8*, i8** %slot
          ret i32 0
        }
      )",
      "ide_extended_memory_interprocedural");

  TaintConfig config;
  config.sources.insert("recv");
  IDEAliasAwareExtendedTaintAnalysis problem(config);
  IDESolver<IDEAliasAwareExtendedTaintAnalysis> solver(problem);
  solver.solve(*module);

  const auto *main = module->getFunction("main");
  const auto *loaded =
      lotus::unittest::findInstructionByName(*main, "loaded");
  const auto *ret = main->back().getTerminator();
  EXPECT_EQ(solver.get_value_at(ret, problem.memory_location(loaded)).kind,
            ExtendedTaintValue::Tainted);
}

} // namespace
} // namespace ifds
