#include "Concurrency/Thread/Sharing/ThreadLocalAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/IR/IRBuilder.h>

using namespace llvm;
using namespace ThreadLocal;
using namespace lotus::unittest;

class ThreadLocalAnalysisTest : public LlvmModuleTest {
protected:
};

TEST_F(ThreadLocalAnalysisTest, StoreThroughStackGepStaysThreadLocal) {
  const char *source = R"(
    %struct.S = type { i32, i32 }

    define i32 @main() {
    entry:
      %obj = alloca %struct.S, align 4
      %field = getelementptr inbounds %struct.S, %struct.S* %obj, i32 0, i32 1
      store i32 7, i32* %field, align 4
      %load = load i32, i32* %field, align 4
      ret i32 %load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *load = findInstructionByName(*main_func, "load");
  ASSERT_NE(load, nullptr);

  EXPECT_TRUE(tla.accessesThreadLocalStorage(load));
}

TEST_F(ThreadLocalAnalysisTest, GetspecificPointeeIsNotDefinitelyThreadLocal) {
  const char *source = R"(
    declare i8* @pthread_getspecific(i32)
    declare i32 @__gxx_personality_v0(...)

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %tls = invoke i8* @pthread_getspecific(i32 0)
              to label %cont unwind label %lpad

    cont:
      %use = ptrtoint i8* %tls to i64
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *tls = findInstructionByName(*main_func, "tls");
  ASSERT_NE(tls, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(tls));
}

TEST_F(ThreadLocalAnalysisTest, EscapedTlsAddressIsNotThreadLocal) {
  const char *source = R"(
    @tls = thread_local global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %typed = bitcast i8* %arg to i32*
      store i32 1, i32* %typed, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      %payload = bitcast i32* @tls to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      %parent_load = load i32, i32* @tls, align 4
      ret i32 %parent_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const GlobalVariable *tls = module->getGlobalVariable("tls");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(tls, nullptr);
  ASSERT_NE(main_func, nullptr);
  const Instruction *parent_load =
      findInstructionByName(*main_func, "parent_load");
  ASSERT_NE(parent_load, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(tls));
  EXPECT_FALSE(tla.accessesThreadLocalStorage(parent_load));
}

TEST_F(ThreadLocalAnalysisTest, UnescapedTlsRemainsThreadLocal) {
  const char *source = R"(
    @tls = thread_local global [2 x i32] zeroinitializer, align 4

    define i32 @main() {
    entry:
      %value = load i32, i32* getelementptr inbounds (
          [2 x i32], [2 x i32]* @tls, i32 0, i32 1), align 4
      ret i32 %value
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const GlobalVariable *tls = module->getGlobalVariable("tls");
  const Function *main_func = module->getFunction("main");
  const Instruction *value = findInstructionByName(*main_func, "value");
  ASSERT_NE(tls, nullptr);
  ASSERT_NE(value, nullptr);
  EXPECT_TRUE(tla.isThreadLocal(tls));
  EXPECT_TRUE(tla.accessesThreadLocalStorage(value));
}

TEST_F(ThreadLocalAnalysisTest,
       LoadFromTlsSlotDoesNotMakeSharedPointeeThreadLocal) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i8* @pthread_getspecific(i32)

    define i32 @main() {
    entry:
      %slot = call i8* @pthread_getspecific(i32 0)
      %typed = bitcast i8* %slot to i32**
      store i32* @shared, i32** %typed, align 8
      %loaded_ptr = load i32*, i32** %typed, align 8
      %loaded_val = load i32, i32* %loaded_ptr, align 4
      ret i32 %loaded_val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *loaded_ptr =
      findInstructionByName(*main_func, "loaded_ptr");
  const Instruction *loaded_val =
      findInstructionByName(*main_func, "loaded_val");
  ASSERT_NE(loaded_ptr, nullptr);
  ASSERT_NE(loaded_val, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(loaded_ptr));
  EXPECT_FALSE(tla.accessesThreadLocalStorage(loaded_val));
}

TEST_F(ThreadLocalAnalysisTest, PthreadHandleStorageRemainsThreadLocal) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *tid = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(tid, nullptr);

  EXPECT_TRUE(tla.isThreadLocal(tid));
}

TEST_F(ThreadLocalAnalysisTest, HelperMediatedThreadPayloadIsNotThreadLocal) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @spawn_helper(i8* %tid, i32* %payload, i8* (i8*)* %fn) {
    entry:
      %payload_raw = bitcast i32* %payload to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn,
                               i8* %payload_raw)
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      call void @spawn_helper(i8* %tid, i32* %slot, i8* (i8*)* @worker)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest,
       LocalPointerCarrierDoesNotHideThreadPayloadEscape) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define i32 @main() {
    entry:
      %shared = alloca i32, align 4
      %carrier = alloca i32*, align 8
      %tid = alloca i8, align 1
      store i32* %shared, i32** %carrier, align 8
      %loaded = load i32*, i32** %carrier, align 8
      %payload = bitcast i32* %loaded to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *shared =
      dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(shared, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(shared));
}

TEST_F(ThreadLocalAnalysisTest, SelectPayloadEscapesAlloca) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define i32 @main(i1 %choose) {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      %selected = select i1 %choose, i32* %slot, i32* null
      %payload = bitcast i32* %selected to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, PhiPayloadEscapesAlloca) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define i32 @main(i1 %choose) {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      br i1 %choose, label %left, label %right
    left:
      br label %merge
    right:
      br label %merge
    merge:
      %merged = phi i32* [ %slot, %left ], [ null, %right ]
      %payload = bitcast i32* %merged to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, AddrSpaceCastPayloadEscapesAlloca) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      %as1 = addrspacecast i32* %slot to i32 addrspace(1)*
      %back = addrspacecast i32 addrspace(1)* %as1 to i32*
      %payload = bitcast i32* %back to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, AggregatePayloadEscapesAlloca) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      %aggregate = insertvalue { i32* } undef, i32* %slot, 0
      %extracted = extractvalue { i32* } %aggregate, 0
      %payload = bitcast i32* %extracted to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, PointerPreservingIntrinsicPayloadEscapes) {
  const char *source = R"(
    declare i8* @llvm.launder.invariant.group.p0i8(i8*)
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      %raw = bitcast i32* %slot to i8*
      %alias = call i8* @llvm.launder.invariant.group.p0i8(i8* %raw)
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %alias)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, PublishedCarrierPublishesPointee) {
  const char *source = R"(
    @published = global i32** null, align 8

    define i32 @main() {
    entry:
      %obj = alloca i32, align 4
      %carrier = alloca i32*, align 8
      store i32* %obj, i32** %carrier, align 8
      store i32** %carrier, i32*** @published, align 8
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *obj = findInstructionByName(*main_func, "obj");
  ASSERT_NE(obj, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(obj));
}

TEST_F(ThreadLocalAnalysisTest, HelperLoadsCarrierAndForksPointee) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define void @spawn(i32** %carrier, i8* %tid) {
    entry:
      %loaded = load i32*, i32** %carrier, align 8
      %payload = bitcast i32* %loaded to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret void
    }

    define i32 @main() {
    entry:
      %obj = alloca i32, align 4
      %carrier = alloca i32*, align 8
      %tid = alloca i8, align 1
      store i32* %obj, i32** %carrier, align 8
      call void @spawn(i32** %carrier, i8* %tid)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *obj = findInstructionByName(*main_func, "obj");
  ASSERT_NE(obj, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(obj));
}

TEST_F(ThreadLocalAnalysisTest, NoCaptureExternalIsNotConfinementProof) {
  const char *source = R"(
    declare void @parallel_apply(i32* nocapture)

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      call void @parallel_apply(i32* %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, NoCaptureReturnedAliasEscapes) {
  const char *source = R"(
    declare i32* @identity(i32* returned nocapture)
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      %alias = call i32* @identity(i32* %slot)
      %payload = bitcast i32* %alias to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, InternalPointerReturnThroughCalleeCastEscapes) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @worker(i8*)

    define internal i32* @identity(i32* %value) {
    entry:
      ret i32* %value
    }

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      %raw = bitcast i32* %slot to i8*
      %alias = call i8* bitcast (i32* (i32*)* @identity to i8* (i8*)*)(i8* %raw)
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %alias)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, ObviouslyThreadLocalRejectsEscapedAlloca) {
  const char *source = R"(
    @published = global i32* null, align 8

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      store i32* %slot, i32** @published, align 8
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Function *main_func = module->getFunction("main");
  const Instruction *slot = findInstructionByName(*main_func, "slot");
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(isObviouslyThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest, ReanalyzeAfterMutationRefreshesResults) {
  const char *source = R"(
    @published = global i32* null, align 8

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  Function *main_func = module->getFunction("main");
  auto *slot = dyn_cast<AllocaInst>(findInstructionByName(*main_func, "slot"));
  GlobalVariable *published = module->getGlobalVariable("published");
  ASSERT_NE(slot, nullptr);
  ASSERT_NE(published, nullptr);
  EXPECT_TRUE(tla.isThreadLocal(slot));

  IRBuilder<> builder(main_func->getEntryBlock().getTerminator());
  builder.CreateStore(slot, published);

  tla.invalidate();
  EXPECT_FALSE(tla.isThreadLocal(slot));
  tla.analyze();
  EXPECT_FALSE(tla.isThreadLocal(slot));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
