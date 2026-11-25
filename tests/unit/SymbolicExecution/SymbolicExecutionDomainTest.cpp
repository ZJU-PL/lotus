#include "SymbolicExecution/Domain/BigInteger.h"
#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Domain/SymbolicMemory.h"
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

} // namespace
