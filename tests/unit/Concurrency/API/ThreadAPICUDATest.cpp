#include "ThreadAPITestSupport.h"

TEST_F(ThreadAPITest, SeparatesDirectCUDALaunchFromGraphMutation) {
  const char *source = R"(
    declare i32 @cudaLaunchKernel(i8*, i8*, i8*, i8**, i64, i8*)
    declare i32 @cudaGraphAddKernelNode(i8*, i8*, i8*, i64, i8*)
    declare void @cleanup()
    define void @kernel() { ret void }
    define void @main(i8** %args) {
      call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
                                 i8* null, i8* null, i8** %args, i64 0,
                                 i8* null)
      call void @cleanup()
      call i32 @cudaGraphAddKernelNode(i8* null, i8* null, i8* null,
                                       i64 0, i8* null)
      call void @cleanup()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  const Instruction *launch = &*it++;
  ++it;
  const Instruction *graph_add = &*it;
  EXPECT_TRUE(api->isForkLike(launch));
  EXPECT_EQ(api->getCUDALaunchedKernel(launch), module->getFunction("kernel"));
  EXPECT_NE(api->getCUDALaunchedKernel(launch), module->getFunction("cleanup"));
  auto payload = api->getForkPayloadArgs(launch);
  ASSERT_EQ(payload.size(), 1u);
  EXPECT_EQ(payload[0], module->getFunction("main")->getArg(0));
  EXPECT_EQ(api->getType(api->getCallee(graph_add)), ThreadAPI::TD_CUDA_STREAM);
  EXPECT_FALSE(api->isForkLike(graph_add));
  EXPECT_EQ(api->getCUDALaunchedKernel(graph_add), nullptr);
}

TEST_F(ThreadAPITest, CUDALaunchLayoutsDistinguishExAndLegacyForms) {
  const char *source = R"(
    declare i32 @cudaLaunchKernel(i8*, i8*, i8*, i8**, i64, i8*)
    declare i32 @cudaLaunchKernelExC(i8*, i8*, i8**)
    declare i32 @cuLaunchKernel(i8*, i32, i32, i32, i32, i32, i32, i32,
                                i8*, i8**)
    declare i32 @cuLaunchKernelEx_v2(i8*, i8*, i8**)
    define void @kernel() { ret void }
    define void @main(i8** %args) {
      call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
                                 i8* null, i8* null, i8** %args, i64 0,
                                 i8* null)
      call i32 @cudaLaunchKernelExC(i8* null,
                                    i8* bitcast (void ()* @kernel to i8*),
                                    i8** %args)
      call i32 @cuLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
                               i32 1, i32 1, i32 1, i32 1, i32 1, i32 1,
                               i32 0, i8* null, i8** %args)
      call i32 @cuLaunchKernelEx_v2(i8* null,
                                    i8* bitcast (void ()* @kernel to i8*),
                                    i8** %args)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  auto it = module->getFunction("main")->front().begin();
  for (unsigned i = 0; i < 4; ++i) {
    const Instruction *launch = &*it++;
    EXPECT_EQ(api->getCUDALaunchedKernel(launch),
              module->getFunction("kernel"));
    auto payload = api->getForkPayloadArgs(launch);
    ASSERT_EQ(payload.size(), 1u);
    EXPECT_EQ(payload.front(), module->getFunction("main")->getArg(0));
  }
}

TEST_F(ThreadAPITest, AggregateCUDALaunchIsRecognizedWithoutFakeOperands) {
  const char *source = R"(
    declare i32 @cuLaunchCooperativeKernelMultiDevice(i8*, i32, i32)
    define void @main(i8* %records) {
      call i32 @cuLaunchCooperativeKernelMultiDevice(i8* %records, i32 1,
                                                      i32 0)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Instruction *launch = &module->getFunction("main")->front().front();
  EXPECT_TRUE(api->isForkLike(launch));
  EXPECT_EQ(api->getType(api->getLLVMCallSite(launch)),
            ThreadAPI::TD_CUDA_MULTI_DEVICE_LAUNCH);
  EXPECT_EQ(api->getCUDALaunchedKernel(launch), nullptr);
  EXPECT_EQ(api->getForkedFun(launch), nullptr);
  EXPECT_TRUE(api->getForkPayloadArgs(launch).empty());
  EXPECT_EQ(api->getSemanticLoweringInfo(api->getCallee(launch)).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
}

