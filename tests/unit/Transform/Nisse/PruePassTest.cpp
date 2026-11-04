#include "TestUtils/LLVMHelpers.h"
#include "Transform/Nisse/Nisse.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

namespace {

struct PassFixture {
  LoopAnalysisManager lam;
  FunctionAnalysisManager fam;
  CGSCCAnalysisManager cgam;
  ModuleAnalysisManager mam;
  PassBuilder pb;

  PassFixture() {
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    fam.registerPass([] { return nisse::NisseAnalysis(); });
    fam.registerPass([] { return nisse::KSAnalysis(); });
    pb.crossRegisterProxies(lam, fam, cgam, mam);
  }
};

static void runDeltaPass(Module &module) {
  PassFixture fixture;
  nisse::DeltaCounterPass pass;
  pass.run(module, fixture.mam);
}

static void runPruePass(Module &module) {
  PassFixture fixture;
  nisse::PruePass pass;
  pass.run(module, fixture.mam);
}

static GlobalVariable *getCounterArray(Module &module, StringRef name) {
  return module.getNamedGlobal(name);
}

static unsigned countDeltaAllocas(Function &function) {
  unsigned count = 0;
  for (Instruction &inst : instructions(function)) {
    auto *alloca = dyn_cast<AllocaInst>(&inst);
    if (alloca && alloca->getName().startswith("prue.delta.")) {
      ++count;
    }
  }
  return count;
}

static unsigned countTaggedStores(Function &function) {
  unsigned count = 0;
  for (Instruction &inst : instructions(function)) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store) {
      continue;
    }
    if (store->getMetadata("nisse.prue.update")) {
      ++count;
    }
  }
  return count;
}

static unsigned countStoresToGlobal(Function &function, StringRef global_name) {
  unsigned count = 0;
  for (Instruction &inst : instructions(function)) {
    auto *store = dyn_cast<StoreInst>(&inst);
    if (!store) {
      continue;
    }
    auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
    if (!gep) {
      continue;
    }
    auto *global = dyn_cast<GlobalVariable>(
        gep->getPointerOperand()->stripPointerCasts());
    if (global && global->getName() == global_name) {
      ++count;
    }
  }
  return count;
}

static bool hasStoreInBlock(Function &function, StringRef block_name,
                            StringRef global_name) {
  for (BasicBlock &bb : function) {
    if (bb.getName() != block_name) {
      continue;
    }
    return llvm::any_of(bb, [&](Instruction &inst) {
      auto *store = dyn_cast<StoreInst>(&inst);
      if (!store) {
        return false;
      }
      auto *gep = dyn_cast<GEPOperator>(store->getPointerOperand());
      if (!gep) {
        return false;
      }
      auto *global = dyn_cast<GlobalVariable>(
          gep->getPointerOperand()->stripPointerCasts());
      return global && global->getName() == global_name;
    });
  }
  return false;
}

static BasicBlock *getBlockByName(Function &function, StringRef block_name) {
  for (BasicBlock &bb : function) {
    if (bb.getName() == block_name) {
      return &bb;
    }
  }
  return nullptr;
}

static Instruction *getInstructionByName(Function &function, StringRef name) {
  for (Instruction &inst : instructions(function)) {
    if (inst.getName() == name) {
      return &inst;
    }
  }
  return nullptr;
}

TEST(PruePassTest, DeltaInstrumentationCreatesLocalCountersAndTaggedUpdates) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define void @instrument_me(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      br label %exit

    else:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  runDeltaPass(*module);

  Function *function = module->getFunction("instrument_me");
  ASSERT_NE(function, nullptr);
  EXPECT_NE(getCounterArray(*module, "prue-counter-array"), nullptr);

  EXPECT_GT(countDeltaAllocas(*function), 0u);
  EXPECT_GT(countTaggedStores(*function), 0u);
}

TEST(PruePassTest, DeltaInstrumentationSkipsMustTailFunctions) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i64 @callee(i64)

    define i64 @musttail_func(i64 %x) {
    entry:
      %call = musttail call i64 @callee(i64 %x)
      ret i64 %call
    }

    define void @regular_func(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %exit

    then:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  runDeltaPass(*module);

  Function *musttail_function = module->getFunction("musttail_func");
  ASSERT_NE(musttail_function, nullptr);
  EXPECT_EQ(countDeltaAllocas(*musttail_function), 0u);
  EXPECT_EQ(countTaggedStores(*musttail_function), 0u);
  EXPECT_EQ(countStoresToGlobal(*musttail_function, "prue-counter-array"), 0u);

  Function *regular_function = module->getFunction("regular_func");
  ASSERT_NE(regular_function, nullptr);
  EXPECT_GT(countDeltaAllocas(*regular_function), 0u);
  EXPECT_GT(countTaggedStores(*regular_function), 0u);
}

TEST(PruePassTest, EliminatesZeroUpdate) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @zero_update() {
    entry:
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, 0
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("zero_update");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 0u);
}

TEST(PruePassTest, SplitsPhiUpdateAndEliminatesZeroArm) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @split_update(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %exit

    then:
      br label %exit

    exit:
      %d = phi i64 [ 0, %entry ], [ 1, %then ]
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("split_update");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_FALSE(hasStoreInBlock(*function, "exit", "prue-counter-array"));
}

TEST(PruePassTest, SplitsLoopPhiWithSameBlockBackedgeValue) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @split_loop_phi(i1 %again) {
    entry:
      br label %loop

    loop:
      %d = phi i64 [ 0, %entry ], [ %next, %loop ]
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      %next = add i64 %d, 1
      br i1 %again, label %loop, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("split_loop_phi");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_FALSE(hasStoreInBlock(*function, "loop", "prue-counter-array"));
}

TEST(PruePassTest, RelocatesUpdateToDefinitionBlock) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @relocate_update(i64 %x) {
    entry:
      br label %then

    then:
      %d = add i64 %x, 1
      br label %exit

    exit:
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("relocate_update");
  ASSERT_NE(function, nullptr);
  EXPECT_TRUE(hasStoreInBlock(*function, "then", "prue-counter-array"));
  EXPECT_FALSE(hasStoreInBlock(*function, "exit", "prue-counter-array"));
}

TEST(PruePassTest, OffloadsWrappedLoopClosure) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @offload_wrapped_loop_closure(i1 %enter_inner, i1 %continue_inner,
                                              i1 %continue_outer) {
    entry:
      br label %outer.header

    outer.header:
      %d1 = phi i64 [ 0, %entry ], [ %d4.f, %outer.latch ]
      br label %outer.body

    outer.body:
      %d1.tr = trunc i64 %d1 to i32
      %d1.z = zext i32 %d1.tr to i64
      br i1 %enter_inner, label %inner.header, label %outer.latch

    inner.header:
      %d2 = phi i64 [ %d1.z, %outer.body ], [ %d3, %inner.body ]
      br label %inner.body

    inner.body:
      %d2.f = freeze i64 %d2
      %d3 = add i64 %d2.f, 1
      br i1 %continue_inner, label %inner.header, label %outer.latch

    outer.latch:
      %d4 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      %d4.f = freeze i64 %d4
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d4.f
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      br i1 %continue_outer, label %outer.header, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("offload_wrapped_loop_closure");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_FALSE(hasStoreInBlock(*function, "outer.latch", "prue-counter-array"));

  auto *d2 = dyn_cast<PHINode>(getInstructionByName(*function, "d2"));
  ASSERT_NE(d2, nullptr);
  BasicBlock *outer_body = getBlockByName(*function, "outer.body");
  ASSERT_NE(outer_body, nullptr);
  auto *incoming = dyn_cast<ConstantInt>(d2->getIncomingValueForBlock(outer_body));
  ASSERT_NE(incoming, nullptr);
  EXPECT_TRUE(incoming->isZero());
}

TEST(PruePassTest, OffloadsSelectBasedLoopClosure) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @offload_select_loop_closure(i1 %enter_inner, i1 %continue_inner,
                                             i1 %continue_outer, i1 %mask) {
    entry:
      br label %outer.header

    outer.header:
      %d1 = phi i64 [ 0, %entry ], [ %d4.f, %outer.latch ]
      br label %outer.body

    outer.body:
      br i1 %enter_inner, label %inner.header, label %outer.latch

    inner.header:
      %d2 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      br label %inner.body

    inner.body:
      %picked = select i1 %mask, i64 %d1, i64 %d2
      %d3 = add i64 %picked, 1
      br i1 %continue_inner, label %inner.header, label %outer.latch

    outer.latch:
      %d4 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      %d4.f = freeze i64 %d4
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d4.f
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      br i1 %continue_outer, label %outer.header, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("offload_select_loop_closure");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_FALSE(hasStoreInBlock(*function, "outer.latch", "prue-counter-array"));

  auto *picked = dyn_cast<SelectInst>(getInstructionByName(*function, "picked"));
  ASSERT_NE(picked, nullptr);
  auto *true_value = dyn_cast<ConstantInt>(picked->getTrueValue());
  ASSERT_NE(true_value, nullptr);
  EXPECT_TRUE(true_value->isZero());
}

TEST(PruePassTest, OffloadRefusesSharedLoopClosureValues) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @observe(i64)
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @offload_shared_loop_value(i1 %enter_inner, i1 %continue_inner,
                                           i1 %continue_outer) {
    entry:
      br label %outer.header

    outer.header:
      %d1 = phi i64 [ 0, %entry ], [ %d4.f, %outer.latch ]
      br label %outer.body

    outer.body:
      %d1.tr = trunc i64 %d1 to i32
      %d1.z = zext i32 %d1.tr to i64
      br i1 %enter_inner, label %inner.header, label %outer.latch

    inner.header:
      %d2 = phi i64 [ %d1.z, %outer.body ], [ %d3, %inner.body ]
      br label %inner.body

    inner.body:
      call void @observe(i64 %d2)
      %d2.f = freeze i64 %d2
      %d3 = add i64 %d2.f, 1
      br i1 %continue_inner, label %inner.header, label %outer.latch

    outer.latch:
      %d4 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      %d4.f = freeze i64 %d4
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d4.f
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      br i1 %continue_outer, label %outer.header, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("offload_shared_loop_value");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_TRUE(hasStoreInBlock(*function, "inner.body", "prue-counter-array"));

  auto *d2 = dyn_cast<PHINode>(getInstructionByName(*function, "d2"));
  ASSERT_NE(d2, nullptr);
  BasicBlock *outer_body = getBlockByName(*function, "outer.body");
  ASSERT_NE(outer_body, nullptr);
  EXPECT_FALSE(isa<ConstantInt>(d2->getIncomingValueForBlock(outer_body)));
}

TEST(PruePassTest, UnpacksReachableAddChainInsideLoopClosure) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @unpack_reachable_adds(i1 %enter_inner, i1 %continue_inner,
                                       i1 %continue_outer) {
    entry:
      br label %outer.header

    outer.header:
      %d1 = phi i64 [ 0, %entry ], [ %d4.f, %outer.latch ]
      br label %outer.body

    outer.body:
      br i1 %enter_inner, label %inner.header, label %outer.latch

    inner.header:
      %d2 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      br label %inner.body

    inner.body:
      %step1 = add i64 %d2, 1
      %d3 = add i64 %step1, 2
      br i1 %continue_inner, label %inner.header, label %outer.latch

    outer.latch:
      %d4 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      %d4.f = freeze i64 %d4
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d4.f
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      br i1 %continue_outer, label %outer.header, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("unpack_reachable_adds");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 2u);
  EXPECT_FALSE(hasStoreInBlock(*function, "outer.latch", "prue-counter-array"));
  EXPECT_TRUE(hasStoreInBlock(*function, "inner.body", "prue-counter-array"));
}

TEST(PruePassTest, UnpackAllowsSharedLoopClosureValues) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare void @observe(i64)
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @unpack_shared_loop_value(i1 %enter_inner, i1 %continue_inner,
                                          i1 %continue_outer) {
    entry:
      br label %outer.header

    outer.header:
      %d1 = phi i64 [ 0, %entry ], [ %d4.f, %outer.latch ]
      br label %outer.body

    outer.body:
      br i1 %enter_inner, label %inner.header, label %outer.latch

    inner.header:
      %d2 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      br label %inner.body

    inner.body:
      call void @observe(i64 %d2)
      %step1 = add i64 %d2, 1
      %d3 = add i64 %step1, 2
      br i1 %continue_inner, label %inner.header, label %outer.latch

    outer.latch:
      %d4 = phi i64 [ %d1, %outer.body ], [ %d3, %inner.body ]
      %d4.f = freeze i64 %d4
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d4.f
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      br i1 %continue_outer, label %outer.header, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("unpack_shared_loop_value");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 2u);
  EXPECT_FALSE(hasStoreInBlock(*function, "outer.latch", "prue-counter-array"));
  EXPECT_TRUE(hasStoreInBlock(*function, "inner.body", "prue-counter-array"));
}

TEST(PruePassTest, LeavesNonAddLoopClosureUnchanged) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    @prue-counter-array = global [1 x i64] zeroinitializer

    define void @non_add_loop_closure(i1 %enter_inner, i1 %continue_inner,
                                      i1 %continue_outer) {
    entry:
      br label %outer.header

    outer.header:
      %d1 = phi i64 [ 1, %entry ], [ %d4.f, %outer.latch ]
      br label %outer.body

    outer.body:
      br i1 %enter_inner, label %inner.header, label %outer.latch

    inner.header:
      %d2 = phi i64 [ %d1, %outer.body ], [ %scaled, %inner.body ]
      br label %inner.body

    inner.body:
      %scaled = mul i64 %d2, 2
      br i1 %continue_inner, label %inner.header, label %outer.latch

    outer.latch:
      %d4 = phi i64 [ %d1, %outer.body ], [ %scaled, %inner.body ]
      %d4.f = freeze i64 %d4
      %ptr = getelementptr inbounds [1 x i64], [1 x i64]* @prue-counter-array,
                                     i64 0, i64 0
      %old = load i64, i64* %ptr
      %new = add i64 %old, %d4.f
      store i64 %new, i64* %ptr, !nisse.prue.update !0
      br i1 %continue_outer, label %outer.header, label %exit

    exit:
      ret void
    }

    !0 = !{i64 0}
  )");
  ASSERT_NE(module, nullptr);

  runPruePass(*module);

  Function *function = module->getFunction("non_add_loop_closure");
  ASSERT_NE(function, nullptr);
  EXPECT_EQ(countStoresToGlobal(*function, "prue-counter-array"), 1u);
  EXPECT_TRUE(hasStoreInBlock(*function, "outer.latch", "prue-counter-array"));
}

} // namespace
