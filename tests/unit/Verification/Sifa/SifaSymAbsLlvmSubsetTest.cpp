#include "Verification/Sifa/SifaSymAbs.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>
#include <stdexcept>

namespace {

TEST(SifaSymAbs, RejectsFloatByDefaultSubset) {
  const char *ir = R"IR(
    define float @f(float %x) {
    entry:
      %y = fadd float %x, 1.000000e+00
      ret float %y
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.validateLlvmSubset = true;
  opt.allowDouble = false;

  EXPECT_THROW((void)lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt), std::invalid_argument);
}

TEST(SifaSymAbs, RejectsFirstClassAggregatesByDefaultSubset) {
  const char *ir = R"IR(
    %pair = type { i32, i32 }

    define %pair @f(i32 %x) {
    entry:
      %p0 = insertvalue %pair undef, i32 %x, 0
      %p1 = insertvalue %pair %p0, i32 7, 1
      ret %pair %p1
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.validateLlvmSubset = true;

  EXPECT_THROW((void)lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt), std::invalid_argument);
}

TEST(SifaSymAbs, ValidSubsetDoesNotThrow) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.validateLlvmSubset = true;

  EXPECT_NO_THROW((void)lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt));
  EXPECT_NO_THROW((void)lotus::sifa::isReachableSymAbs(*M, *F, F->getEntryBlock(), opt));
}

TEST(SifaSymAbs, RejectsUnsupportedInstructionInReachableCallee) {
  const char *ir = R"IR(
    %pair = type { i32, i32 }

    define i32 @helper(%pair %p) {
    entry:
      %x = extractvalue %pair %p, 0
      ret i32 %x
    }

    define i32 @caller(i32 %n) {
    entry:
      %r = call i32 @helper(%pair { i32 1, i32 2 })
      %s = add i32 %r, %n
      ret i32 %s
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *caller = M->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.validateLlvmSubset = true;
  opt.recursive = true;

  EXPECT_THROW((void)lotus::sifa::analyzeSymAbsToReturn(*M, *caller, opt),
               std::invalid_argument);
}

} // namespace

