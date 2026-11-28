#include "CUDAAnalysisTestSupport.h"
#include "Concurrency/CUDA/CUDAFunctionSummary.h"
#include "Concurrency/CUDA/CUDAParticipantAnalysis.h"
#include "Concurrency/CUDA/CUDASemantics.h"

TEST_F(CUDAAnalysisTest, DetectsShiftedCrossWarpAndByteIntervalRaces) {
  const char *source = R"(
    @words = addrspace(1) global [128 x i32] zeroinitializer
    @bytes = addrspace(1) global [128 x i8] zeroinitializer
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %plus32 = add nuw nsw i32 %tid, 32
      %a = getelementptr [128 x i32], [128 x i32] addrspace(1)* @words,
                         i32 0, i32 %tid
      %b = getelementptr [128 x i32], [128 x i32] addrspace(1)* @words,
                         i32 0, i32 %plus32
      store i32 1, i32 addrspace(1)* %a
      store i32 2, i32 addrspace(1)* %b
      %plus2 = add nuw nsw i32 %tid, 2
      %c0 = getelementptr [128 x i8], [128 x i8] addrspace(1)* @bytes,
                          i32 0, i32 %tid
      %c1 = getelementptr [128 x i8], [128 x i8] addrspace(1)* @bytes,
                          i32 0, i32 %plus2
      %w0 = bitcast i8 addrspace(1)* %c0 to i32 addrspace(1)*
      %w1 = bitcast i8 addrspace(1)* %c1 to i32 addrspace(1)*
      store i32 3, i32 addrspace(1)* %w0, align 1
      store i32 4, i32 addrspace(1)* %w1, align 1
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 64)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_global_race);
  EXPECT_GE(summary.global_races.size(), 2u);
}

TEST_F(CUDAAnalysisTest, WrappingArithmeticCannotProveThreadDisjointness) {
  const char *source = R"(
    @buffer = addrspace(1) global [16 x i32] zeroinitializer
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %wrapped = mul i32 %tid, 1073741824
      %ptr = getelementptr [16 x i32], [16 x i32] addrspace(1)* @buffer,
                           i32 0, i32 %wrapped
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 8)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_FALSE(summary.accesses.empty());
  EXPECT_EQ(summary.accesses.front().alias_precision,
            concurrency::cuda::AliasPrecision::NonAffine);
  EXPECT_TRUE(summary.has_global_race);
}

TEST_F(CUDAAnalysisTest, DecodesPeerAndTwoDimensionalMemcpyLayouts) {
  const char *source = R"(
    %stream_t = type opaque
    @src = addrspace(1) global [128 x i8] zeroinitializer
    @dst = addrspace(1) global [128 x i8] zeroinitializer
    declare i32 @cudaMemcpyPeerAsync(i8 addrspace(1)*, i32,
                                     i8 addrspace(1)*, i32, i64, %stream_t*)
    declare i32 @cudaMemcpy2DAsync(i8 addrspace(1)*, i64,
                                   i8 addrspace(1)*, i64, i64, i64, i32,
                                   %stream_t*)
    define i32 @main(%stream_t* %s) {
    entry:
      %p = call i32 @cudaMemcpyPeerAsync(i8 addrspace(1)* getelementptr
          ([128 x i8], [128 x i8] addrspace(1)* @dst, i32 0, i32 0), i32 1,
          i8 addrspace(1)* getelementptr
          ([128 x i8], [128 x i8] addrspace(1)* @src, i32 0, i32 0), i32 0,
          i64 64, %stream_t* %s)
      %d = call i32 @cudaMemcpy2DAsync(i8 addrspace(1)* getelementptr
          ([128 x i8], [128 x i8] addrspace(1)* @dst, i32 0, i32 0), i64 32,
          i8 addrspace(1)* getelementptr
          ([128 x i8], [128 x i8] addrspace(1)* @src, i32 0, i32 0), i64 32,
          i64 16, i64 3, i32 3, %stream_t* %s)
      %r = add i32 %p, %d
      ret i32 %r
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getMemoryTransfers().size(), 2u);
  const auto &peer = analysis.getMemoryTransfers()[0];
  EXPECT_EQ(peer.src->stripPointerCasts()->getName(), "src");
  EXPECT_EQ(peer.size, 64u);
  EXPECT_EQ(peer.kind, concurrency::cuda::TransferKind::DeviceToDevice);
  const auto &copy2d = analysis.getMemoryTransfers()[1];
  EXPECT_EQ(copy2d.width, 16u);
  EXPECT_EQ(copy2d.height, 3u);
  EXPECT_EQ(copy2d.size, 80u);
}

TEST_F(CUDAAnalysisTest, RetainsOpaqueKernelAndThreeDimensionalTransfer) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    %copy3d = type opaque
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    declare i32 @cudaMemcpy3DAsync(%copy3d*, %stream_t*)
    define i32 @main(i8* %unknown_kernel, %copy3d* %params, %stream_t* %s) {
    entry:
      %l = call i32 @cudaLaunchKernel(i8* %unknown_kernel,
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 32, i32 1, i32 1 }, i8** null, i64 0, %stream_t* %s)
      %c = call i32 @cudaMemcpy3DAsync(%copy3d* %params, %stream_t* %s)
      %r = add i32 %l, %c
      ret i32 %r
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  EXPECT_TRUE(analysis.getLaunches().front().is_opaque);
  ASSERT_EQ(analysis.getMemoryTransfers().size(), 1u);
  EXPECT_TRUE(analysis.getMemoryTransfers().front().region_unknown);
}

TEST_F(CUDAAnalysisTest, CanonicalizesReloadsAndSeparatesStreamGenerations) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    declare i32 @cudaStreamCreateWithFlags(%stream_t**, i32)
    declare i32 @cudaStreamDestroy(%stream_t*)
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    define void @kernel() { entry: ret void }
    define i32 @main() {
    entry:
      %slot = alloca %stream_t*
      %c0 = call i32 @cudaStreamCreateWithFlags(%stream_t** %slot, i32 1)
      %s0 = load %stream_t*, %stream_t** %slot
      %l0 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 1, i32 1, i32 1 }, i8** null, i64 0, %stream_t* %s0)
      %s1 = load %stream_t*, %stream_t** %slot
      %l1 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 1, i32 1, i32 1 }, i8** null, i64 0, %stream_t* %s1)
      %d = call i32 @cudaStreamDestroy(%stream_t* %s1)
      %c1 = call i32 @cudaStreamCreateWithFlags(%stream_t** %slot, i32 1)
      %s2 = load %stream_t*, %stream_t** %slot
      %l2 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 1, i32 1, i32 1 }, i8** null, i64 0, %stream_t* %s2)
      ret i32 %c0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 3u);
  EXPECT_TRUE(
      llvm::is_contained(analysis.getLaunches()[1].ordered_dependencies, 0u));
  EXPECT_FALSE(
      llvm::is_contained(analysis.getLaunches()[2].ordered_dependencies, 1u));
  EXPECT_GE(analysis.getStreamAutomata().size(), 2u);
}

TEST_F(CUDAAnalysisTest, OrdersLegacyAndPerThreadDefaultButNotUnknownStream) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    define void @kernel() { entry: ret void }
    define i32 @main(%stream_t* %incoming) {
    entry:
      %legacy = inttoptr i64 1 to %stream_t*
      %ptds = inttoptr i64 2 to %stream_t*
      %l0 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** null, i64 0, %stream_t* %legacy)
      %l1 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** null, i64 0, %stream_t* %ptds)
      %l2 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** null, i64 0, %stream_t* %incoming)
      ret i32 %l2
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 3u);
  EXPECT_TRUE(
      llvm::is_contained(analysis.getLaunches()[1].ordered_dependencies, 0u));
  EXPECT_FALSE(
      llvm::is_contained(analysis.getLaunches()[2].ordered_dependencies, 1u));
  EXPECT_EQ(analysis.getLaunches()[2].stream_kind,
            concurrency::cuda::HostStreamKind::Unknown);
}

TEST_F(CUDAAnalysisTest, ConditionalStreamSyncJoinsToUnknownState) {
  const char *source = R"(
    %stream_t = type opaque
    declare i32 @cudaStreamSynchronize(%stream_t*)
    define i32 @main(%stream_t* %s, i1 %cond) {
    entry:
      br i1 %cond, label %sync, label %join
    sync:
      %r = call i32 @cudaStreamSynchronize(%stream_t* %s)
      br label %join
    join:
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getStreamAutomata().size(), 1u);
  EXPECT_EQ(analysis.getStreamAutomata().front().current_state,
            concurrency::cuda::StreamState::Unknown);
  EXPECT_FALSE(analysis.getStreamAutomata().front().is_exact);
}

TEST_F(CUDAAnalysisTest, PreservesSparseWarpMasksAndPathConditionalSets) {
  const char *source = R"(
    declare void @llvm.nvvm.bar.warp.sync(i32)
    define ptx_kernel void @kernel() {
    entry:
      call void @llvm.nvvm.bar.warp.sync(i32 1431655765)
      call void @llvm.nvvm.bar.warp.sync(i32 -1431655766)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Function *kernel = module->getFunction("kernel");
  ASSERT_NE(kernel, nullptr);
  SmallVector<const Instruction *, 2> calls;
  for (const Instruction &inst : instructions(*kernel)) {
    if (isa<CallBase>(inst)) {
      calls.push_back(&inst);
    }
  }
  ASSERT_EQ(calls.size(), 2u);
  concurrency::cuda::CUDAParticipantAnalysis participants(*kernel);
  const auto lhs = participants.getActiveSetAt(calls[0]);
  const auto rhs = participants.getActiveSetAt(calls[1]);
  EXPECT_TRUE(lhs.has_lane_mask);
  EXPECT_TRUE(rhs.has_lane_mask);
  EXPECT_EQ(concurrency::cuda::computeOverlap(lhs, rhs),
            concurrency::cuda::ParticipantRelation::Disjoint);
}

TEST_F(CUDAAnalysisTest, InstantiatesFunctionEffectsAtEachCallsite) {
  const char *source = R"(
    %stream_t = type opaque
    declare i32 @cudaStreamSynchronize(%stream_t*)
    define void @helper(%stream_t* %s) {
    entry:
      %r = call i32 @cudaStreamSynchronize(%stream_t* %s)
      ret void
    }
    define void @main(%stream_t* %a, %stream_t* %b) {
    entry:
      call void @helper(%stream_t* %a)
      call void @helper(%stream_t* %b)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAFunctionSummaryAnalysis summaries(*module);
  summaries.runAnalysis();
  const auto *summary = summaries.getSummary(module->getFunction("main"));
  ASSERT_NE(summary, nullptr);
  size_t sync_effects = 0;
  SmallPtrSet<const Value *, 2> actuals;
  for (const auto &effect : summary->instantiated_effects) {
    if (effect.effect_class !=
        concurrency::cuda::CUDAEffectClass::Synchronization) {
      continue;
    }
    ++sync_effects;
    ASSERT_FALSE(effect.bindings.empty());
    actuals.insert(effect.bindings.front().second);
  }
  EXPECT_EQ(sync_effects, 2u);
  EXPECT_EQ(actuals.size(), 2u);
}

TEST_F(CUDAAnalysisTest, RecoversStackBuiltKernelArgumentsPerLaunch) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    @a = addrspace(1) global [8 x i32] zeroinitializer
    @b = addrspace(1) global [8 x i32] zeroinitializer
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    define void @kernel(i32 addrspace(1)* %p) {
    entry:
      store i32 1, i32 addrspace(1)* %p
      ret void
    }
    define i32 @main(%stream_t* %s0, %stream_t* %s1) {
    entry:
      %arg0 = alloca i32 addrspace(1)*
      %args0 = alloca [1 x i8*]
      store i32 addrspace(1)* getelementptr
          ([8 x i32], [8 x i32] addrspace(1)* @a, i32 0, i32 0),
          i32 addrspace(1)** %arg0
      %arg0.cast = bitcast i32 addrspace(1)** %arg0 to i8*
      %slot0 = getelementptr [1 x i8*], [1 x i8*]* %args0, i32 0, i32 0
      store i8* %arg0.cast, i8** %slot0
      %l0 = call i32 @cudaLaunchKernel(
          i8* bitcast (void (i32 addrspace(1)*)* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** %slot0, i64 0, %stream_t* %s0)
      %arg1 = alloca i32 addrspace(1)*
      %args1 = alloca [1 x i8*]
      store i32 addrspace(1)* getelementptr
          ([8 x i32], [8 x i32] addrspace(1)* @b, i32 0, i32 0),
          i32 addrspace(1)** %arg1
      %arg1.cast = bitcast i32 addrspace(1)** %arg1 to i8*
      %slot1 = getelementptr [1 x i8*], [1 x i8*]* %args1, i32 0, i32 0
      store i8* %arg1.cast, i8** %slot1
      %l1 = call i32 @cudaLaunchKernel(
          i8* bitcast (void (i32 addrspace(1)*)* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** %slot1, i64 0, %stream_t* %s1)
      ret i32 %l1
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  ASSERT_EQ(analysis.getLaunches()[0].argument_values.size(), 1u);
  ASSERT_EQ(analysis.getLaunches()[1].argument_values.size(), 1u);
  ASSERT_NE(analysis.getLaunches()[0].argument_values[0], nullptr);
  ASSERT_NE(analysis.getLaunches()[1].argument_values[0], nullptr);
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}

TEST_F(CUDAAnalysisTest, MaskedWarpBarrierOnSm70IsNotAutomaticallyMismatch) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare void @llvm.nvvm.bar.warp.sync(i32)
    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %cond = icmp ult i32 %tid, 16
      br i1 %cond, label %then, label %exit
    then:
      call void @llvm.nvvm.bar.warp.sync(i32 65535)
      br label %exit
    exit:
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_FALSE(analysis.getKernelSummaries().front().has_barrier_mismatch);
}

TEST_F(CUDAAnalysisTest, ProtocolEpochsUseReachableNextBoundaries) {
  const char *source = R"(
    declare void @llvm.nvvm.barrier0()
    define ptx_kernel void @kernel(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      call void @llvm.nvvm.barrier0()
      br label %exit
    right:
      call void @llvm.nvvm.barrier0()
      br label %exit
    exit:
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getBarrierEpochs().size(), 2u);
  const auto &first = analysis.getBarrierEpochs()[0];
  const auto &second = analysis.getBarrierEpochs()[1];
  EXPECT_NE(first.exit, second.entry);
  EXPECT_NE(second.exit, first.entry);
}

TEST_F(CUDAAnalysisTest, RejectsNonCudaSemanticNamesAndAS0NameHints) {
  const char *source = R"(
    @shared_counter = global i32 0
    declare void @myEventRecordHelper(i8*)
    define void @host(i8* %device_buffer) {
    entry:
      store i32 1, i32* @shared_counter
      call void @myEventRecordHelper(i8* %device_buffer)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  const Function *host = module->getFunction("host");
  const CallBase *call = nullptr;
  for (const Instruction &inst : instructions(*host)) {
    if (const auto *candidate = dyn_cast<CallBase>(&inst)) {
      call = candidate;
    }
  }
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(concurrency::cuda::lookupCUDASemantic(call), nullptr);
  EXPECT_EQ(concurrency::cuda::CUDAMemoryModel::classify(
                module->getNamedGlobal("shared_counter"))
                .space,
            concurrency::cuda::MemorySpace::Host);
  EXPECT_EQ(concurrency::cuda::CUDAMemoryModel::classify(host->getArg(0)).space,
            concurrency::cuda::MemorySpace::Unknown);
}

TEST_F(CUDAAnalysisTest, RecordsAtomicsMemIntrinsicsAndDeviceHelperBarriers) {
  const char *source = R"(
    @shared = addrspace(3) global [4 x i32] zeroinitializer
    @src = addrspace(1) global [16 x i8] zeroinitializer
    @dst = addrspace(1) global [16 x i8] zeroinitializer
    declare i32 @atomicAdd(i32 addrspace(3)*, i32)
    declare void @llvm.nvvm.barrier0()
    declare void @llvm.memcpy.p1i8.p1i8.i64(i8 addrspace(1)* nocapture writeonly,
                                            i8 addrspace(1)* nocapture readonly,
                                            i64, i1 immarg)
    define void @helper() {
    entry:
      call void @llvm.nvvm.barrier0()
      ret void
    }
    define ptx_kernel void @kernel() {
    entry:
      %p = getelementptr [4 x i32], [4 x i32] addrspace(3)* @shared,
                         i32 0, i32 0
      %old = call i32 @atomicAdd(i32 addrspace(3)* %p, i32 1)
      call void @llvm.memcpy.p1i8.p1i8.i64(
          i8 addrspace(1)* getelementptr
              ([16 x i8], [16 x i8] addrspace(1)* @dst, i32 0, i32 0),
          i8 addrspace(1)* getelementptr
              ([16 x i8], [16 x i8] addrspace(1)* @src, i32 0, i32 0),
          i64 16, i1 false)
      call void @helper()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_EQ(summary.atomic_count, 1u);
  EXPECT_GE(summary.accesses.size(), 3u);
  EXPECT_TRUE(llvm::any_of(summary.synchronizations, [](const auto &sync) {
    return sync.inst && sync.inst->getFunction()->getName() == "helper";
  }));
}

TEST_F(CUDAAnalysisTest, EventWaitOrdersFollowingAsyncTransfer) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    %event_t = type opaque
    @buffer = addrspace(1) global [8 x i32] zeroinitializer
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i32 @cudaMemcpyAsync(i32 addrspace(1)*, i32 addrspace(1)*,
                                 i64, i32, %stream_t*)
    define void @kernel() {
    entry:
      %p = getelementptr [8 x i32], [8 x i32] addrspace(1)* @buffer,
                         i32 0, i32 0
      store i32 1, i32 addrspace(1)* %p
      ret void
    }
    define i32 @main(%stream_t* %producer, %stream_t* %consumer,
                     %event_t* %event) {
    entry:
      %l = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** null, i64 0, %stream_t* %producer)
      %r = call i32 @cudaEventRecord(%event_t* %event, %stream_t* %producer)
      %w = call i32 @cudaStreamWaitEvent(%stream_t* %consumer,
                                         %event_t* %event, i32 0)
      %c = call i32 @cudaMemcpyAsync(
          i32 addrspace(1)* getelementptr
              ([8 x i32], [8 x i32] addrspace(1)* @buffer, i32 0, i32 0),
          i32 addrspace(1)* getelementptr
              ([8 x i32], [8 x i32] addrspace(1)* @buffer, i32 0, i32 1),
          i64 4, i32 3, %stream_t* %consumer)
      ret i32 %c
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}

TEST_F(CUDAAnalysisTest, GraphLaunchIsRetainedAsOpaqueExecution) {
  const char *source = R"(
    %graph_t = type opaque
    %stream_t = type opaque
    declare i32 @cudaGraphLaunch(%graph_t*, %stream_t*)
    define i32 @main(%graph_t* %graph, %stream_t* %stream) {
    entry:
      %r = call i32 @cudaGraphLaunch(%graph_t* %graph, %stream_t* %stream)
      ret i32 %r
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  EXPECT_TRUE(analysis.getLaunches().front().is_opaque);
  EXPECT_EQ(analysis.getLaunches().front().stream,
            module->getFunction("main")->getArg(1));
}

TEST_F(CUDAAnalysisTest, DriverStreamAndEventReloadsShareCanonicalHandles) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque
    declare i32 @cuStreamCreate(%stream_t**, i32)
    declare i32 @cuEventCreate(%event_t**, i32)
    declare i32 @cuEventRecord(%event_t*, %stream_t*)
    declare i32 @cuStreamWaitEvent(%stream_t*, %event_t*, i32)
    declare i32 @cuStreamSynchronize(%stream_t*)
    define i32 @main() {
    entry:
      %ss = alloca %stream_t*
      %es = alloca %event_t*
      %sc = call i32 @cuStreamCreate(%stream_t** %ss, i32 0)
      %ec = call i32 @cuEventCreate(%event_t** %es, i32 0)
      %s0 = load %stream_t*, %stream_t** %ss
      %e0 = load %event_t*, %event_t** %es
      %r = call i32 @cuEventRecord(%event_t* %e0, %stream_t* %s0)
      %s1 = load %stream_t*, %stream_t** %ss
      %e1 = load %event_t*, %event_t** %es
      %w = call i32 @cuStreamWaitEvent(%stream_t* %s1, %event_t* %e1, i32 0)
      %z = call i32 @cuStreamSynchronize(%stream_t* %s1)
      ret i32 %z
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getEventAutomata().size(), 1u);
  EXPECT_TRUE(analysis.getEventAutomata().front().has_record);
  EXPECT_TRUE(analysis.getEventAutomata().front().has_wait);
  ASSERT_EQ(analysis.getStreamAutomata().size(), 1u);
  EXPECT_EQ(analysis.getStreamAutomata().front().current_state,
            concurrency::cuda::StreamState::Synchronized);
}

TEST_F(CUDAAnalysisTest, DistinctKernelPointerArgumentsRemainMayAlias) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    define void @kernel(i32 addrspace(1)* %a, i32 addrspace(1)* %b) {
    entry:
      store i32 1, i32 addrspace(1)* %a
      store i32 2, i32 addrspace(1)* %b
      ret void
    }
    define void @main(i32 addrspace(1)* %a, i32 addrspace(1)* %b) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel(i32 addrspace(1)* %a, i32 addrspace(1)* %b)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getKernelSummaries().front().has_global_race);
}

TEST_F(CUDAAnalysisTest, ShuffleResultIsNotAssumedWarpUniform) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @nvvm.shfl.down.fake(i32)
    define void @kernel() {
    entry:
      %value = call i32 @nvvm.shfl.down.fake(i32 1)
      %cond = icmp eq i32 %value, 0
      br i1 %cond, label %then, label %exit
    then:
      br label %exit
    exit:
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getKernelSummaries().front().has_warp_divergence);
}

TEST_F(CUDAAnalysisTest, OpaqueAndKnownLaunchesRemainPotentiallyConflicting) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    @buffer = addrspace(1) global i32 0
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    define void @known() {
    entry:
      store i32 1, i32 addrspace(1)* @buffer
      ret void
    }
    define i32 @main(i8* %unknown, %stream_t* %s0, %stream_t* %s1) {
    entry:
      %l0 = call i32 @cudaLaunchKernel(i8* %unknown,
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** null, i64 0, %stream_t* %s0)
      %l1 = call i32 @cudaLaunchKernel(i8* bitcast (void ()* @known to i8*),
          %dim3 { i32 1, i32 1, i32 1 }, %dim3 { i32 1, i32 1, i32 1 },
          i8** null, i64 0, %stream_t* %s1)
      ret i32 %l1
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_FALSE(analysis.getInterKernelRaces().empty());
}

TEST_F(CUDAAnalysisTest, DetectsSingleStaticSharedWriteAcrossThreads) {
  const char *source = R"(
    @shared = addrspace(3) global i32 0
    declare void @__set_CUDAConfig(i32, i32)
    define void @kernel() {
    entry:
      store i32 1, i32 addrspace(3)* @shared
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getKernelSummaries().front().has_shared_race);
}

TEST_F(CUDAAnalysisTest, InvalidMaskedWarpBarrierStillReportsMismatch) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare void @llvm.nvvm.bar.warp.sync(i32)
    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %cond = icmp ult i32 %tid, 16
      br i1 %cond, label %then, label %exit
    then:
      call void @llvm.nvvm.bar.warp.sync(i32 255)
      br label %exit
    exit:
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getKernelSummaries().front().has_barrier_mismatch);
}

TEST_F(CUDAAnalysisTest, IgnoresHostAddrspaceZeroMemcpyAndHandlesRecursiveHelpers) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1 immarg)
    define void @host_a(i8* %dst, i8* %src) {
    entry:
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst, i8* %src, i64 8, i1 false)
      call void @host_b(i8* %dst, i8* %src)
      ret void
    }
    define void @host_b(i8* %dst, i8* %src) {
    entry:
      call void @host_a(i8* %dst, i8* %src)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getKernelSummaries().empty());
  EXPECT_TRUE(analysis.getMemoryTransfers().empty());
  EXPECT_EQ(analysis.getOperationCount(), 0u);
}
