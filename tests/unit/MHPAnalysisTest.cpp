/**
 * @file MHPAnalysisTest.cpp
 * @brief Simplified unit tests for MHP Analysis
 */

#include "Analysis/Concurrency/MHPAnalysis.h"

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace mhp;

class MHPAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MHPAnalysisTest", errs());
    }
    return module;
  }
};

// Test 1: Simple main function
TEST_F(MHPAnalysisTest, SimpleMain) {
  const char *source = R"(
    define i32 @main() {
      %x = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  EXPECT_NO_THROW(mhp.analyze());

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_threads, 0);
}

// Test 2: Thread creation
TEST_F(MHPAnalysisTest, ThreadCreation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    
    define i8* @worker(i8* %arg) {
      ret i8* null
    }
    
    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null, 
                                       i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
}

// Test 3: Lock operations
TEST_F(MHPAnalysisTest, LockOperations) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    
    @lock = global i8 0
    
    define i32 @main() {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %x = add i32 1, 2
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_locks, 1);
  EXPECT_GE(stats.num_unlocks, 1);
}

// Main function for tests
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
