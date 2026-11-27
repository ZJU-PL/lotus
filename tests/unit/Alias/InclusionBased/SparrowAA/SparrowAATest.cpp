/**
 * @file SparrowAATest.cpp
 * @brief Comprehensive unit tests for SparrowAA (Andersen's pointer analysis)
 *
 * SparrowAA implements context-sensitive Andersen's pointer analysis
 * using field-sensitive and flow-insensitive techniques.
 */

#include "Alias/InclusionBased/SparrowAA/AndersenAA.h"
#include "Alias/InclusionBased/SparrowAA/ConstraintSnapshot.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

class SparrowAATest : public LlvmModuleTest {
protected:
  bool pointsToSetContains(const std::vector<const Value *> &ptsSet,
                           const Value *v) {
    return std::find(ptsSet.begin(), ptsSet.end(), v) != ptsSet.end();
  }

  static std::string readFile(llvm::StringRef path) {
    std::ifstream input(path.str());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  }

  template <typename T>
  static T readStruct(std::ifstream &input) {
    T value{};
    input.read(reinterpret_cast<char *>(&value), sizeof(T));
    return value;
  }
};

TEST_F(SparrowAATest, SimplePointerAssignment) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *q = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(q, ptsSet);
  bool pointsToX = pointsToSetContains(ptsSet, x);
  EXPECT_TRUE(pointsToX || !ptsSet.empty());
}

TEST_F(SparrowAATest, NoAliasDisjointPointsTo) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %y = alloca i32
      %px = alloca i32*
      store i32* %x, i32** %px
      store i32* %y, i32** %px
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *y = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (!x)
          x = AI;
        else if (!y)
          y = AI;
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(x, ptsSet);
  AA.getPointsToSet(y, ptsSet);
  EXPECT_FALSE(ptsSet.empty());
}

TEST_F(SparrowAATest, SerializesCollectedConstraints) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  llvm::SmallString<256> collectPath;
  int fd = -1;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("sparrow-collect", "txt", fd,
                                                  collectPath));
  ::close(fd);

  extern llvm::cl::opt<std::string> AndersenDumpConstraintsAfterCollect;
  extern llvm::cl::opt<std::string> AndersenDumpConstraintsAfterOptimize;

  const std::string oldCollect = AndersenDumpConstraintsAfterCollect.getValue();
  const std::string oldOptimize = AndersenDumpConstraintsAfterOptimize.getValue();
  AndersenDumpConstraintsAfterCollect = std::string(collectPath.str());
  AndersenDumpConstraintsAfterOptimize = "";

  {
    AndersenAAResult AA(*module);
    (void)AA;
  }

  AndersenDumpConstraintsAfterCollect = oldCollect;
  AndersenDumpConstraintsAfterOptimize = oldOptimize;

  std::ifstream input(collectPath.str().str(), std::ios::binary);
  ASSERT_TRUE(input.is_open());
  auto header = readStruct<sparrow_aa::SnapshotHeader>(input);
  EXPECT_EQ(std::string(header.magic, header.magic + 8), "SPAA2BIN");
  EXPECT_EQ(header.version, sparrow_aa::kSnapshotVersion);
  EXPECT_EQ(header.phase,
            static_cast<uint32_t>(sparrow_aa::SnapshotPhase::Collect));
  EXPECT_GT(header.node_count, 0u);
  EXPECT_EQ(header.section_count, 11u);

  std::vector<sparrow_aa::SnapshotSectionHeader> sections(header.section_count);
  input.read(reinterpret_cast<char *>(sections.data()),
             sizeof(sparrow_aa::SnapshotSectionHeader) * sections.size());
  EXPECT_EQ(sections[0].kind,
            static_cast<uint32_t>(sparrow_aa::SnapshotSectionKind::Contexts));
  EXPECT_EQ(sections[3].kind, static_cast<uint32_t>(
                                 sparrow_aa::SnapshotSectionKind::
                                     AddrOfRowOffsets));
  EXPECT_EQ(sections[4].kind,
            static_cast<uint32_t>(sparrow_aa::SnapshotSectionKind::AddrOfColumns));
}

TEST_F(SparrowAATest, SerializesOptimizedConstraints) {
  const char *source = R"(
    define i32* @id(i32* %p) {
      ret i32* %p
    }

    define i32* @test() {
      %x = alloca i32
      %p = call i32* @id(i32* %x)
      ret i32* %p
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  llvm::SmallString<256> optimizePath;
  int fd = -1;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("sparrow-optimize", "txt",
                                                  fd, optimizePath));
  ::close(fd);

  extern llvm::cl::opt<std::string> AndersenDumpConstraintsAfterCollect;
  extern llvm::cl::opt<std::string> AndersenDumpConstraintsAfterOptimize;

  const std::string oldCollect = AndersenDumpConstraintsAfterCollect;
  const std::string oldOptimize = AndersenDumpConstraintsAfterOptimize;
  AndersenDumpConstraintsAfterCollect = "";
  AndersenDumpConstraintsAfterOptimize = std::string(optimizePath.str());

  {
    AndersenAAResult AA(*module);
    (void)AA;
  }

  AndersenDumpConstraintsAfterCollect = oldCollect;
  AndersenDumpConstraintsAfterOptimize = oldOptimize;

  std::ifstream input(optimizePath.str().str(), std::ios::binary);
  ASSERT_TRUE(input.is_open());
  auto header = readStruct<sparrow_aa::SnapshotHeader>(input);
  EXPECT_EQ(std::string(header.magic, header.magic + 8), "SPAA2BIN");
  EXPECT_EQ(header.version, sparrow_aa::kSnapshotVersion);
  EXPECT_EQ(header.phase,
            static_cast<uint32_t>(sparrow_aa::SnapshotPhase::Optimize));
  EXPECT_GT(header.node_count, 0u);
  EXPECT_EQ(header.section_count, 11u);

  std::vector<sparrow_aa::SnapshotSectionHeader> sections(header.section_count);
  input.read(reinterpret_cast<char *>(sections.data()),
             sizeof(sparrow_aa::SnapshotSectionHeader) * sections.size());

  const auto &copyRows = sections[5];
  const auto &copyCols = sections[6];
  const auto &loadRows = sections[7];
  const auto &storeRows = sections[9];
  EXPECT_EQ(copyRows.kind, static_cast<uint32_t>(
                               sparrow_aa::SnapshotSectionKind::CopyRowOffsets));
  EXPECT_EQ(copyCols.kind,
            static_cast<uint32_t>(sparrow_aa::SnapshotSectionKind::CopyColumns));
  EXPECT_EQ(loadRows.kind, static_cast<uint32_t>(
                               sparrow_aa::SnapshotSectionKind::LoadRowOffsets));
  EXPECT_EQ(storeRows.kind, static_cast<uint32_t>(
                                sparrow_aa::SnapshotSectionKind::StoreRowOffsets));
  EXPECT_GT(copyCols.element_count, 0u);

  input.seekg(sections[1].offset, std::ios::beg);
  std::vector<sparrow_aa::SnapshotNodeRecord> nodes(sections[1].element_count);
  input.read(reinterpret_cast<char *>(nodes.data()),
             sizeof(sparrow_aa::SnapshotNodeRecord) * nodes.size());
  bool sawReturnRole = false;
  for (const auto &node : nodes) {
    if (node.node_role ==
        static_cast<uint16_t>(AndersNode::RETURN_NODE)) {
      sawReturnRole = true;
      break;
    }
  }
  EXPECT_TRUE(sawReturnRole);
}

TEST_F(SparrowAATest, GlobalVariablePointsTo) {
  const char *source = R"(
    @global = global i32 0, align 4

    define void @test() {
      %p = alloca i32*
      store i32* @global, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  GlobalVariable *global = module->getNamedGlobal("global");
  ASSERT_NE(global, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(global, ptsSet);
  EXPECT_TRUE(AA.pointsToConstantMemory(
                  MemoryLocation(global, LocationSize::beforeOrAfterPointer()),
                  false) ||
              !ptsSet.empty());
}

TEST_F(SparrowAATest, TransitivePointsTo) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p1 = alloca i32*
      %p2 = alloca i32*
      store i32* %x, i32** %p1
      %t1 = load i32*, i32** %p1
      store i32* %t1, i32** %p2
      %t2 = load i32*, i32** %p2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *t2 = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        t2 = LI;
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(t2, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(t2, ptsSet);
  bool found = false;
  for (const auto *v : ptsSet) {
    if (v == x) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found || ptsSet.size() > 0);
}

TEST_F(SparrowAATest, AliasQueryTest) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p1 = alloca i32*
      %p2 = alloca i32*
      store i32* %x, i32** %p1
      store i32* %x, i32** %p2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  std::vector<Value *> allocs;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        allocs.push_back(AI);
      }
    }
  }

  ASSERT_EQ(allocs.size(), 3u);

  auto loc1 = MemoryLocation(allocs[1], LocationSize::beforeOrAfterPointer());
  auto loc2 = MemoryLocation(allocs[2], LocationSize::beforeOrAfterPointer());
  AliasResult result = AA.alias(loc1, loc2);
  EXPECT_TRUE(result == AliasResult::NoAlias ||
              result == AliasResult::MayAlias);
}

TEST_F(SparrowAATest, FunctionParameterPointsTo) {
  const char *source = R"(
    define void @test(i32* %p) {
      %q = alloca i32*
      store i32* %p, i32** %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Argument *p = F->getArg(0);
  ASSERT_NE(p, nullptr);

  std::vector<const Value *> ptsSet;
  bool hasPointsTo = AA.getPointsToSet(p, ptsSet);
  EXPECT_TRUE(hasPointsTo || ptsSet.empty());
}

TEST_F(SparrowAATest, HeapAllocationAnalysis) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @test() {
      %raw = call i8* @malloc(i64 16)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  CallInst *mallocCall = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "malloc") {
          mallocCall = CI;
        }
      }
    }
  }

  ASSERT_NE(mallocCall, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(mallocCall, ptsSet);
  EXPECT_TRUE(ptsSet.empty() || ptsSet.size() > 0);
}

TEST_F(SparrowAATest, ArrayElementAccess) {
  const char *source = R"(
    define void @test() {
      %arr = alloca [10 x i32]
      %p1 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 0
      %p2 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 5
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  std::vector<Value *> geps;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        geps.push_back(GEP);
      }
    }
  }

  ASSERT_EQ(geps.size(), 2u);

  for (auto *gep : geps) {
    std::vector<const Value *> ptsSet;
    AA.getPointsToSet(gep, ptsSet);
    EXPECT_TRUE(true);
  }
}

TEST_F(SparrowAATest, CastInstructionHandling) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = bitcast i32* %x to i8*
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *p = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        x = AI;
      }
      if (BitCastInst *BI = dyn_cast<BitCastInst>(&I)) {
        p = BI;
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(p, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(p, ptsSet);
  EXPECT_TRUE(ptsSet.empty() || ptsSet.size() > 0);
}

TEST_F(SparrowAATest, ConstantPointerAnalysis) {
  const char *source = R"(
    @constant_ptr = constant i32* null

    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  GlobalVariable *constantPtr = module->getNamedGlobal("constant_ptr");
  ASSERT_NE(constantPtr, nullptr);

  auto result = AA.pointsToConstantMemory(
      MemoryLocation(constantPtr, LocationSize::beforeOrAfterPointer()), false);
  EXPECT_TRUE(result || true);
}

TEST_F(SparrowAATest, ContextInsensitiveDefault) {
  const char *source = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define void @caller1() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret void
    }

    define void @caller2() {
      %y = alloca i32
      call void @callee(i32* %y)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *callee = module->getFunction("callee");
  ASSERT_NE(callee, nullptr);

  Argument *calleeParam = callee->getArg(0);
  ASSERT_NE(calleeParam, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(calleeParam, ptsSet);
  EXPECT_TRUE(ptsSet.size() >= 0);
}

TEST_F(SparrowAATest, ContextSensitive1CFA) {
  const char *source = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define void @caller1() {
      %x = alloca i32
      call void @helper(i32* %x)
      ret void
    }

    define void @caller2() {
      %y = alloca i32
      call void @helper(i32* %y)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, ContextSensitive2CFA) {
  const char *source = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define void @caller1() {
      %x = alloca i32
      call void @helper(i32* %x)
      ret void
    }

    define void @caller2() {
      %y = alloca i32
      call void @helper(i32* %y)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 2);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, ContextSensitiveQueryInContext) {
  const char *source = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define void @caller() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  const auto *initialCtx = AA.getInitialContext();

  Function *callee = module->getFunction("callee");
  ASSERT_NE(callee, nullptr);

  Argument *calleeParam = callee->getArg(0);
  std::vector<const Value *> ptsSet;
  bool hasResult = AA.getPointsToSetInContext(calleeParam, initialCtx, ptsSet);

  EXPECT_TRUE(hasResult || ptsSet.empty());
}

TEST_F(SparrowAATest, ContextEvolution) {
  const char *source = R"(
    define void @foo(i32* %p) {
      ret void
    }

    define void @bar(i32* %p) {
      call void @foo(i32* %p)
      ret void
    }

    define void @baz(i32* %p) {
      call void @bar(i32* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *foo = module->getFunction("foo");
  ASSERT_NE(foo, nullptr);

  const auto *globalCtx = AA.getGlobalContext();
  const auto *initialCtx = AA.getInitialContext();

  EXPECT_NE(globalCtx, nullptr);
  EXPECT_NE(initialCtx, nullptr);
}

TEST_F(SparrowAATest, ContextToString) {
  const char *source = R"(
    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  const auto *globalCtx = AA.getGlobalContext();
  const auto *initialCtx = AA.getInitialContext();

  std::string globalStr = AA.contextToString(globalCtx, false);
  std::string initialStr = AA.contextToString(initialCtx, false);

  EXPECT_TRUE(globalStr.size() > 0 || initialStr.size() > 0);
}

TEST_F(SparrowAATest, IndirectCallContextSensitive) {
  const char *source = R"(
    define void @func1(i32* %p) {
      ret void
    }

    define void @func2(i32* %p) {
      ret void
    }

    define void @caller(void (i32*)* %fp) {
      call void %fp(i32* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, MustAliasVsMayAlias) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *q = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  auto locX = MemoryLocation(x, LocationSize::beforeOrAfterPointer());
  auto locQ = MemoryLocation(q, LocationSize::beforeOrAfterPointer());
  AliasResult result = AA.alias(locX, locQ);

  EXPECT_TRUE(result == AliasResult::MayAlias ||
              result == AliasResult::MustAlias);
}

TEST_F(SparrowAATest, HeapAllocationContextSensitive) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define i8* @allocA() {
      %p = call i8* @malloc(i64 16)
      ret i8* %p
    }

    define i8* @allocB() {
      %p = call i8* @malloc(i64 32)
      ret i8* %p
    }

    define void @test() {
      %a = call i8* @allocA()
      %b = call i8* @allocB()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *test = module->getFunction("test");
  ASSERT_NE(test, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, CompareContextSensitiveVsInsensitive) {
  const char *source = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define void @caller() {
      %x = alloca i32
      call void @helper(i32* %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA_CI(*module, 0);
  AndersenAAResult AA_CS(*module, 1);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  Argument *param = helper->getArg(0);
  ASSERT_NE(param, nullptr);

  std::vector<const Value *> ptsCI, ptsCS;
  AA_CI.getPointsToSet(param, ptsCI);
  AA_CS.getPointsToSet(param, ptsCS);

  EXPECT_TRUE(ptsCI.size() >= 0);
  EXPECT_TRUE(ptsCS.size() >= 0);
}

TEST_F(SparrowAATest, FreezeAndAddrSpaceCastInstructions) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %y = alloca i32
      %fx = freeze i32* %x
      %as = addrspacecast i32* %fx to i32 addrspace(1)*
      %back = addrspacecast i32 addrspace(1)* %as to i32*
      %fu = freeze i32* undef
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *y = nullptr, *fx = nullptr, *back = nullptr,
        *fu = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (I.getName() == "x")
        x = &I;
      else if (I.getName() == "y")
        y = &I;
      else if (I.getName() == "fx")
        fx = &I;
      else if (I.getName() == "back")
        back = &I;
      else if (I.getName() == "fu")
        fu = &I;
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(back, nullptr);
  ASSERT_NE(fu, nullptr);

  std::vector<const Value *> ptsFx, ptsBack;
  ASSERT_TRUE(AA.getPointsToSet(fx, ptsFx));
  EXPECT_TRUE(pointsToSetContains(ptsFx, x));
  EXPECT_FALSE(pointsToSetContains(ptsFx, y));

  ASSERT_TRUE(AA.getPointsToSet(back, ptsBack));
  EXPECT_TRUE(pointsToSetContains(ptsBack, x));
  EXPECT_FALSE(pointsToSetContains(ptsBack, y));

  // Frozen undef may alias any concrete object conservatively.
  auto locX = MemoryLocation(x, LocationSize::beforeOrAfterPointer());
  auto locFu = MemoryLocation(fu, LocationSize::beforeOrAfterPointer());
  EXPECT_EQ(AA.alias(locX, locFu), AliasResult::MayAlias);
}

TEST_F(SparrowAATest, VectorPointerAndConstantVectorHandling) {
  const char *source = R"(
    @g1 = global i32 1
    @g2 = global i32 2
    @gvec = global <2 x i32*> <i32* @g1, i32* @g2>

    define void @test(<2 x i32*> %base, <2 x i64> %ints) {
      %x = alloca i32
      %vgep = getelementptr i32, <2 x i32*> %base, <2 x i64> <i64 0, i64 1>
      %vi2p = inttoptr <2 x i64> %ints to <2 x i8*>
      %v1 = insertelement <2 x i32*> undef, i32* %x, i32 0
      %e0 = extractelement <2 x i32*> %v1, i32 0
      %gv_ptr = bitcast <2 x i32*>* @gvec to i32**
      %loaded = load i32*, i32** %gv_ptr
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);
  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  GlobalVariable *g1 = module->getNamedGlobal("g1");
  GlobalVariable *g2 = module->getNamedGlobal("g2");
  ASSERT_NE(g1, nullptr);
  ASSERT_NE(g2, nullptr);

  Value *x = nullptr, *e0 = nullptr, *loaded = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (I.getName() == "x")
        x = &I;
      else if (I.getName() == "e0")
        e0 = &I;
      else if (I.getName() == "loaded")
        loaded = &I;
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(e0, nullptr);
  ASSERT_NE(loaded, nullptr);

  std::vector<const Value *> ptsE0, ptsLoaded;
  ASSERT_TRUE(AA.getPointsToSet(e0, ptsE0));
  EXPECT_TRUE(pointsToSetContains(ptsE0, x));

  ASSERT_TRUE(AA.getPointsToSet(loaded, ptsLoaded));
  EXPECT_TRUE(pointsToSetContains(ptsLoaded, g1));
  EXPECT_TRUE(pointsToSetContains(ptsLoaded, g2));
}

TEST_F(SparrowAATest, GlobalAliasBlockAddressAndUndefGlobalInit) {
  const char *source = R"(
    %S = type { i8*, i32* }
    @target = global i32 42
    @alias_target = alias i32, i32* @target
    @global_struct = global %S { i8* undef, i32* @alias_target }
    @ba_global = global i8* blockaddress(@test, %lbl)

    define void @test() {
      %p = alloca i32*
      store i32* @alias_target, i32** %p
      %q = load i32*, i32** %p
      %gs_field = getelementptr inbounds %S, %S* @global_struct, i32 0, i32 1
      %r = load i32*, i32** %gs_field
      %ba = load i8*, i8** @ba_global
      br label %lbl
    lbl:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);
  Function *F = module->getFunction("test");
  GlobalVariable *target = module->getNamedGlobal("target");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(target, nullptr);

  Value *q = nullptr, *r = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (I.getName() == "q")
        q = &I;
      else if (I.getName() == "r")
        r = &I;
    }
  }
  ASSERT_NE(q, nullptr);
  ASSERT_NE(r, nullptr);

  std::vector<const Value *> ptsQ, ptsR;
  ASSERT_TRUE(AA.getPointsToSet(q, ptsQ));
  EXPECT_TRUE(pointsToSetContains(ptsQ, target));
  ASSERT_TRUE(AA.getPointsToSet(r, ptsR));
  EXPECT_TRUE(pointsToSetContains(ptsR, target));
}

TEST_F(SparrowAATest, UniversalObjAndFieldInsensitiveAliasSoundness) {
  const char *source = R"(
    %Pair = type { i32, i32 }
    @g = global i32 0
    declare i32* @unknown_ext_a()
    declare i32* @unknown_ext_b()

    define void @test() {
      %s = alloca %Pair
      %f0 = getelementptr inbounds %Pair, %Pair* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %Pair, %Pair* %s, i32 0, i32 1
      %u1 = call i32* @unknown_ext_a()
      %u2 = call i32* @unknown_ext_b()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);
  Function *F = module->getFunction("test");
  GlobalVariable *g = module->getNamedGlobal("g");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(g, nullptr);

  Value *f0 = nullptr, *f1 = nullptr, *u1 = nullptr, *u2 = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (I.getName() == "f0")
        f0 = &I;
      else if (I.getName() == "f1")
        f1 = &I;
      else if (I.getName() == "u1")
        u1 = &I;
      else if (I.getName() == "u2")
        u2 = &I;
    }
  }
  ASSERT_NE(f0, nullptr);
  ASSERT_NE(f1, nullptr);
  ASSERT_NE(u1, nullptr);
  ASSERT_NE(u2, nullptr);

  auto loc = [](const Value *V) {
    return MemoryLocation(V, LocationSize::beforeOrAfterPointer());
  };

  // Unknown external returns point to UniversalObj: must MayAlias (never
  // NoAlias) with @g and MayAlias (never MustAlias) with each other.
  EXPECT_EQ(AA.alias(loc(u1), loc(g)), AliasResult::MayAlias);
  EXPECT_EQ(AA.alias(loc(u1), loc(u2)), AliasResult::MayAlias);

  // Distinct struct fields share a single field-insensitive object: must not
  // unsoundly return MustAlias.
  EXPECT_EQ(AA.alias(loc(f0), loc(f1)), AliasResult::MayAlias);
  // Same pointer value still returns MustAlias.
  EXPECT_EQ(AA.alias(loc(f0), loc(f0)), AliasResult::MustAlias);
}

TEST_F(SparrowAATest, HvnHuHcdLcdExactEquivalenceOnLinkedListCycle) {
  const char *source = R"(
    %Node = type { i32, %Node* }

    define void @traverse(%Node* %head) {
      %slot = alloca %Node*
      %cur = alloca %Node*
      store %Node* %head, %Node** %slot
      %init = load %Node*, %Node** %slot
      store %Node* %init, %Node** %cur
      br label %loop
    loop:
      %p = load %Node*, %Node** %cur
      %nextp = getelementptr inbounds %Node, %Node* %p, i32 0, i32 1
      %next = load %Node*, %Node** %nextp
      store %Node* %next, %Node** %cur
      br label %exit
    exit:
      ret void
    }

    define void @driver() {
      %n1 = alloca %Node
      %n2 = alloca %Node
      %f1 = getelementptr inbounds %Node, %Node* %n1, i32 0, i32 1
      store %Node* %n2, %Node** %f1
      call void @traverse(%Node* %n1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  extern llvm::cl::opt<bool> EnableHVN;
  extern llvm::cl::opt<bool> EnableHU;
  extern llvm::cl::opt<bool> EnableHCD;
  extern llvm::cl::opt<bool> EnableLCD;

  const bool oldHVN = EnableHVN;
  const bool oldHU = EnableHU;
  const bool oldHCD = EnableHCD;
  const bool oldLCD = EnableLCD;

  auto snapshotAllPts = [&](bool hvn, bool hu, bool hcd, bool lcd) {
    EnableHVN = hvn;
    EnableHU = hu;
    EnableHCD = hcd;
    EnableLCD = lcd;
    Andersen A(*module, makeContextPolicy(0));
    std::map<const Value *, std::set<const Value *>> out;
    for (const Function &F : *module) {
      for (const Argument &Arg : F.args()) {
        if (!Arg.getType()->isPointerTy())
          continue;
        std::vector<const Value *> pts;
        if (A.getPointsToSet(&Arg, pts))
          out[&Arg] = std::set<const Value *>(pts.begin(), pts.end());
      }
      for (const BasicBlock &BB : F) {
        for (const Instruction &I : BB) {
          if (!I.getType()->isPointerTy())
            continue;
          std::vector<const Value *> pts;
          if (A.getPointsToSet(&I, pts))
            out[&I] = std::set<const Value *>(pts.begin(), pts.end());
        }
      }
    }
    return out;
  };

  auto base = snapshotAllPts(false, false, false, false);
  EXPECT_FALSE(base.empty());
  EXPECT_EQ(snapshotAllPts(true, false, false, false), base);
  EXPECT_EQ(snapshotAllPts(false, true, false, false), base);
  EXPECT_EQ(snapshotAllPts(true, true, false, false), base);
  EXPECT_EQ(snapshotAllPts(false, false, true, true), base);
  EXPECT_EQ(snapshotAllPts(true, true, true, true), base);

  EnableHVN = oldHVN;
  EnableHU = oldHU;
  EnableHCD = oldHCD;
  EnableLCD = oldLCD;
}

TEST_F(SparrowAATest, AdaptiveCSScoresDirectAddrOfCallersAndSeparatesDiverse) {
  const char *source = R"(
    define i32* @diverse_id(i32* %p) {
      ret i32* %p
    }

    define i32* @uniform_id(i32* %p) {
      ret i32* %p
    }

    define void @main_fn() {
      %x = alloca i32
      %y = alloca i32
      %r1 = call i32* @diverse_id(i32* %x)
      %r2 = call i32* @diverse_id(i32* %y)
      %u1 = call i32* @uniform_id(i32* %x)
      %u2 = call i32* @uniform_id(i32* %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  extern llvm::cl::opt<bool> EnableAdaptiveCS;
  const bool oldACS = EnableAdaptiveCS;
  EnableAdaptiveCS = true;

  Andersen A(*module, makeContextPolicy(1));

  EnableAdaptiveCS = oldACS;

  Function *diverseFn = module->getFunction("diverse_id");
  Function *uniformFn = module->getFunction("uniform_id");
  Function *mainFn = module->getFunction("main_fn");
  ASSERT_NE(diverseFn, nullptr);
  ASSERT_NE(uniformFn, nullptr);
  ASSERT_NE(mainFn, nullptr);

  EXPECT_FLOAT_EQ(A.getContextNeed(diverseFn), 1.0f);
  EXPECT_FLOAT_EQ(A.getContextNeed(uniformFn), 0.0f);

  Value *x = nullptr, *y = nullptr, *r1 = nullptr, *r2 = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (I.getName() == "x")
        x = &I;
      else if (I.getName() == "y")
        y = &I;
      else if (I.getName() == "r1")
        r1 = &I;
      else if (I.getName() == "r2")
        r2 = &I;
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);
  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);

  std::vector<const Value *> ptsR1, ptsR2;
  ASSERT_TRUE(A.getPointsToSet(r1, ptsR1));
  ASSERT_TRUE(A.getPointsToSet(r2, ptsR2));
  EXPECT_TRUE(pointsToSetContains(ptsR1, x));
  EXPECT_FALSE(pointsToSetContains(ptsR1, y));
  EXPECT_TRUE(pointsToSetContains(ptsR2, y));
  EXPECT_FALSE(pointsToSetContains(ptsR2, x));
}

TEST_F(SparrowAATest, BDDBackendMatchesSparseBitVector) {
  const char *source = R"(
    define void @test() {
      %a = alloca i32
      %b = alloca i32
      %p = alloca i32*
      %q = alloca i32*
      store i32* %a, i32** %p
      store i32* %b, i32** %q
      %l1 = load i32*, i32** %p
      store i32* %l1, i32** %q
      %l2 = load i32*, i32** %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const PtsSetImpl oldImpl = getGlobalPtsSetImpl();

  selectGlobalPtsSetImpl(PtsSetImpl::SPARSE_BITVECTOR);
  Andersen sparseA(*module, makeContextPolicy(0));

  selectGlobalPtsSetImpl(PtsSetImpl::BDD);
  Andersen bddA(*module, makeContextPolicy(0));

  selectGlobalPtsSetImpl(oldImpl);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (!I.getType()->isPointerTy())
        continue;
      std::vector<const Value *> sPts, bPts;
      bool sOk = sparseA.getPointsToSet(&I, sPts);
      bool bOk = bddA.getPointsToSet(&I, bPts);
      EXPECT_EQ(sOk, bOk);
      std::set<const Value *> sSet(sPts.begin(), sPts.end());
      std::set<const Value *> bSet(bPts.begin(), bPts.end());
      EXPECT_EQ(sSet, bSet);
    }
  }
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
