#include "ThreadAPITestSupport.h"

TEST_F(ThreadAPITest, RecognizesCppAtomicWaitNotifyAndJthreadDestructor) {
  const char *source = R"(
    declare void @_ZNSt6atomicIiE4waitEi(i8*, i32)
    declare void @_ZNSt6atomicIiE10notify_oneEv(i8*)
    declare void @_ZNSt6atomicIiE10notify_allEv(i8*)
    declare void @_ZNSt7jthreadD1Ev(i8*)

    define void @main() {
    entry:
      %thr = alloca i8
      call void @_ZNSt7jthreadD1Ev(i8* %thr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *atomic_wait = module->getFunction("_ZNSt6atomicIiE4waitEi");
  const Function *notify_one =
      module->getFunction("_ZNSt6atomicIiE10notify_oneEv");
  const Function *notify_all =
      module->getFunction("_ZNSt6atomicIiE10notify_allEv");
  const Function *jthread_dtor = module->getFunction("_ZNSt7jthreadD1Ev");
  ASSERT_NE(atomic_wait, nullptr);
  ASSERT_NE(notify_one, nullptr);
  ASSERT_NE(notify_all, nullptr);
  ASSERT_NE(jthread_dtor, nullptr);

  EXPECT_EQ(api->getType(atomic_wait), ThreadAPI::TD_ATOMIC_WAIT);
  EXPECT_EQ(api->getType(notify_one), ThreadAPI::TD_ATOMIC_NOTIFY_ONE);
  EXPECT_EQ(api->getType(notify_all), ThreadAPI::TD_ATOMIC_NOTIFY_ALL);
  EXPECT_EQ(api->getType(jthread_dtor), ThreadAPI::TD_JTHREAD_DTOR);
  EXPECT_EQ(api->getSemanticLoweringInfo(atomic_wait).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_EQ(api->getSemanticLoweringInfo(jthread_dtor).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(api->getSemanticLoweringInfo(jthread_dtor).reason,
               "jthread-autojoin-lifetime-unmodeled");

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &*main_func->getEntryBlock().begin();
  EXPECT_FALSE(api->isTDJoin(call));
}

TEST_F(ThreadAPITest, CppRecognitionCoversFreeFunctionsWithoutSubstrings) {
  const char *source = R"(
    declare void @_ZSt5asyncIiEvv()
    declare void @_ZSt11atomic_waitIiEvPKSt6atomicIT_ES1_()
    declare void @_ZNKSt6atomicIiE4waitEi(i8*, i32)
    declare void @debug_latch_count_down()
    declare void @my_barrier_waitE()
    declare void @app_semaphore_releaseE()
    declare void @_ZNSt13__future_base12_State_baseV217_M_complete_asyncEv()
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("_ZSt5asyncIiEvv")),
            ThreadAPI::TD_ASYNC);
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZSt11atomic_waitIiEvPKSt6atomicIT_ES1_")),
            ThreadAPI::TD_ATOMIC_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("_ZNKSt6atomicIiE4waitEi")),
            ThreadAPI::TD_ATOMIC_WAIT);
  for (const char *name : {"debug_latch_count_down", "my_barrier_waitE",
                           "app_semaphore_releaseE",
                           "_ZNSt13__future_base12_State_baseV217_M_complete_asyncEv"})
    EXPECT_EQ(api->getType(module->getFunction(name)), ThreadAPI::TD_DUMMY);
}

