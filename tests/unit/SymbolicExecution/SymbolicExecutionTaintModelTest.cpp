#include "SymbolicExecution/Models/TaintModel.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace SymbolicExecution;
using namespace lotus::unittest;

namespace {

class SymbolicExecutionTaintModelTest : public LlvmModuleTest {};

TEST_F(SymbolicExecutionTaintModelTest, BuiltinReturnSourcesAreRecognized) {
  TaintModel model;

  ASSERT_TRUE(loadModule(R"(
    declare i8* @getenv(i8*)
    define i8* @caller(i8* %name) {
    entry:
      %rv = call i8* @getenv(i8* %name)
      ret i8* %rv
    }
  )"));

  auto *func = module->getFunction("getenv");
  ASSERT_NE(func, nullptr);
  EXPECT_TRUE(model.isFunctionRetAsSource(func));
  EXPECT_FALSE(model.isFunctionArgAsSource(func));
}

TEST_F(SymbolicExecutionTaintModelTest, BuiltinArgumentSourcesAreRecognized) {
  TaintModel model;

  ASSERT_TRUE(loadModule(R"(
    declare i64 @read(i32, i8*, i64)
    define i64 @caller(i32 %fd, i8* %buf, i64 %len) {
    entry:
      %rv = call i64 @read(i32 %fd, i8* %buf, i64 %len)
      ret i64 %rv
    }
  )"));

  auto *func = module->getFunction("read");
  ASSERT_NE(func, nullptr);
  EXPECT_FALSE(model.isFunctionRetAsSource(func));
  EXPECT_TRUE(model.isFunctionArgAsSource(func));

  const auto *args = model.getTaintSourceArguments(func);
  ASSERT_NE(args, nullptr);
  ASSERT_EQ(args->size(), 1u);
  EXPECT_EQ((*args)[0], 1);
}

TEST_F(SymbolicExecutionTaintModelTest, BuiltinTransferVectorDefaultsToEmpty) {
  TaintModel model;

  ASSERT_TRUE(loadModule(R"(
    declare i8* @memcpy(i8*, i8*, i64)
    define i8* @caller(i8* %dst, i8* %src) {
    entry:
      %rv = call i8* @memcpy(i8* %dst, i8* %src, i64 4)
      ret i8* %rv
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *call = findInstruction<CallInst>(*caller, "rv");
  ASSERT_NE(call, nullptr);

  std::vector<Value *> dsts;
  model.getTransferDstVect(call, call->getArgOperand(1), dsts);
  EXPECT_TRUE(dsts.empty());
  EXPECT_TRUE(model.isSinkFunction(module->getFunction("memcpy")));
}

} // namespace
