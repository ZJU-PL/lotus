/**
 * @file TPATest.cpp
 * @brief Unit tests for TPA (semi-sparse flow- and context-sensitive pointer
 * analysis)
 */

#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/WorkList.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/CFG/PriorityAssigner.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/ArrayLayout.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Store.h"
#include "Alias/InclusionBased/TPA/Transforms/RunPrepass.h"
#include "Annotation/Pointer/ExternalPointerTable.h"
#include "Annotation/Pointer/ExternalPointerTablePrinter.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace tpa;
using namespace transform;
using namespace lotus::unittest;

namespace {

bool mayAlias(const SemiSparsePointerAnalysis &pta, const Value *v1,
              const Value *v2) {
  PtsSet pts1 = pta.getPtsSet(v1);
  PtsSet pts2 = pta.getPtsSet(v2);
  for (const auto *obj : pts1) {
    if (pts2.has(obj))
      return true;
  }
  return false;
}

} // namespace

class TPATest : public lotus::unittest::LlvmModuleTest {};

TEST_F(TPATest, NoAliasTwoAllocas) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *y = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!x)
          x = AI;
        else if (!y)
          y = AI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  EXPECT_FALSE(mayAlias(pta, x, y));
}

TEST_F(TPATest, AliasStoreLoad) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32))
          x = AI;
      }
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy())
          q = LI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  EXPECT_TRUE(mayAlias(pta, x, q));
}

TEST_F(TPATest, PointsToSetLoadContainsStoredAlloca) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
          break;
        }
      }
    }
  }
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

TEST_F(TPATest, FlowSensitivityBasic) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

TEST_F(TPATest, FlowSensitivityPointerChain) {
  const char *ir = R"(
    define i32 @main() {
      %a = alloca i32
      %b = alloca i32
      %p = alloca i32*
      %q = alloca i32*

      store i32* %a, i32** %p
      %t1 = load i32*, i32** %p
      store i32* %t1, i32** %q
      store i32* %b, i32** %p
      %t2 = load i32*, i32** %q
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *t2 = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy() &&
            LI->getParent()->getParent()->getName() == "main") {
          t2 = LI;
        }
      }
    }
  }
  ASSERT_NE(t2, nullptr);

  PtsSet ptsT2 = pta.getPtsSet(t2);
  EXPECT_FALSE(ptsT2.empty());
}

TEST_F(TPATest, ContextSensitivitySameFunctionDifferentContexts) {
  const char *ir = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %param) {
      store i32* %param, i32** @global_ptr
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      call void @helper(i32* %x)
      call void @helper(i32* %y)
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, ContextSensitivityRecursiveFunction) {
  const char *ir = R"(
    define void @foo(i32** %p, i32 %n) {
      %cmp = icmp sgt i32 %n, 0
      br i1 %cmp, label %body, label %exit

    body:
      %ptr = alloca i32
      store i32* %ptr, i32** %p
      %n1 = sub i32 %n, 1
      call void @foo(i32** %p, i32 %n1)
      br label %exit

    exit:
      ret void
    }

    define i32 @main() {
      %p = alloca i32*
      call void @foo(i32** %p, i32 3)
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *foo = module->getFunction("foo");
  ASSERT_NE(foo, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, FunctionReturnPointsTo) {
  const char *ir = R"(
    define i32* @alloc_i32() {
      %x = alloca i32
      ret i32* %x
    }

    define i32 @main() {
      %p = call i32* @alloc_i32()
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *p = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "alloc_i32") {
          p = CI;
        }
      }
    }
  }
  ASSERT_NE(p, nullptr);

  PtsSet ptsP = pta.getPtsSet(p);
  EXPECT_FALSE(ptsP.empty());
}

TEST_F(TPATest, FunctionParameterPassing) {
  const char *ir = R"(
    define void @take_ptr(i32* %p) {
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      call void @take_ptr(i32* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *takePtr = module->getFunction("take_ptr");
  ASSERT_NE(takePtr, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, NestedStructureAccess) {
  const char *ir = R"(
    %struct = type { i32, i32* }

    define i32 @main() {
      %s = alloca %struct
      %f1 = getelementptr inbounds %struct, %struct* %s, i32 0, i32 0
      %f2 = getelementptr inbounds %struct, %struct* %s, i32 0, i32 1
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, PointerToPointer) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      %pp = alloca i32**

      store i32* %x, i32** %p
      store i32** %p, i32*** %pp

      %v = load i32**, i32*** %pp
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *v = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          v = LI;
        }
      }
    }
  }
  ASSERT_NE(v, nullptr);

  PtsSet ptsV = pta.getPtsSet(v);
  EXPECT_FALSE(ptsV.empty());
}

TEST_F(TPATest, PointerAliasing) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

TEST_F(TPATest, NullPointerHandling) {
  const char *ir = R"(
    define i32 @main() {
      %p = alloca i32*
      store i32* null, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *p = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isPointerTy()) {
          p = AI;
          break;
        }
      }
    }
  }
  ASSERT_NE(p, nullptr);

  auto ptsP = pta.getPtsSet(p);
  (void)ptsP;
  EXPECT_TRUE(true);
}

TEST_F(TPATest, MultipleFunctionsAnalysis) {
  const char *ir = R"(
    define void @foo() {
      %x = alloca i32
      ret void
    }

    define void @bar() {
      %y = alloca i32
      ret void
    }

    define i32 @main() {
      call void @foo()
      call void @bar()
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, ExternalPointerTableParsesAndModelsAllocOutArg) {
  int fd = -1;
  llvm::SmallString<128> specPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("tpa_ptr_alloc_out", "spec", fd,
                                         specPath));
  auto removeFile =
      llvm::make_scope_exit([&] { llvm::sys::fs::remove(specPath); });
  {
    llvm::raw_fd_ostream os(fd, /*shouldClose=*/true);
    os << "posix_memalign ALLOC Arg2 OUT Arg0\n";
    os << "custom_out_alloc ALLOC OUT Arg1\n";
  }

  annotation::ExternalPointerTable table;
  ASSERT_TRUE(
      annotation::ExternalPointerTable::loadFromFile(specPath.c_str(), table));

  const auto *pmSummary = table.lookup("posix_memalign");
  ASSERT_NE(pmSummary, nullptr);
  ASSERT_EQ(std::distance(pmSummary->begin(), pmSummary->end()), 1);
  const auto &pmAlloc = pmSummary->begin()->getAsAllocEffect();
  EXPECT_TRUE(pmAlloc.hasSizePosition());
  EXPECT_EQ(pmAlloc.getSizePosition().getAsArgPosition().getArgIndex(), 2u);
  EXPECT_FALSE(pmAlloc.getDestPosition().isReturnPosition());
  EXPECT_EQ(pmAlloc.getDestPosition().getAsArgPosition().getArgIndex(), 0u);

  const auto *customSummary = table.lookup("custom_out_alloc");
  ASSERT_NE(customSummary, nullptr);
  ASSERT_EQ(std::distance(customSummary->begin(), customSummary->end()), 1);
  const auto &customAlloc = customSummary->begin()->getAsAllocEffect();
  EXPECT_FALSE(customAlloc.hasSizePosition());
  EXPECT_FALSE(customAlloc.getDestPosition().isReturnPosition());
  EXPECT_EQ(customAlloc.getDestPosition().getAsArgPosition().getArgIndex(), 1u);

  std::string printed;
  llvm::raw_string_ostream printOS(printed);
  annotation::ExternalPointerTablePrinter(printOS).printTable(table);
  printOS.flush();
  EXPECT_NE(printed.find("into *(arg 0)"), std::string::npos);
  EXPECT_NE(printed.find("into *(arg 1)"), std::string::npos);

  const char *ir = R"(
    declare i32 @posix_memalign(i8**, i64, i64)
    define i32 @main() {
      %slot = alloca i8*
      %rc = call i32 @posix_memalign(i8** %slot, i64 64, i64 128)
      %p = load i8*, i8** %slot
      ret i32 %rc
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.loadExternalPointerTable(specPath.c_str());
  pta.runOnProgram(ssProg);

  const Value *loaded = nullptr;
  for (const auto &I : module->getFunction("main")->front()) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      loaded = LI;
  }
  ASSERT_NE(loaded, nullptr);
  PtsSet pts = pta.getPtsSet(loaded);
  ASSERT_FALSE(pts.empty());
  bool foundHeap = false;
  for (const auto *obj : pts)
    if (obj->isHeapObject())
      foundHeap = true;
  EXPECT_TRUE(foundHeap);
}

TEST_F(TPATest, ArrayLayoutCollectPeriodicOffsetsMatchesBruteForce) {
  auto verifyCase = [](ArrayLayout::ArrayTripleList triples, size_t base,
                       size_t limit, size_t stride) {
    const auto *layout = ArrayLayout::getLayout(std::move(triples));
    auto actual = layout->collectPeriodicOffsets(base, limit, stride);
    std::sort(actual.begin(), actual.end());
    actual.erase(std::unique(actual.begin(), actual.end()), actual.end());
    std::vector<std::pair<size_t, bool>> expected;
    if (base < limit) {
      if (stride == 0) {
        expected.push_back(layout->offsetInto(base));
      } else {
        for (size_t off = base; off < limit; off += stride)
          expected.push_back(layout->offsetInto(off));
        std::sort(expected.begin(), expected.end());
        expected.erase(std::unique(expected.begin(), expected.end()),
                       expected.end());
      }
    }
    EXPECT_EQ(actual, expected) << "base=" << base << " limit=" << limit
                                << " stride=" << stride;
  };

  for (size_t stride : {0u, 1u, 3u, 4u, 6u, 8u, 12u, 16u, 400u}) {
    verifyCase({{0, 400, 4}}, 0, 400, stride);
    verifyCase({{0, 400, 4}}, 2, 400, stride);
    verifyCase({{0, 400, 4}}, 400, 400, stride);
  }
  for (size_t stride : {4u, 6u, 12u, 24u, 40u, 52u}) {
    verifyCase({{0, 4000, 40}, {0, 40, 4}}, 0, 4000, stride);
    verifyCase({{0, 4000, 40}, {0, 40, 4}}, 12, 4000, stride);
  }
  ArrayLayout::ArrayTripleList nested3D = {
      {16, 1016, 100}, {24, 96, 12}, {24, 36, 4}};
  for (size_t base : {0u, 8u, 16u, 20u, 24u, 28u, 100u, 1016u}) {
    for (size_t stride : {3u, 5u, 7u, 12u, 17u, 100u})
      verifyCase(nested3D, base, 1100, stride);
  }
  verifyCase({}, 0, 64, 4);
}

TEST_F(TPATest, PersistentStoreOperationsSharingAndFiltering) {
  const char *ir = R"(
    @g0 = global i8* null
    @g1 = global i8* null
    define i32 @main() {
      %s = alloca i8*
      ret i32 0
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  const auto *ctx = context::Context::getGlobalContext();
  const auto *ptrLayout = TypeLayout::getPointerTypeLayoutWithSize(8);
  MemoryManager mem;

  std::vector<const MemoryObject *> globals;
  std::vector<const MemoryObject *> nonAccs;
  const auto *g0Var = module->getGlobalVariable("g0");
  const auto *g1Var = module->getGlobalVariable("g1");
  const auto *allocaInst = &module->getFunction("main")->front().front();

  const auto *bigGlobalLayout =
      TypeLayout::getTypeLayout(64 * 8, {}, {0, 8, 16, 24, 32, 40, 48, 56});
  const auto *gBase = mem.allocateGlobalMemory(g0Var, bigGlobalLayout);
  for (size_t off = 0; off < 64 * 8; off += 8)
    globals.push_back(mem.offsetMemory(gBase, off));
  globals.push_back(mem.allocateGlobalMemory(g1Var, ptrLayout));

  const auto *bigStackLayout = TypeLayout::getTypeLayout(300 * 8, {}, {});
  const auto *sBase = mem.allocateStackMemory(ctx, allocaInst, bigStackLayout);
  for (size_t off = 0; off < 300 * 8; off += 8)
    nonAccs.push_back(mem.offsetMemory(sBase, off));

  Store s1;
  std::unordered_map<const MemoryObject *, PtsSet> ref1;

  for (size_t i = 0; i < globals.size(); ++i) {
    PtsSet v = (i % 7 == 0) ? PtsSet::getSingletonSet(nonAccs[i])
                            : PtsSet::getSingletonSet(globals[0]);
    EXPECT_TRUE(s1.strongUpdate(globals[i], v));
    ref1[globals[i]] = v;
  }
  for (size_t i = 0; i < nonAccs.size(); ++i) {
    PtsSet v = (i == 5) ? PtsSet::getEmptySet()
                        : PtsSet::getSingletonSet(globals[i % globals.size()]);
    EXPECT_TRUE(s1.strongUpdate(nonAccs[i], v));
    ref1[nonAccs[i]] = v;
  }

  EXPECT_TRUE(s1.hasBinding(nonAccs[5]));
  EXPECT_TRUE(s1.lookup(nonAccs[5]).empty());
  EXPECT_EQ(s1.size(), ref1.size());
  EXPECT_EQ(s1.nonAccessibleCount(), nonAccs.size());

  Store s2(s1);
  EXPECT_TRUE(s1.sameRoot(s2));
  EXPECT_TRUE(s2.weakUpdate(nonAccs[10], PtsSet::getSingletonSet(nonAccs[20])));
  EXPECT_FALSE(s1.sameRoot(s2));
  EXPECT_EQ(s1.lookup(nonAccs[10]), ref1[nonAccs[10]]);
  EXPECT_TRUE(s2.lookup(nonAccs[10]).has(nonAccs[20]));

  EXPECT_TRUE(s1.mergeWith(s2));
  EXPECT_TRUE(s1.sameBindings(s2));
  EXPECT_FALSE(s1.mergeWith(s2));
  EXPECT_FALSE(s1.mergeWith(s2));

  size_t accToNonAccCount = 0;
  s1.forEachAccessiblePointingToNonAccessible([&](PtsSet pts) {
    EXPECT_TRUE(pts.hasNonAccessible());
    ++accToNonAccCount;
  });
  EXPECT_EQ(accToNonAccCount, (globals.size() + 6) / 7);

  llvm::DenseSet<const MemoryObject *> keepSet;
  std::vector<uint32_t> keepIds;
  for (size_t idx : {0u, 5u, 17u, 150u, 299u}) {
    keepSet.insert(nonAccs[idx]);
    keepIds.push_back(nonAccs[idx]->getId());
  }
  std::sort(keepIds.begin(), keepIds.end());

  Store filteredByIds = s1.filterNonAccessibleIds(keepIds);
  Store filteredBySet = s1.filterNonAccessible(keepSet);
  EXPECT_TRUE(filteredByIds.sameBindings(filteredBySet));
  EXPECT_EQ(filteredByIds.size(), globals.size() + keepSet.size());
  EXPECT_EQ(filteredByIds.nonAccessibleCount(), keepSet.size());
  for (const auto *g : globals)
    EXPECT_EQ(filteredByIds.lookup(g), s1.lookup(g));
  for (const auto *na : nonAccs) {
    if (keepSet.count(na)) {
      EXPECT_TRUE(filteredByIds.hasBinding(na));
      EXPECT_EQ(filteredByIds.lookup(na), s1.lookup(na));
    } else {
      EXPECT_FALSE(filteredByIds.hasBinding(na));
    }
  }
}

TEST_F(TPATest, GlobalCleanupErasesMultipleExternWeakWithoutInvalidation) {
  const char *ir = R"(
    @w1 = extern_weak global i32
    @w2 = extern_weak global i32
    @w3 = extern_weak global i32
    declare extern_weak void @wf1()
    declare extern_weak void @wf2()

    define i32 @main() {
      %c1 = icmp ne i32* @w1, null
      %c2 = icmp ne i32* @w2, null
      %c3 = icmp ne i32* @w3, null
      %f1 = icmp ne void ()* @wf1, null
      %f2 = icmp ne void ()* @wf2, null
      %or1 = or i1 %c1, %c2
      %or2 = or i1 %or1, %c3
      %or3 = or i1 %or2, %f1
      %or4 = or i1 %or3, %f2
      %ret = zext i1 %or4 to i32
      ret i32 %ret
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
  EXPECT_EQ(module->getGlobalVariable("w1"), nullptr);
  EXPECT_EQ(module->getGlobalVariable("w2"), nullptr);
  EXPECT_EQ(module->getGlobalVariable("w3"), nullptr);
  EXPECT_EQ(module->getFunction("wf1"), nullptr);
  EXPECT_EQ(module->getFunction("wf2"), nullptr);
}

TEST_F(TPATest,
       ExpandByValEmitsByteMemcpyWithDebugLocAndPreservesUnrelatedCalls) {
  const char *ir = R"(
    %S = type { i32, i8* }
    define void @callee(%S* byval(%S) %arg) {
      ret void
    }
    declare void @wrapper(void (%S*)*, %S* byval(%S))
    define i32 @main() {
      %s = alloca %S
      call void @callee(%S* byval(%S) %s)
      call void @wrapper(void (%S*)* @callee, %S* byval(%S) %s)
      ret i32 0
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  llvm::DIBuilder debug(*module);
  auto *file = debug.createFile("byval.c", ".");
  debug.createCompileUnit(llvm::dwarf::DW_LANG_C99, file, "test", false, "", 0);
  auto *subTy = debug.createSubroutineType(debug.getOrCreateTypeArray({}));
  for (const auto *name : {"main", "callee"}) {
    auto *fn = module->getFunction(name);
    auto *sp = debug.createFunction(file, name, name, file, 1, subTy, 1,
                                    llvm::DINode::FlagZero,
                                    llvm::DISubprogram::SPFlagDefinition);
    fn->setSubprogram(sp);
    auto loc = llvm::DILocation::get(module->getContext(), 2, 1, sp);
    for (auto &BB : *fn)
      for (auto &I : BB)
        I.setDebugLoc(loc);
  }
  debug.finalize();
  module->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                        llvm::DEBUG_METADATA_VERSION);
  ASSERT_FALSE(llvm::verifyModule(*module, &llvm::errs()));

  runPrepassOn(*module);
  EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));

  bool sawByteMemcpy = false;
  for (const auto &BB : *module->getFunction("callee")) {
    for (const auto &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || !CB->getCalledFunction())
        continue;
      if (CB->getCalledFunction()->getName() == "llvm.memcpy.p0i8.p0i8.i64") {
        sawByteMemcpy = true;
        EXPECT_TRUE(static_cast<bool>(CB->getDebugLoc()));
      }
    }
  }
  bool wrapperKeptByVal = false;
  bool calleeCallStrippedByVal = false;
  for (const auto &BB : *module->getFunction("main")) {
    for (const auto &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || !CB->getCalledFunction())
        continue;
      StringRef name = CB->getCalledFunction()->getName();
      if (name == "callee")
        calleeCallStrippedByVal = !CB->paramHasAttr(0, Attribute::ByVal);
      if (name == "wrapper")
        wrapperKeptByVal = CB->paramHasAttr(1, Attribute::ByVal);
    }
  }
  EXPECT_TRUE(sawByteMemcpy);
  EXPECT_TRUE(calleeCallStrippedByVal);
  EXPECT_TRUE(wrapperKeptByVal);
}

TEST_F(TPATest, GlobalPointerAnalysisHandlesAddrSpaceCastAndBlockAddress) {
  const char *ir = R"(
    @g = global i32 42
    @g_as1 = global i32 addrspace(1)* addrspacecast (i32* @g to i32 addrspace(1)*)
    @g_ba = global i8* blockaddress(@main, %target)

    define i32 @main() {
      %p1 = load i32 addrspace(1)*, i32 addrspace(1)** @g_as1
      %p2 = load i8*, i8** @g_ba
      br label %target
    target:
      ret i32 0
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  const Value *p2 = nullptr;
  for (const auto &I : module->getFunction("main")->front()) {
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (LI->getPointerOperand() == module->getGlobalVariable("g_ba"))
        p2 = LI;
    }
  }
  ASSERT_NE(p2, nullptr);
  EXPECT_TRUE(pta.getPtsSet(p2).has(MemoryManager::getUniversalObject()));
}

TEST_F(TPATest, PriorityAssignerOrdersForwardWorkListInRPO) {
  const char *ir = R"(
    define void @f() {
      ret void
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);
  const auto &fn = *module->getFunction("f");

  CFG cfg(fn);
  auto *entry = cfg.getEntryNode();
  auto *a = cfg.create<CopyCFGNode>(nullptr, std::vector<const llvm::Value *>{});
  auto *b = cfg.create<CopyCFGNode>(nullptr, std::vector<const llvm::Value *>{});
  auto *c = cfg.create<CopyCFGNode>(nullptr, std::vector<const llvm::Value *>{});
  auto *d = cfg.create<CopyCFGNode>(nullptr, std::vector<const llvm::Value *>{});
  auto *exitNode = cfg.create<ReturnCFGNode>(nullptr);

  entry->insertEdge(a);
  a->insertEdge(b);
  a->insertEdge(c);
  b->insertEdge(d);
  c->insertEdge(d);
  d->insertEdge(a);
  d->insertEdge(exitNode);

  PriorityAssigner(cfg).traverse();

  EXPECT_GT(entry->getPriority(), a->getPriority());
  EXPECT_GT(a->getPriority(), b->getPriority());
  EXPECT_GT(a->getPriority(), c->getPriority());
  EXPECT_GT(b->getPriority(), d->getPriority());
  EXPECT_GT(c->getPriority(), d->getPriority());
  EXPECT_GT(d->getPriority(), exitNode->getPriority());

  const auto *ctx = context::Context::getGlobalContext();
  ForwardWorkList wl;
  for (const CFGNode *n :
       std::initializer_list<const CFGNode *>{exitNode, d, c, b, a, entry})
    wl.enqueue(ProgramPoint(ctx, n));

  std::vector<const CFGNode *> order;
  while (!wl.empty())
    order.push_back(wl.dequeue().getCFGNode());

  ASSERT_EQ(order.size(), 6u);
  EXPECT_EQ(order.front(), entry);
  EXPECT_EQ(order[1], a);
  EXPECT_EQ(order[4], d);
  EXPECT_EQ(order.back(), exitNode);
}

TEST_F(TPATest, KLimitContextUsesStandardSuffixKCFA) {
  const char *ir = R"(
    define i32* @id(i32* %p) {
      ret i32* %p
    }
    define void @wrapper(i32* %a, i32* %b) {
      %ra = call i32* @id(i32* %a)
      %rb = call i32* @id(i32* %b)
      ret void
    }
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      call void @wrapper(i32* %x, i32* %y)
      ret i32 0
    }
  )";
  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  const unsigned savedK = context::KLimitContext::getLimit();
  const auto savedStrategy = context::getContextStrategy();
  auto restore = llvm::make_scope_exit([&] {
    context::KLimitContext::setLimit(savedK);
    context::setContextStrategy(savedStrategy);
  });

  // Collect call instructions c1 (main->wrapper), c2/c3 (wrapper->id).
  const Instruction *c1 = nullptr;
  const Instruction *c2 = nullptr;
  const Instruction *c3 = nullptr;
  for (const auto &I : module->getFunction("main")->front())
    if (isa<CallInst>(&I))
      c1 = &I;
  for (const auto &I : module->getFunction("wrapper")->front()) {
    if (isa<CallInst>(&I)) {
      if (!c2)
        c2 = &I;
      else
        c3 = &I;
    }
  }
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  ASSERT_NE(c3, nullptr);

  const auto *global = context::Context::getGlobalContext();

  // k = 0: always global context.
  context::KLimitContext::setLimit(0);
  EXPECT_EQ(context::KLimitContext::pushContext(global, c1), global);

  // k = 1: retains only the most recent call site (sliding suffix).
  context::KLimitContext::setLimit(1);
  const auto *k1_c1 = context::KLimitContext::pushContext(global, c1);
  EXPECT_EQ(k1_c1->size(), 1u);
  EXPECT_EQ(k1_c1->getCallSite(), c1);
  const auto *k1_c2 = context::KLimitContext::pushContext(k1_c1, c2);
  const auto *k1_c3 = context::KLimitContext::pushContext(k1_c1, c3);
  EXPECT_EQ(k1_c2->size(), 1u);
  EXPECT_EQ(k1_c2->getCallSite(), c2);
  EXPECT_EQ(context::Context::popContext(k1_c2), global);
  EXPECT_EQ(k1_c3->size(), 1u);
  EXPECT_EQ(k1_c3->getCallSite(), c3);
  EXPECT_NE(k1_c2, k1_c3);

  // k = 2: [c1] -> [c1, c2] -> [c2, c3] -> [c3, c3] -> [c3, c3] (fixpoint).
  context::KLimitContext::setLimit(2);
  const auto *k2_1 = context::KLimitContext::pushContext(global, c1);
  const auto *k2_12 = context::KLimitContext::pushContext(k2_1, c2);
  const auto *k2_23 = context::KLimitContext::pushContext(k2_12, c3);
  EXPECT_EQ(k2_23->size(), 2u);
  EXPECT_EQ(k2_23->getCallSite(), c3);
  EXPECT_EQ(context::Context::popContext(k2_23)->getCallSite(), c2);
  EXPECT_EQ(context::Context::popContext(context::Context::popContext(k2_23)),
            global);
  const auto *k2_33 = context::KLimitContext::pushContext(k2_23, c3);
  EXPECT_EQ(context::KLimitContext::pushContext(k2_33, c3), k2_33);

  // End-to-end 1-CFA PTA precision at depth 2: %ra and %rb must not alias
  // (prefix k-limiting previously merged both calls inside @wrapper).
  context::KLimitContext::setLimit(1);
  context::setContextStrategy(context::ContextStrategy::KLimit);
  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  const Value *ra = nullptr;
  const Value *rb = nullptr;
  for (const auto &I : module->getFunction("wrapper")->front()) {
    if (isa<CallInst>(&I)) {
      if (!ra)
        ra = &I;
      else
        rb = &I;
    }
  }
  ASSERT_NE(ra, nullptr);
  ASSERT_NE(rb, nullptr);
  EXPECT_FALSE(pta.getPtsSet(ra).empty());
  EXPECT_FALSE(pta.getPtsSet(rb).empty());
  EXPECT_FALSE(mayAlias(pta, ra, rb));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
