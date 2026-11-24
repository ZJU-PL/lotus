#include <gtest/gtest.h>

#include "Dataflow/IFDS/Analyses/IDEConstantPropagation.h"
#include "Dataflow/IFDS/Solver/IDESolver.h"
#include "Dataflow/IFDS/Solver/IterativeIDESolver.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/LLVMContext.h>

namespace ifds {
namespace {

TEST(IterativeIDESolverTest, CompactIdsPreserveResultsAndCanBeReleased) {
  llvm::LLVMContext context;
  auto module = lotus::unittest::parseModuleChecked(
      context,
      R"(
        define i32 @main() {
        entry:
          %slot = alloca i32
          store i32 7, i32* %slot
          %value = load i32, i32* %slot
          ret i32 %value
        }
      )",
      "iterative_ide_compact");

  IDEConstantPropagation problem;
  IterativeIDESolver<IDEConstantPropagation> iterative(problem);
  iterative.solve(*module);

  const auto *ret = module->getFunction("main")->back().getTerminator();
  const auto *value = lotus::unittest::findInstructionByName(
      *module->getFunction("main"), "value");

  IDESolver<IDEConstantPropagation> baseline(problem);
  baseline.solve(*module);
  EXPECT_EQ(iterative.get_value_at(ret, value),
            baseline.get_value_at(ret, value));

  const auto stats = iterative.get_iterative_statistics();
  EXPECT_GT(stats.path_edges, 0U);
  EXPECT_GT(stats.unique_instructions, 0U);
  EXPECT_NE(iterative.instruction_id(ret),
            IterativeIDESolver<IDEConstantPropagation>::invalid_id());

  iterative.release_compact_storage();
  const auto released = iterative.get_iterative_statistics();
  EXPECT_EQ(released.path_edges, 0U);
  EXPECT_EQ(released.unique_instructions, 0U);
  EXPECT_EQ(released.unique_facts, 0U);
}

} // namespace
} // namespace ifds
