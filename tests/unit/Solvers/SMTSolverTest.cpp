#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"
#include "Solvers/SMT/LIBSMT/SMTModel.h"
#include "Solvers/SMT/LIBSMT/SMTSolver.h"

#include <gtest/gtest.h>

namespace {

TEST(SMTSolverTest, LazyScopesDeduplicateAndPreserveAssertionsAndModels) {
  SMTFactory factory;
  SMTSolver solver = factory.createSimpleSMTSolver();

  // Empty solver is trivially SAT and can still materialize a model lazily.
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  EXPECT_TRUE(solver.assertions().empty());
  SMTModel emptyModel = solver.getSMTModel();
  (void)emptyModel;

  SMTExpr x = factory.createBitVecConst("x", 32);
  SMTExpr y = factory.createBitVecConst("y", 32);

  // Literal true and duplicate assertions are suppressed.
  solver.add(factory.createBoolVal(true));
  solver.add(x == 7);
  solver.add(x == 7);
  EXPECT_EQ(solver.assertions().size(), 1u);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  // Repeated check with unchanged assertions hits cached result.
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);

  // Unflushed scope push/add/pop never corrupts outer assertions.
  solver.push();
  solver.add(y == 9);
  EXPECT_EQ(solver.getNumScopes(), 1u);
  EXPECT_EQ(solver.assertions().size(), 2u);
  solver.pop();
  EXPECT_EQ(solver.getNumScopes(), 0u);
  EXPECT_EQ(solver.assertions().size(), 1u);

  // Literal false yields O(1) UNSAT and restores SAT on pop.
  solver.push();
  solver.add(factory.createBoolVal(false));
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
  solver.pop();
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);

  // Flushed nested scope push/check/pop refreshes Z3 model cleanly.
  solver.push();
  solver.add(y == x + 1);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  SMTModel nestedModel = solver.getSMTModel();
  EXPECT_EQ(nestedModel.getIntValue(y, true).first, 8);
  solver.pop();

  // After pop() invalidates Z3's model, getSMTModel() lazily re-checks.
  SMTModel outerModel = solver.getSMTModel();
  EXPECT_EQ(outerModel.getIntValue(x, true).first, 7);

  solver.reset();
  EXPECT_TRUE(solver.assertions().empty());
  EXPECT_EQ(solver.getNumScopes(), 0u);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
}

TEST(SMTSolverTest, AddAllAssertsVectorExactlyOnceWithoutSwitchFallthrough) {
  SMTFactory factory;
  SMTSolver solver = factory.createSMTSolverWithTactic("qfbv");

  SMTExpr x = factory.createBitVecConst("x", 32);
  SMTExpr y = factory.createBitVecConst("y", 32);
  SMTExprVec vec = factory.createEmptySMTExprVec();
  vec.push_back(x == 10);
  vec.push_back(y == x + 5);

  solver.addAll(vec);
  EXPECT_EQ(solver.assertions().size(), 2u);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);

  solver.add(y != 15);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Unsat);
}

TEST(SMTSolverTest, QFBVLogicCollapsedScopesAndRenamingWorkCleanly) {
  SMTFactory factory;
  SMTSolver solver = factory.createSMTSolverForLogic("QF_BV");

  SMTExpr x = factory.createBitVecConst("node.x", 32);
  SMTExpr y = factory.createBitVecConst("y", 32);
  ASSERT_NE(x.getSymbolCStr(), nullptr);
  EXPECT_STREQ(x.getSymbolCStr(), "node.x");
  EXPECT_EQ(x.getSymbol(), "node.x");

  // Nested empty pushes collapse cleanly without corrupting scope marks.
  solver.push();
  solver.push();
  SMTExpr eq1 = (x == 42);
  solver.add(eq1);
  EXPECT_TRUE(solver.containsAssertion(static_cast<Z3_ast>(eq1.getZ3Expr())));
  EXPECT_EQ(solver.pendingAssertions().size(), 1u);
  EXPECT_EQ(solver.scopeAssertionMarks().size(), 2u);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);

  solver.push();
  solver.add(y == x + 1);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  EXPECT_EQ(solver.getSMTModel().getIntValue(y, true).first, 43);
  solver.pop();

  // Outer assertion survives inner pop; popping to empty resets cleanly.
  EXPECT_EQ(solver.pendingAssertions().size(), 1u);
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);
  solver.pop(2);
  EXPECT_TRUE(solver.pendingAssertions().empty());
  EXPECT_FALSE(solver.containsAssertion(static_cast<Z3_ast>(eq1.getZ3Expr())));
  EXPECT_EQ(solver.check(), SMTSolver::SMTRT_Sat);

  // Fast hash-table renaming cache + explicit cache clear.
  SMTExprVec vec = factory.createEmptySMTExprVec();
  vec.push_back(x == y);
  std::unordered_map<std::string, SMTExpr> mapping;
  auto renamed1 = factory.rename(vec, "_CS1", mapping);
  EXPECT_FALSE(renamed1.second);
  EXPECT_EQ(renamed1.first.size(), 1u);
  factory.clearRenamingCache();
  mapping.clear();
  auto renamed2 = factory.rename(vec, "_CS2", mapping);
  EXPECT_FALSE(renamed2.second);
  EXPECT_EQ(renamed2.first.size(), 1u);
}

} // namespace
