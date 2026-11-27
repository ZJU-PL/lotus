#include "SymbolicExecution/Domain/BigInteger.h"
#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Domain/SymbolicMemory.h"
#include "SymbolicExecution/Solver/ConstraintRepr.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/Type.h>

using namespace llvm;
using namespace SymbolicExecution;
using namespace lotus::unittest;

namespace {

class SymbolicExecutionDomainTest : public LlvmModuleTest {};

TEST(SymbolicExecutionBigIntegerTest, ArithmeticAndComparison) {
  BigInteger a(42);
  BigInteger b(-10);

  EXPECT_EQ(a + b, BigInteger(32));
  EXPECT_EQ(a - b, BigInteger(52));
  EXPECT_EQ(a * b, BigInteger(-420));
  EXPECT_EQ(-b, BigInteger(10));
  EXPECT_EQ(BigInteger(100).sdiv(BigInteger(4)), BigInteger(25));
  EXPECT_LT(b, a);
  EXPECT_GT(a, b);
}

TEST_F(SymbolicExecutionDomainTest, PropertySymExprArithmeticAndMapping) {
  ASSERT_TRUE(loadModule(R"(
    define void @f() {
    entry:
      ret void
    }
  )"));

  Type *i64Ty = Type::getInt64Ty(context);
  ProgramValuePtr vx(i64Ty, "x");
  ProgramValuePtr vy(i64Ty, "y");

  PropertyValuePtr px = GetProperty<PropertySymExpr>(Var(vx));
  PropertyValuePtr py = GetProperty<PropertySymExpr>(Var(vy));
  PropertyValuePtr c5 = GetProperty<PropertyInteger>(5);

  // 2*x + y + 5 - x => x + y + 5
  PropertyValuePtr expr = (px + px) + py + c5 - px;
  ASSERT_TRUE(IsaProperty<PropertySymExpr>(expr));
  const auto *sym = CastProperty<PropertySymExpr>(expr);
  EXPECT_EQ(sym->getOffsets(), BigInteger(5));
  EXPECT_EQ(sym->getUsedVars().size(), 2u);

  // Cancelling terms reduce back to PropertyInteger
  PropertyValuePtr diff = (px + c5) - px;
  ASSERT_TRUE(IsaProperty<PropertyInteger>(diff));
  EXPECT_EQ(CastProperty<PropertyInteger>(diff)->getAsBoundInt(), 5);

  // Variable substitution
  std::unordered_map<Var, PropertyValuePtr> subst;
  subst[Var(vx)] = GetProperty<PropertyInteger>(10);
  subst[Var(vy)] = GetProperty<PropertyInteger>(7);
  PropertyValuePtr evaluated = expr->map(subst);
  ASSERT_TRUE(IsaProperty<PropertyInteger>(evaluated));
  EXPECT_EQ(CastProperty<PropertyInteger>(evaluated)->getAsBoundInt(), 22);
}

TEST_F(SymbolicExecutionDomainTest, SymbolicMemoryOffsetAndEquality) {
  ASSERT_TRUE(loadModule(R"(
    define void @f() {
    entry:
      ret void
    }
  )"));

  Type *i8PtrTy = Type::getInt8PtrTy(context);
  ProgramValuePtr base(i8PtrTy, "buf");
  PropertyValuePtr sz = GetProperty<PropertyInteger>(64);

  PTItem item(base, PTItem::MK_CONCRETE, 0, sz);
  EXPECT_TRUE(item.isConcrete());
  EXPECT_FALSE(item.isSymbolic());
  EXPECT_FALSE(item.isOffsetSymbolic());
  EXPECT_EQ(item.getConstOffset(), BigInteger(0));

  PTItem shifted = item.offsetBy(16);
  EXPECT_EQ(shifted.getConstOffset(), BigInteger(16));
  EXPECT_EQ(shifted.getAllocSite(), base);
  EXPECT_EQ(shifted.getSize(), sz);
}

TEST_F(SymbolicExecutionDomainTest,
       VacuousVarHashAndCanonicalTrueCondition) {
  ProgramValuePtr empty;
  Var emptyVar(empty);
  EXPECT_EQ(std::hash<Var>()(emptyVar), 0u);

  // Default-constructed Condition and getTrueCond() share canonical index.
  Condition defTrue;
  Condition explicitTrue = Condition::getTrueCond();
  EXPECT_TRUE(defTrue.isTrue());
  EXPECT_EQ(defTrue, explicitTrue);
  EXPECT_EQ(defTrue.getIndex(), explicitTrue.getIndex());

  // BigInteger(0) and BigInteger(-1) retain >= 2 signed bits.
  EXPECT_GE(BigInteger(0).getVal().getBitWidth(), 2u);
  EXPECT_GE(BigInteger(-1).getVal().getBitWidth(), 2u);
}

TEST_F(SymbolicExecutionDomainTest,
       PathCondSolverBatchCheckAndRenameExactness) {
  ASSERT_TRUE(loadModule(R"(
    define void @f() {
    entry:
      ret void
    }
  )"));

  PathCondSolver Solver(module->getDataLayout());
  Type *i32Ty = Type::getInt32Ty(context);
  ProgramValuePtr vx(i32Ty, "x");
  ProgramValuePtr vy(i32Ty, "y");

  SMTExpr xExpr = Solver.getExpr(Var(vx));
  SMTExpr yExpr = Solver.getExpr(Var(vy));
  SMTExpr zero = Solver.buildBitVecVal(0, 32);
  SMTExpr five = Solver.buildBitVecVal(5, 32);

  // Context: x == 5 (SAT); disconnected y is unconstrained.
  SMTExprVec Ctx = Solver.createEmptySMTExprVec();
  Ctx.push_back(xExpr == five);

  SMTExprVec P1 = Solver.createEmptySMTExprVec();
  P1.push_back(xExpr.basic_sgt(zero)); // SAT
  SMTExprVec P2 = Solver.createEmptySMTExprVec();
  P2.push_back(xExpr.basic_slt(zero)); // UNSAT
  SMTExprVec P3 = Solver.createEmptySMTExprVec();
  P3.push_back(xExpr.basic_sgt(zero)); // Duplicate of P1 -> SAT
  SMTExprVec P4 = Solver.createEmptySMTExprVec();
  P4.push_back(yExpr == five); // Disconnected component -> SAT
  SMTExprVec P5 = Solver.createEmptySMTExprVec(); // Empty -> SAT

  auto Res = Solver.batchCheckPredicates(Ctx, {P1, P2, P3, P4, P5});
  ASSERT_EQ(Res.size(), 5u);
  EXPECT_TRUE(Res[0]);
  EXPECT_FALSE(Res[1]);
  EXPECT_TRUE(Res[2]);
  EXPECT_TRUE(Res[3]);
  EXPECT_TRUE(Res[4]);

  // Rename single and compound expressions consistently.
  SMTExpr renamedX = Solver.renameExpr(xExpr, "_cs1");
  SMTExpr renamedCmp = Solver.renameExpr(xExpr == five, "_cs1");
  SMTExprVec CheckEq = Solver.createEmptySMTExprVec();
  CheckEq.push_back(renamedCmp);
  CheckEq.push_back(renamedX != five);
  EXPECT_FALSE(Solver.isConstraintSat(CheckEq));
}

} // namespace
