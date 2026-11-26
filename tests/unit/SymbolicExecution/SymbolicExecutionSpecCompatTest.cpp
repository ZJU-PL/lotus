#include "SymbolicExecution/Models/MemoryAPI.h"
#include "SymbolicExecution/Support/AnalysisContext.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace SymbolicExecution;
using namespace lotus::unittest;

namespace {

class SymbolicExecutionSpecCompatTest : public LlvmModuleTest {};

TEST_F(SymbolicExecutionSpecCompatTest,
       DeclarationCallIsNotTreatedAsDefiniteCall) {
  ASSERT_TRUE(loadModule(R"(
    declare i32 @puts(i8*)

    define i32 @caller(i8* %msg) {
    entry:
      %rv = call i32 @puts(i8* %msg)
      ret i32 %rv
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *call = findInstruction<CallInst>(*caller);
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(isDefiniteCall(call));
}

TEST_F(SymbolicExecutionSpecCompatTest, DefinedCallIsTreatedAsDefiniteCall) {
  ASSERT_TRUE(loadModule(R"(
    define i32 @callee(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @caller(i32 %x) {
    entry:
      %rv = call i32 @callee(i32 %x)
      ret i32 %rv
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *call = findInstruction<CallInst>(*caller);
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(isDefiniteCall(call));

  AnalysisContext Ctx(module.get(), &module->getDataLayout(), nullptr);
  Ctx.computeCallGraphOrder();
  EXPECT_TRUE(Ctx.isFunctionTopLevel(caller));
  EXPECT_FALSE(Ctx.isFunctionTopLevel(module->getFunction("callee")));
  EXPECT_EQ(Ctx.getFunctionDepth(caller), 1u);
  EXPECT_EQ(Ctx.getFunctionDepth(module->getFunction("callee")), 0u);
}

TEST_F(SymbolicExecutionSpecCompatTest, HeapAllocSizeRecognizesAllocatorsOnly) {
  ASSERT_TRUE(loadModule(R"(
    declare i8* @malloc(i64)
    declare i8* @memcpy(i8*, i8*, i64)
    declare void @free(i8*)

    define void @caller(i8* %dst, i8* %src) {
    entry:
      %buf = call i8* @malloc(i64 16)
      %copy = call i8* @memcpy(i8* %dst, i8* %src, i64 4)
      call void @free(i8* %buf)
      ret void
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  auto *malloc_call = findInstruction<CallInst>(*caller, "buf");
  ASSERT_NE(malloc_call, nullptr);
  auto malloc_sizes = getHeapAllocSizeArgs(malloc_call);
  ASSERT_EQ(malloc_sizes.size(), 1u);
  EXPECT_EQ(malloc_sizes[0], 0);

  auto *memcpy_call = findInstruction<CallInst>(*caller, "copy");
  ASSERT_NE(memcpy_call, nullptr);
  EXPECT_TRUE(getHeapAllocSizeArgs(memcpy_call).empty());

  EXPECT_TRUE(isFreeLikeFunction(module->getFunction("free")));
  EXPECT_FALSE(isFreeLikeFunction(module->getFunction("malloc")));
}

TEST(SymbolicExecutionSpecCompatStandaloneTest,
     KnownLibCoverageIncludesCoreLibs) {
  EXPECT_TRUE(isKnownLib("strlen"));
  EXPECT_TRUE(isKnownLib("memcpy"));
  EXPECT_TRUE(isKnownLib("recv"));
  EXPECT_TRUE(isKnownLib("malloc"));
  EXPECT_TRUE(isKnownLib("kmalloc"));
  EXPECT_FALSE(isKnownLib("definitely_not_a_known_lib"));
}

} // namespace
